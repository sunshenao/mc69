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

#ifndef TRANSFER_METADATA
#define TRANSFER_METADATA

#include <glog/logging.h>
#include <jsoncpp/json/json.h>
#include <netdb.h>

#include <atomic>
#include <cstdint>
#include <etcd/SyncClient.hpp>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

#include "common.h"
#include "topology.h"

namespace mooncake {

// 前向声明
struct MetadataStoragePlugin;  // 元数据存储插件
struct HandShakePlugin;        // 握手协议插件

/**
 * @class TransferMetadata
 * @brief 传输元数据管理类，负责管理分布式系统中的元数据
 *
 * 该类负责：
 * 1. 管理内存段和设备的元数据信息
 * 2. 提供元数据的存储和同步机制
 * 3. 支持多种协议的设备发现和握手
 * 4. 维护全局的拓扑信息
 */
class TransferMetadata {
   public:
    /**
     * @struct DeviceDesc
     * @brief 设备描述符，描述网络设备（如RDMA网卡）的属性
     */
    struct DeviceDesc {
        std::string name;    // 设备名称 如 mlx5_0、eth0 等）
        uint16_t lid;        // 本地标识符（用于IB网络）
        std::string gid;     // 全局标识符（用于RoCE网络）
    };

    /**
     * @struct BufferDesc
     * @brief 缓冲区描述符，描述一块已注册的内存区域
     */
    struct BufferDesc {
        std::string name;           // 缓冲区名称
        uint64_t addr;             // 内存地址
        uint64_t length;           // 内存长度
        std::vector<uint32_t> lkey; // 本地内存密钥列表
        std::vector<uint32_t> rkey; // 远程内存密钥列表
    };

    /**  是一种将 NVMe（Non-Volatile Memory Express，非易失性内存标准）协议通过网络（如以太网、InfiniBand、RoCE 等）扩展到远程存储设备的技术。
     它允许服务器像访问本地 NVMe SSD 一样，通过高速网络访问远程的 NVMe 存储，具备高带宽、低延迟的特点，常用于数据中心和高性能计算场景。
     也就是说NVMe 可以看作本地文件
     * @struct NVMeoFBufferDesc
     * @brief NVMe-oF缓冲区描述符，描述NVMe设备上的存储区域
     */
    struct NVMeoFBufferDesc {
        std::string file_path;     // 文件路径
        uint64_t length;           // 区域长度
        std::unordered_map<std::string, std::string> local_path_map;  // 本地路径映射
    };

    // 内存段ID类型定义
    using SegmentID = uint64_t;

    /**
     * @struct SegmentDesc
     * @brief 内存段描述符，描述一个完整的内存段及其访问方式
     */
    struct SegmentDesc {
        std::string name;           // 段名称
        std::string protocol;       // 使用的传输协议
        std::vector<DeviceDesc> devices;     // RDMA设备列表
        Topology topology;                    // 网络拓扑信息
        std::vector<BufferDesc> buffers;     // RDMA缓冲区列表
        std::vector<NVMeoFBufferDesc> nvmeof_buffers;  // NVMe-oF缓冲区列表
    };

   public:

//   RPC主要用于以下几个方面
//a) 节点发现和管理
    //每个节点启动时都会注册自己的 RPC 信息到元数据服务
    //其他节点可以通过查询元数据服务发现集群中的其他节点
    //用于监控节点状态，发现节点失效
//b) 握手协议协调
    //在建立 RDMA 连接前，需要交换连接信息
    //使用 RPC 完成初始的握手过程

    /**
     * @brief RPC元数据描述结构体
     * 用于描述RPC节点的基本信息和状态
     */
    struct RpcMetaDesc {
        std::string ip_or_host_name;  // 主机名或IP地址
        uint16_t port;                // RPC服务端口号
        uint8_t status;              // 节点状态（0=正常，非0=异常）

        /**
         * @brief 将RPC元数据转换为JSON格式
         * @param value JSON对象引用
         */
        void toJson(Json::Value& value) const {
            value["ip_or_host_name"] = ip_or_host_name;
            value["port"] = port;
            value["status"] = status;
        }

        /**
         * @brief 从JSON格式解析RPC元数据
         * @param value JSON对象
         * @return 解析是否成功
         */
        bool fromJson(const Json::Value& value) {
            if (!value.isMember("ip_or_host_name") || !value.isMember("port") ||
                !value.isMember("status"))
                return false;
            ip_or_host_name = value["ip_or_host_name"].asString();
            port = value["port"].asUInt();
            status = value["status"].asUInt();
            return true;
        }
    };

    /**
     * @brief 内存段描述结构体
     * 描述一段已注册的内存区域的属性
     */
    struct SegmentDesc {
        size_t offset;             // 段内偏移量
        size_t length;            // 段长度（字节）
        std::string location;     // 存储位置（如 "CPU", "GPU"）
        bool remote_accessible;    // 是否允许远程访问
        uint8_t status;          // 段状态（0=正常，非0=异常）

        /**
         * @brief 将内存段描述转换为JSON格式
         * @param value JSON对象引用
         */
        void toJson(Json::Value& value) const;

        /**
         * @brief 从JSON格式解析内存段描述
         * @param value JSON对象
         * @return 解析是否成功
         */
        bool fromJson(const Json::Value& value);
    };

    /**
     * @brief 构造函数
     * @param metadata_conn_string 元数据服务连接字符串
     * 例如：
     * - "etcd://localhost:2379" 使用etcd作为元数据存储
     * - "redis://localhost:6379" 使用Redis作为元数据存储
     * - "http://localhost:8080" 使用HTTP服务作为元数据存储
     */
    TransferMetadata(const std::string& metadata_conn_string);

    /**
     * @brief 启动元数据服务
     * @param local_server_name 本地服务器名称
     * @param ip_or_host_name 本机IP或主机名
     * @param rpc_port RPC服务端口
     * @return 成功返回0，失败返回错误码
     *
     * 启动元数据服务：
     * 1. 连接到元数据存储后端
     * 2. 注册本地节点信息
     * 3. 启动握手服务
     * 4. 开始监控其他节点状态
     */
    int start(const std::string& local_server_name,
             const std::string& ip_or_host_name,
             uint16_t rpc_port);

    /**
     * @brief 停止元数据服务
     * 清理资源并关闭服务
     */
    void stop();

    /**
     * @brief 通过段名称获取段描述符
     * @param segment_name 段名称
     * @param force_update 是否强制从存储服务更新
     * @return 段描述符的共享指针
     */
    std::shared_ptr<SegmentDesc> getSegmentDescByName(
        const std::string &segment_name, bool force_update = false);

    /**
     * @brief 通过段ID获取段描述符
     * @param segment_id 段ID
     * @param force_update 是否强制从存储服务更新
     * @return 段描述符的共享指针
     */
    std::shared_ptr<SegmentDesc> getSegmentDescByID(SegmentID segment_id,
                                                    bool force_update = false);

    /**
     * @brief 更新本地段描述符
     * @param segment_id 段ID，默认为本地段
     * @return 成功返回0，失败返回错误码
     */
    int updateLocalSegmentDesc(SegmentID segment_id = LOCAL_SEGMENT_ID);

    /**
     * @brief 更新指定段的描述符
     * @param segment_name 段名称
     * @param desc 新的段描述符
     * @return 成功返回0，失败返回错误码
     */
    int updateSegmentDesc(const std::string &segment_name,
                          const SegmentDesc &desc);

    /**
     * @brief 获取指定段的描述符
     * @param segment_name 段名称
     * @return 段描述符的共享指针
     */
    std::shared_ptr<SegmentDesc> getSegmentDesc(
        const std::string &segment_name);

    /**
     * @brief 获取段ID
     * @param segment_name 段名称
     * @return 段ID
     */
    SegmentID getSegmentID(const std::string &segment_name);

    /**
     * @brief 同步段缓存
     *
     * 将本地的段元数据同步到存储服务
     * @return 成功返回0，失败返回错误码
     */
    int syncSegmentCache();

    /**
     * @brief 移除段描述符
     * @param segment_name 段名称
     * @return 成功返回0，失败返回错误码
     */
    int removeSegmentDesc(const std::string &segment_name);

    /**
     * @brief 添加本地内存缓冲区
     * @param buffer_desc 缓冲区描述符
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    int addLocalMemoryBuffer(const BufferDesc &buffer_desc,
                             bool update_metadata);

    /**
     * @brief 移除本地内存缓冲区
     * @param addr 内存地址
     * @param update_metadata 是否更新元数据
     * @return 成功返回0，失败返回错误码
     */
    int removeLocalMemoryBuffer(void *addr, bool update_metadata);

    /**
     * @brief 添加本地段
     * @param segment_id 段ID
     * @param segment_name 段名称
     * @param desc 段描述符
     * @return 成功返回0，失败返回错误码
     */
    int addLocalSegment(SegmentID segment_id, const std::string &segment_name,
                        std::shared_ptr<SegmentDesc> &&desc);

    /**
     * @brief 添加RPC元数据条目
     * @param server_name 服务器名称
     * @param desc RPC元数据描述符
     * @return 成功返回0，失败返回错误码
     */
    int addRpcMetaEntry(const std::string &server_name, RpcMetaDesc &desc);

    /**
     * @brief 移除RPC元数据条目
     * @param server_name 服务器名称
     * @return 成功返回0，失败返回错误码
     */
    int removeRpcMetaEntry(const std::string &server_name);

    /**
     * @brief 获取RPC元数据条目
     * @param server_name 服务器名称
     * @param desc RPC元数据描述符
     * @return 成功返回0，失败返回错误码
     */
    int getRpcMetaEntry(const std::string &server_name, RpcMetaDesc &desc);

    /**
     * @brief 获取本地RPC元数据
     * @return 本地RPC元数据描述符的常量引用
     */
    const RpcMetaDesc &localRpcMeta() const { return local_rpc_meta_; }

    /**
     * @brief 握手请求处理函数类型
     *
     * 用户需实现该函数，处理接收到的握手请求
     * @param peer_desc 对端握手描述符
     * @param local_desc 本地握手描述符
     * @return 成功返回0，失败返回错误码
     */
    using OnReceiveHandShake = std::function<int(const HandShakeDesc &peer_desc,
                                                 HandShakeDesc &local_desc)>;
    /**
     * @brief 启动握手守护进程
     *
     * 该进程负责接收和处理握手请求
     * @param on_receive_handshake 用户实现的握手处理函数
     * @param listen_port 监听端口
     * @return 成功返回0，失败返回错误码
     */
    int startHandshakeDaemon(OnReceiveHandShake on_receive_handshake,
                             uint16_t listen_port);

    /**
     * @brief 发送握手请求
     *
     * 该函数用于向对端发送握手请求，并接收响应
     * @param peer_server_name 对端服务器名称
     * @param local_desc 本地握手描述符
     * @param peer_desc 对端握手描述符
     * @return 成功返回0，失败返回错误码
     */
    int sendHandshake(const std::string &peer_server_name,
                      const HandShakeDesc &local_desc,
                      HandShakeDesc &peer_desc);

   private:
    // local cache
    RWSpinlock segment_lock_;  // 段描述符读写锁
    std::unordered_map<uint64_t, std::shared_ptr<SegmentDesc>>
        segment_id_to_desc_map_;  // 段ID到段描述符的映射
    std::unordered_map<std::string, uint64_t> segment_name_to_id_map_;  // 段名称到段ID的映射

    RWSpinlock rpc_meta_lock_;  // RPC元数据读写锁
    std::unordered_map<std::string, RpcMetaDesc> rpc_meta_map_;  // RPC元数据映射
    RpcMetaDesc local_rpc_meta_;  // 本地RPC元数据

    std::atomic<SegmentID> next_segment_id_;  // 下一个段ID

    std::shared_ptr<HandShakePlugin> handshake_plugin_;  // 握手协议插件
    std::shared_ptr<MetadataStoragePlugin> storage_plugin_;  // 元数据存储插件
};

}  // namespace mooncake

#endif  // TRANSFER_METADATA