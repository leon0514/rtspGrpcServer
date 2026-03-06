#pragma once

#include <string>
#include <memory>
#include <vector>
#include <mutex>

class FrameMemoryPool : public std::enable_shared_from_this<FrameMemoryPool>
{
public:
    // 默认预分配容量，建议设为足以容纳一帧最大 JPEG 的大小（例如 3MB）
    explicit FrameMemoryPool(size_t default_capacity = 3 * 1024 * 1024)
        : default_capacity_(default_capacity) {}

    ~FrameMemoryPool() = default;

    // 获取一个可用的 Buffer（以带自定义删除器的 shared_ptr 形式返回）
    std::shared_ptr<std::string> acquire()
    {
        std::unique_ptr<std::string> str;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pool_.empty())
            {
                str = std::move(pool_.back());
                pool_.pop_back();
            }
        }

        if (!str)
        {
            str = std::make_unique<std::string>();
            str->reserve(default_capacity_);
        }
        else
        {
            str->clear(); // 清空数据，但保留底层 capacity 避免重新分配
        }

        // 使用 weak_ptr 防止内存池提前析构导致删除器访问野指针
        std::weak_ptr<FrameMemoryPool> weak_pool = shared_from_this();

        // 返回带有自定义删除器的 shared_ptr
        return std::shared_ptr<std::string>(str.release(), [weak_pool](std::string *ptr)
                                            {
            if (auto pool = weak_pool.lock()) {
                pool->release(std::unique_ptr<std::string>(ptr));
            } else {
                // 如果内存池已经销毁（流已停止），则直接释放内存
                delete ptr; 
            } });
    }

private:
    // 供自定义删除器回调使用
    void release(std::unique_ptr<std::string> str)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 限制池的最大缓存数量，防止异常情况下内存无限膨胀
        if (pool_.size() < 3)
        {
            pool_.push_back(std::move(str));
        }
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<std::string>> pool_;
    size_t default_capacity_;
};