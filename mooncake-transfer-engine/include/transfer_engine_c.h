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

#ifndef TRANSFER_ENGINE_C
#define TRANSFER_ENGINE_C

#include <stddef.h>
#include <stdint.h>

/**
 * @file transfer_engine_c.h
 * @brief 传输引擎的C语言接口封装
 *
 * 本文件为C++实现的传输引擎提供C语言接口，
 * 使得C语言程序也能方便地使用传输引擎的功能。
 * 所有指针类型参数指向的内存在函数返回后即可释放。
 */

#ifdef __cplusplus
extern "C" {
#endif  // __cplusplus

// 基本类型定义
#define segment_handle_t int32_t    // 段句柄类型
#define segment_id_t int32_t        // 段ID类型
#define batch_id_t uint64_t         // 批次ID类型
#define LOCAL_SEGMENT (0)           // 本地段标识
#define INVALID_BATCH UINT64_MAX    // 无效批次标识

// 操作码定义
#define OPCODE_READ (0)   // 读操作
#define OPCODE_WRITE (1)  // 写操作

/**
 * @struct transfer_request
 * @brief 传输请求结构
 *
 * 描述一个数据传输操作的详细信息
 */
struct transfer_request {
    int opcode;                // 操作类型：读/写
    void *source;              // 源数据地址
    segment_id_t target_id;    // 目标段ID
    uint64_t target_offset;    // 目标段内的偏移量
    uint64_t length;          // 传输长度
};

typedef struct transfer_request transfer_request_t;

// 传输状态定义
#define STATUS_WAITING (0)     // 等待中
#define STATUS_PENDING (1)     // 进行中
#define STATUS_INVALID (2)     // 无效的
#define STATUS_CANNELED (3)    // 已取消
#define STATUS_COMPLETED (4)   // 已完成
#define STATUS_TIMEOUT (5)     // 超时
#define STATUS_FAILED (6)      // 失败

/**
 * @struct transfer_status
 * @brief 传输状态结构
 *
 * 描述传输操作的当前状态
 */
struct transfer_status {
    int status;                // 状态码
    uint64_t transferred_bytes; // 已传输字节数
};

/**
 * @struct segment_desc
 * @brief 段描述符结构
 *
 * 描述一个内存段或存储段的属性，
 * 支持RDMA和NVMe-oF两种类型
 */
struct segment_desc {
    int type;  // 段类型：RDMA / NVMeoF
    union {
        // RDMA类型段的描述信息
        struct {
            void *addr;             // 内存地址
            uint64_t size;          // 内存大小
            const char *location;    // 位置标识
        } rdma;
        // NVMe-oF类型段的描述信息
        struct {
            const char *file_path;       // 文件路径
            const char *subsystem_name;   // 子系统名称
            const char *proto;            // 使用的协议
            const char *ip;              // 服务器IP
            uint64_t port;               // 服务端口
        } nvmeof;
    } desc_;
};

typedef struct transfer_status transfer_status_t;

/**
 * @struct buffer_entry
 * @brief 缓冲区条目结构
 *
 * 描述一块连续的内存缓冲区
 */
struct buffer_entry {
    void *addr;       // 内存地址
    size_t length;    // 内存长度
};
typedef struct buffer_entry buffer_entry_t;

typedef struct segment_desc segment_desc_t;
typedef void *transfer_engine_t;   // 传输引擎句柄
typedef void *transport_t;         // 传输协议句柄

/**
 * @brief 创建传输引擎实例
 * @param metadata_conn_string 元数据服务连接字符串
 * @param local_server_name 本地服务器名称
 * @param ip_or_host_name IP地址或主机名
 * @param rpc_port RPC服务端口
 * @return 传输引擎句柄
 *
 * 注意：所有字符串参数在函数返回后即可释放，
 * 传输引擎会在内部保存副本
 */
transfer_engine_t createTransferEngine(const char *metadata_conn_string,
                                       const char *local_server_name,
                                       const char *ip_or_host_name,
                                       uint64_t rpc_port);

/**
 * @brief 安装传输协议
 * @param engine 传输引擎句柄
 * @param proto 协议名称
 * @return 传输协议句柄
 */
transport_t installTransport(transfer_engine_t engine, const char *proto,
                             void **args);

/**
 * @brief 卸载传输协议
 * @param engine 传输引擎句柄
 * @param proto 协议名称
 * @return 成功返回0，失败返回非零值
 */
int uninstallTransport(transfer_engine_t engine, const char *proto);

/**
 * @brief 打开一个段
 * @param engine 传输引擎句柄
 * @param segment_name 段名称
 * @return 段ID
 *
 * 注意：段ID是一个唯一标识符，用于在后续操作中引用该段
 */
segment_id_t openSegment(transfer_engine_t engine, const char *segment_name);

/**
 * @brief 关闭一个段
 * @param engine 传输引擎句柄
 * @param segment_id 段ID
 * @return 成功返回0，失败返回非零值
 */
int closeSegment(transfer_engine_t engine, segment_id_t segment_id);

/**
 * @brief 销毁传输引擎实例
 * @param engine 传输引擎句柄
 *
 * 释放传输引擎占用的所有资源
 */
void destroyTransferEngine(transfer_engine_t engine);

/**
 * @brief 注册本地内存
 * @param engine 传输引擎句柄
 * @param addr 内存地址
 * @param length 内存长度
 * @param location 位置标识
 * @param remote_accessible 是否可远程访问
 * @return 成功返回0，失败返回非零值
 */
int registerLocalMemory(transfer_engine_t engine, void *addr, size_t length,
                        const char *location, int remote_accessible);

/**
 * @brief 取消注册本地内存
 * @param engine 传输引擎句柄
 * @param addr 内存地址
 * @return 成功返回0，失败返回非零值
 */
int unregisterLocalMemory(transfer_engine_t engine, void *addr);

/**
 * @brief 批量注册本地内存
 * @param engine 传输引擎句柄
 * @param buffer_list 缓冲区条目数组
 * @param buffer_len 缓冲区条目数量
 * @param location 位置标识
 * @return 成功返回0，失败返回非零值
 */
int registerLocalMemoryBatch(transfer_engine_t engine,
                             buffer_entry_t *buffer_list, size_t buffer_len,
                             const char *location);

/**
 * @brief 批量取消注册本地内存
 * @param engine 传输引擎句柄
 * @param addr_list 内存地址数组
 * @param addr_len 地址数量
 * @return 成功返回0，失败返回非零值
 */
int unregisterLocalMemoryBatch(transfer_engine_t engine, void **addr_list,
                               size_t addr_len);

/**
 * @brief 分配批次ID
 * @param engine 传输引擎句柄
 * @param batch_size 批次大小
 * @return 批次ID
 *
 * 注意：批次ID用于标识一组传输请求
 */
batch_id_t allocateBatchID(transfer_engine_t engine, size_t batch_size);

/**
 * @brief 提交传输请求
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @param entries 传输请求数组
 * @param count 请求数量
 * @return 成功返回0，失败返回非零值
 */
int submitTransfer(transfer_engine_t engine, batch_id_t batch_id,
                   struct transfer_request *entries, size_t count);

/**
 * @brief 获取传输状态
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @param task_id 任务ID
 * @param status 存放状态的结构体指针
 * @return 成功返回0，失败返回非零值
 */
int getTransferStatus(transfer_engine_t engine, batch_id_t batch_id,
                      size_t task_id, struct transfer_status *status);

/**
 * @brief 释放批次ID
 * @param engine 传输引擎句柄
 * @param batch_id 批次ID
 * @return 成功返回0，失败返回非零值
 */
int freeBatchID(transfer_engine_t engine, batch_id_t batch_id);

/**
 * @brief 同步段缓存
 * @param engine 传输引擎句柄
 * @return 成功返回0，失败返回非零值
 */
int syncSegmentCache(transfer_engine_t engine);

#ifdef __cplusplus
}
#endif  // __cplusplus

#endif  // TRANSFER_ENGINE_C
