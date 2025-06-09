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

#include "transport/tcp_transport/tcp_transport.h"

#include <bits/stdint-uintn.h>
#include <glog/logging.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <memory>

#include "common.h"
#include "transfer_engine.h"
#include "transfer_metadata.h"
#include "transport/transport.h"

namespace mooncake {

// 类型别名，简化代码
using tcpsocket = boost::asio::ip::tcp::socket;

// 默认传输缓冲区大小：64KB
const static size_t kDefaultBufferSize = 65536;

/**
 * @struct SessionHeader
 * @brief TCP会话头部结构
 *
 * 包含了传输必要的元数据：
 * - size: 传输数据大小
 * - addr: 目标地址
 * - opcode: 操作类型（读/写）
 */
struct SessionHeader {
    uint64_t size;      // 数据大小
    uint64_t addr;      // 目标地址
    uint8_t opcode;     // 操作类型
};

/**
 * @class Session
 * @brief TCP会话类，处理单个TCP连接
 *
 * 该类实现了异步TCP传输：
 * 1. 支持读写操作
 * 2. 使用异步IO提高性能
 * 3. 实现了流量控制
 */
struct Session : public std::enable_shared_from_this<Session> {
    /**
     * @brief 构造函数
     * @param socket TCP套接字
     */
    explicit Session(tcpsocket socket) : socket_(std::move(socket)) {}

    tcpsocket socket_;                  // TCP套接字
    SessionHeader header_;              // 会话头部
    uint64_t total_transferred_bytes_;  // 已传输字节数
    char *local_buffer_;               // 本地缓冲区
    std::function<void(TransferStatusEnum)> on_finalize_;  // 完成回调
    std::mutex session_mutex_;         // 会话锁

    /**
     * @brief 发起传输
     * @param buffer 本地缓冲区
     * @param dest_addr 目标地址
     * @param size 数据大小
     * @param opcode 操作类型
     *
     * 该函数启动TCP传输：
     * 1. 设置传输参数
     * 2. 发送会话头部
     * 3. 根据操作类型读写数据
     */
    void initiate(void *buffer, uint64_t dest_addr, size_t size,
                  TransferRequest::OpCode opcode) {
        session_mutex_.lock();
        local_buffer_ = (char *)buffer;
        // 主机序转网络序
        header_.addr = htole64(dest_addr);
        header_.size = htole64(size);
        header_.opcode = (uint8_t)opcode;
        total_transferred_bytes_ = 0;
        writeHeader();
    }

    /**
     * @brief 接受新连接
     *
     * 该函数处理新的TCP连接：
     * 1. 重置传输计数器
     * 2. 开始读取会话头部
     */
    void onAccept() {
        session_mutex_.lock();
        total_transferred_bytes_ = 0;
        readHeader();
    }

   private:
    /**
     * @brief 写入会话头部
     *
     * 该函数异步写入会话头部：
     * 1. 提交异步写请求
     * 2. 根据操作类型继续读或写数据
     * 3. 错误时回调通知失败
     */
    void writeHeader() {
        // LOG(INFO) << "writeHeader";
        auto self(shared_from_this());
        boost::asio::async_write(
            socket_, boost::asio::buffer(&header_, sizeof(SessionHeader)),
            [this, self](const boost::system::error_code &ec, std::size_t len) {
                // 检查写入是否成功
                if (ec || len != sizeof(SessionHeader)) {
                    if (on_finalize_) on_finalize_(TransferStatusEnum::FAILED);
                    session_mutex_.unlock();
                    return;
                }
                // 根据操作类型继续操作
                if (header_.opcode == (uint8_t)TransferRequest::WRITE)
                    writeBody();  // 写操作：继续写数据
                else
                    readBody();   // 读操作：开始读数据
            });
    }

    /**
     * @brief 读取会话头部
     *
     * 该函数异步读取会话头部：
     * 1. 提交异步读请求
     * 2. 解析头部信息
     * 3. 根据操作类型继续读或写数据
     */
    void readHeader() {
        // LOG(INFO) << "readHeader";
        auto self(shared_from_this());
        boost::asio::async_read(
            socket_, boost::asio::buffer(&header_, sizeof(SessionHeader)),
            [this, self](const boost::system::error_code &ec, std::size_t len) {
                // 检查读取是否成功
                if (ec || len != sizeof(SessionHeader)) {
                    if (on_finalize_) on_finalize_(TransferStatusEnum::FAILED);
                    session_mutex_.unlock();
                    return;
                }

                // 网络序转主机序
                local_buffer_ = (char *)(le64toh(header_.addr));
                // 根据操作类型继续操作
                if (header_.opcode == (uint8_t)TransferRequest::WRITE)
                    readBody();    // 写操作：接收数据
                else
                    writeBody();   // 读操作：发送数据
            });
    }

    /**
     * @brief 写入数据体
     *
     * 该函数异步写入数据：
     * 1. 计算当前传输块大小
     * 2. 提交异步写请求
     * 3. 处理完成回调
     */
    void writeBody() {
        // LOG(INFO) << "writeBody";
        auto self(shared_from_this());
        const size_t remaining_bytes = le64toh(header_.size) - total_transferred_bytes_;
        if (!remaining_bytes) {
            // 传输完成
            if (on_finalize_) on_finalize_(TransferStatusEnum::COMPLETED);
            session_mutex_.unlock();
            return;
        }
        // 计算传输块大小
        const size_t transfer_size =
            std::min(remaining_bytes, kDefaultBufferSize);
        // 异步写入数据
        boost::asio::async_write(
            socket_,
            boost::asio::buffer(local_buffer_ + total_transferred_bytes_,
                               transfer_size),
            [this, self](const boost::system::error_code &ec,
                         std::size_t len) {
                // 检查写入是否成功
                if (ec) {
                    if (on_finalize_) on_finalize_(TransferStatusEnum::FAILED);
                    session_mutex_.unlock();
                    return;
                }
                // 更新传输计数
                total_transferred_bytes_ += len;
                writeBody();  // 继续写入剩余数据
            });
    }

    /**
     * @brief 读取数据体
     *
     * 该函数异步读取数据：
     * 1. 计算当前传输块大小
     * 2. 提交异步读请求
     * 3. 处理完成回调
     */
    void readBody() {
        // LOG(INFO) << "readBody";
        auto self(shared_from_this());
        const size_t remaining_bytes = le64toh(header_.size) - total_transferred_bytes_;
        if (!remaining_bytes) {
            // 传输完成
            if (on_finalize_) on_finalize_(TransferStatusEnum::COMPLETED);
            session_mutex_.unlock();
            return;
        }
        // 计算传输块大小
        const size_t transfer_size =
            std::min(remaining_bytes, kDefaultBufferSize);
        // 异步读取数据
        boost::asio::async_read(
            socket_,
            boost::asio::buffer(local_buffer_ + total_transferred_bytes_,
                               transfer_size),
            [this, self](const boost::system::error_code &ec,
                         std::size_t len) {
                // 检查读取是否成功
                if (ec) {
                    if (on_finalize_) on_finalize_(TransferStatusEnum::FAILED);
                    session_mutex_.unlock();
                    return;
                }
                // 更新传输计数
                total_transferred_bytes_ += len;
                readBody();  // 继续读取剩余数据
            });
    }
};

struct TcpContext {
    TcpContext(short port)
        : acceptor(io_context, boost::asio::ip::tcp::endpoint(
                                   boost::asio::ip::tcp::v4(), port)) {}

    void doAccept() {
        acceptor.async_accept(
            [this](boost::system::error_code ec, tcpsocket socket) {
                if (!ec)
                    std::make_shared<Session>(std::move(socket))->onAccept();
                doAccept();
            });
    }

    boost::asio::io_context io_context;
    boost::asio::ip::tcp::acceptor acceptor;
};

TcpTransport::TcpTransport() : context_(nullptr), running_(false) {
    // TODO
}

TcpTransport::~TcpTransport() {
    if (running_) {
        running_ = false;
        context_->io_context.stop();
        thread_.join();
    }

    if (context_) {
        delete context_;
        context_ = nullptr;
    }

    metadata_->removeSegmentDesc(local_server_name_);
}

int TcpTransport::install(std::string &local_server_name,
                          std::shared_ptr<TransferMetadata> meta,
                          std::shared_ptr<Topology> topo) {
    metadata_ = meta;
    local_server_name_ = local_server_name;

    int ret = allocateLocalSegmentID();
    if (ret) {
        LOG(ERROR) << "TcpTransport: cannot allocate local segment";
        return -1;
    }

    ret = metadata_->updateLocalSegmentDesc();
    if (ret) {
        LOG(ERROR) << "TcpTransport: cannot publish segments, "
                      "check the availability of metadata storage";
        return -1;
    }

    context_ = new TcpContext(meta->localRpcMeta().rpc_port);
    running_ = true;
    thread_ = std::thread(&TcpTransport::worker, this);
    return 0;
}

int TcpTransport::allocateLocalSegmentID() {
    auto desc = std::make_shared<SegmentDesc>();
    if (!desc) return ERR_MEMORY;
    desc->name = local_server_name_;
    desc->protocol = "tcp";
    metadata_->addLocalSegment(LOCAL_SEGMENT_ID, local_server_name_,
                               std::move(desc));
    return 0;
}

int TcpTransport::registerLocalMemory(void *addr, size_t length,
                                      const std::string &location,
                                      bool remote_accessible,
                                      bool update_metadata) {
    (void)remote_accessible;
    BufferDesc buffer_desc;
    buffer_desc.name = local_server_name_;
    buffer_desc.addr = (uint64_t)addr;
    buffer_desc.length = length;
    return metadata_->addLocalMemoryBuffer(buffer_desc, update_metadata);
}

int TcpTransport::unregisterLocalMemory(void *addr, bool update_metadata) {
    return metadata_->removeLocalMemoryBuffer(addr, update_metadata);
}

int TcpTransport::registerLocalMemoryBatch(
    const std::vector<Transport::BufferEntry> &buffer_list,
    const std::string &location) {
    for (auto &buffer : buffer_list)
        registerLocalMemory(buffer.addr, buffer.length, location, true, false);
    return metadata_->updateLocalSegmentDesc();
}

int TcpTransport::unregisterLocalMemoryBatch(
    const std::vector<void *> &addr_list) {
    for (auto &addr : addr_list) unregisterLocalMemory(addr, false);
    return metadata_->updateLocalSegmentDesc();
}

int TcpTransport::getTransferStatus(BatchID batch_id, size_t task_id,
                                    TransferStatus &status) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    const size_t task_count = batch_desc.task_list.size();
    if (task_id >= task_count) return ERR_INVALID_ARGUMENT;
    auto &task = batch_desc.task_list[task_id];
    status.transferred_bytes = task.transferred_bytes;
    uint64_t success_slice_count = task.success_slice_count;
    uint64_t failed_slice_count = task.failed_slice_count;
    if (success_slice_count + failed_slice_count ==
        (uint64_t)task.slices.size()) {
        if (failed_slice_count) {
            status.s = TransferStatusEnum::FAILED;
        } else {
            status.s = TransferStatusEnum::COMPLETED;
        }
        task.is_finished = true;
    } else {
        status.s = TransferStatusEnum::WAITING;
    }
    return 0;
}

int TcpTransport::submitTransfer(BatchID batch_id,
                                 const std::vector<TransferRequest> &entries) {
    auto &batch_desc = *((BatchDesc *)(batch_id));
    if (batch_desc.task_list.size() + entries.size() > batch_desc.batch_size) {
        LOG(ERROR) << "TcpTransport: Exceed the limitation of current batch's "
                      "capacity";
        return ERR_TOO_MANY_REQUESTS;
    }

    size_t task_id = batch_desc.task_list.size();
    batch_desc.task_list.resize(task_id + entries.size());

    for (auto &request : entries) {
        TransferTask &task = batch_desc.task_list[task_id];
        ++task_id;
        task.total_bytes = request.length;
        auto slice = new Slice();
        slice->source_addr = (char *)request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->tcp.dest_addr = request.target_offset;
        slice->task = &task;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        task.slices.push_back(slice);
        startTransfer(slice);
    }

    return 0;
}

int TcpTransport::submitTransferTask(
    const std::vector<TransferRequest *> &request_list,
    const std::vector<TransferTask *> &task_list) {
    for (size_t index = 0; index < request_list.size(); ++index) {
        auto &request = *request_list[index];
        auto &task = *task_list[index];
        task.total_bytes = request.length;
        auto slice = new Slice();
        slice->source_addr = (char *)request.source;
        slice->length = request.length;
        slice->opcode = request.opcode;
        slice->tcp.dest_addr = request.target_offset;
        slice->task = &task;
        slice->target_id = request.target_id;
        slice->status = Slice::PENDING;
        task.slices.push_back(slice);
        startTransfer(slice);
    }
    return 0;
}

void TcpTransport::worker() {
    while (running_) {
        try {
            context_->doAccept();
            context_->io_context.run();
        } catch (std::exception &e) {
            LOG(ERROR) << "TcpTransport: exception: " << e.what();
        }
    }
}

void TcpTransport::startTransfer(Slice *slice) {
    try {
        boost::asio::ip::tcp::resolver resolver(context_->io_context);
        boost::asio::ip::tcp::socket socket(context_->io_context);
        auto desc = metadata_->getSegmentDescByID(slice->target_id);
        if (!desc) {
            slice->markFailed();
            return;
        }

        TransferMetadata::RpcMetaDesc meta_entry;
        if (metadata_->getRpcMetaEntry(desc->name, meta_entry)) {
            slice->markFailed();
            return;
        }

        auto endpoint_iterator = resolver.resolve(
            boost::asio::ip::tcp::v4(), meta_entry.ip_or_host_name,
            std::to_string(meta_entry.rpc_port));
        boost::asio::connect(socket, endpoint_iterator);
        auto session = std::make_shared<Session>(std::move(socket));
        session->on_finalize_ = [slice](TransferStatusEnum status) {
            if (status == TransferStatusEnum::COMPLETED)
                slice->markSuccess();
            else
                slice->markFailed();
        };
        session->initiate(slice->source_addr, slice->tcp.dest_addr,
                          slice->length, slice->opcode);
    } catch (std::exception &e) {
        LOG(ERROR) << "TcpTransport: ASIO exception: " << e.what();
        slice->markFailed();
    }
}
}  // namespace mooncake