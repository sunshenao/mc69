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

#include "transfer_engine_c.h"
#include <cstdint>
#include <memory>
#include "transfer_engine.h"
#include "transport/transport.h"

using namespace mooncake;

/**
 * @brief 创建传输引擎实例（C接口）
 *
 * @param metadata_conn_string 元数据服务连接字符串
 * @param local_server_name 本地服务器名称
 * @param ip_or_host_name IP或主机名
 * @param rpc_port RPC端口号
 * @return 传输引擎句柄，失败返回NULL
 *
 * 该函数为C程序提供创建传输引擎的接口：
 * 1. 创建C++的传输引擎对象
 * 2. 初始化传输引擎
 * 3. 将C++对象转换为不透明指针返回
 */
transfer_engine_t createTransferEngine(const char *metadata_conn_string,
                                       const char *local_server_name,
                                       const char *ip_or_host_name,
                                       uint64_t rpc_port) {
    // 创建C++传输引擎对象
    TransferEngine *native = new TransferEngine();
    // 初始化引擎
    int ret = native->init(metadata_conn_string, local_server_name,
                           ip_or_host_name, rpc_port);
    if (ret) {
        delete native;
        return nullptr;
    }
    // 转换为C接口句柄
    return (transfer_engine_t)native;
}

/**
 * @brief 安装传输协议（C接口）
 *
 * @param engine 传输引擎句柄
 * @param proto 协议名称（"rdma"、"tcp"等）
 * @param args 协议参数
 * @return 传输协议句柄
 *
 * 该函数为传输引擎安装新的传输协议：
 * 1. 将句柄转换为C++对象
 * 2. 调用原生安装函数
 * 3. 将结果转换为C接口句柄
 */
transport_t installTransport(transfer_engine_t engine, const char *proto,
                             void **args) {
    TransferEngine *native = (TransferEngine *)engine;
    return (transport_t)native->installTransport(proto, args);
}

/**
 * @brief 卸载传输协议（C接口）
 *
 * @param engine 传输引擎句柄
 * @param proto 协议名称
 * @return 成功返回0，失败返回错误码
 */
int uninstallTransport(transfer_engine_t engine, const char *proto) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->uninstallTransport(proto);
}

/**
 * @brief 销毁传输引擎（C接口）
 *
 * @param engine 传输引擎句柄
 *
 * 该函数释放传输引擎的所有资源：
 * 1. 将句柄转换为C++对象
 * 2. 删除对象，触发析构函数
 */
void destroyTransferEngine(transfer_engine_t engine) {
    TransferEngine *native = (TransferEngine *)engine;
    delete native;
}

/**
 * @brief 打开内存段（C接口）
 *
 * @param engine 传输引擎句柄
 * @param segment_name 段名称
 * @return 段句柄
 */
segment_id_t openSegment(transfer_engine_t engine, const char *segment_name) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->openSegment(segment_name);
}

/**
 * @brief 关闭内存段（C接口）
 *
 * @param engine 传输引擎句柄
 * @param segment_id 段句柄
 * @return 成功返回0，失败返回错误码
 */
int closeSegment(transfer_engine_t engine, segment_id_t segment_id) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->closeSegment(segment_id);
}

/**
 * @brief 注册本地内存（C接口）
 *
 * @param engine 传输引擎句柄
 * @param addr 内存地址
 * @param length 内存长度
 * @param location 位置标识
 * @param remote_accessible 是否允许远程访问
 * @return 成功返回0，失败返回错误码
 *
 * 该函数为C程序提供内存注册接口：
 * 1. 将内存注册到传输引擎
 * 2. 同时更新元数据服务
 */
int registerLocalMemory(transfer_engine_t engine, void *addr, size_t length,
                        const char *location, int remote_accessible) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->registerLocalMemory(addr, length, location,
                                       remote_accessible, true);
}

/**
 * @brief 取消注册本地内存（C接口）
 *
 * @param engine 传输引擎句柄
 * @param addr 内存地址
 * @return 成功返回0，失败返回错误码
 */
int unregisterLocalMemory(transfer_engine_t engine, void *addr) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->unregisterLocalMemory(addr);
}

/**
 * @brief 批量注册本地内存（C接口）
 *
 * @param engine 传输引擎句柄
 * @param buffer_list 缓冲区数组
 * @param buffer_len 数组长度
 * @param location 位置标识
 * @return 成功返回0，失败返回错误码
 *
 * 该函数将C语言的缓冲区数组转换为C++格式：
 * 1. 创建C++的缓冲区列表
 * 2. 复制每个缓冲区的信息
 * 3. 调用批量注册函数
 */
int registerLocalMemoryBatch(transfer_engine_t engine,
                             buffer_entry_t *buffer_list, size_t buffer_len,
                             const char *location) {
    TransferEngine *native = (TransferEngine *)engine;
    // 转换缓冲区格式
    std::vector<BufferEntry> native_buffer_list;
    for (size_t i = 0; i < buffer_len; ++i) {
        BufferEntry entry;
        entry.addr = buffer_list[i].addr;
        entry.length = buffer_list[i].length;
        native_buffer_list.push_back(entry);
    }
    return native->registerLocalMemoryBatch(native_buffer_list, location);
}

/**
 * @brief 批量取消注册本地内存（C接口）
 *
 * @param engine 传输引擎句柄
 * @param addr_list 地址数组
 * @param addr_len 数组长度
 * @return 成功返回0，失败返回错误码
 *
 * 该函数将C语言的地址数组转换为C++格式：
 * 1. 创建C++的地址列表
 * 2. 复制每个地址
 * 3. 调用批量注销函数
 */
int unregisterLocalMemoryBatch(transfer_engine_t engine, void **addr_list,
                               size_t addr_len) {
    TransferEngine *native = (TransferEngine *)engine;
    // 转换地址格式
    std::vector<void *> native_addr_list;
    for (size_t i = 0; i < addr_len; ++i)
        native_addr_list.push_back(addr_list[i]);
    return native->unregisterLocalMemoryBatch(native_addr_list);
}

/**
 * @brief 分配批次ID（C接口）
 *
 * @param engine 传输引擎句柄
 * @param batch_size 批次大小
 * @return 批次ID
 */
batch_id_t allocateBatchID(transfer_engine_t engine, size_t batch_size) {
    TransferEngine *native = (TransferEngine *)engine;
    return (batch_id_t)native->allocateBatchID(batch_size);
}

/**
 * @brief 提交传输请求（C接口）
 *
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @param entries 传输请求数组
 * @param count 请求数量
 * @return 成功返回0，失败返回错误码
 *
 * 该函数将C语言的传输请求数组转换为C++格式：
 * 1. 创建C++的请求列表
 * 2. 复制每个请求的信息
 * 3. 调用原生提交函数
 */
int submitTransfer(transfer_engine_t engine, batch_id_t batch_id,
                   struct transfer_request *entries, size_t count) {
    TransferEngine *native = (TransferEngine *)engine;
    // 转换传输请求格式
    std::vector<Transport::TransferRequest> native_entries;
    native_entries.resize(count);
    for (size_t index = 0; index < count; index++) {
        native_entries[index].opcode =
            (Transport::TransferRequest::OpCode)entries[index].opcode;
        native_entries[index].source = entries[index].source;
        native_entries[index].target_id = entries[index].target_id;
        native_entries[index].target_offset = entries[index].target_offset;
        native_entries[index].length = entries[index].length;
    }
    return native->submitTransfer((Transport::BatchID)batch_id, native_entries);
}

/**
 * @brief 获取传输状态（C接口）
 *
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @param task_id 任务ID
 * @param status 状态结构体指针
 * @return 成功返回0，失败返回错误码
 *
 * 该函数获取指定任务的传输状态：
 * 1. 调用原生状态查询函数
 * 2. 将结果复制到输出参数
 */
int getTransferStatus(transfer_engine_t engine, batch_id_t batch_id,
                      size_t task_id, struct transfer_status *status) {
    TransferEngine *native = (TransferEngine *)engine;
    Transport::TransferStatus native_status;
    int rc = native->getTransferStatus((Transport::BatchID)batch_id, task_id,
                                       native_status);
    if (rc == 0) {
        status->status = (int)native_status.s;
        status->transferred_bytes = native_status.transferred_bytes;
    }
    return rc;
}

/**
 * @brief 释放批次ID（C接口）
 *
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @return 成功返回0，失败返回错误码
 */
int freeBatchID(transfer_engine_t engine, batch_id_t batch_id) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->freeBatchID(batch_id);
}

/**
 * @brief 同步段缓存（C接口）
 *
 * @param engine 传输引擎句柄
 * @return 成功返回0，失败返回错误码
 *
 * 该函数强制将所有段的缓存数据写入底层存储：
 * 1. 遍历所有已打开的段
 * 2. 调用原生同步函数
 */
int syncSegmentCache(transfer_engine_t engine) {
    TransferEngine *native = (TransferEngine *)engine;
    return native->syncSegmentCache();
}
