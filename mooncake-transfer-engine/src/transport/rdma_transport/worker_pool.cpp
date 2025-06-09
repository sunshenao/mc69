// Copyright 2024 KVCache.AI
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "transport/rdma_transport/worker_pool.h"

#include <sys/epoll.h>

#include <cassert>

#include "config.h"
#include "transport/rdma_transport/rdma_context.h"
#include "transport/rdma_transport/rdma_endpoint.h"
#include "transport/rdma_transport/rdma_transport.h"

#ifdef USE_CUDA
#include <cuda_runtime.h>
#endif  // USE_CUDA

namespace mooncake {

/**
 * @brief 根据全局配置获取传输工作线程数量
 */
const static int kTransferWorkerCount = globalConfig().workers_per_ctx;

/**
 * @brief 工作线程池构造函数
 * @param context RDMA上下文引用
 * @param numa_socket_id NUMA节点ID，用于CPU亲和性设置
 *
 * 初始化工作线程池：
 * 1. 设置基本参数和计数器
 * 2. 初始化切片队列
 * 3. 创建工作线程
 */
WorkerPool::WorkerPool(RdmaContext &context, int numa_socket_id)
    : context_(context),              // RDMA上下文
      numa_socket_id_(numa_socket_id),// NUMA节点ID
      workers_running_(true),         // 工作线程运行标志
      suspended_flag_(0),            // 暂停标志
      redispatch_counter_(0),        // 重分发计数器
      submitted_slice_count_(0),      // 已提交切片计数
      processed_slice_count_(0) {     // 已处理切片计数

    // 初始化每个分片的切片队列计数器
    for (int i = 0; i < kShardCount; ++i)
        slice_queue_count_[i].store(0, std::memory_order_relaxed);

    // 分配集体切片队列空间
    collective_slice_queue_.resize(kTransferWorkerCount);

    // 创建工作线程
    for (int i = 0; i < kTransferWorkerCount; ++i)
        worker_thread_.emplace_back(
            std::thread(std::bind(&WorkerPool::transferWorker, this, i)));

    // 创建并启动监控线程
    monitor_thread_ = std::thread(std::bind(&WorkerPool::monitorWorker, this));
}

/**
 * @brief 工作线程池析构函数
 *
 * 清理工作线程池资源：
 * 1. 停止所有工作线程
 * 2. 等待线程结束
 * 3. 清理未处理的切片
 */
WorkerPool::~WorkerPool() {
    // 停止所有工作线程
    workers_running_ = false;

    // 通知所有工作线程结束
    for (auto &queue : collective_slice_queue_) {
        std::unique_lock<std::mutex> lock(queue.mutex);
        queue.cv.notify_all();
    }

    // 等待所有工作线程结束
    for (auto &thread : worker_thread_)
        thread.join();

    // 等待监控线程结束
    monitor_thread_.join();

    // 清理所有未处理的切片
    for (auto &queue : collective_slice_queue_) {
        std::unique_lock<std::mutex> lock(queue.mutex);
        for (auto &slice_list : queue.slice_queue) {
            for (auto slice : slice_list) {
                slice->status = Transport::Slice::FAILED;
                __sync_fetch_and_sub(slice->rdma.qp_depth, 1);
                *((volatile uint64_t *)&slice->task->failed_slice_count) += 1;
            }
        }
    }
}

/**
 * @brief 提交切片到工作队列
 * @param slice_list 切片列表
 * @return 成功返回0，失败返回错误码
 *
 * 该方法将切片分配给工作线程：
 * 1. 选择负载最小的工作队列
 * 2. 将切片列表添加到队列
 * 3. 通知工作线程处理
 */
int WorkerPool::submitPostSend(const std::vector<Transport::Slice *> &slice_list) {
    if (slice_list.empty()) return 0;

    // 更新提交计数
    submitted_slice_count_ += slice_list.size();

    // 选择负载最小的工作队列
    size_t min_queue = 0;
    size_t min_size = SIZE_MAX;
    for (size_t i = 0; i < collective_slice_queue_.size(); i++) {
        auto &queue = collective_slice_queue_[i];
        std::unique_lock<std::mutex> lock(queue.mutex);
        size_t size = 0;
        for (auto &list : queue.slice_queue)
            size += list.size();
        if (size < min_size) {
            min_size = size;
            min_queue = i;
        }
    }

    // 将切片列表添加到选中的队列
    auto &queue = collective_slice_queue_[min_queue];
    std::unique_lock<std::mutex> lock(queue.mutex);
    queue.slice_queue.push_back(slice_list);

    // 通知工作线程处理新任务
    queue.cv.notify_one();
    return 0;
}

/**
 * @brief 工作线程主函数
 * @param thread_id 线程ID
 *
 * 工作线程的主循环：
 * 1. 等待新的切片任务
 * 2. 处理切片传输请求
 * 3. 轮询完成队列
 * 4. 处理错误和重试
 */
void WorkerPool::transferWorker(int thread_id) {
    // 设置线程亲和性
    if (numa_socket_id_ >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(numa_socket_id_, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }

    // 设置线程名称
    std::string thread_name = "rdma_worker_" + std::to_string(thread_id);
    pthread_setname_np(pthread_self(), thread_name.c_str());

    std::vector<Transport::Slice *> slice_list;
    std::vector<Transport::Slice *> failed_slice_list;

    // 主循环
    while (workers_running_) {
        // 检查暂停标志
        if (suspended_flag_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // 处理完成事件
        performPollCq(thread_id);

        // 获取新的切片任务
        auto &queue = collective_slice_queue_[thread_id];
        {
            std::unique_lock<std::mutex> lock(queue.mutex);
            if (queue.slice_queue.empty()) {
                queue.cv.wait_for(lock, std::chrono::microseconds(100));
                continue;
            }
            slice_list = std::move(queue.slice_queue.front());
            queue.slice_queue.pop_front();
        }

        // 处理切片传输请求
        while (!slice_list.empty()) {
            failed_slice_list.clear();
            // 提交RDMA发送请求
            int rc = context_.submitPostSend(slice_list, failed_slice_list);
            if (rc) {
                LOG(ERROR) << "Failed to submit post send requests";
                for (auto slice : slice_list) {
                    slice->status = Transport::Slice::FAILED;
                    *((volatile uint64_t *)&slice->task->failed_slice_count) += 1;
                }
                break;
            }

            // 处理失败的切片
            if (!failed_slice_list.empty()) {
                redispatch(failed_slice_list, thread_id);
            }
        }
    }
}

/**
 * @brief 轮询完成队列
 * @param thread_id 线程ID
 *
 * 处理RDMA完成事件：
 * 1. 轮询完成队列获取事件
 * 2. 更新切片状态
 * 3. 处理完成和失败的情况
 */
void WorkerPool::performPollCq(int thread_id) {
    struct ibv_wc wc[kMaxPollCqBatch];  // 完成事件数组

    // 轮询完成队列
    int num_entries = ibv_poll_cq(context_.getCQ(), kMaxPollCqBatch, wc);
    if (num_entries < 0) {
        LOG(ERROR) << "Failed to poll CQ";
        return;
    }

    // 处理每个完成事件
    for (int i = 0; i < num_entries; ++i) {
        Transport::Slice *slice = (Transport::Slice *)wc[i].wr_id;
        // 更新工作请求深度
        __sync_fetch_and_sub(slice->rdma.qp_depth, 1);

        if (wc[i].status == IBV_WC_SUCCESS) {
            // 传输成功
            slice->status = Transport::Slice::SUCCESS;
            *((volatile uint64_t *)&slice->task->transferred_bytes) += slice->length;
            *((volatile uint64_t *)&slice->task->success_slice_count) += 1;
        } else {
            // 传输失败
            LOG(ERROR) << "Work completion failed with status: "
                      << ibv_wc_status_str(wc[i].status);
            slice->status = Transport::Slice::FAILED;
            *((volatile uint64_t *)&slice->task->failed_slice_count) += 1;
        }
    }

    // 更新统计信息
    processed_slice_count_ += num_entries;
}

/**
 * @brief 重新分发失败的切片
 * @param slice_list 失败的切片列表
 * @param thread_id 当前线程ID
 *
 * 处理传输失败的切片：
 * 1. 检查重试次数
 * 2. 重新提交或标记为失败
 * 3. 在工作线程间重新分配
 */
void WorkerPool::redispatch(std::vector<Transport::Slice *> &slice_list,
                           int thread_id) {
    if (slice_list.empty()) return;

    // 增加重分发计数
    redispatch_counter_ += slice_list.size();

    // 选择下一个工作队列
    size_t next_queue = (thread_id + 1) % collective_slice_queue_.size();
    auto &queue = collective_slice_queue_[next_queue];

    // 将切片添加到队列
    {
        std::unique_lock<std::mutex> lock(queue.mutex);
        queue.slice_queue.push_back(slice_list);
    }

    // 通知工作线程处理
    queue.cv.notify_one();
}

/**
 * @brief 监控线程主函数
 *
 * 负责监控工作线程池的状态：
 * 1. 监控RDMA设备状态
 * 2. 收集性能统计信息
 * 3. 处理异常情况
 */
void WorkerPool::monitorWorker() {
    // 设置线程名称
    pthread_setname_np(pthread_self(), "rdma_monitor");

    while (workers_running_) {
        // 处理RDMA异步事件
        doProcessContextEvents();

        // 检查设备状态
        if (!context_.active()) {
            LOG(ERROR) << "RDMA device is not active";
            suspended_flag_ = 1;
        }

        // 休眠一段时间
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

/**
 * @brief 处理RDMA上下文事件
 * @return 成功返回0，失败返回错误码
 *
 * 处理RDMA设备的异步事件：
 * 1. 端口状态变化
 * 2. 路径迁移事件
 * 3. QP状态变化
 */
int WorkerPool::doProcessContextEvents() {
    struct ibv_async_event event;

    // 获取异步事件
    if (ibv_get_async_event(context_.getContext(), &event)) {
        if (errno != EAGAIN) {
            PLOG(ERROR) << "Failed to get async event";
            return ERR_DEVICE;
        }
        return 0;
    }

    // 处理不同类型的事件
    switch (event.event_type) {
        case IBV_EVENT_PORT_ACTIVE:
            LOG(INFO) << "Port active event received";
            suspended_flag_ = 0;
            break;

        case IBV_EVENT_PORT_ERR:
            LOG(ERROR) << "Port error event received";
            suspended_flag_ = 1;
            break;

        default:
            LOG(INFO) << "Async event received: " << event.event_type;
            break;
    }

    // 确认事件处理完成
    ibv_ack_async_event(&event);
    return 0;
}

} // namespace mooncake
