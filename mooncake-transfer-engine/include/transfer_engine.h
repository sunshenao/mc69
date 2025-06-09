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

#ifndef MULTI_TRANSFER_ENGINE_H_
#define MULTI_TRANSFER_ENGINE_H_

// 系统头文件和标准库
#include <asm-generic/errno-base.h>
#include <bits/stdint-uintn.h>
#include <limits.h>
#include <string.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 项目相关头文件
#include "multi_transport.h"         // 多传输协议支持
#include "transfer_metadata.h"       // 传输元数据管理
#include "transport/transport.h"     // 基础传输层

namespace mooncake {

// 类型别名定义，简化代码可读性
using TransferRequest = Transport::TransferRequest;     // 传输请求
using TransferStatus = Transport::TransferStatus;       // 传输状态
using TransferStatusEnum = Transport::TransferStatusEnum; // 传输状态枚举
using SegmentHandle = Transport::SegmentHandle;         // 段句柄
using SegmentID = Transport::SegmentID;                 // 段ID
using BatchID = Transport::BatchID;                     // 批次ID
using BufferEntry = Transport::BufferEntry;             // 缓冲区条目

/**
 * @class TransferEngine
 * @brief 传输引擎核心类，负责管理和协调数据传输
 *
 * TransferEngine是整个传输系统的核心组件，它提供了：
 * 1. 内存管理：注册/注销本地内存
 * 2. 传输控制：提交传输请求，监控传输状态
 * 3. 元数据管理：维护分布式系统中的内存段信息
 * 4. 多协议支持：支持TCP、RDMA等多种传输协议
 */
class TransferEngine {
   public:
    /**
     * @brief 构造函数
     * @param auto_discover 是否自动发现网络中的其他节点
     */
    TransferEngine(bool auto_discover = true)
        : metadata_(nullptr),
          local_topology_(std::make_shared<Topology>()),
          auto_discover_(auto_discover) {}

    ~TransferEngine() { freeEngine(); }

    /**
     * @brief 初始化传输引擎
     * @param metadata_conn_string 元数据服务连接字符串
     * @param local_server_name 本地服务器名称
     * @param ip_or_host_name IP地址或主机名
     * @param rpc_port RPC服务端口号
     * @return 成功返回0，失败返回错误码
     *
     * 该函数完成传输引擎的初始化工作，包括：
     * 1. 连接元数据服务
     * 2. 注册本地节点信息
     * 3. 启动RPC服务
     * 4. 初始化传输层
     */
    int init(const std::string &metadata_conn_string,
             const std::string &local_server_name,
             const std::string &ip_or_host_name, uint64_t rpc_port = 12345);

    /**
     * @brief 释放传输引擎资源
     * @return 成功返回0，失败返回错误码
     */
    int freeEngine();

    /**
     * @brief 安装传输协议
     * @param proto 协议名称
     * @param args 协议参数
     * @return 传输协议实例指针
     *
     * @note 仅用于测试 - 该函数只在测试环境中使用，用于手动安装和配置传输协议
     * 在正常运行环境中，传输协议的安装和配置由系统自动完成
     */
    Transport *installTransport(const std::string &proto, void **args);

    /**
     * @brief 卸载传输协议
     * @param proto 协议名称
     * @return 成功返回0，失败返回错误码
     *
     * @note 仅用于测试 - 该函数只在测试环境中使用，用于手动卸载传输协议
     * 在正常运行环境中，传输协议的生命周期由系统自动管理
     */
    int uninstallTransport(const std::string &proto);

    /**
     * @brief 打开一个内存段
     * @param segment_name 段名称
     * @return 段句柄
     *
     * 该函数用于打开一个命名的内存段，如果不存在则创建
     */
    SegmentHandle openSegment(const std::string &segment_name);

    /**
     * @brief 关闭内存段
     * @param handle 段句柄
     * @return 成功返回0，失败返回错误码
     */
    int closeSegment(SegmentHandle handle);

    /**
     * @brief 注册本地内存区域
     * @param addr 内存地址
     * @param length 内存长度
     * @param location 位置标识
     * @param remote_accessible 是否允许远程访问
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     *
     * 该函数将本地内存注册到传输引擎中，使其可以：
     * 1. 被远程节点访问（如果remote_accessible为true）
     * 2. 参与高效的零拷贝传输
     * 3. 被元数据服务发现（如果update_metadata为true）
     */
    int registerLocalMemory(void *addr, size_t length,
                            const std::string &location,
                            bool remote_accessible = true,
                            bool update_metadata = true);

    /**
     * @brief 取消注册本地内存
     * @param addr 内存地址
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemory(void *addr, bool update_metadata = true);

    /**
     * @brief 批量注册本地内存
     * @param buffer_list 缓冲区列表
     * @param location 位置标识
     * @return 成功返回0，失败返回错误码
     */
    int registerLocalMemoryBatch(const std::vector<BufferEntry> &buffer_list,
                                 const std::string &location);

    /**
     * @brief 批量取消注册本地内存
     * @param addr_list 地址列表
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemoryBatch(const std::vector<void *> &addr_list);

    /**
     * @brief 分配批次ID
     * @param batch_size 批次大小
     * @return 批次ID
     *
     * 批次ID用于对多个传输请求进行分组，
     * 便于后续的状态查询和同步操作
     */
    BatchID allocateBatchID(size_t batch_size) {
        return multi_transports_->allocateBatchID(batch_size);
    }

    /**
     * @brief 释放批次ID
     * @param batch_id 批次ID
     * @return 成功返回0，失败返回错误码
     */
    int freeBatchID(BatchID batch_id) {
        return multi_transports_->freeBatchID(batch_id);
    }

    /**
     * @brief 提交传输请求
     * @param batch_id 批次ID
     * @param entries 传输请求列表
     * @return 成功返回0，失败返回错误码
     *
     * 该函数用于提交一组传输请求，这些请求将：
     * 1. 被分配到合适的传输协议处理
     * 2. 异步执行数据传输
     * 3. 可以通过batch_id查询状态
     */
    int submitTransfer(BatchID batch_id,
                       const std::vector<TransferRequest> &entries) {
        return multi_transports_->submitTransfer(batch_id, entries);
    }

    /**
     * @brief 获取传输状态
     * @param batch_id 批次ID
     * @param task_id 任务ID
     * @param status 状态输出参数
     * @return 成功返回0，失败返回错误码
     */
    int getTransferStatus(BatchID batch_id, size_t task_id,
                          TransferStatus &status) {
        return multi_transports_->getTransferStatus(batch_id, task_id, status);
    }

    /**
     * @brief 同步段缓存
     * @return 成功返回0，失败返回错误码
     *
     * 该函数强制同步本地的段缓存与元数据服务，
     * 确保本地看到的内存段信息是最新的
     */
    int syncSegmentCache() { return metadata_->syncSegmentCache(); }

    /**
     * @brief 获取元数据服务实例
     * @return 元数据服务的共享指针
     */
    std::shared_ptr<TransferMetadata> getMetadata() { return metadata_; }

    // auto_discover_ 的注释也应该更新
    // Discover topology and install transports automatically when it's true.
    // @note 仅用于测试 - 设置为 false 仅用于测试场景，正常运行时应始终为 true
    bool auto_discover_;

    /**
     * @brief 检查内存区域是否重叠
     * @param addr 内存地址
     * @param length 内存长度
     * @return 如果有重叠返回true，否则返回false
     *
     * @note 仅用于测试 - 该函数用于测试内存注册的边界情况
     */
    bool checkOverlap(void *addr, uint64_t length);

   private:
    struct MemoryRegion {
        void *addr;
        uint64_t length;
        std::string location;
        bool remote_accessible;
    };

    std::shared_ptr<TransferMetadata> metadata_;
    std::string local_server_name_;
    std::shared_ptr<MultiTransport> multi_transports_;
    std::vector<MemoryRegion> local_memory_regions_;
    std::shared_ptr<Topology> local_topology_;
};
}  // namespace mooncake

#endif
