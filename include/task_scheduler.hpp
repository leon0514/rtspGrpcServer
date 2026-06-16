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

    // 获取特定 GPU 的线程池
    ThreadPool &getComputePool(int gpu_id = -1)
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // 如果是 CPU 任务
        if (gpu_id < 0)
        {
            if (!cpu_compute_pool_)
            {
                throw std::runtime_error("TaskScheduler::getComputePool: CPU compute pool is not initialized");
            }
            return *cpu_compute_pool_;
        }

        // 防止越界：如果系统没有检测到 GPU（或 GPU 数量为空），直接回退到 CPU 池
        if (gpu_id >= static_cast<int>(gpu_compute_pools_.size()))
        {
            spdlog::warn("Request gpu_id {} exceeds detected count {} (or no GPU available), fallback to CPU.", gpu_id, gpu_compute_pools_.size());
            if (!cpu_compute_pool_)
            {
                throw std::runtime_error("TaskScheduler::getComputePool: CPU compute pool is not initialized");
            }
            return *cpu_compute_pool_;
        }

        return *gpu_compute_pools_[gpu_id];
    }

    // 初始化：创建 CPU 计算线程池，并为每个检测到的 GPU 创建对应的计算线程池
    void init(size_t compute_threads_per_gpu, size_t cpu_compute_threads)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_)
        {
            spdlog::warn("TaskScheduler::init called more than once, ignoring.");
            return;
        }

        cpu_compute_pool_ = std::make_unique<ThreadPool>(cpu_compute_threads);

        int gpu_count = 0;
        cudaGetDeviceCount(&gpu_count);
        if (gpu_count > 0)
        {
            gpu_compute_pools_.reserve(gpu_count);
            for (int i = 0; i < gpu_count; ++i)
            {
                gpu_compute_pools_.emplace_back(std::make_unique<ThreadPool>(compute_threads_per_gpu));
            }
        }

        initialized_ = true;
        spdlog::info("TaskScheduler initialized. GPUs detected: {}", gpu_count);
    }

    // 程序退出前调用：显式销毁所有线程池，确保 thread_local CUDA 资源
    // 在 CUDA runtime 静态卸载之前被释放，避免崩溃
    void shutdown()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        gpu_compute_pools_.clear();
        cpu_compute_pool_.reset();
        initialized_ = false;
        spdlog::info("TaskScheduler shutdown complete.");
    }

private:
    TaskScheduler() = default;

    std::unique_ptr<ThreadPool> cpu_compute_pool_;
    std::vector<std::unique_ptr<ThreadPool>> gpu_compute_pools_;

    std::mutex mutex_;
    bool initialized_ = false;
};
