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

#include "transport/nvmeof_transport/nvmeof_transport.h"

#include <bits/stdint-uintn.h>
#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>

#include "common.h"
#include "transfer_engine.h"
#include "transfer_metadata.h"
#include "transport/nvmeof_transport/cufile_context.h"
#include "transport/nvmeof_transport/cufile_desc_pool.h"
#include "transport/transport.h"

namespace mooncake {

/**
 * @brief NVMe-oF传输协议实现类
 *
 * 该类实现了基于NVMe over Fabrics的高性能存储访问：
 * 1. 支持NVIDIA GPUDirect Storage技术
 * 2. 支持NVMe-oF的各种传输类型（RDMA、TCP、FC等）
 * 3. 提供异步IO接口
 */
NVMeoFTransport::NVMeoFTransport() {
    // 初始化CUDA文件系统驱动
    CUFILE_CHECK(cuFileDriverOpen());
    // 创建文件描述符池
    desc_pool_ = std::make_shared<CUFileDescPool>();
}

/**
 * @brief 析构函数，清理资源
 */
NVMeoFTransport::~NVMeoFTransport() {}

/**
 * @brief 将CUFile传输状态转换为标准传输状态
 *
 * @param status CUFile状态码
 * @return 对应的传输状态枚举值
 *
 * 该函数映射CUFile特有的状态码到通用的传输状态：
 * - WAITING -> 等待中
 * - PENDING -> 进行中
 * - COMPLETE -> 已完成
 * - FAILED -> 失败
 * 等等
 */
Transport::TransferStatusEnum from_cufile_transfer_status(
    CUfileStatus_t status) {
    switch (status) {
        case CUFILE_WAITING:
            return Transport::WAITING;
        case CUFILE_PENDING:
            return Transport::PENDING;
        case CUFILE_INVALID:
            return Transport::INVALID;
        case CUFILE_CANCELED:
            return Transport::CANNELED;
        case CUFILE_COMPLETE:
            return Transport::COMPLETED;
        case CUFILE_TIMEOUT:
            return Transport::TIMEOUT;
        case CUFILE_FAILED:
            return Transport::FAILED;
        default:
            return Transport::FAILED;
    }
}

/**
 * @brief 分配批次ID
 *
 * @param batch_size 批次大小
 * @return 批次ID
 *
 * 该函数完成批次的初始化：
 * 1. 创建NVMe-oF特有的批次描述符
 * 2. 从描述符池分配CUFile描述符
 * 3. 预分配状态数组和切片映射
 */
NVMeoFTransport::BatchID NVMeoFTransport::allocateBatchID(size_t batch_size) {
    // 创建NVMe-oF批次描述符
    auto nvmeof_desc = new NVMeoFBatchDesc();
    // 分配基础批次ID
    auto batch_id = Transport::allocateBatchID(batch_size);
    auto &batch_desc = *((BatchDesc *)(batch_id));
    // 分配CUFile描述符
    nvmeof_desc->desc_idx_ = desc_pool_->allocCUfileDesc(batch_size);
    // 预分配状态数组和切片映射
    nvmeof_desc->transfer_status.reserve(batch_size);
    nvmeof_desc->task_to_slices.reserve(batch_size);
    batch_desc.context = nvmeof_desc;
    return batch_id;
}

/**
 * @brief 获取传输状态
 *
 * @param batch_id 批次ID
 * @param task_id 任务ID
 * @param status 状态输出参数
 * @return 成功返回0，失败返回错误码
 *
 * 该函数检查NVMe-oF传输的状态：
 * 1. 获取任务对应的切片范围
 * 2. 检查每个切片的传输状态
 * 3. 汇总并返回整体状态
 */
int NVMeoFTransport::getTransferStatus(BatchID batch_id, size_t task_id,
                                       TransferStatus &status) {
    // 获取批次和任务信息
    auto &batch_desc = *((BatchDesc *)(batch_id));
    auto &task = batch_desc.task_list[task_id];
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    // 初始化传输状态
    TransferStatus transfer_status = {.s = Transport::PENDING,
                                      .transferred_bytes = 0};

    // 获取任务对应的切片范围
    auto [slice_id, slice_num] = nvmeof_desc.task_to_slices[task_id];

    // 检查每个切片的状态
    for (size_t i = slice_id; i < slice_id + slice_num; ++i) {
        auto event =
            desc_pool_->getTransferStatus(nvmeof_desc.desc_idx_, slice_id);
        transfer_status.s = from_cufile_transfer_status(event.status);

        // 如果切片完成，累加传输字节数
        if (transfer_status.s == COMPLETED) {
            transfer_status.transferred_bytes += event.ret;
        } else {
            break;  // 任一切片未完成，整个任务就未完成
        }
    }

    // 如果所有切片都完成，标记任务完成
    if (transfer_status.s == COMPLETED) {
        task.is_finished = true;
    }
    status = transfer_status;
    return 0;
}

/**
 * @brief 提交传输请求
 *
 * @param batch_id 批次ID
 * @param entries 传输请求条目
 * @return 成功返回0，失败返回错误码
 *
 * 该函数处理传输请求的提交：
 * 1. 检查批次和任务的有效性
 * 2. 为每个传输请求分配切片并初始化相关参数
 * 3. 将请求提交给CUFile进行处理
 */
int NVMeoFTransport::submitTransfer(
    BatchID batch_id, const std::vector<TransferRequest> &entries) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));

    // 检查任务数量是否超过批次大小
    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size)
        return -1;

    size_t task_id = batch_desc.task_list.size();
    size_t slice_id = desc_pool_->getSliceNum(nvmeof_desc.desc_idx_);
    batch_desc.task_list.resize(task_id + entries.size());
    std::unordered_map<SegmentID, std::shared_ptr<SegmentDesc>>
        segment_desc_map;
    // segment_desc_map[LOCAL_SEGMENT_ID] =
    // getSegmentDescByID(LOCAL_SEGMENT_ID);
    for (auto &request : entries) {
        TransferTask &task = batch_desc.task_list[task_id];
        auto target_id = request.target_id;

        // 获取目标段描述符
        if (!segment_desc_map.count(target_id)) {
            segment_desc_map[target_id] =
                metadata_->getSegmentDescByID(target_id);
            assert(segment_desc_map[target_id] != nullptr);
        }

        auto &desc = segment_desc_map.at(target_id);
        // LOG(INFO) << "desc " << desc->name << " " << desc->protocol;
        assert(desc->protocol == "nvmeof");
        // TODO: solving iterator invalidation due to vector resize
        // Handle File Offset
        uint32_t buffer_id = 0;
        uint64_t segment_start = request.target_offset;
        uint64_t segment_end = request.target_offset + request.length;
        uint64_t current_offset = 0;
        for (auto &buffer_desc : desc->nvmeof_buffers) {
            bool is_overlap = overlap(
                (void *)segment_start, request.length, (void *)current_offset,
                buffer_desc
                    .length);  // this buffer intersects with user's target
            if (is_overlap) {
                // 1. get_slice_start
                uint64_t slice_start = std::max(segment_start, current_offset);
                // 2. slice_end
                uint64_t slice_end =
                    std::min(segment_end, current_offset + buffer_desc.length);
                // 3. init slice and put into TransferTask
                const char *file_path =
                    buffer_desc.local_path_map[local_server_name_].c_str();
                void *source_addr =
                    (char *)request.source + slice_start - segment_start;
                uint64_t file_offset = slice_start - current_offset;
                uint64_t slice_len = slice_end - slice_start;
                addSliceToTask(source_addr, slice_len, file_offset,
                               request.opcode, task, file_path);
                // 4. get cufile handle
                auto buf_key = std::make_pair(target_id, buffer_id);
                CUfileHandle_t fh;
                {
                    // TODO: upgrade
                    RWSpinlock::WriteGuard guard(context_lock_);
                    if (!segment_to_context_.count(buf_key)) {
                        segment_to_context_[buf_key] =
                            std::make_shared<CuFileContext>(file_path);
                    }
                    fh = segment_to_context_.at(buf_key)->getHandle();
                }
                // 5. add cufile request
                addSliceToCUFileBatch(source_addr, file_offset, slice_len,
                                      nvmeof_desc.desc_idx_, request.opcode,
                                      fh);
            }
            ++buffer_id;
            current_offset += buffer_desc.length;
        }

        nvmeof_desc.transfer_status.push_back(
            TransferStatus{.s = PENDING, .transferred_bytes = 0});
        nvmeof_desc.task_to_slices.push_back({slice_id, task.slices.size()});
        ++task_id;
        slice_id += task.slices.size();
    }

    desc_pool_->submitBatch(nvmeof_desc.desc_idx_);
    // LOG(INFO) << "submit nr " << slice_id << " start " << start_slice_id;
    return 0;
}

/**
 * @brief 释放批次ID
 *
 * @param batch_id 批次ID
 * @return 成功返回0，失败返回错误码
 *
 * 该函数释放批次相关资源：
 * 1. 获取NVMe-oF描述符
 * 2. 释放CUFile描述符
 * 3. 调用基类的释放函数
 */
int NVMeoFTransport::freeBatchID(BatchID batch_id) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    auto &nvmeof_desc = *((NVMeoFBatchDesc *)(batch_desc.context));
    int desc_idx = nvmeof_desc.desc_idx_;
    int rc = Transport::freeBatchID(batch_id);
    if (rc < 0) {
        return -1;
    }
    desc_pool_->freeCUfileDesc(desc_idx);
    return 0;
}

/**
 * @brief 安装传输协议
 *
 * @param local_server_name 本地服务器名称
 * @param meta 传输元数据
 * @param topo 拓扑信息
 * @return 成功返回0，失败返回错误码
 *
 * 该函数完成传输协议的安装：
 * 1. 调用基类的安装函数
 * 2. 初始化本地服务器名称
 * 3. 存储元数据和拓扑信息
 */
int NVMeoFTransport::install(std::string &local_server_name,
                             std::shared_ptr<TransferMetadata> meta,
                             std::shared_ptr<Topology> topo) {
    return Transport::install(local_server_name, meta, topo);
}

/**
 * @brief 注册本地内存
 *
 * @param addr 内存地址
 * @param length 内存长度
 * @param location 位置描述
 * @param remote_accessible 是否可远程访问
 * @param update_metadata 是否更新元数据
 * @return 成功返回0，失败返回错误码
 *
 * 该函数将本地内存区域注册到CUDA文件系统：
 * 1. 设置内存属性
 * 2. 更新元数据（如果需要）
 */
int NVMeoFTransport::registerLocalMemory(void *addr, size_t length,
                                         const std::string &location,
                                         bool remote_accessible,
                                         bool update_metadata) {
    (void)remote_accessible;
    (void)update_metadata;
    CUFILE_CHECK(cuFileBufRegister(addr, length, 0));
    return 0;
}

/**
 * @brief 注销本地内存
 *
 * @param addr 内存地址
 * @param update_metadata 是否更新元数据
 * @return 成功返回0，失败返回错误码
 *
 * 该函数将本地内存区域从CUDA文件系统注销：
 * 1. 清理相关资源
 * 2. 更新元数据（如果需要）
 */
int NVMeoFTransport::unregisterLocalMemory(void *addr, bool update_metadata) {
    (void)update_metadata;
    CUFILE_CHECK(cuFileBufDeregister(addr));
    return 0;
}

/**
 * @brief 将切片添加到任务
 *
 * @param source_addr 源地址
 * @param slice_len 切片长度
 * @param target_start 目标起始位置
 * @param op 操作类型
 * @param task 目标任务
 * @param file_path 文件路径
 *
 * 该函数将数据切片信息添加到传输任务中：
 * 1. 创建新的切片对象
 * 2. 设置切片属性（长度、起始位置、操作类型等）
 * 3. 将切片添加到任务的切片列表
 */
void NVMeoFTransport::addSliceToTask(void *source_addr, uint64_t slice_len,
                                     uint64_t target_start,
                                     TransferRequest::OpCode op,
                                     TransferTask &task,
                                     const char *file_path) {
    Slice *slice = new Slice();
    slice->source_addr = (char *)source_addr;
    slice->length = slice_len;
    slice->opcode = op;
    slice->nvmeof.file_path = file_path;
    slice->nvmeof.start = target_start;
    slice->task = &task;
    slice->status = Slice::PENDING;
    task.total_bytes += slice->length;
    task.slices.push_back(slice);
}

/**
 * @brief 将切片添加到CUFile批次
 *
 * @param source_addr 源地址
 * @param file_offset 文件偏移
 * @param slice_len 切片长度
 * @param desc_id 描述符ID
 * @param op 操作类型
 * @param fh CUfile句柄
 *
 * 该函数将切片的传输参数推送到CUFile描述符中：
 * 1. 设置IO参数（源地址、文件偏移、长度等）
 * 2. 提交参数给描述符池
 */
void NVMeoFTransport::addSliceToCUFileBatch(
    void *source_addr, uint64_t file_offset, uint64_t slice_len,
    uint64_t desc_id, TransferRequest::OpCode op, CUfileHandle_t fh) {
    CUfileIOParams_t params;
    params.mode = CUFILE_BATCH;
    params.opcode =
        op == Transport::TransferRequest::READ ? CUFILE_READ : CUFILE_WRITE;
    params.cookie = (void *)0;
    params.u.batch.devPtr_base = source_addr;
    params.u.batch.devPtr_offset = 0;
    params.u.batch.file_offset = file_offset;
    params.u.batch.size = slice_len;
    params.fh = fh;
    // LOG(INFO) << "params " << "base " << request.source << " offset " <<
    // request.target_offset << " length " << request.length;
    desc_pool_->pushParams(desc_id, params);
}
}  // namespace mooncake