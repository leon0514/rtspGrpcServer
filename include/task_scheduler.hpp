#pragma once
#include "thread_pool.hpp"
#include <memory>
#include <vector>
#include <mutex>
#include <cuda_runtime.h>
#include <spdlog/spdlog.h>

class TaskScheduler
{
public:
    static TaskScheduler &instance()
    {
        static TaskScheduler instance;
        return instance;
    }

    // IO 线程池
    ThreadPool &getIOPool() { return *io_pool_; }

    // 【修改点】获取特定 GPU 的线程池 (懒加载模式)
    ThreadPool &getComputePool(int gpu_id = -1)
    {
        // 如果是 CPU 任务
        if (gpu_id < 0)
        {
            return *cpu_compute_pool_;
        }

        // 防止越界
        if (gpu_id >= static_cast<int>(gpu_compute_pools_.size()))
        {
            spdlog::warn("Request gpu_id {} exceeds detected count {}, fallback to CPU.", gpu_id, gpu_compute_pools_.size());
            return *cpu_compute_pool_;
        }

        // 【核心修改】双重检查锁定 (Double-Checked Locking) 实现懒加载
        if (!gpu_compute_pools_[gpu_id])
        {
            std::lock_guard<std::mutex> lock(init_mutex_);
            if (!gpu_compute_pools_[gpu_id])
            {
                spdlog::info("Lazy initializing ThreadPool for GPU {}...", gpu_id);
                // 只有真的用到了这张卡，才创建线程池！
                gpu_compute_pools_[gpu_id] = std::make_unique<ThreadPool>(threads_per_gpu_);
            }
        }
        return *gpu_compute_pools_[gpu_id];
    }

    // 初始化：不再直接创建 GPU 线程池，只是预留位置
    void init(size_t io_threads, size_t compute_threads_per_gpu, size_t cpu_compute_threads)
    {
        io_pool_ = std::make_unique<ThreadPool>(io_threads);
        cpu_compute_pool_ = std::make_unique<ThreadPool>(cpu_compute_threads);

        threads_per_gpu_ = compute_threads_per_gpu; // 记下来，以后用

        int gpu_count = 0;
        cudaGetDeviceCount(&gpu_count);
        if (gpu_count > 0)
        {
            // 只是把 vector 的大小扩充好，填入 nullptr，不创建实际对象！
            gpu_compute_pools_.resize(gpu_count);
        }

        spdlog::info("TaskScheduler initialized. GPUs detected: {} (Lazy Loading enabled)", gpu_count);
    }

private:
    TaskScheduler() = default;

    std::unique_ptr<ThreadPool> io_pool_;
    std::unique_ptr<ThreadPool> cpu_compute_pool_;

    // 改为 vector 存放 unique_ptr，初始为空
    std::vector<std::unique_ptr<ThreadPool>> gpu_compute_pools_;

    std::mutex init_mutex_; // 用于懒加载的锁
    size_t threads_per_gpu_ = 2;
};