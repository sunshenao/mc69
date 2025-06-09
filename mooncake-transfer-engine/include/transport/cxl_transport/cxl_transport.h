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

#ifndef CXL_TRANSPORT_H_
#define CXL_TRANSPORT_H_

#include <infiniband/verbs.h>

#include <atomic>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "transfer_metadata.h"
#include "transport/transport.h"

namespace mooncake {

// 前向声明
class TransferMetadata;  // 元数据服务

/**
 * @class CxlTransport
 * @brief CXL (Compute Express Link) 传输协议实现类
 *
 * 该类实现了基于CXL的内存访问，支持：
 * 1. 直接远程内存访问（CXL.mem）
 * 2. 内存扩展和池化（CXL.mem）
 * 3. 缓存一致性（CXL.cache）
 * 4. 设备直通（CXL.io）
 */
class CxlTransport : public Transport {
   public:
    // 从元数据服务继承的类型定义
    using BufferDesc = TransferMetadata::BufferDesc;      // 缓冲区描述符
    using SegmentDesc = TransferMetadata::SegmentDesc;    // 段描述符
    using HandShakeDesc = TransferMetadata::HandShakeDesc;  // 握手描述符

   public:
    /**
     * @brief 构造函数
     *
     * 初始化CXL传输实例：
     * 1. 检测CXL设备
     * 2. 设置内存映射
     * 3. 初始化缓存管理
     */
    CxlTransport();

    /**
     * @brief 析构函数
     *
     * 清理CXL资源：
     * 1. 解除内存映射
     * 2. 释放设备句柄
     * 3. 停止设备访问
     */
    ~CxlTransport();

    /**
     * @brief 分配批次ID
     * @param batch_size 批次大小
     * @return 批次ID
     */
    BatchID allocateBatchID(size_t batch_size) override;

    /**
     * @brief 提交传输请求
     * @param batch_id 批次ID
     * @param entries 传输请求列表
     * @return 成功返回0，失败返回错误码
     *
     * 该函数处理CXL传输请求：
     * 1. 建立内存映射
     * 2. 设置缓存属性
     * 3. 执行内存访问
     */
    int submitTransfer(BatchID batch_id,
                       const std::vector<TransferRequest> &entries) override;

    /**
     * @brief 获取传输状态
     * @param batch_id 批次ID
     * @param task_id 任务ID
     * @param status 状态输出参数
     * @return 成功返回0，失败返回错误码
     */
    int getTransferStatus(BatchID batch_id, size_t task_id,
                          TransferStatus &status) override;

    /**
     * @brief 释放批次ID
     * @param batch_id 批次ID
     * @return 成功返回0，失败返回错误码
     */
    int freeBatchID(BatchID batch_id) override;

   private:
    /**
     * @brief 安装CXL传输协议
     * @param local_server_name 本地服务器名称
     * @param meta 元数据服务实例
     * @param topo 网络拓扑实例
     * @return 成功返回0，失败返回错误码
     *
     * 该函数完成CXL传输的初始化：
     * 1. 打开CXL设备
     * 2. 建立内存映射
     * 3. 配置缓存策略
     * 4. 注册到元数据服务
     */
    int install(std::string &local_server_name,
                std::shared_ptr<TransferMetadata> meta,
                std::shared_ptr<Topology> topo) override;

    /**
     * @brief 注册本地内存区域
     * @param addr 内存地址
     * @param length 内存长度
     * @param location 位置标识
     * @param remote_accessible 是否允许远程访问
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     *
     * 该函数将内存区域注册到CXL设备：
     * 1. 设置内存属性
     * 2. 配置缓存策略
     * 3. 更新元数据服务
     */
    int registerLocalMemory(void *addr, size_t length,
                            const std::string &location, bool remote_accessible,
                            bool update_metadata) override;

    /**
     * @brief 取消注册本地内存区域
     * @param addr 内存地址
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemory(void *addr,
                              bool update_metadata = false) override;

    /**
     * @brief 批量注册本地内存区域
     * @param buffer_list 缓冲区列表
     * @param location 位置标识
     * @return 成功返回0，失败返回错误码
     */
    int registerLocalMemoryBatch(
        const std::vector<Transport::BufferEntry> &buffer_list,
        const std::string &location) override {
        return 0;  // CXL不需要实现此功能
    }

    /**
     * @brief 批量取消注册本地内存区域
     * @param addr_list 地址列表
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override {
        return 0;  // CXL不需要实现此功能
    }

    /**
     * @brief 获取传输协议名称
     * @return 返回"cxl"
     */
    const char *getName() const override { return "cxl"; }

   private:
    std::unordered_map<void*, size_t> registered_memory_;  // 已注册的内存区域
    RWSpinlock memory_lock_;                              // 内存操作锁
    std::atomic<BatchID> next_batch_id_;                  // 下一个批次ID
    std::vector<std::thread> workers_;                    // 工作线程池
};

}  // namespace mooncake

#endif