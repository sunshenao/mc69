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

#ifndef ERROR_H
#define ERROR_H

/**
 * @file error.h
 * @brief 错误码定义
 *
 * 本文件定义了传输引擎中使用的所有错误码，
 * 错误码按功能模块分类，具体分为：
 * 1. 基础错误 (-1 ~ -99)
 * 2. 网络相关错误 (-100 ~ -199)
 * 3. 元数据相关错误 (-200 ~ -299)
 * 4. 系统相关错误 (-300 ~ -399)
 */

//
// 基础错误码定义 (-1 ~ -99)
//

/** 无效参数错误 */
#define ERR_INVALID_ARGUMENT (-1)

/** 请求数量超过限制 */
#define ERR_TOO_MANY_REQUESTS (-2)

/** 访问未注册的内存地址 */
#define ERR_ADDRESS_NOT_REGISTERED (-3)

/** 批次正忙 */
#define ERR_BATCH_BUSY (-4)

/** 设备未找到 */
#define ERR_DEVICE_NOT_FOUND (-6)

/** 内存地址重叠 */
#define ERR_ADDRESS_OVERLAPPED (-7)

//
// 网络相关错误码定义 (-100 ~ -199)
//

/** DNS解析错误：无法解析主机名或IP地址 */
#define ERR_DNS (-100)

/** Socket错误：创建、绑定、连接等socket操作失败 */
#define ERR_SOCKET (-101)

/** 连接超时：建立连接或数据传输超时 */
#define ERR_TIMEOUT (-102)

/** 连接断开：远程端点断开连接 */
#define ERR_DISCONNECTED (-103)

/** JSON格式错误：无法解析或格式不正确 */
#define ERR_MALFORMED_JSON (-104)

/** 握手请求被拒绝 */
#define ERR_REJECT_HANDSHAKE (-105)

//
// 元数据相关错误码定义 (-200 ~ -299)
//

/** 元数据服务不可用：无法连接到元数据服务 */
#define ERR_METADATA_SERVICE_UNAVAILABLE (-200)

/** 元数据不存在：请求的元数据键不存在 */
#define ERR_METADATA_NOT_FOUND (-201)

/** 元数据操作失败：读取、写入或删除操作失败 */
#define ERR_METADATA_OPERATION_FAILED (-202)

//
// 系统相关错误码定义 (-300 ~ -399)
//

/** 内存分配失败：系统内存不足 */
#define ERR_OUT_OF_MEMORY (-300)

/** 权限不足：无权限执行操作 */
#define ERR_PERMISSION_DENIED (-301)

/** IO错误：文件或设备IO操作失败 */
#define ERR_IO (-302)

/** 资源耗尽：系统资源（如文件描述符）耗尽 */
#define ERR_RESOURCE_EXHAUSTED (-303)

/** 设备错误：硬件设备操作失败 */
#define ERR_DEVICE (-304)

/** 功能未实现 */
#define ERR_NOT_IMPLEMENTED (-305)

#endif  // ERROR_H