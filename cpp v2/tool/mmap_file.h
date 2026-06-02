#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <stdexcept>

// =====================================================================
//  MmapFile — 跨平台内存映射文件封装
//
//  Windows: CreateFileMapping + MapViewOfFile
//  Linux:   mmap + MAP_PRIVATE
//
//  用途：消除 File I/O 瓶颈，预期提升 15% 性能
// =====================================================================

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

class MmapFile {
public:
    // 构造函数：打开并映射文件
    explicit MmapFile(const char* filename) {
#ifdef _WIN32
        // Windows 实现
        hFile_ = CreateFileA(
            filename,
            GENERIC_READ,
            FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open file: " + std::string(filename));
        }

        // 获取文件大小
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile_, &fileSize)) {
            CloseHandle(hFile_);
            throw std::runtime_error("Failed to get file size: " + std::string(filename));
        }
        size_ = static_cast<size_t>(fileSize.QuadPart);

        // 创建文件映射
        hMapping_ = CreateFileMappingA(
            hFile_,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr
        );

        if (hMapping_ == nullptr) {
            CloseHandle(hFile_);
            throw std::runtime_error("Failed to create file mapping: " + std::string(filename));
        }

        // 映射视图
        data_ = static_cast<const char*>(MapViewOfFile(
            hMapping_,
            FILE_MAP_READ,
            0,
            0,
            0
        ));

        if (data_ == nullptr) {
            CloseHandle(hMapping_);
            CloseHandle(hFile_);
            throw std::runtime_error("Failed to map view of file: " + std::string(filename));
        }

#else
        // Linux/Unix 实现
        fd_ = open(filename, O_RDONLY);
        if (fd_ == -1) {
            throw std::runtime_error("Failed to open file: " + std::string(filename));
        }

        // 获取文件大小
        struct stat st;
        if (fstat(fd_, &st) == -1) {
            close(fd_);
            throw std::runtime_error("Failed to get file size: " + std::string(filename));
        }
        size_ = static_cast<size_t>(st.st_size);

        // 内存映射
        data_ = static_cast<const char*>(mmap(
            nullptr,
            size_,
            PROT_READ,
            MAP_PRIVATE,
            fd_,
            0
        ));

        if (data_ == MAP_FAILED) {
            close(fd_);
            throw std::runtime_error("Failed to mmap file: " + std::string(filename));
        }

        // 建议内核顺序读取
        madvise(const_cast<char*>(data_), size_, MADV_SEQUENTIAL);
#endif
    }

    // 析构函数：释放资源
    ~MmapFile() {
#ifdef _WIN32
        if (data_ != nullptr) {
            UnmapViewOfFile(data_);
        }
        if (hMapping_ != nullptr) {
            CloseHandle(hMapping_);
        }
        if (hFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile_);
        }
#else
        if (data_ != nullptr && data_ != MAP_FAILED) {
            munmap(const_cast<char*>(data_), size_);
        }
        if (fd_ != -1) {
            close(fd_);
        }
#endif
    }

    // 禁用拷贝
    MmapFile(const MmapFile&) = delete;
    MmapFile& operator=(const MmapFile&) = delete;

    // 移动构造
    MmapFile(MmapFile&& other) noexcept
        : data_(other.data_), size_(other.size_)
#ifdef _WIN32
        , hFile_(other.hFile_), hMapping_(other.hMapping_)
#else
        , fd_(other.fd_)
#endif
    {
        other.data_ = nullptr;
        other.size_ = 0;
#ifdef _WIN32
        other.hFile_ = INVALID_HANDLE_VALUE;
        other.hMapping_ = nullptr;
#else
        other.fd_ = -1;
#endif
    }

    // 获取数据指针
    const char* data() const { return data_; }

    // 获取文件大小
    size_t size() const { return size_; }

    // 检查是否有效
    bool isValid() const {
#ifdef _WIN32
        return data_ != nullptr && size_ > 0;
#else
        return data_ != nullptr && data_ != MAP_FAILED && size_ > 0;
#endif
    }

private:
    const char* data_ = nullptr;
    size_t size_ = 0;

#ifdef _WIN32
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMapping_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// =====================================================================
//  MmapFileReader — 基于 mmap 的高性能文件读取器
//
//  替代 AxsbeFileReader，消除逐行读取的 I/O 开销
//  复用 parseKeyValueLine 解析逻辑
// =====================================================================

#include "axsbe_base.h"
#include "axsbe_order.h"
#include "axsbe_exe.h"
#include "axsbe_snap_stock.h"
#include "field_parser.h"
#include <unordered_map>
#include <string>

class MmapFileReader {
public:
    MmapFileReader(const std::string& filename)
        : mmapFile_(filename.c_str()), ptr_(mmapFile_.data()), end_(ptr_ + mmapFile_.size()) {
        // 预读取第一行
        advance();
    }

    bool hasNext() const { return hasNext_; }

    int next(AxsbeOrder& order, AxsbeExe& exe, AxsbeSnapStock& snap) {
        if (!hasNext_) return -1;

        // 返回当前消息类型和数据
        order = currentOrder_;
        exe = currentExe_;
        snap = currentSnap_;

        int type = currentType_;

        // 预读取下一行
        advance();

        return type;
    }

    // 关闭（释放资源）
    void close() {
        // mmap 资源在析构时自动释放
    }

private:
    MmapFile mmapFile_;
    const char* ptr_;
    const char* end_;
    bool hasNext_ = false;
    int currentType_ = -1;
    AxsbeOrder currentOrder_;
    AxsbeExe currentExe_;
    AxsbeSnapStock currentSnap_;

    // 解析下一行
    // [v2.6] 零分配优化：直接使用 mmap 指针对 (lineStart, lineEnd)
    // 消除每条消息的 std::string 分配（~200-500ns/消息）
    void advance() {
        hasNext_ = false;

        while (ptr_ < end_) {
            // 跳过空行和回车符
            while (ptr_ < end_ && (*ptr_ == '\n' || *ptr_ == '\r')) {
                ++ptr_;
            }

            if (ptr_ >= end_) {
                return;
            }

            // 找到行尾
            const char* lineStart = ptr_;
            while (ptr_ < end_ && *ptr_ != '\n' && *ptr_ != '\r') {
                ++ptr_;
            }
            const char* lineEnd = ptr_;

            // 跳过非键值对行
            size_t lineLen = static_cast<size_t>(lineEnd - lineStart);
            if (lineLen < 2 || lineStart[0] != '/' || lineStart[1] != '/') {
                continue;
            }

            // [v2.6] 直接使用指针对解析，无需创建 std::string
            int64_t msgType = 0;
            if (!extractField(lineStart, lineEnd, "MsgType", msgType)) {
                continue;
            }

            currentType_ = static_cast<int>(msgType);

            if (msgType == MsgType_order) {
                currentOrder_ = AxsbeOrder{};
                currentOrder_.loadFromLine(lineStart, lineEnd);
                hasNext_ = true;
                return;
            } else if (msgType == MsgType_exe) {
                currentExe_ = AxsbeExe{};
                currentExe_.loadFromLine(lineStart, lineEnd);
                hasNext_ = true;
                return;
            } else if (msgType == MsgType_snap) {
                currentSnap_ = AxsbeSnapStock{};
                currentSnap_.loadFromLine(lineStart, lineEnd);
                hasNext_ = true;
                return;
            }
            // 其他 MsgType 跳过
        }
    }
};