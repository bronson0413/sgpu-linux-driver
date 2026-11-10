// SPDX-License-Identifier: GPL-2.0
// C++20 user-space runtime for /dev/sgpu. Device, Buffer and Fence are RAII
// types: a Buffer or Fence keeps its GPU context open for as long as it lives.
#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "sgpu_uapi.h"

namespace sgpu {

[[noreturn]] inline void throw_errno(const std::string &what) {
    throw std::system_error(errno, std::generic_category(), what);
}

class FileHandle {
public:
    explicit FileHandle(const char *path) : fd_(::open(path, O_RDWR | O_CLOEXEC)) {
        if (fd_ < 0) throw_errno(std::string("open ") + path);
    }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;
    ~FileHandle() { ::close(fd_); }
    int get() const noexcept { return fd_; }

private:
    int fd_;
};

using SharedFile = std::shared_ptr<FileHandle>;

class Buffer {
public:
    Buffer(SharedFile file, std::uint32_t handle, std::uint64_t va, std::size_t size) noexcept
        : file_(std::move(file)), handle_(handle), va_(va), size_(size) {}
    Buffer(Buffer &&o) noexcept
        : file_(std::move(o.file_)), handle_(std::exchange(o.handle_, 0)), va_(o.va_), size_(o.size_) {}
    Buffer &operator=(Buffer &&o) noexcept {
        if (this != &o) {
            release();
            file_ = std::move(o.file_);
            handle_ = std::exchange(o.handle_, 0);
            va_ = o.va_;
            size_ = o.size_;
        }
        return *this;
    }
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() { release(); }

    std::uint64_t va(std::uint64_t offset = 0) const noexcept { return va_ + offset; }
    std::size_t size() const noexcept { return size_; }
    std::uint32_t handle() const noexcept { return handle_; }

    void write(std::uint64_t offset, std::span<const std::uint8_t> data) {
        transfer(SGPU_IOC_BO_WRITE, offset, data.size(), data.data(), "BO_WRITE");
    }
    void read(std::uint64_t offset, std::span<std::uint8_t> data) const {
        transfer(SGPU_IOC_BO_READ, offset, data.size(), data.data(), "BO_READ");
    }

    // Drop the handle now. In-flight jobs keep the memory alive in the kernel.
    void release() noexcept {
        if (handle_) {
            sgpu_bo_destroy args{};
            args.handle = std::exchange(handle_, 0);
            ::ioctl(file_->get(), SGPU_IOC_BO_DESTROY, &args);
        }
    }

private:
    void transfer(unsigned long cmd, std::uint64_t offset, std::size_t len, const void *p,
                  const char *what) const {
        sgpu_bo_xfer args{};
        args.handle = handle_;
        args.offset = offset;
        args.len = len;
        args.user_ptr = reinterpret_cast<std::uintptr_t>(p);
        if (::ioctl(file_->get(), cmd, &args) != 0) throw_errno(what);
    }

    SharedFile file_;
    std::uint32_t handle_;
    std::uint64_t va_;
    std::size_t size_;
};

class CommandList {
public:
    CommandList &nop() { return add(SGPU_OP_NOP, 0, 0, 0); }
    CommandList &fill(const Buffer &dst, std::uint64_t offset, std::uint32_t len, std::uint8_t value) {
        return add(SGPU_OP_FILL, value, dst.va(offset), len);
    }
    CommandList &copy(const Buffer &src, std::uint64_t src_off, const Buffer &dst, std::uint64_t dst_off,
                      std::uint32_t len) {
        return add(SGPU_OP_COPY, src.va(src_off), dst.va(dst_off), len);
    }
    CommandList &raw(std::uint32_t opcode, std::uint64_t src, std::uint64_t dst, std::uint32_t len) {
        return add(opcode, src, dst, len);
    }
    std::span<const sgpu_cmd> commands() const noexcept { return cmds_; }

private:
    CommandList &add(std::uint32_t opcode, std::uint64_t src, std::uint64_t dst, std::uint32_t len) {
        sgpu_cmd c{};
        c.opcode = opcode;
        c.src = src;
        c.dst = dst;
        c.len = len;
        cmds_.push_back(c);
        return *this;
    }
    std::vector<sgpu_cmd> cmds_;
};

class Fence {
public:
    Fence(SharedFile file, std::uint64_t value) noexcept : file_(std::move(file)), value_(value) {}
    std::uint64_t value() const noexcept { return value_; }

    // Returns the job status: 0, or the negative errno of a failed job.
    int wait(std::uint32_t timeout_ms = 30000) const {
        sgpu_wait args{};
        args.fence = value_;
        args.timeout_ms = timeout_ms;
        if (::ioctl(file_->get(), SGPU_IOC_WAIT, &args) != 0) throw_errno("WAIT");
        return args.status;
    }

private:
    SharedFile file_;
    std::uint64_t value_;
};

class Device {
public:
    explicit Device(const char *path = "/dev/sgpu") : file_(std::make_shared<FileHandle>(path)) {}

    int fd() const noexcept { return file_->get(); }

    sgpu_info info() const {
        sgpu_info out{};
        if (::ioctl(fd(), SGPU_IOC_INFO, &out) != 0) throw_errno("INFO");
        return out;
    }

    Buffer alloc(std::size_t size) const {
        sgpu_bo_create args{};
        args.size = size;
        if (::ioctl(fd(), SGPU_IOC_BO_CREATE, &args) != 0) throw_errno("BO_CREATE");
        return Buffer(file_, args.handle, args.gpu_va, size);
    }

    Fence submit(const CommandList &list) const {
        auto cmds = list.commands();
        sgpu_submit args{};
        args.cmds_ptr = reinterpret_cast<std::uintptr_t>(cmds.data());
        args.count = static_cast<std::uint32_t>(cmds.size());
        if (::ioctl(fd(), SGPU_IOC_SUBMIT, &args) != 0) throw_errno("SUBMIT");
        return Fence(file_, args.fence);
    }

private:
    SharedFile file_;
};

}  // namespace sgpu
