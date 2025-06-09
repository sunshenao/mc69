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

#include "transport/rdma_transport/rdma_endpoint.h"

#include <glog/logging.h>

#include <cassert>
#include <cstddef>

#include "config.h"

namespace mooncake {
/**
 * @brief RDMA连接参数常量定义
 */
const static uint8_t MAX_HOP_LIMIT = 16;  // 最大跳数限制，用于路由
const static uint8_t TIMEOUT = 14;        // 超时时间，以4.096微秒为单位
const static uint8_t RETRY_CNT = 7;       // 重试次数，发生错误时重试

/**
 * @brief RDMA端点构造函数
 * @param context RDMA上下文引用
 *
 * 初始化端点的基本状态：
 * 1. 设置初始化状态
 * 2. 标记端点为活跃状态
 * 3. 关联RDMA上下文
 */
RdmaEndPoint::RdmaEndPoint(RdmaContext &context)
    : context_(context), status_(INITIALIZING), active_(true) {}

/**
 * @brief RDMA端点析构函数
 *
 * 清理端点资源：
 * 1. 检查队列对列表
 * 2. 如果存在队列对，调用deconstruct()清理
 */
RdmaEndPoint::~RdmaEndPoint() {
    if (!qp_list_.empty()) deconstruct();
}

/**
 * @brief 构造RDMA端点
 * @param cq 完成队列指针
 * @param num_qp_list 队列对数量
 * @param max_sge_per_wr 每个工作请求的最大分散/聚集元素数
 * @param max_wr_depth 最大工作请求深度
 * @param max_inline_bytes 最大内联数据大小
 * @return 成功返回0，失败返回错误码
 *
 * 该函数完成端点的初始化配置：
 * 1. 验证端点状态
 * 2. 分配队列对资源
 * 3. 初始化工作请求深度计数器
 * 4. 创建所需数量的队列对
 */
int RdmaEndPoint::construct(ibv_cq *cq, size_t num_qp_list,
                           size_t max_sge_per_wr, size_t max_wr_depth,
                           size_t max_inline_bytes) {
    // 验证端点状态
    if (status_.load(std::memory_order_relaxed) != INITIALIZING) {
        LOG(ERROR) << "Endpoint has already been constructed";
        return ERR_ENDPOINT;
    }

    // 初始化队列对列表
    qp_list_.resize(num_qp_list);

    // 设置最大工作请求深度
    max_wr_depth_ = (int)max_wr_depth;
    // 为每个队列对分配工作请求深度计数器
    wr_depth_list_ = new volatile int[num_qp_list];
    if (!wr_depth_list_) {
        LOG(ERROR) << "Failed to allocate memory for work request depth list";
        return ERR_MEMORY;
    }

    // 初始化每个队列对的工作请求深度
    for (size_t i = 0; i < num_qp_list; ++i) {
        wr_depth_list_[i] = 0;
        // 创建队列对
        if (createQueuePair(i, cq, max_sge_per_wr, max_wr_depth,
                           max_inline_bytes)) {
            LOG(ERROR) << "Failed to create queue pair " << i;
            return ERR_QP_CREATE;
        }
    }

    // 更新端点状态为未连接
    status_.store(UNCONNECTED, std::memory_order_release);
    return 0;
}

/**
 * @brief 创建单个队列对
 * @param qp_index 队列对索引
 * @param cq 完成队列指针
 * @param max_sge 最大分散/聚集元素数
 * @param max_wr 最大工作请求数
 * @param max_inline 最大内联数据大小
 * @return 成功返回0，失败返回错误码
 *
 * 该函数创建并初始化单个RDMA队列对：
 * 1. 设置队列对属性
 * 2. 创建队列对
 * 3. 将队列对转换到INIT状态
 */
int RdmaEndPoint::createQueuePair(size_t qp_index, ibv_cq *cq,
                                 size_t max_sge, size_t max_wr,
                                 size_t max_inline) {
    struct ibv_qp_init_attr qp_init_attr;
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));

    // 设置发送和接收队列的参数
    qp_init_attr.send_cq = cq;                // 发送完成队列
    qp_init_attr.recv_cq = cq;                // 接收完成队列
    qp_init_attr.srq = nullptr;               // 不使用共享接收队列
    qp_init_attr.cap.max_send_wr = max_wr;    // 最大发送工作请求数
    qp_init_attr.cap.max_recv_wr = max_wr;    // 最大接收工作请求数
    qp_init_attr.cap.max_send_sge = max_sge;  // 最大发送分散/聚集元素数
    qp_init_attr.cap.max_recv_sge = max_sge;  // 最大接收分散/聚集元素数
    qp_init_attr.cap.max_inline_data = max_inline;  // 最大内联数据大小
    qp_init_attr.qp_type = IBV_QPT_RC;        // 使用可靠连接传输服务
    qp_init_attr.sq_sig_all = 0;              // 不为所有发送请求生成完成事件

    // 创建队列对
    qp_list_[qp_index] = ibv_create_qp(context_.getPd(), &qp_init_attr);
    if (!qp_list_[qp_index]) {
        LOG(ERROR) << "Failed to create queue pair";
        return ERR_QP_CREATE;
    }

    // 修改队列对状态为INIT
    return modifyQueuePairState(qp_index, IBV_QPS_INIT);
}

/**
 * @brief 修改队列对状态
 * @param qp_index 队列对索引
 * @param target_state 目标状态
 * @return 成功返回0，失败返回错误码
 *
 * 该函数用于修改队列对的状态：
 * 1. 根据目标状态设置队列对属性
 * 2. 调用ibv_modify_qp修改状态
 * 3. 验证状态转换结果
 */
int RdmaEndPoint::modifyQueuePairState(size_t qp_index,
                                      enum ibv_qp_state target_state) {
    struct ibv_qp_attr attr;
    int attr_mask;
    memset(&attr, 0, sizeof(attr));

    switch (target_state) {
        case IBV_QPS_INIT:
            // 初始化状态配置
            attr.qp_state = IBV_QPS_INIT;
            attr.port_num = context_.getPort();
            attr.pkey_index = 0;
            attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE |  // 本地写权限
                                 IBV_ACCESS_REMOTE_READ |   // 远程读权限
                                 IBV_ACCESS_REMOTE_WRITE;   // 远程写权限
            attr_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                       IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
            break;

        case IBV_QPS_RTR:
            // 准备接收状态配置
            attr.qp_state = IBV_QPS_RTR;
            attr.path_mtu = IBV_MTU_4096;           // 最大传输单元大小
            attr.dest_qp_num = remote_qp_num_;      // 对端队列对编号
            attr.rq_psn = 0;                        // 接收序列号
            attr.max_dest_rd_atomic = 1;            // 最大目标原子操作数
            attr.min_rnr_timer = 12;                // 最小RNR NAK定时器值

            // 设置主路径属性
            attr.ah_attr.is_global = 1;             // 使用全局路由
            attr.ah_attr.grh.hop_limit = MAX_HOP_LIMIT;  // 最大跳数
            attr.ah_attr.grh.dgid = remote_gid_;    // 目标GID
            attr.ah_attr.grh.sgid_index = context_.getGidIndex();  // 源GID索引
            attr.ah_attr.dlid = remote_lid_;        // 目标LID
            attr.ah_attr.sl = 0;                    // 服务级别
            attr.ah_attr.src_path_bits = 0;         // 源路径位
            attr.ah_attr.port_num = context_.getPort();  // 端口号

            attr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                       IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                       IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
            break;

        case IBV_QPS_RTS:
            // 准备发送状态配置
            attr.qp_state = IBV_QPS_RTS;
            attr.timeout = TIMEOUT;                 // 传输超时时间
            attr.retry_cnt = RETRY_CNT;             // 重试次数
            attr.rnr_retry = RETRY_CNT;             // RNR重试次数
            attr.sq_psn = 0;                        // 发送序列号
            attr.max_rd_atomic = 1;                 // 最大原子操作数
            attr_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                       IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                       IBV_QP_MAX_QP_RD_ATOMIC;
            break;

        case IBV_QPS_ERR:
            // 错误状态配置
            attr.qp_state = IBV_QPS_ERR;
            attr_mask = IBV_QP_STATE;
            break;

        default:
            LOG(ERROR) << "Unknown QP state: " << target_state;
            return ERR_INVALID_ARGUMENT;
    }

    // 修改队列对状态
    if (ibv_modify_qp(qp_list_[qp_index], &attr, attr_mask)) {
        LOG(ERROR) << "Failed to modify QP state to " << target_state;
        return ERR_QP_MODIFY;
    }

    return 0;
}

int RdmaEndPoint::deconstruct() {
    for (size_t i = 0; i < qp_list_.size(); ++i) {
        if (wr_depth_list_[i] != 0)
            LOG(WARNING)
                << "Outstanding work requests found, CQ will not be generated";

        if (ibv_destroy_qp(qp_list_[i])) {
            PLOG(ERROR) << "Failed to destroy QP";
            return ERR_ENDPOINT;
        }
    }
    qp_list_.clear();
    delete[] wr_depth_list_;
    return 0;
}

int RdmaEndPoint::destroyQP() { return deconstruct(); }

void RdmaEndPoint::setPeerNicPath(const std::string &peer_nic_path) {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(WARNING) << "Previous connection will be discarded";
        disconnectUnlocked();
    }
    peer_nic_path_ = peer_nic_path;
}

int RdmaEndPoint::setupConnectionsByActive() {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(INFO) << "Connection has been established";
        return 0;
    }
    HandShakeDesc local_desc, peer_desc;
    local_desc.local_nic_path = context_.nicPath();
    local_desc.peer_nic_path = peer_nic_path_;
    local_desc.qp_num = qpNum();

    auto peer_server_name = getServerNameFromNicPath(peer_nic_path_);
    auto peer_nic_name = getNicNameFromNicPath(peer_nic_path_);
    if (peer_server_name.empty() || peer_nic_name.empty()) {
        LOG(ERROR) << "Parse peer nic path failed: " << peer_nic_path_;
        return ERR_INVALID_ARGUMENT;
    }

    int rc = context_.engine().sendHandshake(peer_server_name, local_desc,
                                             peer_desc);
    if (rc) return rc;

    if (peer_desc.local_nic_path != peer_nic_path_ ||
        peer_desc.peer_nic_path != local_desc.local_nic_path) {
        LOG(ERROR) << "Invalid argument: received packet mismatch";
        return ERR_REJECT_HANDSHAKE;
    }

    auto segment_desc =
        context_.engine().meta()->getSegmentDescByName(peer_server_name);
    if (segment_desc) {
        for (auto &nic : segment_desc->devices)
            if (nic.name == peer_nic_name)
                return doSetupConnection(nic.gid, nic.lid, peer_desc.qp_num);
    }
    LOG(ERROR) << "Peer NIC " << peer_nic_name << " not found in "
               << peer_server_name;
    return ERR_DEVICE_NOT_FOUND;
}

int RdmaEndPoint::setupConnectionsByPassive(const HandShakeDesc &peer_desc,
                                            HandShakeDesc &local_desc) {
    RWSpinlock::WriteGuard guard(lock_);
    if (connected()) {
        LOG(WARNING) << "Re-establish connection: " << toString();
        disconnectUnlocked();
    }

    peer_nic_path_ = peer_desc.local_nic_path;
    if (peer_desc.peer_nic_path != context_.nicPath()) {
        local_desc.reply_msg = "Invalid argument: peer nic path inconsistency";
        LOG(ERROR) << local_desc.reply_msg;
        return ERR_REJECT_HANDSHAKE;
    }

    auto peer_server_name = getServerNameFromNicPath(peer_nic_path_);
    auto peer_nic_name = getNicNameFromNicPath(peer_nic_path_);
    if (peer_server_name.empty() || peer_nic_name.empty()) {
        local_desc.reply_msg = "Parse peer nic path failed: " + peer_nic_path_;
        LOG(ERROR) << local_desc.reply_msg;
        return ERR_INVALID_ARGUMENT;
    }

    local_desc.local_nic_path = context_.nicPath();
    local_desc.peer_nic_path = peer_nic_path_;
    local_desc.qp_num = qpNum();

    auto segment_desc =
        context_.engine().meta()->getSegmentDescByName(peer_server_name);
    if (segment_desc) {
        for (auto &nic : segment_desc->devices)
            if (nic.name == peer_nic_name)
                return doSetupConnection(nic.gid, nic.lid, peer_desc.qp_num,
                                         &local_desc.reply_msg);
    }
    local_desc.reply_msg =
        "Peer nic not found in that server: " + peer_nic_path_;
    LOG(ERROR) << local_desc.reply_msg;
    return ERR_DEVICE_NOT_FOUND;
}

void RdmaEndPoint::disconnect() {
    RWSpinlock::WriteGuard guard(lock_);
    disconnectUnlocked();
}

void RdmaEndPoint::disconnectUnlocked() {
    for (size_t i = 0; i < qp_list_.size(); ++i) {
        if (wr_depth_list_[i] != 0)
            LOG(WARNING) << "Outstanding work requests will be dropped";
    }
    ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RESET;
    for (size_t i = 0; i < qp_list_.size(); ++i) {
        int ret = ibv_modify_qp(qp_list_[i], &attr, IBV_QP_STATE);
        if (ret) PLOG(ERROR) << "Failed to modity QP to RESET";
    }
    peer_nic_path_.clear();
    for (size_t i = 0; i < qp_list_.size(); ++i) wr_depth_list_[i] = 0;
    status_.store(UNCONNECTED, std::memory_order_release);
}

const std::string RdmaEndPoint::toString() const {
    auto status = status_.load(std::memory_order_relaxed);
    if (status == CONNECTED)
        return "EndPoint: local " + context_.nicPath() + ", peer " +
               peer_nic_path_;
    else
        return "EndPoint: local " + context_.nicPath() + " (unconnected)";
}

bool RdmaEndPoint::hasOutstandingSlice() const {
    if (active_) return true;
    for (size_t i = 0; i < qp_list_.size(); i++)
        if (wr_depth_list_[i] != 0) return true;
    return false;
}

int RdmaEndPoint::submitPostSend(
    std::vector<Transport::Slice *> &slice_list,
    std::vector<Transport::Slice *> &failed_slice_list) {
    RWSpinlock::WriteGuard guard(lock_);
    int qp_index = SimpleRandom::Get().next(qp_list_.size());
    int wr_count = std::min(max_wr_depth_ - wr_depth_list_[qp_index],
                            (int)slice_list.size());
    if (wr_count == 0) return 0;

    ibv_send_wr wr_list[wr_count], *bad_wr = nullptr;
    ibv_sge sge_list[wr_count];
    memset(wr_list, 0, sizeof(ibv_send_wr) * wr_count);
    for (int i = 0; i < wr_count; ++i) {
        auto slice = slice_list[i];
        auto &sge = sge_list[i];
        sge.addr = (uint64_t)slice->source_addr;
        sge.length = slice->length;
        sge.lkey = slice->rdma.source_lkey;

        auto &wr = wr_list[i];
        wr.wr_id = (uint64_t)slice;
        wr.opcode = slice->opcode == Transport::TransferRequest::READ
                        ? IBV_WR_RDMA_READ
                        : IBV_WR_RDMA_WRITE;
        wr.num_sge = 1;
        wr.sg_list = &sge;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.next = (i + 1 == wr_count) ? nullptr : &wr_list[i + 1];
        wr.imm_data = 0;
        wr.wr.rdma.remote_addr = slice->rdma.dest_addr;
        wr.wr.rdma.rkey = slice->rdma.dest_rkey;
        slice->status = Transport::Slice::POSTED;
        slice->rdma.qp_depth = &wr_depth_list_[qp_index];
        // if (globalConfig().verbose)
        // {
        //     LOG(INFO) << "WR: local addr " << slice->source_addr
        //               << " remote addr " << slice->rdma.dest_addr
        //               << " rkey " << slice->rdma.dest_rkey;
        // }
    }
    __sync_fetch_and_add(&wr_depth_list_[qp_index], wr_count);
    int rc = ibv_post_send(qp_list_[qp_index], wr_list, &bad_wr);
    if (rc) {
        PLOG(ERROR) << "Failed to ibv_post_send";
        while (bad_wr) {
            int i = bad_wr - wr_list;
            failed_slice_list.push_back(slice_list[i]);
            __sync_fetch_and_sub(&wr_depth_list_[qp_index], 1);
            bad_wr = bad_wr->next;
        }
    }
    slice_list.erase(slice_list.begin(), slice_list.begin() + wr_count);
    return 0;
}

std::vector<uint32_t> RdmaEndPoint::qpNum() const {
    std::vector<uint32_t> ret;
    for (int qp_index = 0; qp_index < (int)qp_list_.size(); ++qp_index)
        ret.push_back(qp_list_[qp_index]->qp_num);
    return ret;
}

int RdmaEndPoint::doSetupConnection(const std::string &peer_gid,
                                    uint16_t peer_lid,
                                    std::vector<uint32_t> peer_qp_num_list,
                                    std::string *reply_msg) {
    if (qp_list_.size() != peer_qp_num_list.size()) {
        std::string message =
            "QP count mismatch in peer and local endpoints, check "
            "MC_MAX_EP_PER_CTX";
        LOG(ERROR) << "[Handshake] " << message;
        if (reply_msg) *reply_msg = message;
        return ERR_INVALID_ARGUMENT;
    }

    for (int qp_index = 0; qp_index < (int)qp_list_.size(); ++qp_index) {
        int ret = doSetupConnection(qp_index, peer_gid, peer_lid,
                                    peer_qp_num_list[qp_index], reply_msg);
        if (ret) return ret;
    }

    status_.store(CONNECTED, std::memory_order_relaxed);
    return 0;
}

int RdmaEndPoint::doSetupConnection(int qp_index, const std::string &peer_gid,
                                    uint16_t peer_lid, uint32_t peer_qp_num,
                                    std::string *reply_msg) {
    if (qp_index < 0 || qp_index > (int)qp_list_.size())
        return ERR_INVALID_ARGUMENT;
    auto &qp = qp_list_[qp_index];

    // Any state -> RESET
    ibv_qp_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RESET;
    int ret = ibv_modify_qp(qp, &attr, IBV_QP_STATE);
    if (ret) {
        std::string message = "Failed to modity QP to RESET";
        PLOG(ERROR) << "[Handshake] " << message;
        if (reply_msg) *reply_msg = message + ": " + strerror(errno);
        return ERR_ENDPOINT;
    }

    // RESET -> INIT
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = context_.portNum();
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                           IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
    ret = ibv_modify_qp(
        qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (ret) {
        std::string message =
            "Failed to modity QP to INIT, check local context port num";
        PLOG(ERROR) << "[Handshake] " << message;
        if (reply_msg) *reply_msg = message + ": " + strerror(errno);
        return ERR_ENDPOINT;
    }

    // INIT -> RTR
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = context_.activeMTU();
    if (globalConfig().mtu_length < attr.path_mtu)
        attr.path_mtu = globalConfig().mtu_length;
    ibv_gid peer_gid_raw;
    std::istringstream iss(peer_gid);
    for (int i = 0; i < 16; ++i) {
        int value;
        iss >> std::hex >> value;
        peer_gid_raw.raw[i] = static_cast<uint8_t>(value);
        if (i < 15) iss.ignore(1, ':');
    }
    attr.ah_attr.grh.dgid = peer_gid_raw;
    // TODO gidIndex and portNum must fetch from REMOTE
    attr.ah_attr.grh.sgid_index = context_.gidIndex();
    attr.ah_attr.grh.hop_limit = MAX_HOP_LIMIT;
    attr.ah_attr.dlid = peer_lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.static_rate = 0;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.port_num = context_.portNum();
    attr.dest_qp_num = peer_qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 16;
    attr.min_rnr_timer = 12;  // 12 in previous implementation
    ret = ibv_modify_qp(qp, &attr,
                        IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_MIN_RNR_TIMER |
                            IBV_QP_AV | IBV_QP_MAX_DEST_RD_ATOMIC |
                            IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);
    if (ret) {
        std::string message =
            "Failed to modity QP to RTR, check mtu, gid, peer lid, peer qp num";
        PLOG(ERROR) << "[Handshake] " << message;
        if (reply_msg) *reply_msg = message + ": " + strerror(errno);
        return ERR_ENDPOINT;
    }

    // RTR -> RTS
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = TIMEOUT;
    attr.retry_cnt = RETRY_CNT;
    attr.rnr_retry = 7;  // or 7,RNR error
    attr.sq_psn = 0;
    attr.max_rd_atomic = 16;
    ret = ibv_modify_qp(qp, &attr,
                        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                            IBV_QP_MAX_QP_RD_ATOMIC);
    if (ret) {
        std::string message = "Failed to modity QP to RTS";
        PLOG(ERROR) << "[Handshake] " << message;
        if (reply_msg) *reply_msg = message + ": " + strerror(errno);
        return ERR_ENDPOINT;
    }

    return 0;
}
}  // namespace mooncake