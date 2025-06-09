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

#ifndef NVMEOF_TRANSPORT_H_
#define NVMEOF_TRANSPORT_H_

#include <bits/stdint-uintn.h>

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include "cufile_context.h"     // NVIDIA GPUDirect Storage支持
#include "cufile_desc_pool.h"   // 文件描述符池
#include "transfer_metadata.h"   // 元数据服务
#include "transport/transport.h" // 基础传输协议

namespace mooncake {

/**
 * @class NVMeoFTransport
 * @brief NVMe over Fabrics传输协议实现类
 *
 * 该类实现了基于NVMe-oF的高性能存储访问，支持：
 * 1. 直接远程NVMe设备访问
 * 2. GPUDirect Storage加速
 * 3. 零拷贝数据传输
 * 4. 批量IO操作
 */
class NVMeoFTransport : public Transport {
   public:
    /**
     * @brief 构造函数
     *
     * 初始化NVMe-oF传输实例：
     * 1. 初始化CUDA文件系统
     * 2. 创建描述符池
     * 3. 设置IO上下文
     */
    NVMeoFTransport();

    /**
     * @brief 析构函数
     *
     * 清理资源：
     * 1. 关闭所有文件句柄
     * 2. 释放描述符池
     * 3. 停止工作线程
     */
    ~NVMeoFTransport();

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
     * 该函数处理NVMe-oF传输请求：
     * 1. 准备CUFile操作
     * 2. 切分大文件操作
     * 3. 提交异步IO
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
     * @struct NVMeoFBatchDesc
     * @brief NVMe-oF批次描述符
     *
     * 用于跟踪批量IO操作的状态和进度
     */
    struct NVMeoFBatchDesc {
        size_t desc_idx_;                  // 描述符索引
        std::vector<TransferStatus> transfer_status;  // 传输状态列表
        // 任务ID到切片范围的映射
        std::vector<std::pair<uint64_t, uint64_t>> task_to_slices;
    };

    /**
     * @struct pair_hash
     * @brief pair类型的哈希函数对象
     */
    struct pair_hash {
        template <class T1, class T2>
        std::size_t operator()(const std::pair<T1, T2> &pair) const {
            auto hash1 = std::hash<T1>{}(pair.first);
            auto hash2 = std::hash<T2>{}(pair.second);
            return hash1 ^ hash2;
        }
    };

    /**
     * @brief 安装NVMe-oF传输协议
     * @param local_server_name 本地服务器名称
     * @param meta 元数据服务实例
     * @param topo 网络拓扑实例
     * @return 成功返回0，失败返回错误码
     *
     * 该函数完成NVMe-oF传输的初始化：
     * 1. 初始化CUFile系统
     * 2. 创建工作线程池
     * 3. 注册到元数据服务
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
        return 0;  // NVMe-oF不需要实现此功能
    }

    /**
     * @brief 批量取消注册本地内存区域
     * @param addr_list 地址列表
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override {
        return 0;  // NVMe-oF不需要实现此功能
    }

    /**
     * @brief 添加切片到任务
     * @param source_addr 源地址
     * @param slice_len 切片长度
     * @param target_start 目标起始位置
     * @param op 操作类型
     * @param task 任务实例
     * @param file_path 文件路径
     *
     * 该函数将大文件操作切分成小块：
     * 1. 创建切片描述符
     * 2. 设置IO参数
     * 3. 添加到任务列表
     */
    void addSliceToTask(void *source_addr, uint64_t slice_len,
                        uint64_t target_start, TransferRequest::OpCode op,
                        TransferTask &task, const char *file_path);

    /**
     * @brief 添加切片到CUFile批次
     * @param source_addr 源地址
     * @param file_offset 文件偏移
     * @param slice_len 切片长度
     * @param desc_id 描述符ID
     * @param op 操作类型
     * @param fh 文件句柄
     *
     * 该函数准备CUFile操作：
     * 1. 设置DMA映射
     * 2. 准备IO向量
     * 3. 添加到批次队列
     */
    void addSliceToCUFileBatch(void *source_addr, uint64_t file_offset,
                               uint64_t slice_len, uint64_t desc_id,
                               TransferRequest::OpCode op, CUfileHandle_t fh);

    /**
     * @brief 获取传输协议名称
     * @return 返回"nvmeof"
     */
    const char *getName() const override { return "nvmeof"; }

   private:
    // 段句柄和偏移量到CUFile上下文的映射
    std::unordered_map<std::pair<SegmentHandle, uint64_t>,
                       std::shared_ptr<CuFileContext>, pair_hash>
        segment_to_context_;

    std::vector<std::thread> workers_;  // 工作线程池

    std::shared_ptr<CUFileDescPool> desc_pool_;
    RWSpinlock context_lock_;
};
}  // namespace mooncake

#endif