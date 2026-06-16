#pragma once

#include <iostream>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <vector>
#include <future>
#include <functional>
#include <tuple>
#include <utility>
#include <memory>

/**
 * 轻量可移动任务包装器
 *
 * std::function 要求内部可调用对象可复制，因此无法保存捕获了 std::unique_ptr
 * 等仅移动类型的 lambda。Task 通过类型擦除 + unique_ptr 实现仅移动的任务队列。
 */
class ThreadPoolTask
{
    struct Base
    {
        virtual ~Base() = default;
        virtual void call() = 0;
    };

    template <typename F>
    struct Impl : Base
    {
        F f;
        explicit Impl(F &&f_) : f(std::move(f_)) {}
        void call() override { f(); }
    };

    std::unique_ptr<Base> impl_;

public:
    ThreadPoolTask() = default;

    template <typename F>
    ThreadPoolTask(F &&f)
        : impl_(std::make_unique<Impl<F>>(std::forward<F>(f)))
    {
    }

    ThreadPoolTask(ThreadPoolTask &&) = default;
    ThreadPoolTask &operator=(ThreadPoolTask &&) = default;

    ThreadPoolTask(const ThreadPoolTask &) = delete;
    ThreadPoolTask &operator=(const ThreadPoolTask &) = delete;

    void operator()()
    {
        if (impl_)
            impl_->call();
    }

};

class ThreadPool
{
public:
    ThreadPool(int num) : running_(true)
    {
        for (int i = 0; i < num; i++)
        {
            threads_.emplace_back([this]()
                                  {
                while (true)
                {
                    ThreadPoolTask task;
                    {
                        std::unique_lock<std::mutex> lock(mtx_);
                        // 线程阻塞直到有任务到任务队列中或线程池停止
                        cv_.wait(lock, [this]{ return !tasks_.empty() || !running_; });
                        // 如果队列空了并且线程池需要被停止，直接返回
                        if (tasks_.empty() && !running_)
                        {
                            return;
                        }
                        // 从任务队列中取出任务
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    try
                    {
                        task();
                    }
                    catch (const std::exception &e)
                    {
                        std::cerr << "[ThreadPool] Task exception: " << e.what() << std::endl;
                    }
                    catch (...)
                    {
                        std::cerr << "[ThreadPool] Unknown task exception" << std::endl;
                    }
                } });
        }
    }

    // 析构函数
    // 1. 在持有锁的情况下设置 running_ 为 false
    // 2. 通知所有线程（避免丢失唤醒）
    // 3. 等待线程执行完成
    ~ThreadPool()
    {
        {
            std::unique_lock<std::mutex> lock(mtx_);
            running_ = false;
        }
        cv_.notify_all();
        for (auto &thread : threads_)
        {
            if (thread.joinable())
                thread.join();
        }
    }

    // 提交任务到任务队列中
    // 1. 使用变长参数模板接收函数的所有参数
    // 2. 使用 std::forward 完美转发，支持移动语义（如 std::unique_ptr）
    // 3. 通知线程有数据到队列中了
    template <typename Func, typename... Args>
    void enqueue(Func &&func, Args &&...args)
    {
        ThreadPoolTask task(
            [f = std::forward<Func>(func),
             tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable {
                std::apply(f, std::move(tup));
            });

        {
            std::unique_lock<std::mutex> lock(mtx_);
            if (!running_)
            {
                throw std::runtime_error("enqueue on stopped ThreadPool");
            }
            tasks_.emplace(std::move(task));
        }
        cv_.notify_one();
    }

private:
    // 线程列表
    std::vector<std::thread> threads_;
    // 任务队列
    std::queue<ThreadPoolTask> tasks_;
    std::condition_variable cv_;
    std::mutex mtx_;

    std::atomic<bool> running_;
};
