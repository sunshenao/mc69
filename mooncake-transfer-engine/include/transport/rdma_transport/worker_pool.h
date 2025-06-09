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

#ifndef WORKER_H
#define WORKER_H

#include <queue>
#include <unordered_set>

#include "rdma_context.h"

namespace mooncake {

/**
 * @class WorkerPool
 * @brief RDMA工作线程池，负责处理异步的RDMA操作
 *
 * 工作线程池主要职责：
 * 1. 处理发送请求的提交和完成
 * 2. 轮询完成队列(CQ)获取完成事件
 * 3. 重新分发未完成的传输任务
 * 4. 监控RDMA设备状态
 */
class WorkerPool {
   public:
    /**
     * @brief 构造函数
     * @param context RDMA上下文引用
     * @param numa_socket_id NUMA节点ID，用于CPU亲和性设置
     *
     * 初始化工作线程池：
     * 1. 创建工作线程
     * 2. 设置CPU亲和性
     * 3. 初始化任务队列
     */
    WorkerPool(RdmaContext &context, int numa_socket_id = 0);

    /**
     * @brief 析构函数
     * 清理所有工作线程和资源
     */
    ~WorkerPool();

    /**
     * @brief 提交发送切片到工作队列
     * @param slice_list 要发送的切片列表
     * @return 成功返回0，失败返回错误码
     *
     * 该方法由传输层调用，用于提交新的发送请求
     */
    int submitPostSend(const std::vector<Transport::Slice *> &slice_list);

   private:
    /**
     * @brief 执行发送操作
     * @param thread_id 工作线程ID
     *
     * 该方法在工作线程中运行，负责：
     * 1. 从队列获取待发送的切片
     * 2. 构建和提交RDMA发送请求
     * 3. 处理发送完成事件
     */
    void performPostSend(int thread_id);

    /**
     * @brief 轮询完成队列
     * @param thread_id 工作线程ID
     *
     * 持续轮询完成队列，处理：
     * 1. 发送完成事件
     * 2. 接收完成事件
     * 3. 错误事件
     */
    void performPollCq(int thread_id);

    /**
     * @brief 重新分发未完成的传输任务
     * @param slice_list 切片列表
     * @param thread_id 工作线程ID
     *
     * 当任务执行失败时：
     * 1. 重试传输
     * 2. 在线程间重新分配负载
     * 3. 处理错误情况
     */
    void redispatch(std::vector<Transport::Slice *> &slice_list, int thread_id);

    /**
     * @brief 传输工作线程主函数
     * @param thread_id 线程ID
     *
     * 工作线程的主要逻辑：
     * 1. 处理发送请求
     * 2. 轮询完成事件
     * 3. 执行错误恢复
     */
    void transferWorker(int thread_id);

    /**
     * @brief 监控线程主函数
     *
     * 负责监控：
     * 1. RDMA设备状态
     * 2. 线程健康状况
     * 3. 资源使用情况
     */
    void monitorWorker();

    /**
     * @brief 处理上下文事件
     * @return 成功返回0，失败返回错误码
     *
     * 处理RDMA异步事件：
     * 1. 端口状态变化
     * 2. QP状态变化
     * 3. 路径迁移事件
     */
    int doProcessContextEvents();

   private:
    RdmaContext &context_;                    // RDMA上下文引用
    const int numa_socket_id_;               // NUMA节点ID

    std::vector<std::thread> worker_thread_;  // 工作线程列表
    std::atomic<bool> workers_running_;       // 线程运行状态标志

    /**
     * @brief 任务队列结构，用于存储待处理的传输任务
     */
    struct TaskQueue {
        std::queue<std::vector<Transport::Slice *>> task_queue;  // 任务队列
        std::mutex mutex;                                       // 队列互斥锁
        std::condition_variable cv;                            // 条件变量，用于任务通知
    };

    std::vector<TaskQueue> task_queues_;     // 每个线程的任务队列
    std::atomic<size_t> next_queue_;         // 下一个要使用的队列索引

    // 性能统计
    std::atomic<uint64_t> total_polls_;      // 总轮询次数
    std::atomic<uint64_t> total_works_;      // 总处理工作数
    std::atomic<uint64_t> total_timeouts_;   // 总超时次数
};
}  // namespace mooncake

#endif  // WORKER_H
