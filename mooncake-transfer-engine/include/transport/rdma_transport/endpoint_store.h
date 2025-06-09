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

#ifndef ENDPOINT_STORE_H_
#define ENDPOINT_STORE_H_

#include <infiniband/verbs.h>

#include <atomic>
#include <memory>
#include <optional>

#include "rdma_context.h"
#include "rdma_endpoint.h"

using namespace mooncake;

namespace mooncake {
/**
 * @class EndpointStore
 * @brief RDMA端点存储管理器的抽象基类
 *
 * 该类负责管理RDMA端点（Endpoint）的生命周期和缓存：
 * 1. 端点的创建、获取和删除
 * 2. 端点的缓存策略管理
 * 3. 端点资源的复用和回收
 *
 * 不同的实现类可以提供不同的缓存淘汰策略：
 * - FIFO (先进先出)
 * - LRU (最近最少使用)
 * - LFU (最不经常使用)
 */
class EndpointStore {
   public:
    /**
     * @brief 根据对端NIC路径获取RDMA端点
     * @param peer_nic_path 对端网卡设备路径，例如 "mlx5_0"
     * @return 端点的共享指针，如果不存在返回nullptr
     *
     * 该方法会：
     * 1. 在缓存中查找对应的端点
     * 2. 如果找到，更新端点的使用状态
     * 3. 如果未找到，返回nullptr
     */
    virtual std::shared_ptr<RdmaEndPoint> getEndpoint(
        std::string peer_nic_path) = 0;

    /**
     * @brief 创建并插入新的RDMA端点
     * @param peer_nic_path 对端网卡设备路径
     * @param context RDMA上下文指针
     * @return 新创建的端点的共享指针
     *
     * 该方法会：
     * 1. 创建新的RDMA端点
     * 2. 初始化端点的资源（QP、CQ等）
     * 3. 将端点加入缓存管理
     * 4. 如果缓存已满，可能会触发淘汰策略
     */
    virtual std::shared_ptr<RdmaEndPoint> insertEndpoint(
        std::string peer_nic_path, RdmaContext *context) = 0;

    /**
     * @brief 删除指定的RDMA端点
     * @param peer_nic_path 对端网卡设备路径
     * @return 成功返回0，失败返回错误码
     *
     * 该方法会：
     * 1. 从缓存中移除端点
     * 2. 清理端点占用的资源
     * 3. 更新缓存状态
     */
    virtual int deleteEndpoint(std::string peer_nic_path) = 0;

    /**
     * @brief 析构函数
     * 清理所有缓存的端点资源
     */
    virtual ~EndpointStore() = default;
};

// FIFO
class FIFOEndpointStore : public EndpointStore {
   public:
    FIFOEndpointStore(size_t max_size) : max_size_(max_size) {}
    std::shared_ptr<RdmaEndPoint> getEndpoint(std::string peer_nic_path);
    std::shared_ptr<RdmaEndPoint> insertEndpoint(std::string peer_nic_path,
                                                 RdmaContext *context);
    int deleteEndpoint(std::string peer_nic_path);
    void evictEndpoint();
    void reclaimEndpoint();
    size_t getSize();

    int destroyQPs();

   private:
    RWSpinlock endpoint_map_lock_;
    std::unordered_map<std::string, std::shared_ptr<RdmaEndPoint>>
        endpoint_map_;
    std::unordered_map<std::string, std::list<std::string>::iterator> fifo_map_;
    std::list<std::string> fifo_list_;

    std::unordered_set<std::shared_ptr<RdmaEndPoint>> waiting_list_;

    size_t max_size_;
};

// NSDI 24, similar to clock with quick demotion
class SIEVEEndpointStore : public EndpointStore {
   public:
    SIEVEEndpointStore(size_t max_size)
        : waiting_list_len_(0), max_size_(max_size){};
    std::shared_ptr<RdmaEndPoint> getEndpoint(std::string peer_nic_path);
    std::shared_ptr<RdmaEndPoint> insertEndpoint(std::string peer_nic_path,
                                                 RdmaContext *context);
    int deleteEndpoint(std::string peer_nic_path);
    void evictEndpoint();
    void reclaimEndpoint();
    size_t getSize();

    int destroyQPs();

   private:
    RWSpinlock endpoint_map_lock_;
    // The bool represents visited
    std::unordered_map<
        std::string, std::pair<std::shared_ptr<RdmaEndPoint>, std::atomic_bool>>
        endpoint_map_;
    std::unordered_map<std::string, std::list<std::string>::iterator> fifo_map_;
    std::list<std::string> fifo_list_;

    std::optional<std::list<std::string>::iterator> hand_;

    std::unordered_set<std::shared_ptr<RdmaEndPoint>> waiting_list_;
    std::atomic<int> waiting_list_len_;

    size_t max_size_;
};
}  // namespace mooncake

#endif

