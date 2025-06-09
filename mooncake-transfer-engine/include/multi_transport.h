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

#ifndef MULTI_TRANSPORT_H_
#define MULTI_TRANSPORT_H_

#include <unordered_map>
#include "transport/transport.h"

namespace mooncake {

/**
 * @class MultiTransport
 * @brief 多传输协议管理器，支持多种传输协议的统一管理和调度
 *
 * 该类的主要功能：
 * 1. 管理多个传输协议实例（如TCP、RDMA、NVMeOF等）
 * 2. 根据传输请求特征选择最优的传输协议
 * 3. 提供统一的传输接口，屏蔽底层协议差异
 * 4. 管理批次和任务的生命周期
 */
class MultiTransport {
   public:
    // 从Transport类继承的类型定义
    using BatchID = Transport::BatchID;                 // 批次ID类型
    using TransferRequest = Transport::TransferRequest; // 传输请求类型
    using TransferStatus = Transport::TransferStatus;   // 传输状态类型
    using BatchDesc = Transport::BatchDesc;             // 批次描述符类型

    // 无效批次ID常量
    const static BatchID INVALID_BATCH_ID = Transport::INVALID_BATCH_ID;

    /**
     * @brief 构造函数
     * @param metadata 元数据服务实例
     * @param local_server_name 本地服务器名称
     *
     * 初始化多传输协议管理器，建立与元数据服务的连接
     */
    MultiTransport(std::shared_ptr<TransferMetadata> metadata,
                   std::string &local_server_name);

    ~MultiTransport();

    /**
     * @brief 分配新的批次ID
     * @param batch_size 批次大小（最大并发传输数）
     * @return 新分配的批次ID
     *
     * 用于创建一个新的传输批次，可以包含多个传输请求
     */
    BatchID allocateBatchID(size_t batch_size);

    /**
     * @brief 释放批次ID及其相关资源
     * @param batch_id 要释放的批次ID
     * @return 成功返回0，失败返回错误码
     *
     * 当批次中的所有传输任务完成后，应调用此函数释放资源
     */
    int freeBatchID(BatchID batch_id);

    /**
     * @brief 提交一组传输请求
     * @param batch_id 批次ID
     * @param entries 传输请求列表
     * @return 成功提交的传输请求数量
     *
     * 该函数会：
     * 1. 为每个请求选择最优的传输协议
     * 2. 将请求分发给相应的传输协议处理
     * 3. 跟踪所有请求的执行状态
     */
    int submitTransfer(BatchID batch_id,
                       const std::vector<TransferRequest> &entries);

    /**
     * @brief 查询传输状态
     * @param batch_id 批次ID
     * @param task_id 任务ID
     * @param status 状态输出参数
     * @return 完成返回1，进行中返回0，错误返回负值
     */
    int getTransferStatus(BatchID batch_id, size_t task_id,
                          TransferStatus &status);

    /**
     * @brief 安装新的传输协议
     * @param proto 协议名称
     * @param topo 网络拓扑信息
     * @return 传输协议实例指针
     *
     * 向管理器注册新的传输协议，使其可用于数据传输
     */
    Transport *installTransport(const std::string &proto,
                                std::shared_ptr<Topology> topo);

    /**
     * @brief 获取指定协议的传输实例
     * @param proto 协议名称
     * @return 传输协议实例指针，不存在则返回nullptr
     */
    Transport *getTransport(const std::string &proto);

    /**
     * @brief 列出所有已安装的传输协议
     * @return 传输协议实例指针列表
     */
    std::vector<Transport *> listTransports();

   private:
    /**
     * @brief 为传输请求选择最优的传输协议
     * @param entry 传输请求
     * @return 选中的传输协议实例指针
     *
     * 根据以下因素选择传输协议：
     * 1. 传输数据的大小
     * 2. 源端和目标端的硬件能力
     * 3. 网络条件和带宽需求
     * 4. 协议的特性和开销
     */
    Transport *selectTransport(const TransferRequest &entry);

   private:
    std::shared_ptr<TransferMetadata> metadata_;        // 元数据服务实例
    std::string local_server_name_;                     // 本地服务器名称
    std::map<std::string, std::shared_ptr<Transport>> transport_map_;  // 协议映射表
    RWSpinlock batch_desc_lock_;                       // 批次描述符锁
    std::unordered_map<BatchID, std::shared_ptr<BatchDesc>> batch_desc_set_;  // 批次集合
};

}  // namespace mooncake

#endif  // MULTI_TRANSPORT_H_