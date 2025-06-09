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

#ifndef TRANSFER_METADATA_PLUGIN
#define TRANSFER_METADATA_PLUGIN

#include "transfer_metadata.h"

namespace mooncake {

/**
 * @struct MetadataStoragePlugin
 * @brief 元数据存储插件接口
 *
 * 该接口定义了元数据存储的基本操作：
 * 1. 创建存储实例
 * 2. 获取元数据
 * 3. 设置元数据
 * 4. 删除元数据
 *
 * 插件可以支持不同的后端存储，如：
 * - etcd
 * - Redis
 * - ZooKeeper
 * - 本地文件系统
 */
struct MetadataStoragePlugin {
    /**
     * @brief 创建存储插件实例
     * @param conn_string 连接字符串
     * @return 插件实例的共享指针
     *
     * 工厂方法，根据连接字符串创建对应的存储插件：
     * - "etcd://host:port" -> Etcd存储
     * - "redis://host:port" -> Redis存储
     * - "file:///path" -> 本地文件存储
     */
    static std::shared_ptr<MetadataStoragePlugin> Create(
        const std::string &conn_string);

    MetadataStoragePlugin() {}
    virtual ~MetadataStoragePlugin() = default;

    /**
     * @brief 获取元数据
     * @param key 元数据的键
     * @param value 用于存储获取到的值
     * @return 成功返回true，失败返回false
     *
     * 从存储后端读取指定键的元数据，数据以JSON格式返回
     */
    virtual bool get(const std::string &key, Json::Value &value) = 0;

    /**
     * @brief 设置元数据
     * @param key 元数据的键
     * @param value 要存储的值（JSON格式）
     * @return 成功返回true，失败返回false
     *
     * 将JSON格式的元数据存储到指定的键下
     */
    virtual bool set(const std::string &key, const Json::Value &value) = 0;

    /**
     * @brief 删除元数据
     * @param key 要删除的元数据键
     * @return 成功返回true，失败返回false
     *
     * 从存储后端删除指定键的元数据
     */
    virtual bool remove(const std::string &key) = 0;
};

/**
 * @struct HandShakePlugin
 * @brief 握手协议插件接口
 *
 * 该接口定义了节点间握手协议的基本操作：
 * 1. 启动握手服务守护进程
 * 2. 发送握手请求
 * 3. 处理握手响应
 *
 * 握手过程用于：
    节点发现：让各个节点能找到彼此，知道对方的存在和基本信息。
    能力协商：在通信前，双方可以交换各自支持的功能或参数，确保兼容。
    安全认证：通过握手过程验证对方身份，防止未授权访问。
    连接建立：完成必要的前置步骤后，正式建立通信连接。
 */
struct HandShakePlugin {
    using OnReceiveCallBack =
        std::function<void(const Json::Value &, Json::Value &)>;

    /**
     * @brief 创建握手插件实例
     * @param conn_string 连接字符串
     * @return 插件实例的共享指针
     */
    static std::shared_ptr<HandShakePlugin> Create(
        const std::string &conn_string);

    virtual ~HandShakePlugin() = default;

    /**
     * @brief 启动握手服务守护进程
     * @param on_recv_callback 接收到握手消息时的回调函数
     * @param listen_port 监听端口
     * @return 成功返回0，失败返回错误码
     *
     * 启动一个守护进程监听握手请求：
     * 1. 绑定指定端口
     * 2. 等待连接请求
     * 3. 处理握手消息
     * 4. 调用回调函数处理业务逻辑
     */
    virtual int startDaemon(OnReceiveCallBack on_recv_callback,
                          uint16_t listen_port) = 0;

    /**
     * @brief 发送握手请求
     * @param ip_or_host_name 目标IP或主机名
     * @param rpc_port 目标RPC端口
     * @param local 本地节点信息（JSON格式）
     * @param peer 用于存储对端响应的节点信息
     * @return 成功返回0，失败返回错误码
     *
     * 向目标节点发送握手请求：
     * 1. 建立连接
     * 2. 发送本地信息
     * 3. 等待对端响应
     * 4. 解析并存储对端信息
     */
    virtual int send(std::string ip_or_host_name, uint16_t rpc_port,
                    const Json::Value &local, Json::Value &peer) = 0;
};

}  // namespace mooncake

#endif  // TRANSFER_METADATA_PLUGIN

