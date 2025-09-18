/*******************************************************************************
 * 版权所有 / Copyright (c) 2025
 * 作者 / Author: Bighiung
 *
 * 文件介绍 / Description:
 * 本文件提供一套基于 std::atomic 的同步工具，包括：
 *   - SmartLock         : 高性能自旋锁，支持自动让出 CPU / 休眠
 *                             High-performance spin lock, supports automatic CPU yield/sleep
 *   - RWSmartLock         : 高性能读写互斥自旋锁，支持自动让出 CPU / 休眠
 *                             High-performance  Read-Write Lock spin lock, supports automatic CPU yield/sleep
 *   - RecursiveSmartLock: 支持同线程递归锁，不阻塞自身
 *                             Recursive lock for the same thread without self-blocking
 *   - AtomicSemaphore       : 原子信号量，多线程同步控制
 *                             Atomic semaphore for multi-thread synchronization
 *   - BHGCDController       : 基于 SmartLock + atomic 的队列任务式线程池管理器
 *                             支持 barrier 任务和普通任务的有序执行
 *                             Queue-based task-based thread pool manager based on SmartLock + atomic,
 *                             supports ordered execution of barrier and normal tasks
 *                             复刻了 MacOS/iOS 的 GCD 框架，借鉴其思想
 *                             Inspired by MacOS/iOS Grand Central Dispatch (GCD) framework
 *
 * 功能简述 / Features:
 *   1. 高性能线程同步，杜绝使用 mutex 和 condition_variable
 *      High-performance thread synchronization, avoid using of mutex and condition_variable
 *   2. 支持递归锁和自动休眠策略，减少自旋消耗
 *      Supports recursive locks and automatic sleep strategy to reduce spin overhead
 *   3. 支持读写互斥锁，单写入多读取
 *      Support read-write mutex lock, single write multiple read.
 *   4. 支持线程池优先级，统一管理任务调度
 *      Supports thread pool priorities and unified task scheduling
 *   5. barrier 任务可以阻塞普通任务，保证执行顺序
 *      Barrier tasks can block normal tasks to guarantee execution order
 *   6. barrier 可以用于读写互斥场景，简化读写互斥操作
 *      Barriers can be used for read-write mutual exclusion, simplifying read-write lock operations
 *   7. group dispatch 可以简化并行任务的依赖关系，在多个任务完成后执行后续操作
 *      Group dispatch can simplify the dependencies of concurrent tasks
 *      and perform subsequent operations after all the tasks are completed.
 *   8. 线程池调度和 barrier 模仿 MacOS/iOS GCD 的设计思想
 *      Thread pool scheduling and barriers mimic the design of iOS/MacOS Grand Center Dispatching
 ******************************************************************************/


#pragma once
#include <atomic>
#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <chrono>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <random>

#ifdef _WIN32
#include <windows.h>  // Windows API
#elif defined(__linux__) || defined(__APPLE__)
// POSIX 线程 / POSIX threads
#include <pthread.h>
#include <sched.h>
#endif


template <typename T>
inline T wrap20(T value) {
    return value & 0xFFFFFu; // 0xFFFF = 65535
}

template <typename T>
inline T wrap16(T value) {
    return value & 0xFFFFu; // 0xFFFF = 65535
}

template <typename T>
inline T wrap12(T value) {
    return value & 0xFFFu; // 0xFFF = 4095
}

template <typename T>
inline T wrap8(T value) {
    return value & 0xFFu; // 0xFF = 255
}

int randomInt(int min = 0, int max = 1000000) {
    static std::mt19937 engine{std::random_device{}()};
    std::uniform_int_distribution<int> dist(min, max);
    return dist(engine);
}

// ================= SpinLockBase 自旋锁基类 =================
template<typename Derived>
class SpinLockBase {

protected:
    std::atomic_flag flag = ATOMIC_FLAG_INIT;  // 原子标志位 / Atomic flag

    uint32_t _maxSpinCount;  // 自旋次数 / Spin count
    uint32_t _maxYieldCount;  // 让出CPU次数 / Yield count
    uint32_t _sleepMicros;  // 休眠微秒数 / Sleep microseconds
    bool _needSleep;

    void spinLockLoop() {
        uint32_t spins = 0, yields = 0;  // 当前自旋和让出计数 / Current spin and yield counters
        uint32_t actualSleepMicros = 0;

        while (true) {
            // 尝试获得锁 / Try to acquire lock
            if (!flag.test_and_set(std::memory_order_acquire)) {
                return;
            }

            // 自旋 / Spin
            if (++spins < _maxSpinCount)
                continue;

            // 自旋次数过多 让出CPU / If spinned to many circles,then Yield CPU
            if (++yields < _maxYieldCount) {
                std::this_thread::yield();
                continue;
            }

            if (_needSleep) {
                actualSleepMicros += _sleepMicros;
                actualSleepMicros = wrap16(actualSleepMicros);

                // 循环等待次数实在太多，微休眠 / If loop ran to many time,sleep briefly.
                std::this_thread::sleep_for(std::chrono::microseconds(actualSleepMicros));
            }
        }
    }

public:
    SpinLockBase(bool needSleep = true, int spin = 5, int yield = 8, int sleep_us = 3)
        : _maxSpinCount(spin), _maxYieldCount(yield), _sleepMicros(sleep_us), _needSleep(needSleep) {}

    void unlock() { flag.clear(std::memory_order_release); }  // 解锁 / Unlock
};

// ================= SmartLock 普通智能自旋锁 =================
class SmartLock : public SpinLockBase<SmartLock> {
public:
    using SpinLockBase::SpinLockBase;  // 继承构造函数 / Inherit constructor

    void lock() { spinLockLoop(); }  // 上锁 / Lock
};

// ================= RWSmartLock 智能自旋读写锁 / Intelligent Spin-Based Read-Write Lock =================
class RWSmartLock : public SpinLockBase<RWSmartLock> {
private:
    std::atomic<int> readCount;

    void spinLockWriting() {

        uint32_t spins = 0, yields = 0;  // 当前自旋和让出计数 / Current spin and yield counters
        uint32_t actualSleepMicros = 0;
        
        // 等待直到不再有其他写入操作。/ Wait until there are no other write operations.
        while (flag.test_and_set(std::memory_order_acquire)) {
            if (++spins < _maxSpinCount) continue;

            if (++yields < _maxYieldCount) {
                std::this_thread::yield();
                continue;
            }

            if (_needSleep) {
                actualSleepMicros += _sleepMicros;
                actualSleepMicros = wrap16(actualSleepMicros);
                std::this_thread::sleep_for(std::chrono::microseconds(actualSleepMicros));
            }
        }

        // 等待直到不再有其他读取入操作。/ Wait until there are no other read operations.
        spins = yields = actualSleepMicros = 0;

        while (readCount.load(std::memory_order_acquire) != 0) {
            if (++spins < _maxSpinCount) continue;

            if (++yields < _maxYieldCount) {
                std::this_thread::yield();
                continue;
            }

            if (_needSleep) {
                actualSleepMicros += _sleepMicros;
                actualSleepMicros = wrap16(actualSleepMicros);
                std::this_thread::sleep_for(std::chrono::microseconds(actualSleepMicros));
            }
        }

    }

    void spinLoopForReadable() {

        uint32_t spins = 0, yields = 0;  // 当前自旋和让出计数 / Current spin and yield counters
        uint32_t actualSleepMicros = 0;

        // 等待直到不再有其他读写入操作。/ Wait until there are no other read or write operations.
        while (flag.test() == true) {

            if (++spins < _maxSpinCount) {
                continue;
            }

            if (++yields < _maxYieldCount) {
                std::this_thread::yield();
                continue;
            }

            if (_needSleep) {
                actualSleepMicros += _sleepMicros;
                actualSleepMicros = wrap16(actualSleepMicros);

                // 循环等待次数实在太多，微休眠 / If loop ran to many time,sleep briefly.
                std::this_thread::sleep_for(std::chrono::microseconds(actualSleepMicros));
            }

        }

    }

public:
    using SpinLockBase::SpinLockBase;  // 继承构造函数 / Inherit constructor

    void writeLock() {
        spinLockWriting();
    }

    void writeUnlock() {
        unlock();
    }

    void readLock() {

        // 确保没有其他任何写操作 / Ensure there is no writing operation
        spinLoopForReadable();
        // 增加当前读的计数 / Increase readcount
        readCount.fetch_add(1);

    }

    void readUnlock() {
        // 减少读取计数 / Decrease readcount
        readCount.fetch_sub(1);
    }

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
    uint32_t _sleepMicroseconds;

private:
    uint32_t _maxRepeatCount;
    uint32_t _maxYieldCount;

public:
    explicit AtomicSemaphore(int init = 0,
                             uint32_t maxRepeat = 3,
                             uint32_t maxYield = 3) :count(init),_maxRepeatCount(maxRepeat),_maxYieldCount(maxYield),_sleepMicroseconds(3) {}  // 构造函数 / Constructor

    // 释放n个资源 / Release n resources
    void release(int n = 1) {
        count.fetch_add(n, std::memory_order_release);
    }

    // 获取资源 / Acquire resource
    void acquire(int n = 1) {

        uint32_t repeatCount = 0,yieldCount = 0,actualSleepMicroseconds = 0;

        while (true) {
            // 读取当前计数 / Load current count

            int expected = count.load(std::memory_order_acquire);
            if (expected > 0 && count.compare_exchange_weak(expected, expected - n,
                                                          std::memory_order_acq_rel)) {
                // 尝试减n成功则返回 / Try decrement and return if successful
                // 清空等待计数计数 / Clear count for waiting
                return;
            }

            if (repeatCount < _maxRepeatCount) {
                repeatCount++;
                continue;
            }

            if (yieldCount < _maxRepeatCount) {
                yieldCount++;
                std::this_thread::yield();  // 失败则让出CPU / Yield CPU
                continue;
            }

            actualSleepMicroseconds += _sleepMicroseconds;
            actualSleepMicroseconds = wrap16(actualSleepMicroseconds);
            std::this_thread::sleep_for(std::chrono::microseconds(actualSleepMicroseconds));

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

    // 普通任务
    void enqueue(const std::function<void()>& func) {
        TaskItem task{-1,std::move(func), false};
        _tasksLock.lock();
        _tasksQ.emplace(task);
        _tasksLock.unlock();
        _tasksSem.release();
    }

    // barrier 任务
    void enqueueBarrier(const std::function<void()>& func) {
        TaskItem task{-1,std::move(func), true};
        _tasksLock.lock();
        _tasksQ.emplace(task);
        _tasksLock.unlock();
        _tasksSem.release();
    }

    // 获得一个用于派发组任务的唯一标识 / Obtain a unique identifier for dispatch grouped tasks.
    int getGroupId() {
        // 随机生成一个groupId
        _groupOptLock.lock();

        int groupId = randomInt();

        size_t repeatCount = 0;
        while (_existGroups.count(groupId)) {
            groupId = randomInt();
            repeatCount++;
            if (repeatCount > 100) {
                // 获取唯一groupId 失败
                _groupOptLock.unlock();
                return -1;
            }
        }

        // 添加一个存在的group
        _existGroups.insert(groupId);

        _groupOptLock.unlock();
        return groupId;
    }

    // 将一个任务添加到一个组
    void enqueueGroup(int groupId, const std::function<void()>& func) {

        _groupOptLock.lock();

        if (_existGroups.count(groupId) == 0) {
            // 不存在的groupId，作为普通任务入队。
            enqueue(func);
            _groupOptLock.unlock();
            return;
        }

        if (_activeGroups.contains(groupId)) {
            // 这个组处于活跃状态，已经在进行执行，不能入队，只能作为普通任务入队
            // This group is in an active state and is already being executed. 
            // It cannot be queued and can only be added to the queue as a regular task.
            enqueue(func);
            _groupOptLock.unlock();
            return;
        }

        // 增加一个组的活跃任务计数 / Increase the active task count of a group
        _groupActiveTaskCount[groupId]++;

        _groupOptLock.unlock();

        // 添加组任务 / Add to task queue as a grouped task
        TaskItem task{groupId, std::move(func), false};
        _tasksLock.lock();
        _tasksQ.emplace(task);
        _tasksLock.unlock();
        _tasksSem.release();
    }

    // 设置某一个组任务，完成后的回调
    void setCallbackForGroup(int groupId,
                             const std::function<void()>& func ,
                             bool fireInstantly = true) {
        _groupOptLock.lock();
        if (_existGroups.contains(groupId)) {
            // 已经添加了group对应的任务，申请过groupId。
            _groupIdCallback[groupId] = std::move(func);
            if (fireInstantly && _groupActiveTaskCount[groupId] > 0) {
                // 让这个任务跑起来。
                fireGroup(groupId);
            }
        }
        _groupOptLock.unlock();

    }

    // 触发一个组任务开始工作 / Trigger the start of a group task to begin working.
    bool fireGroup(int groupId) {
        _groupOptLock.lock();
        if (_groupActiveTaskCount[groupId] > 0) {
            // 如果已经添加了group对应的任务。将当前groupId 设置为活跃，可以执行。
            // If the corresponding tasks for the group have already been added, 
            // set the current groupId as active and it can be executed.
            _activeGroups.insert(groupId);
            _groupOptLock.unlock();
            return true;
        }
        _groupOptLock.unlock();
        return false;
    }

private:
    struct TaskItem {
        int groupID = -1;
        std::function<void()> func;
        bool isBarrier;
    };

    SmartLock _groupOptLock;
    std::unordered_map<int, std::function<void()>> _groupIdCallback;
    std::unordered_map<int, std::atomic<uint32_t>> _groupActiveTaskCount;
    std::unordered_set<int> _existGroups;
    std::unordered_set<int> _activeGroups;

    std::vector<std::thread> threads;
    SmartLock _tasksLock;
    std::atomic<bool> stopFlag;

    std::queue<TaskItem> _tasksQ;
    std::atomic<bool> barrierActive;  // barrier 正在执行
    std::atomic<int> normalTaskCount;  // normal task 正在执行

    // 用于workers 获取任务的信号量 / Semaphore used by workers to obtain tasks
    AtomicSemaphore _tasksSem;

    const ThreadPriority poolPriority;

    void startThreads(size_t count) {
        for (size_t i = 0; i < count; ++i) {
            threads.emplace_back([this]() { workLoop(); });
            setThreadPriority(threads.back(), poolPriority);
        }
    }

    void stop() {
        stopFlag.store(true);
        _tasksSem.release((int)threads.size());
        for (auto &t : threads)
            if (t.joinable()) t.join();
        threads.clear();
    }

    inline void excueteGroupTask(TaskItem &task) {

        int groupId = task.groupID;
        uint32_t yieldCount = 0;
        int sleepMicroseconds = 3;

        // 只有当前groupId 处于活跃状态才执行，否则一直阻塞。
        while (_activeGroups.contains(groupId) == false) {
            if (yieldCount > 15) {
                sleepMicroseconds = std::max(3,sleepMicroseconds);
                sleepMicroseconds += sleepMicroseconds;
                sleepMicroseconds = wrap20(sleepMicroseconds);
                std::this_thread::sleep_for(std::chrono::microseconds(sleepMicroseconds));
                yieldCount = 0;
                continue;
            }
            std::this_thread::yield();
            yieldCount++;
        }

        // 执行任务
        task.func();

        // 修改当前group的相关信息

        _groupOptLock.lock();

        _groupActiveTaskCount[groupId]--;

        if (_groupActiveTaskCount[groupId] == 0) {
            // 当前组的任务执行完毕的情况下。
            // 执行当前组的回调
            if (_groupIdCallback.contains(groupId)) {
                _groupIdCallback[groupId]();
            }
            // 清空当前任务的相关计数
            eraseRecordForGroupId(groupId);
        }

        _groupOptLock.unlock();
    }

    inline void eraseRecordForGroupId(int groupId) {
        _groupActiveTaskCount.erase(groupId);
        _groupIdCallback.erase(groupId);
        _existGroups.erase(groupId);
        _activeGroups.erase(groupId);
    }

#define WORKER_MAX_YIELD_COUNT 100
    
    void workLoop() {

        uint32_t yieldCount = 0, sleepMicrosecons = 0;

        while (!stopFlag.load()) {

            // 确保没有正在运行的格栅任务 Ensure there is no running barrier tasks

            if (barrierActive.load(std::memory_order_acquire) == false) {

                _tasksSem.acquire(); // 等待任务 Waiting for a task
                _tasksLock.lock();
                TaskItem &task = _tasksQ.front();
                std::function<void()> taskFunc = std::move(task.func);
                _tasksQ.pop();
                _tasksLock.unlock();

                // Reset yield count
                yieldCount = 0;

                // barrier / normal
                if (task.isBarrier) {
                    barrierActive.store(true); // 阻止其他线程取任务
                    while (normalTaskCount.load(std::memory_order_acquire) != 0) {
                        std::this_thread::yield();
                    }
                    taskFunc(); // 立即执行 barrier
                    barrierActive.store(false); // barrier 完成，允许其他任务继续
                    continue;
                } else {
                    normalTaskCount.fetch_add(1, std::memory_order_release);
                    if (task.groupID >= 0) {
                        // 是一个组任务。
                        excueteGroupTask(task);
                    } else {
                        taskFunc(); // 立即执行普通任务
                    }
                    normalTaskCount.fetch_sub(1, std::memory_order_release);
                    continue;
                }

            } else {
                if (yieldCount > WORKER_MAX_YIELD_COUNT) {
                    // 没有执行任务的情况下，让出cpu次数太多，则进入休眠，避免浪费CPU资源
                    // If the count of yield is too high without performing any tasks, the worker will enter sleep mode to avoid wasting CPU resources.
                    sleepMicrosecons += 10;
                    sleepMicrosecons = wrap8(sleepMicrosecons);
                    std::this_thread::sleep_for(std::chrono::microseconds(sleepMicrosecons));
                    continue;
                }
                yieldCount++;
                std::this_thread::yield(); // 队列为空或 barrier 阻塞时让出 CPU / Yield the CPU when the queue is empty or the barrier is blocked.
            }

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

}
