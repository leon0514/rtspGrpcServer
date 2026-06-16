#include "timer_scheduler.hpp"
#include <spdlog/spdlog.h>

TimerScheduler &TimerScheduler::instance()
{
    static TimerScheduler inst;
    return inst;
}

TimerScheduler::~TimerScheduler()
{
    stop();
}

void TimerScheduler::start()
{
    if (running_.exchange(true))
        return;
    worker_ = std::thread(&TimerScheduler::runLoop, this);
}

void TimerScheduler::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_.exchange(false))
            return;
    }
    cv_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

void TimerScheduler::schedule(int delay_ms, Task task)
{
    auto deadline = Clock::now() + std::chrono::milliseconds(delay_ms);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.emplace(deadline, std::move(task));
    }
    cv_.notify_one();
}

void TimerScheduler::runLoop()
{
    while (running_)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (tasks_.empty())
        {
            // 使用谓词等待，避免虚假唤醒或丢失唤醒导致长时间阻塞
            cv_.wait(lock, [this]{ return !tasks_.empty() || !running_; });
            continue;
        }

        auto now = Clock::now();
        auto it = tasks_.begin();
        if (it->first <= now)
        {
            // 提取所有到期的任务
            std::vector<Task> ready;
            while (it != tasks_.end() && it->first <= now)
            {
                ready.push_back(std::move(it->second));
                it = tasks_.erase(it);
            }
            lock.unlock();

            // 直接在 TimerScheduler 线程上执行到期任务，并捕获异常避免线程退出
            for (auto &t : ready)
            {
                try
                {
                    t();
                }
                catch (const std::exception &e)
                {
                    spdlog::error("[TimerScheduler] Task exception: {}", e.what());
                }
                catch (...)
                {
                    spdlog::error("[TimerScheduler] Unknown task exception");
                }
            }
        }
        else
        {
            // 等待到下一个任务的到期时间，使用谓词以便被 stop() 唤醒
            cv_.wait_until(lock, it->first, [this]{ return !tasks_.empty() && tasks_.begin()->first <= Clock::now() || !running_; });
        }
    }
}
