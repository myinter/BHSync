/*******************************************************************************
 * 版权所有 / Copyright (c) 2025
 * 作者 / Author: Bighiung
 *
 * 文件介绍 / Description:
 * 本文件提供一套基于 std::atomic 的同步工具，包括：
 *   - SmartSpinLock         : 高性能自旋锁，支持自动让出 CPU / 休眠
 *                             High-performance spin lock, supports automatic CPU yield/sleep
 *   - RecursiveSmartSpinLock: 支持同线程递归锁，不阻塞自身
 *                             Recursive lock for the same thread without self-blocking
 *   - AtomicSemaphore       : 原子信号量，多线程同步控制
 *                             Atomic semaphore for multi-thread synchronization
 *   - BHGCDController       : 基于 SmartSpinLock + atomic 的线程池管理器
 *                             支持 barrier 任务和普通任务的有序执行
 *                             Thread pool manager using SmartSpinLock + atomic,
 *                             supports ordered execution of barrier and normal tasks
 *                             复刻了 MacOS/iOS 的 GCD 框架，借鉴其思想
 *                             Inspired by MacOS/iOS Grand Central Dispatch (GCD) framework
 *
 * 功能简述 / Features:
 *   1. 高性能线程同步，尽可能避免 mutex 和 condition_variable
 *      High-performance thread synchronization, minimizing use of mutex and condition_variable
 *   2. 支持递归锁和自动休眠策略，减少自旋消耗
 *      Supports recursive locks and automatic sleep strategy to reduce spin overhead
 *   3. 支持线程池优先级，统一管理任务调度
 *      Supports thread pool priorities and unified task scheduling
 *   4. barrier 任务可以阻塞普通任务，保证执行顺序
 *      Barrier tasks can block normal tasks to guarantee execution order
 *   5. barrier 可以用于读写互斥场景，简化读写互斥操作
 *      Barriers can be used for read-write mutual exclusion, simplifying read-write lock operations
 *   6. 线程池调度和 barrier 模仿 MacOS/iOS GCD 的设计思想
 *      Thread pool scheduling and barriers mimic the design of iOS/MacOS Grand Center Dispatching
  * 许可协议 / License: MIT
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * 本软件免费提供给任何人使用，包括但不限于使用、复制、修改、合并、发布、分发、再许可及销售。
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * 在软件的所有副本或主要部分中必须包含上述版权声明和许可声明。
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * 本软件按“原样”提供，不提供任何明示或暗示的担保，包括但不限于对适销性、
 * 特定用途适用性或非侵权的保证。在任何情况下，作者或版权持有人均不对因软件使用
 * 或其他交易引起的任何索赔、损害或其他责任负责。

 ******************************************************************************/

#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <chrono>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <mutex>

#ifdef _WIN32
#include <windows.h>  // Windows API
#elif defined(__linux__) || defined(__APPLE__)
// POSIX 线程 / POSIX threads
#include <pthread.h>
#include <sched.h>
#endif

// ================= SpinLockBase 自旋锁基类 =================
template<typename Derived>
class SpinLockBase {
protected:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;  // 原子标志位 / Atomic flag
    int spinCount;  // 自旋次数 / Spin count
    int yieldCount;  // 让出CPU次数 / Yield count
    int sleepMicros;  // 休眠微秒数 / Sleep microseconds

    void spinLockLoop() {
        int spins = 0, yields = 0;  // 当前自旋和让出计数 / Current spin and yield counters
        while (true) {
            // 尝试获得锁 / Try to acquire lock
            if (!flag.test_and_set(std::memory_order_acquire)) 
             return;
            // 自旋 / Spin
            if (++spins < spinCount) 
               continue;
            // 自旋次数过多 让出CPU / If spinned to many circles,then Yield CPU
            if (++yields < yieldCount) {
                std::this_thread::yield();
                continue;
            }
            // 自选和让出CPU计数数清零 / Reset yieldCount and spinCount
            yieldCount = 0;
            spinCount = 0;

            // 循环次数是在太多，微休眠 / If loop ran to many time,sleep briefly.
            std::this_thread::sleep_for(std::chrono::microseconds(sleepMicros));
        }
    }

public:
    SpinLockBase(int spin = 5, int yield = 8, int sleep_us = 2000)
        : spinCount(spin), yieldCount(yield), sleepMicros(sleep_us) {}

    void unlock() { flag.clear(std::memory_order_release); }  // 解锁 / Unlock
};

// ================= SmartLock 普通智能自旋锁 =================
class SmartLock : public SpinLockBase<SmartLock> {
public:
    using SpinLockBase::SpinLockBase;  // 继承构造函数 / Inherit constructor

    void lock() { spinLockLoop(); }  // 上锁 / Lock
};

// ================= RecursiveSmartLock 递归自旋锁 =================
// 不会阻塞住进入触发上锁的线程 It will not block the thread that enters the critical section and triggers the locking mechanism.
class RecursiveSmartLock : public SpinLockBase<RecursiveSmartLock> {
    // 锁拥有者线程ID，用于避免同一个线程锁住自己 / Owner thread ID for avoiding a thread from block itself.
    std::thread::id owner;
public:
    // 继承构造函数 / Inherit constructor
    using SpinLockBase::SpinLockBase;

    void lock() {
        // 当前线程ID / Current thread ID
        std::thread::id this_id = std::this_thread::get_id();
        // 如果当前线程已持有锁，则直接返回，避免当前线程被阻塞 / Return if already locked by this thread, avoiding current thread blocked.
        if(owner == this_id)
            return;
        // 自旋获取锁 / Spin to acquire lock
        spinLockLoop();
        // 设置锁拥有者 / Set owner
        owner = this_id;
    }

    void unlock() {
        if(owner == std::this_thread::get_id()) {  // 只有拥有者才能解锁 / Only owner can unlock
            owner = std::thread::id();  // 清除锁拥有者 / Clear owner
            SpinLockBase::unlock();  // 释放锁 / Unlock base
        }
    }
};

// ================= AtomicSemaphore 原子信号量 =================
class AtomicSemaphore {
    std::atomic<int> count;  // 计数 / Counter
public:
    explicit AtomicSemaphore(int init = 0) : count(init) {}  // 构造函数 / Constructor
    void release(int n = 1) { count.fetch_add(n, std::memory_order_release); }  // 释放n个资源 / Release n resources
    void acquire(int n = 1) {  // 获取资源 / Acquire resource
        while (true) {
            int expected = count.load(std::memory_order_acquire);  // 读取当前计数 / Load current count
            if (expected > 0 && count.compare_exchange_weak(expected, expected - n,
                                                          std::memory_order_acq_rel)) return;  // 尝试减1成功则返回 / Try decrement and return if successful
            std::this_thread::yield();  // 失败则让出CPU / Yield CPU
        }
    }
};

// ================= ThreadPriority 线程优先级枚举 =================
// 线程优先级 / Thread priorities
enum class ThreadPriority {
    UI,
    High,
    Normal,
    Background
};

// 用于不同平台，设置进程优先级的包装方法
// A middle-layer method for setting process priorities on different platforms
inline void setThreadPriority(std::thread &t, ThreadPriority p) {
#ifdef _WIN32
    int pri = THREAD_PRIORITY_NORMAL;  // 默认优先级 / Default priority
    switch (p) {
        case ThreadPriority::UI: pri = THREAD_PRIORITY_TIME_CRITICAL; break;  // UI 优先级 / UI priority
        case ThreadPriority::High: pri = THREAD_PRIORITY_HIGHEST; break;  // 高优先级 / High priority
        case ThreadPriority::Normal: pri = THREAD_PRIORITY_NORMAL; break;  // 普通优先级 / Normal priority
        case ThreadPriority::Background: pri = THREAD_PRIORITY_LOWEST; break;  // 后台优先级 / Background priority
    }
    SetThreadPriority(t.native_handle(), pri);  // 设置Windows线程优先级 / Set Windows thread priority
#elif defined(__linux__) || defined(__APPLE__)
    sched_param sch; int policy; pthread_getschedparam(pthread_self(), &policy, &sch);  // 获取线程调度参数 / Get thread scheduling params
    switch (p) {
        case ThreadPriority::UI: sch.sched_priority = sched_get_priority_max(SCHED_FIFO); break;  // UI 优先级 / UI priority
        case ThreadPriority::High: sch.sched_priority = (sched_get_priority_max(SCHED_FIFO)*3)/4; break;  // 高优先级 / High priority
        case ThreadPriority::Normal: sch.sched_priority = 0; break;  // 普通优先级 / Normal priority
        case ThreadPriority::Background: sch.sched_priority = sched_get_priority_min(SCHED_FIFO); break;  // 后台优先级 / Background priority
    }
    pthread_setschedparam(t.native_handle(), SCHED_FIFO, &sch);  // 设置POSIX线程优先级 / Set POSIX thread priority
#else
    (void)t; (void)p;  // 占位 / Placeholder
#endif
}

// ================= BHGCDController 线程池 with non-blocking Barrier =================

namespace BHGCD {

// 线程优先级枚举 / Thread priority enum
enum class ThreadPriority {
    UI,
    High,
    Normal,
    Background
};

// 设置线程优先级 / Set thread priority
inline void setThreadPriority(std::thread &t, ThreadPriority p) {
#ifdef _WIN32
    int pri = THREAD_PRIORITY_NORMAL;
    switch (p) {
        case ThreadPriority::UI: pri = THREAD_PRIORITY_TIME_CRITICAL; break;
        case ThreadPriority::High: pri = THREAD_PRIORITY_HIGHEST; break;
        case ThreadPriority::Normal: pri = THREAD_PRIORITY_NORMAL; break;
        case ThreadPriority::Background: pri = THREAD_PRIORITY_LOWEST; break;
    }
    SetThreadPriority(t.native_handle(), pri);
#elif defined(__linux__) || defined(__APPLE__)
    sched_param sch; int policy; pthread_getschedparam(pthread_self(), &policy, &sch);
    switch (p) {
        case ThreadPriority::UI: sch.sched_priority = sched_get_priority_max(SCHED_FIFO); break;
        case ThreadPriority::High: sch.sched_priority = (sched_get_priority_max(SCHED_FIFO)*3)/4; break;
        case ThreadPriority::Normal: sch.sched_priority = 0; break;
        case ThreadPriority::Background: sch.sched_priority = sched_get_priority_min(SCHED_FIFO); break;
    }
    pthread_setschedparam(t.native_handle(), SCHED_FIFO, &sch);
#else
    (void)t; (void)p;
#endif
}

class BHGCDController {
public:
    explicit BHGCDController(size_t threadCount = std::thread::hardware_concurrency(),
                             ThreadPriority priority = ThreadPriority::Normal)
        : stopFlag(false),
          barrierActive(false),
          poolPriority(priority) {
        startThreads(threadCount);
    }

    ~BHGCDController() { stop(); }

    // 普通任务入队 Regular task entry into the queue
    void enqueue(const std::function<void()>& func) {
        TaskItem task{func, false};
        lock.lock();
        tasks.push(task);
        lock.unlock();
        sem.release();
    }

    // barrier 任务入队.Barrier 任务会等待之前的任务完成，同时阻塞住后续入队的任务。
    // Barrier tasks entry into the queue. 
    // Barrier tasks will wait for former tasks and blocks later tasks
    void enqueueBarrier(const std::function<void()>& func) {
        TaskItem task{func, true};
        lock.lock();
        tasks.push(task);
        lock.unlock();
        sem.release();
    }

    void stop() {
        stopFlag.store(true);
        sem.release((int)threads.size());
        for (auto &t : threads)
            if (t.joinable()) t.join();
        threads.clear();
    }

private:
    struct TaskItem {
        std::function<void()> func;
        bool isBarrier;
    };

    std::vector<std::thread> threads;
    SmartLock lock;
    std::atomic<bool> stopFlag;

    std::queue<TaskItem> tasks;
    std::atomic<bool> barrierActive;  // barrier 正在执行的标志符号。 Flag for whether there is barrier task in running.
    AtomicSemaphore sem;

    const ThreadPriority poolPriority;

    void startThreads(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            threads.emplace_back([this]() { workLoop(); });
            setThreadPriority(threads.back(), poolPriority);
        }
    }

    void workLoop() {

        while (!stopFlag.load()) {
            
            // 确保没有正在运行的格栅任务 Ensure there is no running barrier tasks

            if (barrierActive.load() == false) {

                sem.acquire(); // 等待任务 Waiting for a task

                lock.lock();

                if (!tasks.empty()) {
                    TaskItem task = tasks.front();

                    // barrier 优先处理
                    if (task.isBarrier) {
                        if (!barrierActive.load()) {
                            tasks.pop();
                            barrierActive.store(true); // 阻止其他线程取任务
                            lock.unlock();
                            task.func(); // 立即执行 barrier
                            barrierActive.store(false); // barrier 完成，允许其他任务继续
                            continue;
                        }
                    } else {
                        if (!barrierActive.load()) {
                            tasks.pop();
                            lock.unlock();

                            task.func(); // 立即执行普通任务
                            continue;
                        }
                    }
                }

                lock.unlock();
            }
            // Release the CPU when the queue is empty or when there is a barrier task is running.
            std::this_thread::yield(); // 队列为空或 存在barrier 阻塞时让出 CPU
        }
    }
};
// 默认全局控制器实例 / Default global controllers
struct GlobalBHGCDControllers {
    BHGCDController ui{4, ThreadPriority::UI};
    BHGCDController high{4, ThreadPriority::High};
    BHGCDController normal{std::thread::hardware_concurrency(), ThreadPriority::Normal};
    BHGCDController background{2, ThreadPriority::Background};
};

// 以任务队列的名义对外暴露 Expose it externally in the name of a task queue.
static GlobalBHGCDControllers queues;

inline GlobalBHGCDControllers& GetBHGCDControllers() {
    return queues;
}

}
