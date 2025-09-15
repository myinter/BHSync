# Bighiung Sync Library / 同步工具库

作者 / Author: **Bighiung**
日期 / Date: 2025

---

## 简介 / Introduction

本库提供一套基于 `std::atomic` 的高性能线程同步工具，复刻了 iOS 的 GCD 框架思想，包括以下模块：
This library provides a set of high-performance thread synchronization tools based on `std::atomic`, inspired by the iOS GCD framework. Modules include:

* **SmartLock**：高性能自旋锁，支持自动让出 CPU / 休眠
  High-performance spin lock, supports automatic CPU yield/sleep
* **RecursiveSmartLock**：支持同线程递归锁，不阻塞自身
  Recursive lock for the same thread without self-blocking
* **AtomicSemaphore**：原子信号量，多线程同步控制
  Atomic semaphore for multi-thread synchronization
* **BHGCDController**：线程池管理器，支持 barrier 任务和普通任务的有序执行
  Thread pool manager, supports ordered execution of barrier and normal tasks
  复刻 Mac/iOS GCD 框架思想
  Inspired by the Mac/iOS Grand Central Dispatch (GCD) framework

---

## 功能特点 / Features

1. 高性能线程同步，尽量避免 `mutex` 和 `condition_variable`
   High-performance thread synchronization, minimizing use of mutex and condition\_variable
2. 支持递归锁和自动休眠策略，减少自旋消耗
   Supports recursive locks and automatic sleep strategy to reduce spin overhead
3. 支持线程池优先级，统一管理任务调度
   Supports thread pool priorities and unified task scheduling
4. barrier 任务可以阻塞普通任务，保证执行顺序
   Barrier tasks can block normal tasks to guarantee execution order
5. barrier 可以用于读写互斥场景，简化读写互斥操作
   Barriers can be used for read-write mutual exclusion, simplifying read-write lock operations
6. 线程池调度和 barrier 模仿 iOS GCD 的设计思想
   Thread pool scheduling and barriers mimic the design of iOS GCD

---

## 使用方法 / Usage

### 1. SmartLock / RecursiveSmartLock

```cpp
#include "Sync.hpp"

SmartLock lock;

void criticalSection() {
    lock.lock();   // 加锁 / lock
    // 临界区操作 / critical section
    lock.unlock(); // 解锁 / unlock
}

// 递归锁 / Recursive lock
RecursiveSmartLock rlock;

void recursiveFunction(int depth) {
    rlock.lock();
    if (depth > 0) recursiveFunction(depth - 1);
    rlock.unlock();
}
```

---

### 2. AtomicSemaphore 多线程同步打印 / AtomicSemaphore Demo

```cpp
#include <iostream>
#include <thread>
#include "Sync.hpp"

AtomicSemaphore semA(2), semB(0), semC(0);

void printA() {
    BHGCD::queues.high.enqueue([] {
            for (int i = 0; i < 100; ++i) {
                semA.acquire();
                std::cout << "A";
                semB.release();
            }
    });
}

void printB() {
    BHGCD::queues.high.enqueue([] {
            for (int i = 0; i < 100; ++i) {
                semB.acquire();
                semB.acquire();
                std::cout << "B";
                semC.release();
            }
    });
}

void printC() {
    BHGCD::queues.high.enqueue([] {
            for (int i = 0; i < 100; ++i) {
                semC.acquire();
                std::cout << "C";
                semA.release(2);
            }
    });
}

int main() {
    printA();
    printB();
    printC();
    return 0;
}
```

* 功能说明 / Description:
  使用原子信号量控制线程交替打印，保证顺序执行 AA -> B -> C。
  Demonstrates atomic semaphore for synchronized printing in order: AA -> B -> C.

```text

AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-AABC-

```


---

### 3. BHGCDController Barrier 与普通任务调度 / GCD-style Thread Pool Demo

```cpp
#include <iostream>
#include "Sync.hpp"

    // 1. 向 UI 队列提交一些普通任务
    for (int i = 0; i < 5; ++i) {
        BHGCD::queues.ui.enqueue([i]() {
            std::cout<<std::endl << "[UI] Task " << i << " before barrier running on thread "
                      << std::this_thread::get_id() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(5000));
        });
    }

    // 3. 提交一个 barrier 任务
    BHGCD::queues.ui.enqueueBarrier([]() {
        std::cout << "[UI] Barrier Task running (waits all before, blocks after) on thread "
                  << std::this_thread::get_id() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    // 2. 提交 barrier 之后的任务
    for (int i = 5; i < 10; ++i) {
        BHGCD::queues.ui.enqueue([i]() {
            std::cout << "[UI] Task " << i << " after barrier, and before another barrier, running on thread "
                      << std::this_thread::get_id() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    }
    
    // 再次提交barrier任务
    BHGCD::queues.ui.enqueueBarrier([]() {
        std::cout << "\n [UI] Barrier Task running (waits all before, blocks after) on thread "
                  << std::this_thread::get_id() << "\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    });

    // 再次提交barrier任务之后的任务。
    for (int i = 20; i < 30; ++i) {
        BHGCD::queues.ui.enqueue([i]() {
            std::cout << "[UI] Task " << i << " after barrier, running on thread "
                      << std::this_thread::get_id() << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        });
    }

    return 0;
```
任务执行顺序 Sequence of execution of tasks：

```text

[UI] Task 0 before barrier running on thread 0x16fe87000
[UI] Task 1 before barrier running on thread 0x16ff9f000
[UI] Task 2 before barrier running on thread 0x17002b000
[UI] Task 3 before barrier running on thread 0x16ff13000
[UI] Task 4 before barrier running on thread 0x16fe87000

[UI] Barrier Task running (waits all before, blocks after) on thread 0x16ff9f000

[UI] Task 5 after barrier, and before another barrier, running on thread 0x16ff13000
[UI] Task 6 after barrier, and before another barrier, running on thread 0x17002b000
[UI] Task 7 after barrier, and before another barrier, running on thread 0x16ff9f000
[UI] Task 8 after barrier, and before another barrier, running on thread 0x16ff13000
[UI] Task 9 after barrier, and before another barrier, running on thread 0x16ff9f000

[UI] Barrier Task running (waits all before, blocks after) on thread 0x17002b000

[UI] Task 20 after barrier, running on thread 0x16ff9f000
[UI] Task 21 after barrier, running on thread 0x17002b000
[UI] Task 22 after barrier, running on thread 0x16ff13000
[UI] Task 25 after barrier, running on thread 0x16ff9f000
[UI] Task 24 after barrier, running on thread 0x17002b000
[UI] Task 26 after barrier, running on thread 0x16ff9f000
[UI] Task 27 after barrier, running on thread 0x16ff13000
[UI] Task 28 after barrier, running on thread 0x17002b000
[UI] Task 29 after barrier, running on thread 0x16ff9f000

```

* 功能说明 / Description:
  演示 barrier 任务阻塞普通任务的执行顺序，类似 iOS/MacOS GCD 的行为。
  Demonstrates barrier tasks blocking normal tasks, mimicking iOS/MacOS GCD behavior.
  
  BHGCDController 提供的任务队列，可以在多线程环境下确保任务开启的先后顺序，使用FIFO的顺序触发任务执行。同时可以利用barrier方式派发任务实现读写互斥。
  The task queue provided by BHGCDController can ensure the sequence of task initiation in a multi-threaded environment.
  Meanwhile, it can use the barrier method to dispatch tasks to achieve mutual exclusion between reading and writing.
---

### 4. SmartLock 与 std::mutex 性能对比 / Performance Demo

```cpp

// ---------------------------
// 测试 SmartLock 性能 / Test for performance of std:mutex
// ---------------------------
void testSmartSpinLock(size_t numThreads, size_t iterations) {
    SmartLock lock;             // 自定义自旋锁
    size_t counter = 0;             // 共享计数器

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < iterations; ++j) {
                lock.lock();
                ++counter;
                lock.unlock();
            }
        });
    }
    for (auto &t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[SmartLock] Threads=" << numThreads
              << ", Iterations=" << iterations
              << ", Counter=" << counter
              << ", Time=" << elapsed << "ms\n";
}

// ---------------------------
// 测试 std::mutex 性能 / Test for performance of std:mutex
// ---------------------------
void testStdMutex(size_t numThreads, size_t iterations) {
    std::mutex lock;                // 标准互斥锁
    size_t counter = 0;             // 共享计数器

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (size_t i = 0; i < numThreads; ++i) {
        threads.emplace_back([&]() {
            for (size_t j = 0; j < iterations; ++j) {
                std::lock_guard<std::mutex> guard(lock);
                ++counter;
            }
        });
    }
    for (auto &t : threads) t.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[std::mutex]    Threads=" << numThreads
              << ", Iterations=" << iterations
              << ", Counter=" << counter
              << ", Time=" << elapsed << "ms\n";
}

```
性能差异 Difference in performance:

```text
[SmartLock] Threads=8, Iterations=10000000, Counter=80000000, Time=812ms
[std::mutex] Threads=8, Iterations=10000000, Counter=80000000, Time=3024ms
```

* 功能说明 / Description:
  对比 SmartLock 与 std::mutex 的性能消耗。
  Compare the performance of SmartSpinLock vs std::mutex.

---

## 注意事项 / Notes

1. SmartLock 可以同时适配长和短的临界区 /SmartLock is suitable for both short and long critical sections
2. BHGCDController 的线程优先级在创建时固定，不可修改 / priority is fixed after creation
3. BHGCDController 提供 4个默认的任务队列：ui high normal background / The BHGCDController provides 4 default task queues: ui, high, normal, background
4. barrier 任务可以用于简化读写互斥操作，无需在操作代码中添加对读写锁的使用 / barrier tasks simplify read-write locks

---
