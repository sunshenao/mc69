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

#ifndef TOPOLOGY_H
#define TOPOLOGY_H

#include <glog/logging.h>
#include <jsoncpp/json/json.h>
#include <netdb.h>

#include <atomic>
#include <cstdint>
#include <etcd/SyncClient.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "common.h"

namespace mooncake {

/**
 * @struct TopologyEntry
 * @brief 拓扑条目，描述存储类型与网络适配器(HCA)的映射关系
 *
 * 每个拓扑条目包含：
 * 1. 存储类型名称
 * 2. 优选的网络适配器列表
 * 3. 可用的备选网络适配器列表
 */
struct TopologyEntry {
    std::string name;                      // 存储类型名称
    std::vector<std::string> preferred_hca; // 优选的网络适配器列表
    std::vector<std::string> avail_hca;     // 可用的备选网络适配器列表

    /**
     * @brief 将拓扑条目转换为JSON格式
     * @param value 用于存储转换后的JSON对象
     *
     * 将拓扑条目的所有字段序列化为JSON格式：
     * {
     *   "name": "存储类型名称",
     *   "preferred_hca": ["首选HCA1", "首选HCA2", ...],
     *   "avail_hca": ["备选HCA1", "备选HCA2", ...]
     * }
     */
    void toJson(Json::Value& value) const;

    /**
     * @brief 从JSON格式解析拓扑条目
     * @param value 包含拓扑信息的JSON对象
     * @return 解析是否成功
     */
    bool fromJson(const Json::Value& value);
};

/**
 * @class Topology
 * @brief 网络拓扑管理器
 *
 * 负责管理和维护整个系统的网络拓扑信息：
 * 1. 管理本地与远程节点的连接关系
 * 2. 监控网络设备状态
 * 3. 提供拓扑信息查询服务
 * 4. 自动发现和更新网络拓扑
 */
class Topology {
   public:
    /**
     * @brief 构造函数
     * @param metadata 元数据服务实例
     * @param local_server_name 本地服务器名称
     */
    Topology(std::shared_ptr<TransferMetadata> metadata,
            const std::string& local_server_name);

    /**
     * @brief 启动拓扑管理服务
     * @return 成功返回0，失败返回错误码
     *
     * 启动后会：
     * 1. 定期检查网络设备状态
     * 2. 自动发现新加入的节点
     * 3. 更新节点之间的连接状态
     * 4. 维护最优传输路径信息
     */
    int start();

    /**
     * @brief 停止拓扑管理服务
     */
    void stop();

    /**
     * @brief 获取指定存储类型的拓扑信息
     * @param storage_type 存储类型名称
     * @return 对应的拓扑条目，不存在则返回nullptr
     */
    std::shared_ptr<TopologyEntry> getTopology(const std::string& storage_type);

    /**
     * @brief 更新指定存储类型的拓扑信息
     * @param storage_type 存储类型名称
     * @param entry 新的拓扑条目
     * @return 成功返回true，失败返回false
     */
    bool updateTopology(const std::string& storage_type,
                       const TopologyEntry& entry);

    /**
     * @brief 检查拓扑是否为空
     * @return true表示拓扑为空，false表示非空
     */
    bool empty() const;

    /**
     * @brief 清空拓扑信息
     */
    void clear();

    /**
     * @brief 自动发现网络设备和拓扑关系
     * @return 成功返回0，失败返回错误码
     *
     * 该函数会：
     * 1. 扫描系统中的网络设备
     * 2. 建立设备间的拓扑关系
     * 3. 更新拓扑矩阵
     */
    int discover();

    /**
     * @brief 从JSON字符串解析拓扑信息
     * @param topology_json JSON格式的拓扑描述
     * @return 成功返回0，失败返回错误码
     */
    int parse(const std::string &topology_json);

    /**
     * @brief 禁用指定的网络设备
     * @param device_name 设备名称
     * @return 成功返回0，失败返回错误码
     */
    int disableDevice(const std::string &device_name);

    /**
     * @brief 将拓扑信息转换为字符串格式
     * @return 拓扑信息的字符串表示
     */
    std::string toString() const;

    /**
     * @brief 将拓扑信息转换为JSON格式
     * @return 拓扑信息的JSON表示
     */
    Json::Value toJson() const;

    /**
     * @brief 为指定存储类型选择合适的网络设备
     * @param storage_type 存储类型
     * @param retry_count 重试次数
     * @return 选中的设备索引，失败返回错误码
     *
     * 设备选择策略：
     * 1. 优先从优选列表中选择
     * 2. 如果优选列表无可用设备，从备选列表选择
     * 3. 支持失败重试
     */
    int selectDevice(const std::string storage_type, int retry_count = 0);

    /**
     * @brief 获取拓扑矩阵
     * @return 当前的拓扑矩阵
     */
    TopologyMatrix getMatrix() const { return matrix_; }

    /**
     * @brief 获取网络适配器列表
     * @return 当前系统中的网络适配器列表
     */
    const std::vector<std::string> &getHcaList() const { return hca_list_; }

   private:
    /**
     * @brief 解析并构建内部拓扑结构
     * @return 成功返回0，失败返回错误码
     */
    int resolve();

   private:
    TopologyMatrix matrix_;  // 拓扑矩阵
    std::vector<std::string> hca_list_;  // 网络适配器列表

    /**
     * @struct ResolvedTopologyEntry
     * @brief 已解析的拓扑条目，将设备名称映射为索引
     */
    struct ResolvedTopologyEntry {
        std::vector<int> preferred_hca;  // 优选设备索引列表
        std::vector<int> avail_hca;      // 备选设备索引列表
    };

    // 存储类型到已解析拓扑条目的映射
    std::unordered_map<std::string /* storage type */, ResolvedTopologyEntry>
        resolved_matrix_;
};

}  // namespace mooncake

#endif // TOPOLOGY_H