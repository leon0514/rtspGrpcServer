#pragma once

#include <iostream>
#include <atomic>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <sys/stat.h>
#include <cstring>
#include <semaphore.h>
#include <spdlog/spdlog.h>

constexpr size_t MAX_SHM_FRAME_SIZE = 6  * 2560 * 1440; // 3MB
constexpr int SHM_SLOT_COUNT = 8;

struct alignas(64) ShmMeta
{
    uint64_t actual_size;    // 实际数据字节数
    uint64_t width;          // 图像宽度
    uint64_t height;         // 图像高度
    uint64_t timestamp;      // 时间戳 (ms)
    
    // 图像格式描述 ===
    uint32_t channels;       // 通道数: 1=GRAY, 3=BGR, 4=BGRA
    uint32_t depth;          // 位深: CV_8U=0, CV_16U=2, CV_32F=5 等
    uint32_t step;           // 行字节数 (含 padding)，用于非连续内存 [[11]]
    uint32_t reserved;       // 对齐填充
};

struct alignas(64) ShmFrameSlot
{
    std::atomic<uint64_t> sequence{0};   // 8 bytes  0
    ShmMeta meta;                        // 32 bytes (4 * 8)
    uint8_t payload[MAX_SHM_FRAME_SIZE]; // 变长数据存放区
};

// 8 + 32 + 3*1024*1024 = 3145728 bytes per slot

struct ShmLayout
{
    ShmFrameSlot slots[SHM_SLOT_COUNT];
    alignas(64) std::atomic<uint64_t> head_idx{0};
};

class ZeroCopyChannel
{
public:
    ZeroCopyChannel(const std::string &stream_id, int role)
        : stream_id_(stream_id), role_(role)
    {

        size_t total_size = sizeof(ShmLayout);
        std::string shm_path = "/" + stream_id_;

        if (role_ == 0)
        {
            shm_fd_ = shm_open(shm_path.c_str(), O_CREAT | O_RDWR, 0666);
            ftruncate(shm_fd_, total_size);
            layout_ = (ShmLayout *)mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0);
        }
        else
        {
            shm_fd_ = shm_open(shm_path.c_str(), O_RDONLY, 0666);
            layout_ = (ShmLayout *)mmap(nullptr, total_size, PROT_READ, MAP_SHARED, shm_fd_, 0);
        }

        // 创建/打开跨进程通知信号量（只有生产者需要创建）
        std::string sem_name = "/" + stream_id_ + "_notify";
        if (role_ == 0)
        {
            // 临时清除 umask，确保信号量文件权限真正为 0666（跨用户/容器访问）
            auto old_umask = umask(0);
            notify_sem_ = sem_open(sem_name.c_str(), O_CREAT | O_RDWR, 0666, 0);
            umask(old_umask);
        }
        else
        {
            notify_sem_ = sem_open(sem_name.c_str(), 0);
        }
        if (notify_sem_ == SEM_FAILED)
        {
            spdlog::warn("[ZeroCopyChannel] Notify semaphore unavailable for {} (errno={}), SHM will work without cross-process notify", stream_id_, errno);
            notify_sem_ = nullptr; // 信号量可选，不影响核心功能
        }
        else
        {
            spdlog::info("[ZeroCopyChannel] Notify semaphore ready: {} (role={})", sem_name, role_ == 0 ? "producer" : "consumer");
        }

        // printf("--- Memory Map Debug ---\n");
        // printf("Offset of sequence: %zu\n", offsetof(ShmFrameSlot, sequence));
        // printf("Offset of meta: %zu\n", offsetof(ShmFrameSlot, meta));
        // printf("Offset of payload: %zu\n", offsetof(ShmFrameSlot, payload));
        // printf("Size of ShmFrameSlot: %zu\n", sizeof(ShmFrameSlot));
    }

    ~ZeroCopyChannel()
    {
        if (layout_)
            munmap(layout_, sizeof(ShmLayout));
        if (shm_fd_ >= 0)
            close(shm_fd_);
        if (notify_sem_)
        {
            sem_close(notify_sem_);
            if (role_ == 0)
            {
                sem_unlink(("/" + stream_id_ + "_notify").c_str());
                spdlog::info("[ZeroCopyChannel] Notify semaphore cleaned up: /{}_notify", stream_id_);
            }
            notify_sem_ = nullptr;
        }
    }

    bool write_frame_mat(const cv::Mat& frame, uint64_t timestamp)
    {
        if (!layout_ || frame.empty())
        {
            return false;
        }
            
        
        // 1. 确保数据连续（关键！）[[20]][[24]]
        cv::Mat continuous_frame = frame;
        if (!frame.isContinuous())
        {
            continuous_frame = frame.clone(); // 非连续时深拷贝一份
        }
        
        // 2. 计算数据大小
        const size_t data_size = continuous_frame.total() * continuous_frame.elemSize();
        if (data_size > MAX_SHM_FRAME_SIZE)
        {
            printf("Frame size %zu exceeds maximum allowed %zu\n", data_size, MAX_SHM_FRAME_SIZE);
            return false;
        }
        
        // 3. 获取槽位
        uint64_t count = write_count_++;
        size_t idx = count % SHM_SLOT_COUNT;
        ShmFrameSlot &slot = layout_->slots[idx];
        
        // 4. 标记开始写入 (sequence 奇数=写入中)
        slot.sequence.fetch_add(1, std::memory_order_acquire);
        
        // 5. 写入元数据
        slot.meta.actual_size = data_size;
        slot.meta.width = frame.cols;
        slot.meta.height = frame.rows;
        slot.meta.timestamp = timestamp;
        slot.meta.channels = frame.channels();
        slot.meta.depth = frame.depth();      // OpenCV 内部 depth 枚举
        slot.meta.step = static_cast<uint32_t>(continuous_frame.step[0]); // 行字节数 [[11]]
        
        // 6. 零拷贝式内存传输（仅一次 memcpy）
        std::memcpy(slot.payload, continuous_frame.data, data_size);
        
        // 7. 标记写入完成 (sequence 偶数=就绪)
        slot.sequence.fetch_add(1, std::memory_order_release);
        
        // 8. 更新全局索引
        layout_->head_idx.store(count, std::memory_order_release);

        // 9. 通知等待的客户端有新帧
        if (notify_sem_)
        {
            if (sem_post(notify_sem_) != 0)
            {
                spdlog::debug("[ZeroCopyChannel] sem_post failed for {} (errno={})", stream_id_, errno);
            }
        }
        
        return true;
    }

    // 核心修改：支持动态宽高和大小
    void write_frame(const uint8_t *src_data, uint64_t size, uint64_t w, uint64_t h, uint64_t ts)
    {
        if (!layout_ || size > MAX_SHM_FRAME_SIZE)
            return;

        uint64_t count = write_count_++;
        size_t idx = count % SHM_SLOT_COUNT;
        ShmFrameSlot &slot = layout_->slots[idx];

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

        // 6. 通知等待的客户端有新帧
        if (notify_sem_)
        {
            if (sem_post(notify_sem_) != 0)
            {
                spdlog::debug("[ZeroCopyChannel] sem_post failed for {} (errno={})", stream_id_, errno);
            }
        }
    }

    void cleanup()
    {
        if (layout_)
        {
            munmap(layout_, sizeof(ShmLayout));
            layout_ = nullptr;
        }
        if (shm_fd_ >= 0)
        {
            close(shm_fd_);
            shm_fd_ = -1;
        }
        // 关键步骤：从内核中删除该共享内存对象
        shm_unlink(("/" + stream_id_).c_str());
        // 删除通知信号量
        if (notify_sem_)
        {
            sem_close(notify_sem_);
            sem_unlink(("/" + stream_id_ + "_notify").c_str());
            spdlog::info("[ZeroCopyChannel] Notify semaphore cleaned up: /{}_notify", stream_id_);
            notify_sem_ = nullptr;
        }
    }

private:
    std::string stream_id_;
    int role_;
    int shm_fd_ = -1;
    ShmLayout *layout_ = nullptr;
    uint64_t write_count_ = 0;
    sem_t *notify_sem_ = nullptr;
};