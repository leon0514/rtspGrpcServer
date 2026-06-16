#pragma once

#include <functional>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>

class TimerScheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Task = std::function<void()>;

    static TimerScheduler& instance();

    // 启动调度器线程（通常在程序初始化时调用）
    void start();

    // 停止调度器线程（程序退出时调用）
    void stop();

    // 延迟 delay_ms 毫秒后执行 task
    // task 将在 TimerScheduler 自己的线程中直接执行
    void schedule(int delay_ms, Task task);

private:
    TimerScheduler() = default;
    ~TimerScheduler();

    void runLoop();

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::multimap<TimePoint, Task> tasks_;
    std::atomic<bool> running_{false};
};
