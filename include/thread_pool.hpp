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
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(mtx_);
                        // 线程阻塞直到有任务(需要执行的函数)到任务队列中
                        cv_.wait(lock, [this]{ return !tasks_.empty() || !running_; });
                        // 如果队列空了并且线程池需要被停止了,直接返回,结束循环
                        if (tasks_.empty() && !running_)
                        {
                            return;
                        }
                         // 从任务队列中取出任务
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                    // 这样通过 lambda 创建的 task 对象实际上是一个可调用对象。
                    // 当你调用 task() 时，它会使用捕获的 f 和 args... 来执行原始的函数或 Lambda 表达式。
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
    // 2. 使用 std::forward 完美转发保证参数的左值右值状态, 避免无意间的拷贝或移动，从而提高了效率。
    // 3. 通知线程有数据到队列中了
    // F&& f, Args&&... args 这个是万能引用
    template <typename Func, typename... Args>
    void enqueue(Func &&func, Args &&...args)
    {
        // 使用 lambda + tuple 捕获，支持移动语义（如 std::unique_ptr）
        std::function<void()> task =
            [f = std::forward<Func>(func),
             tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]() mutable {
                std::apply(f, std::move(tup));
            };
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
    std::queue<std::function<void()>> tasks_;
    std::condition_variable cv_;
    std::mutex mtx_;

    std::atomic<bool> running_;
};
