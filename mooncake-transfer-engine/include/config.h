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

#ifndef CONFIG_H
#define CONFIG_H

#include <glog/logging.h>
#include <infiniband/verbs.h>
#include <jsoncpp/json/json.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace mooncake {

/**
 * @struct GlobalConfig
 * @brief 全局配置参数结构体
 *
 * 包含了系统运行所需的各种参数设置，主要包括：
 * 1. RDMA相关参数配置
 * 2. 网络通信参数配置
 * 3. 系统资源限制配置
 * 4. 性能调优相关参数
 */
struct GlobalConfig {
    // RDMA上下文配置
    size_t num_cq_per_ctx = 1;             // 每个上下文的完成队列(CQ)数量
    size_t num_comp_channels_per_ctx = 1;   // 每个上下文的完成通道数量
    uint8_t port = 1;                      // RDMA端口号
    int gid_index = 0;                     // GID索引（用于RoCE网络）

    // 队列配置
    size_t max_cqe = 4096;                 // 最大完成队列项数
    int max_ep_per_ctx = 256;             // 每个上下文最大端点数
    size_t num_qp_per_ep = 2;             // 每个端点的队列对(QP)数量
    size_t max_sge = 4;                   // 最大分散/聚集元素数量
    size_t max_wr = 256;                  // 最大工作请求数量
    size_t max_inline = 64;               // 最大内联数据大小（字节）

    // 网络配置
    ibv_mtu mtu_length = IBV_MTU_4096;    // MTU大小（最大传输单元）
    uint16_t handshake_port = 12001;      // 握手协议端口号

    // 线程和性能配置
    int workers_per_ctx = 2;              // 每个上下文的工作线程数
    bool verbose = false;                 // 是否输出详细日志
    size_t slice_size = 65536;           // 传输切片大小（字节）
    int retry_cnt = 8;                   // 重试次数
};

/**
 * @brief 加载全局配置
 * @param config 配置结构体引用
 *
 * 从配置文件或环境变量中加载配置参数
 */
void loadGlobalConfig(GlobalConfig &config);

/**
 * @brief 输出当前全局配置
 *
 * 将当前的配置参数打印到日志中，用于调试
 */
void dumpGlobalConfig();

/**
 * @brief 根据设备属性更新全局配置
 * @param device_attr RDMA设备属性
 *
 * 根据实际硬件能力调整配置参数
 */
void updateGlobalConfig(ibv_device_attr &device_attr);

/**
 * @brief 获取全局配置实例
 * @return 全局配置结构体引用
 *
 * 获取系统唯一的全局配置实例
 */
GlobalConfig &globalConfig();

/**
 * @brief 获取默认握手端口
 * @return 默认使用的握手协议端口号
 */
uint16_t getDefaultHandshakePort();

}  // namespace mooncake

#endif  // CONFIG_H