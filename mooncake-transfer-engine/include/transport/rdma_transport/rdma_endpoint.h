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

#ifndef RDMA_ENDPOINT_H
#define RDMA_ENDPOINT_H

#include <queue>

#include "rdma_context.h"

namespace mooncake {

/**
 * @class RdmaEndPoint
 * @brief RDMA端点类，表示本地NIC和远程NIC之间的所有队列对(QP)连接
 *
 * 端点的生命周期：
 * 1. 构造阶段：分配资源但不指定对端
 * 2. 握手阶段：与远程端点交换连接信息
 *    - 主动方调用setupConnectionsByActive()
 *    - 被动方通过RPC服务调用setupConnectionsByPassive()
 * 3. 连接阶段：握手完成后状态设置为CONNECTED
 * 4. 断开阶段：用户调用disconnect()或检测到错误时断开连接
 *
 * 端点状态流转：
 * INITIALIZING -> UNCONNECTED -> CONNECTED -> UNCONNECTED
 */
class RdmaEndPoint {
   public:
    /**
     * @brief 端点状态枚举
     */
    enum Status {
        INITIALIZING,    // 初始化中，资源分配阶段
        UNCONNECTED,     // 未连接，等待握手或连接断开后
        CONNECTED,       // 已连接，可以进行数据传输
    };

   public:
    /**
     * @brief 构造函数
     * @param context RDMA上下文引用
     *
     * 初始化端点并分配必要的资源：
     * 1. 创建所需数量的队列对(QP)
     * 2. 初始化完成队列(CQ)
     * 3. 设置初始状态为INITIALIZING
     */
    RdmaEndPoint(RdmaContext &context);

    /**
     * @brief 析构函数
     *
     * 清理所有RDMA资源：
     * 1. 断开现有连接
     * 2. 销毁所有队列对
     * 3. 释放相关资源
     */
    ~RdmaEndPoint();

    /**
     * @brief 构建端点的基础设施
     * @param num_qp 队列对数量
     * @param max_sge 最大分散/聚集元素数
     * @param max_wr 最大工作请求数
     * @param max_inline 最大内联数据大小
     * @return 成功返回0，失败返回错误码
     */
    int construct(size_t num_qp = 2, size_t max_sge = 4,
                  size_t max_wr = 256, size_t max_inline = 64);

    /**
     * @brief 主动方建立连接
     * @param peer_nic_path 对端NIC路径，格式：server_name@nic_name
     * @return 成功返回0，失败返回错误码
     */
    int setupConnectionsByActive(const std::string &peer_nic_path);

    /**
     * @brief 被动方建立连接
     * @param lid 对端本地标识符
     * @param gid 对端全局标识符
     * @param qp_nums 对端队列对编号列表
     * @return 成功返回0，失败返回错误码
     */
    int setupConnectionsByPassive(uint16_t lid, const std::string &gid,
                                 const std::vector<uint32_t> &qp_nums);

    /**
     * @brief 断开连接
     * @return 成功返回0，失败返回错误码
     *
     * 该方法会：
     * 1. 将所有队列对转换到ERROR状态
     * 2. 清空所有未完成的工作请求
     * 3. 重置连接状态为UNCONNECTED
     */
    int disconnect();

    /**
     * @brief 获取当前状态
     * @return 端点状态枚举值
     */
    Status getStatus() const { return status_; }

    /**
     * @brief 获取对端NIC路径
     * @return 对端NIC路径字符串
     */
    const std::string &getPeerNicPath() const { return peer_nic_path_; }

   private:
    /**
     * @brief 创建单个队列对
     * @param qp_index 队列对索引
     * @return 成功返回0，失败返回错误码
     */
    int createQueuePair(size_t qp_index);

    /**
     * @brief 修改队列对状态
     * @param qp_index 队列对索引
     * @param target_state 目标状态
     * @return 成功返回0，失败返回错误码
     */
    int modifyQueuePairState(size_t qp_index,
                            enum ibv_qp_state target_state);

   private:
    RdmaContext &context_;                    // RDMA上下文引用
    std::string peer_nic_path_;              // 对端NIC路径
    std::atomic<Status> status_;             // 当前状态

    size_t num_qp_;                          // 队列对数量
    size_t max_sge_;                         // 最大分散/聚集元素数
    size_t max_wr_;                          // 最大工作请求数
    size_t max_inline_;                      // 最大内联数据大小

    std::vector<struct ibv_qp *> qp_list_;   // 队列对列表
    std::vector<struct ibv_cq *> cq_list_;   // 完成队列列表
};

}  // namespace mooncake

#endif  // RDMA_ENDPOINT_H