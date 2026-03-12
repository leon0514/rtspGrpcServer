#pragma once

#include <iostream>
#include <atomic>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>

constexpr size_t MAX_SHM_FRAME_SIZE = 3 * 1024 * 1024; // 3MB
constexpr int SHM_SLOT_COUNT = 8;


struct alignas(64) ShmMeta {
    uint64_t actual_size;
    uint64_t width;
    uint64_t height;
    uint64_t timestamp;
};

struct alignas(64) ShmFrameSlot {
    std::atomic<uint64_t> sequence{0}; // 8 bytes  0
    ShmMeta meta;                                  // 32 bytes (4 * 8)
    uint8_t payload[MAX_SHM_FRAME_SIZE];           // 变长数据存放区
};

// 8 + 32 + 3*1024*1024 = 3145728 bytes per slot

struct ShmLayout {
    ShmFrameSlot slots[SHM_SLOT_COUNT];
    alignas(64) std::atomic<uint64_t> head_idx{0};
};

class ZeroCopyChannel {
public:
    ZeroCopyChannel(const std::string& stream_id, int role) 
        : stream_id_(stream_id), role_(role) {
        
        size_t total_size = sizeof(ShmLayout);
        std::string shm_path = "/" + stream_id_;

        if (role_ == 0) {
            shm_fd_ = shm_open(shm_path.c_str(), O_CREAT | O_RDWR, 0666);
            ftruncate(shm_fd_, total_size);
            layout_ = (ShmLayout*)mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        } else {
            shm_fd_ = shm_open(shm_path.c_str(), O_RDONLY, 0666);
            layout_ = (ShmLayout*)mmap(nullptr, total_size, PROT_READ, MAP_SHARED, shm_fd_, 0);
        }

        printf("--- Memory Map Debug ---\n");
        printf("Offset of sequence: %zu\n", offsetof(ShmFrameSlot, sequence));
        printf("Offset of meta: %zu\n", offsetof(ShmFrameSlot, meta));
        printf("Offset of payload: %zu\n", offsetof(ShmFrameSlot, payload));
        printf("Size of ShmFrameSlot: %zu\n", sizeof(ShmFrameSlot));
    }

    ~ZeroCopyChannel() {
        if (layout_) munmap(layout_, sizeof(ShmLayout));
        if (shm_fd_ >= 0) close(shm_fd_);
    }

    // 核心修改：支持动态宽高和大小
    void write_frame(const uint8_t* src_data, uint64_t size, uint64_t w, uint64_t h, uint64_t ts) {
        if (!layout_ || size > MAX_SHM_FRAME_SIZE) return;

        uint64_t count = write_count_++;
        size_t idx = count % SHM_SLOT_COUNT;
        ShmFrameSlot& slot = layout_->slots[idx];

        // 1. 标记开始写入
        slot.sequence.fetch_add(1, std::memory_order_acquire);

        // 2. 写入元数据
        slot.meta.actual_size = size;
        slot.meta.width = w;
        slot.meta.height = h;
        slot.meta.timestamp = ts;

        // 3. 拷贝实际数据
        std::memcpy(slot.payload, src_data, size);

        // 4. 标记写入完成
        slot.sequence.fetch_add(1, std::memory_order_release);

        // 5. 更新索引
        layout_->head_idx.store(count, std::memory_order_release);
    }

    void cleanup() 
    {
        if (layout_) {
            munmap(layout_, sizeof(ShmLayout));
            layout_ = nullptr;
        }
        if (shm_fd_ >= 0) {
            close(shm_fd_);
            shm_fd_ = -1;
        }
        // 关键步骤：从内核中删除该共享内存对象
        shm_unlink(("/" + stream_id_).c_str());
    }

private:
    std::string stream_id_;
    int role_;
    int shm_fd_ = -1;
    ShmLayout* layout_ = nullptr;
    uint64_t write_count_ = 0;
};