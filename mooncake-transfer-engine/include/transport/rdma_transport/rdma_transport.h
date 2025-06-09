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

#ifndef TRANSFER_ENGINE
#define TRANSFER_ENGINE

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

#include "topology.h"
#include "transfer_metadata.h"
#include "transport/transport.h"

namespace mooncake {

class RdmaContext;    // RDMA上下文前向声明
class RdmaEndPoint;   // RDMA端点前向声明
class TransferMetadata;  // 元数据服务前向声明
class WorkerPool;     // 工作线程池前向声明

/**
 * @class RdmaTransport
 * @brief RDMA传输协议实现类
 *
 * 该类实现了基于RDMA（远程直接内存访问）的高性能数据传输，
 * 支持以下特性：
 * 1. 零拷贝传输
 * 2. 内核旁路
 * 3. 硬件卸载
 * 4. 异步操作
 */
class RdmaTransport : public Transport {
   public:
    /**
     * @brief 构造函数
     * @param storage_type 存储类型名称
     *
     * 初始化RDMA传输层:
     * 1. 设置存储类型
     * 2. 初始化设备列表
     * 3. 准备工作线程池
     */
    RdmaTransport(const std::string &storage_type);

    /**
     * @brief 析构函数
     * 清理所有RDMA资源：
     * 1. 销毁所有端点
     * 2. 释放工作线程池
     * 3. 清理设备上下文
     */
    virtual ~RdmaTransport();

    /**
     * @brief 安装RDMA传输组件
     * @param local_server_name 本地服务器名称
     * @param meta 元数据服务实例
     * @param topo 网络拓扑实例
     * @return 成功返回0，失败返回错误码
     *
     * 该方法会：
     * 1. 初始化本地RDMA设备
     * 2. 创建设备上下文
     * 3. 启动工作线程池
     * 4. 注册到元数据服务
     */
    virtual int install(std::string &local_server_name,
                       std::shared_ptr<TransferMetadata> meta,
                       std::shared_ptr<Topology> topo) override;

    /**
     * @brief 提交传输批次
     * @param batch_id 批次ID
     * @param entries 传输请求列表
     * @return 成功提交的请求数量，失败返回错误码
     *
     * 处理流程：
     * 1. 验证批次和请求的有效性
     * 2. 选择合适的传输路径
     * 3. 构建RDMA工作请求
     * 4. 提交到工作线程池
     */
    virtual int submitTransfer(BatchID batch_id,
                             const std::vector<TransferRequest> &entries) override;

    /**
     * @brief 获取传输状态
     * @param batch_id 批次ID
     * @param task_id 任务ID
     * @param status 状态输出参数
     * @return 1表示完成，0表示进行中，负数表示错误
     */
    virtual int getTransferStatus(BatchID batch_id, size_t task_id,
                                TransferStatus &status) override;

    /**
     * @brief 获取传输层名称
     * @return "RDMA"
     */
    virtual const std::string& name() const override {
        static const std::string name = "RDMA";
        return name;
    }

   protected:
    /**
     * @brief 注册本地内存
     * @param addr 内存地址
     * @param length 内存长度
     * @param location 存储位置
     * @param remote_accessible 是否允许远程访问
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    virtual int registerLocalMemory(void *addr, size_t length,
                                  const std::string &location,
                                  bool remote_accessible,
                                  bool update_metadata = true) override;

    /**
     * @brief 注销本地内存
     * @param addr 内存地址
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    virtual int unregisterLocalMemory(void *addr,
                                    bool update_metadata = true) override;

   private:
    /**
     * @brief 批量注册本地内存
     * @param buffer_list 缓冲区列表
     * @param location 存储位置
     * @return 成功返回0，失败返回错误码
     */
    virtual int registerLocalMemoryBatch(
        const std::vector<BufferEntry> &buffer_list,
        const std::string &location) override;

    /**
     * @brief 批量注销本地内存
     * @param addr_list 地址列表
     * @return 成功返回0，失败返回错误码
     */
    virtual int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override;

   private:
    std::string storage_type_;                // 存储类型名称

    // RDMA设备和上下文管理
    std::vector<std::shared_ptr<RdmaContext>> ctx_list_;  // 上下文列表
    RWSpinlock ctx_list_lock_;                           // 上下文列表读写锁

    // 端点管理
    std::shared_ptr<EndpointStore> endpoint_store_;      // 端点存储管理器

    // 工作线程池管理
    std::vector<std::shared_ptr<WorkerPool>> worker_pool_list_;  // 工作线程池列表
    std::atomic<size_t> next_worker_pool_;                      // 下一个要使用的线程池

    // 传输任务管理
    RWSpinlock transfer_lock_;                          // 传输操作读写锁
    std::atomic<uint64_t> next_transfer_id_;            // 下一个传输任务ID
};

