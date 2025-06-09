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

#ifndef TCP_TRANSPORT_H_
#define TCP_TRANSPORT_H_

#include <infiniband/verbs.h>

#include <atomic>
#include <boost/asio.hpp>  // 使用boost::asio进行异步IO
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
class TcpContext;       // TCP上下文

/**
 * @class TcpTransport
 * @brief TCP传输协议实现类
 *
 * 该类实现了基于TCP的数据传输，提供：
 * 1. 可靠的数据传输
 * 2. 流量控制
 * 3. 拥塞控制
 * 4. 异步IO操作
 */
class TcpTransport : public Transport {
   public:
    // 从元数据服务继承的类型定义
    using BufferDesc = TransferMetadata::BufferDesc;      // 缓冲区描述符
    using SegmentDesc = TransferMetadata::SegmentDesc;    // 段描述符
    using HandShakeDesc = TransferMetadata::HandShakeDesc;  // 握手描述符

   public:
    /**
     * @brief 构造函数
     *
     * 初始化TCP传输协议实例，
     * 设置异步IO上下文
     */
    TcpTransport();

    /**
     * @brief 析构函数
     *
     * 清理TCP资源：
     * 1. 关闭所有连接
     * 2. 停止工作线程
     * 3. 释放上下文资源
     */
    ~TcpTransport();

    /**
     * @brief 提交传输请求
     * @param batch_id 批次ID
     * @param entries 传输请求列表
     * @return 成功返回0，失败返回错误码
     *
     * 该函数异步处理传输请求：
     * 1. 建立TCP连接（如果需要）
     * 2. 切分大数据传输
     * 3. 提交异步写操作
     */
    int submitTransfer(BatchID batch_id,
                       const std::vector<TransferRequest> &entries) override;

    /**
     * @brief 提交传输任务
     * @param request_list 请求列表
     * @param task_list 任务列表
     * @return 成功返回0，失败返回错误码
     */
    int submitTransferTask(
        const std::vector<TransferRequest *> &request_list,
        const std::vector<TransferTask *> &task_list) override;

    /**
     * @brief 获取传输状态
     * @param batch_id 批次ID
     * @param task_id 任务ID
     * @param status 状态输出参数
     * @return 成功返回0，失败返回错误码
     */
    int getTransferStatus(BatchID batch_id, size_t task_id,
                          TransferStatus &status) override;

   private:
    /**
     * @brief 安装TCP传输协议
     * @param local_server_name 本地服务器名称
     * @param meta 元数据服务实例
     * @param topo 网络拓扑实例
     * @return 成功返回0，失败返回错误码
     *
     * 该函数完成TCP传输的初始化：
     * 1. 创建异步IO上下文
     * 2. 启动工作线程
     * 3. 注册到元数据服务
     */
    int install(std::string &local_server_name,
                std::shared_ptr<TransferMetadata> meta,
                std::shared_ptr<Topology> topo);

    /**
     * @brief 分配本地段ID
     * @return 新分配的段ID
     */
    int allocateLocalSegmentID();

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
                            bool update_metadata);

    /**
     * @brief 取消注册本地内存区域
     * @param addr 内存地址
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemory(void *addr, bool update_metadata = false);

    /**
     * @brief 批量注册本地内存区域
     * @param buffer_list 缓冲区列表
     * @param location 位置标识
     * @return 成功返回0，失败返回错误码
     */
    int registerLocalMemoryBatch(
        const std::vector<Transport::BufferEntry> &buffer_list,
        const std::string &location);

    /**
     * @brief 批量取消注册本地内存区域
     * @param addr_list 地址列表
     * @return 成功返回0，失败返回错误码
     */
    int unregisterLocalMemoryBatch(
        const std::vector<void *> &addr_list) override;

    /**
     * @brief 工作线程函数
     *
     * 该函数在独立线程中运行：
     * 1. 处理异步IO事件
     * 2. 管理TCP连接
     * 3. 更新传输状态
     */
    void worker();

    /**
     * @brief 开始传输操作
     * @param slice 传输切片
     *
     * 该函数执行实际的数据传输：
     * 1. 准备发送/接收缓冲区
     * 2. 提交异步操作
     * 3. 设置完成回调
     */
    void startTransfer(Slice *slice);

    /**
     * @brief 获取传输协议名称
     * @return 返回"tcp"
     */
    const char *getName() const override { return "tcp"; }

   private:
    TcpContext *context_;          // TCP上下文
    std::atomic_bool running_;     // 运行标志
    std::thread thread_;           // 工作线程
};

}  // namespace mooncake

#endif  // TCP_TRANSPORT_H_
