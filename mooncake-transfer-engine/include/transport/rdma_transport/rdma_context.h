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

#ifndef RDMA_CONTEXT_H
#define RDMA_CONTEXT_H

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <infiniband/verbs.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "common.h"
#include "rdma_transport.h"
#include "transport/transport.h"

namespace mooncake {

/**
 * @class RdmaContext
 * @brief RDMA设备上下文管理类
 *
 * 该类负责管理单个RDMA网卡(NIC)的所有资源，包括：
 * 1. 内存区域（Memory Region）管理
 * 2. 完成队列（Completion Queue）管理
 * 3. 端点（QP）管理
 * 4. 事件通道管理
 *
 * 每个物理网卡对应一个RdmaContext实例。
 */
class RdmaContext {
   public:
    /**
     * @brief 构造函数
     * @param engine RDMA传输引擎的引用
     * @param device_name 设备名称，如 "mlx5_0"
     */
    RdmaContext(RdmaTransport &engine, const std::string &device_name);

    /**
     * @brief 析构函数
     * 负责清理所有RDMA资源：
     * 1. 销毁所有端点
     * 2. 注销内存区域
     * 3. 释放完成队列和事件通道
     */
    ~RdmaContext();

    /**
     * @brief 构建RDMA上下文
     * @param num_cq_list 完成队列数量
     * @param num_comp_channels 完成事件通道数量
     * @param port RDMA端口号
     * @param gid_index GID索引（用于RoCE网络）
     * @param max_cqe 每个完成队列的最大条目数
     * @param max_ep_per_ctx 每个上下文的最大端点数
     * @param max_qp_per_ep 每个端点的最大队列对数
     * @param max_sge 每个请求的最大分散/聚集元素数
     * @param max_wr 每个队列对的最大工作请求数
     * @param max_inline 最大内联数据大小
     * @return 成功返回0，失败返回错误码
     */
    int construct(size_t num_cq_list = 1, size_t num_comp_channels = 1,
                  uint8_t port = 1, int gid_index = 0, size_t max_cqe = 4096,
                  int max_ep_per_ctx = 256, size_t num_qp_per_ep = 2,
                  size_t max_sge = 4, size_t max_wr = 256,
                  size_t max_inline = 64);

    /**
     * @brief 注册内存区域
     * @param addr 内存地址
     * @param length 内存长度
     * @param access_flags 访问权限标志
     * @return 已注册的内存区域指针，失败返回nullptr
     *
     * 该方法会：
     * 1. 向RDMA设备注册内存区域
     * 2. 获取内存区域的本地密钥(lkey)和远程密钥(rkey)
     * 3. 将内存区域加入管理集合
     */
    struct ibv_mr *registerMemory(void *addr, size_t length,
                                 int access_flags = IBV_ACCESS_LOCAL_WRITE |
                                                  IBV_ACCESS_REMOTE_READ |
                                                  IBV_ACCESS_REMOTE_WRITE);

    /**
     * @brief 注销内存区域
     * @param addr 内存地址
     * @return 成功返回0，失败返回错误码
     */
    int deregisterMemory(void *addr);

    /**
     * @brief 从内存池分配页对齐的内存
     * @param size 需要的内存大小
     * @return 分配的内存地址
     */
    void *allocAlignedMemory(size_t size);

    /**
     * @brief 归还分配的页对齐内存
     * @param ptr 要释放的内存地址
     */
    void freeAlignedMemory(void *ptr);

    /**
     * @brief 获取当前上下文的端点数量
     * @return 端点数量
     */
    size_t getEndpointCount() const { return endpoint_count_; }

    /**
     * @brief 获取设备名称
     * @return 设备名称字符串
     */
    const std::string &getDeviceName() const { return device_name_; }

    /**
     * @brief 获取本地标识符(LID)
     * @return 本地标识符
     */
    uint16_t getLid() const { return port_attr_.lid; }

    /**
     * @brief 获取全局标识符(GID)
     * @return GID字符串
     */
    const std::string &getGid() const { return gid_str_; }

   private:
    /**
     * @brief 初始化工作池
     * 创建并启动完成队列的事件处理线程
     */
    void initializeWorkerPool();

    /**
     * @brief 清理工作池资源
     * 停止所有事件处理线程并释放资源
     */
    void cleanupWorkerPool();

   private:
    int openRdmaDevice(const std::string &device_name, uint8_t port,
                       int gid_index);

    int joinNonblockingPollList(int event_fd, int data_fd);

    int getBestGidIndex(const std::string &device_name,
                        struct ibv_context *context, ibv_port_attr &port_attr,
                        uint8_t port);

   public:
    int submitPostSend(const std::vector<Transport::Slice *> &slice_list);

   private:
    const std::string device_name_;
    RdmaTransport &engine_;

    ibv_context *context_ = nullptr;
    ibv_pd *pd_ = nullptr;
    int event_fd_ = -1;

    // 内存区域映射表，用于管理所有注册的内存区域
    std::unordered_map<void *, struct ibv_mr *> mr_map_;
    RWSpinlock mr_map_lock_;                    // 内存区域映射表的读写锁
};

}  // namespace mooncake

#endif  // RDMA_CONTEXT_H
