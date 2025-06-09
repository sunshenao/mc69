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

#ifndef TRANSPORT_H_
#define TRANSPORT_H_

// 系统和标准库头文件
#include <bits/stdint-uintn.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

#include "transfer_metadata.h"

namespace mooncake {

class TransferMetadata;

/**
 * @class Transport
 * @brief 传输层基类，定义了所有传输协议必须实现的接口
 *
 * 该类作为所有具体传输协议（如TCP、RDMA）的抽象基类，
 * 提供了统一的接口定义。默认情况下，所有接口函数：
 * - 成功时返回0（或非空指针）
 * - 失败时返回-1（或空指针），并设置errno
 */
class Transport {
    friend class TransferEngine;
    friend class MultiTransport;

   public:
    /**
     * @brief 基础类型定义
     * 这些类型在整个传输系统中被广泛使用
     */
    using SegmentID = uint64_t;           // 内存段标识符，用于唯一标识一块已注册的内存
    using BatchID = uint64_t;             // 批次标识符，用于标识一组传输请求
    using SegmentHandle = void*;          // 内存段句柄，指向实际的内存地址

    /**
     * @brief 传输状态枚举
     * 用于表示传输请求的当前状态
     */
    enum class TransferStatusEnum {
        IDLE = 0,       // 空闲状态，未开始传输
        PENDING,        // 待处理状态，已提交但未开始
        TRANSFERRING,   // 传输中状态
        COMPLETED,      // 传输完成状态
        ERROR          // 错误状态，传输失败
    };

    /**
     * @brief 传输状态结构体
     * 包含传输请求的详细状态信息
     */
    struct TransferStatus {
        TransferStatusEnum status;   // 当前状态
        int error_code;             // 错误码，0表示成功
        uint64_t transferred_bytes;  // 已传输的字节数
    };

    /**
     * @brief 缓冲区条目结构体
     * 描述一块连续的内存区域
     */
    struct BufferEntry {
        SegmentHandle segment;     // 内存段句柄
        size_t offset;            // 在内存段中的偏移量（字节）
        size_t length;           // 内存区域长度（字节）
    };

    /**
     * @brief 传输请求结构体
     * 描述一个完整的传输任务
     */
    struct TransferRequest {
        std::vector<BufferEntry> local_entries;   // 本地缓冲区列表
        std::vector<BufferEntry> remote_entries;  // 远程缓冲区列表
        bool is_write;                           // 是否为写操作（true=写，false=读）
    };

    /**
     * @brief 批次描述符结构体
     * 包含一组传输请求的描述信息
     */
    struct BatchDesc {
        BatchID batch_id;                         // 批次ID
        std::vector<TransferRequest> requests;    // 传输请求列表
        size_t total_bytes;                      // 总传输字节数
    };

    // 无效批次ID常量
    static const BatchID INVALID_BATCH_ID = UINT64_MAX;

    /**
     * @brief 获取传输层的名称
     * @return 传输层名称（如 "RDMA", "TCP" 等）
     */
    virtual const std::string& name() const = 0;

   public:
    virtual ~Transport() {}

    /// @brief Create a batch with specified maximum outstanding transfers.
    virtual BatchID allocateBatchID(size_t batch_size);

    /// @brief Free an allocated batch.
    virtual int freeBatchID(BatchID batch_id);

    /// @brief Submit a batch of transfer requests to the batch.
    /// @return The number of successfully submitted transfers on success. If
    /// that number is less than nr, errno is set.
    virtual int submitTransfer(BatchID batch_id,
                               const std::vector<TransferRequest> &entries) = 0;

    virtual int submitTransferTask(
        const std::vector<TransferRequest *> &request_list,
        const std::vector<TransferTask *> &task_list) {
        return ERR_NOT_IMPLEMENTED;
    }

    /// @brief Get the status of a submitted transfer. This function shall not
    /// be called again after completion.
    /// @return Return 1 on completed (either success or failure); 0 if still in
    /// progress.
    virtual int getTransferStatus(BatchID batch_id, size_t task_id,
                                  TransferStatus &status) = 0;

    std::shared_ptr<TransferMetadata> &meta() { return metadata_; }

   protected:
    virtual int install(std::string &local_server_name,
                        std::shared_ptr<TransferMetadata> meta,
                        std::shared_ptr<Topology> topo);

    std::string local_server_name_;
    std::shared_ptr<TransferMetadata> metadata_;

    RWSpinlock batch_desc_lock_;
    std::unordered_map<BatchID, std::shared_ptr<BatchDesc>> batch_desc_set_;

   private:
    virtual int registerLocalMemory(void *addr, size_t length,
                                    const std::string &location,
                                    bool remote_accessible,
                                    bool update_metadata = true) = 0;

    virtual int unregisterLocalMemory(void *addr,
                                      bool update_metadata = true) = 0;

    virtual int registerLocalMemoryBatch(
        const std::vector<BufferEntry> &buffer_list,
        const std::string &location) = 0;

    virtual int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) = 0;

    virtual const char *getName() const = 0;
};
}  // namespace mooncake

#endif  // TRANSPORT_H_