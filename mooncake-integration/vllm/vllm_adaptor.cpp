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

#include "vllm_adaptor.h"
#include <cassert>

/**
 * @class VLLMAdaptor
 * @brief vLLM适配器，用于将Mooncake传输引擎集成到vLLM中
 *
 * 该类为vLLM提供以下功能：
 * 1. 高性能的跨节点内存传输
 * 2. 智能的内存池管理
 * 3. 统一的设备管理接口
 */
VLLMAdaptor::VLLMAdaptor() {}

/**
 * @brief 析构函数，清理所有资源
 *
 * 释放的资源包括：
 * 1. 关闭所有段句柄
 * 2. 释放传输引擎
 * 3. 释放所有分配的内存缓冲区
 */
VLLMAdaptor::~VLLMAdaptor() {
    // 关闭所有段句柄
    for (auto &handle : handle_map_) engine_->closeSegment(handle.second);
    handle_map_.clear();
    engine_.reset();
    // 释放普通缓冲区
    for (auto &buffer : buffer_list_) free(buffer);
    buffer_list_.clear();
    // 释放大型缓冲区
    for (auto &buffer : large_buffer_list_) free(buffer);
    large_buffer_list_.clear();
}

/**
 * @brief 格式化设备名称列表
 * @param device_names 逗号分隔的设备名称字符串
 * @return JSON格式的设备名称数组
 *
 * 将逗号分隔的设备名称转换为JSON数组格式：
 * 输入："mlx5_0,mlx5_1"
 * 输出：["mlx5_0","mlx5_1"]
 */
std::string formatDeviceNames(const std::string &device_names) {
    // 按逗号分割设备名称
    std::stringstream ss(device_names);
    std::string item;
    std::vector<std::string> tokens;
    while (getline(ss, item, ',')) {
        tokens.push_back(item);
    }

    // 构建JSON格式字符串
    std::string formatted;
    for (size_t i = 0; i < tokens.size(); ++i) {
        formatted += "\"" + tokens[i] + "\"";
        if (i < tokens.size() - 1) {
            formatted += ",";
        }
    }
    return formatted;
}

/**
 * @brief 解析连接字符串
 * @param conn_string 连接字符串
 * @return 协议和域名的配对
 *
 * 解析形如"protocol://domain"的连接字符串：
 * - 如果没有指定协议，默认使用"etcd"
 * - 返回<协议, 域名>对
 */
std::pair<std::string, std::string> parseConnectionString(
    const std::string &conn_string) {
    std::pair<std::string, std::string> result;
    std::string proto = "etcd";  // 默认协议
    std::string domain;

    // 查找协议分隔符
    std::size_t pos = conn_string.find("://");
    if (pos != std::string::npos) {
        proto = conn_string.substr(0, pos);
        domain = conn_string.substr(pos + 3);
    } else {
        domain = conn_string;
    }

    result.first = proto;
    result.second = domain;
    return result;
}

/**
 * @brief 初始化vLLM适配器
 * @param local_hostname 本地主机名
 * @param metadata_server 元数据服务地址
 * @param protocol 传输协议（"rdma"或"tcp"）
 * @param device_name 设备名称
 * @return 成功返回0，失败返回-1
 */
int VLLMAdaptor::initialize(const char *local_hostname,
                            const char *metadata_server, const char *protocol,
                            const char *device_name) {
    // 解析连接字符串，获取协议和域名
    auto conn_string = parseConnectionString(metadata_server);
    return initializeExt(local_hostname, conn_string.second.c_str(), protocol,
                         device_name, conn_string.first.c_str());
}

/**
 * @brief 扩展的初始化函数
 * @param local_hostname 本地主机名
 * @param metadata_server 元数据服务地址
 * @param protocol 传输协议
 * @param device_name 设备名称
 * @param metadata_type 元数据服务类型
 * @return 成功返回0，失败返回-1
 *
 * 该函数完成适配器的初始化：
 * 1. 创建传输引擎实例
 * 2. 初始化传输引擎
 * 3. 安装传输协议
 * 4. 初始化内存池
 */
int VLLMAdaptor::initializeExt(const char *local_hostname,
                               const char *metadata_server,
                               const char *protocol, const char *device_name,
                               const char *metadata_type) {
    // 构建完整的连接字符串
    std::string conn_string = metadata_server;
    if (conn_string.find("://") == std::string::npos)
        conn_string =
            std::string(metadata_type) + "://" + std::string(metadata_server);

    // 创建并初始化传输引擎
    engine_ = std::make_unique<TransferEngine>(false);
    auto hostname_port = parseHostNameWithPort(local_hostname);
    int ret = engine_->init(conn_string, local_hostname,
                            hostname_port.first.c_str(), hostname_port.second);
    if (ret) return -1;

    // 安装传输协议
    xport_ = nullptr;
    if (strcmp(protocol, "rdma") == 0) {
        // 对于RDMA，需要设置网卡优先级矩阵
        auto device_names = formatDeviceNames(device_name);
        std::string nic_priority_matrix =
            "{\"cpu:0\": [[" + device_names + "], []]}";
        void **args = (void **)malloc(2 * sizeof(void *));
        args[0] = (void *)nic_priority_matrix.c_str();
        args[1] = nullptr;
        xport_ = engine_->installTransport("rdma", args);
    } else if (strcmp(protocol, "tcp") == 0) {
        // TCP协议不需要额外参数
        xport_ = engine_->installTransport("tcp", nullptr);
    } else {
        LOG(ERROR) << "Unsupported protocol";
        return -1;
    }

    if (!xport_) return -1;

    // 初始化内存池
    free_list_.resize(kSlabSizeKBTabLen);
    doBuddyAllocate(kMaxClassId);
    return 0;
}

/**
 * @brief 分配原始内存缓冲区
 * @param capacity 需要的容量（字节）
 * @return 缓冲区指针，失败返回nullptr
 *
 * 该函数分配并注册内存：
 * 1. 分配指定大小的内存
 * 2. 将内存注册到传输引擎
 * 3. 失败时释放资源
 */
char *VLLMAdaptor::allocateRawBuffer(size_t capacity) {
    // 分配内存
    auto buffer = malloc(capacity);
    if (!buffer) return nullptr;
    // 注册到传输引擎
    int ret = engine_->registerLocalMemory(buffer, capacity, "cpu:0");
    if (ret) {
        free(buffer);
        return nullptr;
    }
    return (char *)buffer;
}

/**
 * @brief 查找适合指定大小的内存类
 * @param size 需要的内存大小（字节）
 * @return 内存类ID，-1表示大小超出范围
 *
 * 该函数根据请求的大小选择合适的内存类：
 * - 支持的内存类大小定义在kSlabSizeKB中
 * - 选择能容纳请求大小的最小内存类
 */
int VLLMAdaptor::findClassId(size_t size) {
    // 检查是否超过最大支持的大小
    if (size > 1024ull * kSlabSizeKB[kMaxClassId]) return -1;
    // 从大到小查找合适的内存类
    for (int i = kMaxClassId - 2; i >= 0; --i)
        if (size > 1024ull * kSlabSizeKB[i]) return i + 1;
    return 0;
}

/**
 * @brief 伙伴系统内存分配
 * @param class_id 内存类ID
 * @return 成功返回0，失败返回非零值
 *
 * 该函数实现伙伴系统的内存分配：
 * 1. 如果是最大类，直接分配新内存
 * 2. 否则从更大的类中分裂出所需大小
 */
int VLLMAdaptor::doBuddyAllocate(int class_id) {
    // 处理最大类的分配
    if (class_id == kMaxClassId) {
        auto buffer = allocateRawBuffer(kDefaultBufferCapacity);
        buffer_list_.push_back(buffer);
        // 将大块内存切分并加入空闲列表
        for (size_t offset = 0; offset < kDefaultBufferCapacity;
             offset += 1024ull * kSlabSizeKB[kMaxClassId])
            free_list_[kMaxClassId].push(buffer + offset);
        return 0;
    }

    // 处理其他类的分配
    if (free_list_[class_id + 1].empty()) {
        // 如果更大的类没有空闲内存，递归分配
        int ret = doBuddyAllocate(class_id + 1);
        if (ret) return ret;
    }

    // 从更大的类中分裂出两块相等的内存
    assert(!free_list_[class_id + 1].empty());
    char *buffer = free_list_[class_id + 1].top();
    free_list_[class_id + 1].pop();
    free_list_[class_id].push(buffer);
    free_list_[class_id].push(buffer + kSlabSizeKB[class_id] * 1024);
    return 0;
}

/**
 * @brief 分配托管内存缓冲区
 * @param length 需要的内存大小（字节）
 * @return 内存地址，失败返回0
 *
 * 该函数从内存池分配内存：
 * 1. 对于大内存直接分配
 * 2. 对于小内存从内存池分配
 * 3. 如果内存池空间不足，会触发新的分配
 */
uintptr_t VLLMAdaptor::allocateManagedBuffer(size_t length) {
    std::lock_guard<std::mutex> guard(mutex_);
    // 查找合适的内存类
    int class_id = findClassId(length);
    if (class_id < 0) {
        // 大内存直接分配
        char *buffer = allocateRawBuffer(length);
        if (buffer) large_buffer_list_.insert(buffer);
        return (uintptr_t)buffer;
    }

    // 从内存池分配
    if (free_list_[class_id].empty())
        if (doBuddyAllocate(class_id)) return 0;
    assert(!free_list_[class_id].empty());
    char *buffer = free_list_[class_id].top();
    free_list_[class_id].pop();
    return (uintptr_t)buffer;
}

/**
 * @brief 释放托管内存缓冲区
 * @param buffer_addr 内存地址
 * @param length 内存大小（字节）
 * @return 成功返回0，失败返回非零值
 *
 * 该函数处理内存释放：
 * 1. 大内存直接释放
 * 2. 小内存返回到内存池
 */
int VLLMAdaptor::freeManagedBuffer(uintptr_t buffer_addr, size_t length) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto buffer = (char *)buffer_addr;
    // 查找内存类
    int class_id = findClassId(length);
    if (class_id < 0) {
        // 大内存直接释放
        large_buffer_list_.erase(buffer);
        engine_->unregisterLocalMemory(buffer);
        free(buffer);
        return 0;
    }
    // 小内存返回到内存池
    free_list_[class_id].push(buffer);
    return 0;
}

/**
 * @brief 同步传输数据
 * @param target_hostname 目标主机名
 * @param buffer 源缓冲区地址
 * @param peer_buffer_address 目标缓冲区地址
 * @param length 传输长度（字节）
 * @return 成功返回0，失败返回-1
 *
 * 该函数执行同步数据传输：
 * 1. 获取或创建目标段句柄
 * 2. 提交传输请求
 * 3. 等待传输完成
 */
int VLLMAdaptor::transferSync(const char *target_hostname, uintptr_t buffer,
                              uintptr_t peer_buffer_address, size_t length) {
    // 获取或创建目标段句柄
    Transport::SegmentHandle handle;
    if (handle_map_.count(target_hostname)) {
        handle = handle_map_[target_hostname];
    } else {
        handle = engine_->openSegment(target_hostname);
        if (handle == (Transport::SegmentHandle)-1) return -1;
        handle_map_[target_hostname] = handle;
    }

    // 创建并提交传输请求
    auto batch_id = engine_->allocateBatchID(1);
    TransferRequest entry;
    entry.opcode = TransferRequest::READ;
    entry.length = length;
    entry.source = (void *)buffer;
    entry.target_id = handle;
    entry.target_offset = peer_buffer_address;

    int ret = engine_->submitTransfer(batch_id, {entry});
    if (ret < 0) return -1;

    // 等待传输完成
    TransferStatus status;
    while (true) {
        int ret = engine_->getTransferStatus(batch_id, 0, status);
        LOG_ASSERT(!ret);
        if (status.s == TransferStatusEnum::COMPLETED) {
            engine_->freeBatchID(batch_id);
            return 0;
        } else if (status.s == TransferStatusEnum::FAILED) {
            engine_->freeBatchID(batch_id);
            return -1;
        }
    }
}

/**
 * @brief 实验性接口：注册外部内存
 * @param buffer_addr 内存地址
 * @param capacity 内存大小（字节）
 * @return 成功返回0，失败返回非零值
 */
int VLLMAdaptor::expRegisterMemory(uintptr_t buffer_addr, size_t capacity) {
    char *buffer = reinterpret_cast<char *>(buffer_addr);
    return engine_->registerLocalMemory(buffer, capacity, "cpu:0");
}

/**
 * @brief 实验性接口：取消注册外部内存
 * @param buffer_addr 内存地址
 * @return 成功返回0，失败返回非零值
 */
int VLLMAdaptor::expUnregisterMemory(uintptr_t buffer_addr) {
    char *buffer = reinterpret_cast<char *>(buffer_addr);
    return engine_->unregisterLocalMemory(buffer);
}

/**
 * @brief Python绑定
 *
 * 使用pybind11为Python提供接口：
 * 1. 构造和析构函数
 * 2. 初始化函数
 * 3. 内存管理函数
 * 4. 传输函数
 * 5. 实验性接口
 */
namespace py = pybind11;

PYBIND11_MODULE(mooncake_vllm_adaptor, m) {
    py::class_<VLLMAdaptor>(m, "mooncake_vllm_adaptor")
        .def(py::init<>())
        .def("initialize", &VLLMAdaptor::initialize)
        .def("initializeExt", &VLLMAdaptor::initializeExt)
        .def("allocateManagedBuffer", &VLLMAdaptor::allocateManagedBuffer)
        .def("freeManagedBuffer", &VLLMAdaptor::freeManagedBuffer)
        .def("transferSync", &VLLMAdaptor::transferSync)
        .def("writeBytesToBuffer", &VLLMAdaptor::writeBytesToBuffer)
        .def("readBytesFromBuffer", &VLLMAdaptor::readBytesFromBuffer)
        .def("expRegisterMemory", &VLLMAdaptor::expRegisterMemory)
        .def("expUnregisterMemory", &VLLMAdaptor::expUnregisterMemory);
}
