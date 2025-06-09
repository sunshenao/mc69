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

#include "transport/rdma_transport/endpoint_store.h"

#include <glog/logging.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

#include "config.h"
#include "transport/rdma_transport/rdma_context.h"
#include "transport/rdma_transport/rdma_endpoint.h"

namespace mooncake {

/**
 * @brief 从端点存储中获取RDMA端点
 * @param peer_nic_path 对端网卡路径
 * @return 端点的共享指针，如果不存在返回nullptr
 *
 * 该函数从FIFO缓存中查找端点：
 * 1. 使用读锁保护并发访问
 * 2. 在映射表中查找对端路径
 * 3. 返回找到的端点或nullptr
 */
std::shared_ptr<RdmaEndPoint> FIFOEndpointStore::getEndpoint(
    std::string peer_nic_path) {
    RWSpinlock::ReadGuard guard(endpoint_map_lock_);
    auto iter = endpoint_map_.find(peer_nic_path);
    if (iter != endpoint_map_.end()) return iter->second;
    return nullptr;
}

/**
 * @brief 向端点存储中插入新的RDMA端点
 * @param peer_nic_path 对端网卡路径
 * @param context RDMA上下文指针
 * @return 新创建的端点或已存在的端点
 *
 * 该函数处理端点的创建和插入：
 * 1. 检查端点是否已存在
 * 2. 创建新端点并初始化
 * 3. 如果缓存已满，触发淘汰
 * 4. 将新端点加入FIFO队列
 */
std::shared_ptr<RdmaEndPoint> FIFOEndpointStore::insertEndpoint(
    std::string peer_nic_path, RdmaContext *context) {
    RWSpinlock::WriteGuard guard(endpoint_map_lock_);

    // 检查端点是否已存在
    if (endpoint_map_.find(peer_nic_path) != endpoint_map_.end()) {
        LOG(INFO) << "Endpoint " << peer_nic_path
                  << " already exists in FIFOEndpointStore";
        return endpoint_map_[peer_nic_path];
    }

    // 创建新的端点
    auto endpoint = std::make_shared<RdmaEndPoint>(*context);
    if (!endpoint) {
        LOG(ERROR) << "Failed to allocate memory for RdmaEndPoint";
        return nullptr;
    }

    // 配置端点参数
    auto &config = globalConfig();
    if (int ret = endpoint->construct(
            context->getCQ(), config.num_qp_per_ep, config.max_sge,
            config.max_wr, config.max_inline)) {
        LOG(ERROR) << "Failed to construct endpoint: " << ret;
        return nullptr;
    }

    // 设置对端路径
    endpoint->setPeerNicPath(peer_nic_path);

    // 如果缓存已满，执行淘汰
    if (endpoint_map_.size() >= max_size_) {
        evictEndpoint();
    }

    // 将新端点加入FIFO队列和映射表
    fifo_list_.push_back(peer_nic_path);
    fifo_map_[peer_nic_path] = --fifo_list_.end();
    endpoint_map_[peer_nic_path] = endpoint;

    return endpoint;
}

/**
 * @brief 从端点存储中删除RDMA端点
 * @param peer_nic_path 对端网卡路径
 * @return 成功返回0，失败返回错误码
 *
 * 该函数处理端点的删除：
 * 1. 从映射表中移除端点
 * 2. 从FIFO队列中移除
 * 3. 清理端点资源
 */
int FIFOEndpointStore::deleteEndpoint(std::string peer_nic_path) {
    RWSpinlock::WriteGuard guard(endpoint_map_lock_);

    // 从映射表中查找端点
    auto iter = endpoint_map_.find(peer_nic_path);
    if (iter == endpoint_map_.end()) {
        LOG(ERROR) << "Endpoint " << peer_nic_path << " not found";
        return ERR_ENDPOINT;
    }

    // 检查端点是否有未完成的传输
    if (iter->second->hasOutstandingSlice()) {
        waiting_list_.insert(iter->second);
        LOG(INFO) << "Endpoint " << peer_nic_path
                  << " has outstanding slices, deferred deletion";
        return 0;
    }

    // 从FIFO队列和映射表中移除
    auto fifo_iter = fifo_map_.find(peer_nic_path);
    if (fifo_iter != fifo_map_.end()) {
        fifo_list_.erase(fifo_iter->second);
        fifo_map_.erase(fifo_iter);
    }
    endpoint_map_.erase(iter);

    return 0;
}

/**
 * @brief 淘汰最早插入的端点
 *
 * FIFO淘汰策略：
 * 1. 从队列头部获取最早的端点
 * 2. 检查端点是否可以被淘汰
 * 3. 删除端点并清理资源
 */
void FIFOEndpointStore::evictEndpoint() {
    while (!fifo_list_.empty()) {
        std::string victim = fifo_list_.front();
        auto iter = endpoint_map_.find(victim);
        if (iter == endpoint_map_.end()) {
            fifo_list_.pop_front();
            fifo_map_.erase(victim);
            continue;
        }

        if (iter->second->hasOutstandingSlice()) {
            waiting_list_.insert(iter->second);
            LOG(INFO) << "Endpoint " << victim
                      << " has outstanding slices, deferred eviction";
            fifo_list_.pop_front();
            fifo_map_.erase(victim);
            endpoint_map_.erase(iter);
            return;
        }

        LOG(INFO) << "Evicting endpoint " << victim;
        fifo_list_.pop_front();
        fifo_map_.erase(victim);
        endpoint_map_.erase(iter);
        return;
    }
}

/**
 * @brief 回收已完成传输的端点
 *
 * 检查等待列表中的端点：
 * 1. 遍历所有等待回收的端点
 * 2. 对于没有未完成传输的端点进行清理
 * 3. 从等待列表中移除已处理的端点
 */
void FIFOEndpointStore::reclaimEndpoint() {
    std::unordered_set<std::shared_ptr<RdmaEndPoint>> to_remove;
    for (auto &endpoint : waiting_list_) {
        if (!endpoint->hasOutstandingSlice()) {
            to_remove.insert(endpoint);
        }
    }

    for (auto &endpoint : to_remove) {
        waiting_list_.erase(endpoint);
    }
}

/**
 * @brief 获取当前存储的端点数量
 * @return 端点数量
 */
size_t FIFOEndpointStore::getSize() {
    RWSpinlock::ReadGuard guard(endpoint_map_lock_);
    return endpoint_map_.size();
}

/**
 * @brief 销毁所有端点的队列对
 * @return 成功返回0，失败返回错误码
 *
 * 该函数在关闭时清理所有资源：
 * 1. 遍历所有端点
 * 2. 销毁每个端点的队列对
 * 3. 清空存储结构
 */
int FIFOEndpointStore::destroyQPs() {
    RWSpinlock::WriteGuard guard(endpoint_map_lock_);
    for (auto &entry : endpoint_map_) {
        if (entry.second->destroyQP()) {
            LOG(ERROR) << "Failed to destroy QPs in endpoint " << entry.first;
            return ERR_ENDPOINT;
        }
    }
    endpoint_map_.clear();
    fifo_map_.clear();
    fifo_list_.clear();
    waiting_list_.clear();
    return 0;
}

}  // namespace mooncake

