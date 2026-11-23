// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file LICENSE or https://www.opensource.org/licenses/mit-license.php.

#ifndef DEFI_UTIL_THREADPOOL_H
#define DEFI_UTIL_THREADPOOL_H

#include <sync.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/threadnames.h>

#include <condition_variable>
#include <functional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/**
 * @brief Fixed-size thread pool for running arbitrary tasks concurrently.
 *
 * The thread pool maintains a set of worker threads that consume and execute
 * tasks submitted through Submit(). Once started, tasks can be queued and
 * processed asynchronously until Stop() is called.
 *
 * ### Thread-safety and lifecycle
 * - `Start()` and `Stop()` must be called from a controller (non-worker) thread.
 *   Calling `Stop()` from a worker thread will deadlock, as it waits for all
 *   workers to join, including the current one.
 *
 * - `Submit()` can be called from any thread, including workers. It safely
 *   enqueues new work for execution as long as the pool has active workers and
 *   the work queue is not full.
 *
 * - `Interrupt()` stops new task submission and lets queued ones drain.
 *   Callers can continue other shutdown steps and call Stop() at the end to
 *   ensure no remaining tasks are left to execute.
 *
 * - `Stop()` prevents further task submission and blocks until all the
 *   queued ones are completed.
 */
class ThreadPool
{
private:
    std::string m_name;
    Mutex m_mutex;
    std::queue<std::function<void()>> m_work_queue;
    std::condition_variable m_cv;
    // Note: m_interrupt must be guarded by m_mutex, and cannot be replaced by an unguarded atomic bool.
    // This ensures threads blocked on m_cv reliably observe the change and proceed correctly without missing signals.
    bool m_interrupt{false};
    std::vector<std::thread> m_workers;
    //! Maximum number of queued work items; 0 means unlimited.
    const size_t m_max_queue_depth;

    void WorkerThread()
    {
        util::ThreadRename(strprintf("%s.worker", m_name));
        WAIT_LOCK(m_mutex, wait_lock);
        for (;;) {
            std::function<void()> task;
            {
                // Wait only if needed; avoid sleeping when a new task was submitted while we were processing another one.
                if (!m_interrupt && m_work_queue.empty()) {
                    // Block until the pool is interrupted or a task is available.
                    m_cv.wait(wait_lock, [&]() { return m_interrupt || !m_work_queue.empty(); });
                }

                // If stopped and no work left, exit worker
                if (m_interrupt && m_work_queue.empty()) {
                    return;
                }

                task = std::move(m_work_queue.front());
                m_work_queue.pop();
            }

            // Execute the task without the lock
            task();
        }
    }

public:
    explicit ThreadPool(const std::string& name, size_t max_queue_depth = 0)
        : m_name(name), m_max_queue_depth(max_queue_depth) {}

    ~ThreadPool()
    {
        Stop(); // In case it hasn't been stopped.
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Start worker threads.
     *
     * Creates and launches `num_workers` threads that begin executing tasks
     * from the queue. If the pool is already started, throws.
     */
    void Start(int num_workers)
    {
        Assume(num_workers > 0);
        LOCK(m_mutex);
        if (m_interrupt) throw std::runtime_error("Thread pool has been interrupted or is stopping");
        if (!m_workers.empty()) throw std::runtime_error("Thread pool already started");

        m_workers.reserve(num_workers);
        for (int i = 0; i < num_workers; i++) {
            m_workers.emplace_back([this] { WorkerThread(); });
        }
    }

    /**
     * @brief Stop all worker threads and wait for them to exit.
     *
     * Sets the interrupt flag, wakes all waiting workers, and joins them.
     * Any remaining tasks in the queue will be processed before returning.
     */
    void Stop()
    {
        // Notify workers and join them
        std::vector<std::thread> threads_to_join;
        {
            LOCK(m_mutex);
            // Ensure Stop() is not called from a worker thread while workers are still registered,
            // otherwise a self-join deadlock would occur.
            auto id = std::this_thread::get_id();
            for (const auto& worker : m_workers) Assume(worker.get_id() != id);
            // Early shutdown to return right away on any concurrent Submit() call
            m_interrupt = true;
            threads_to_join.swap(m_workers);
        }
        m_cv.notify_all();
        // Free resources
        for (auto& worker : threads_to_join) worker.join();

        // Since we currently wait for tasks completion, sanity-check empty queue
        LOCK(m_mutex);
        Assume(m_work_queue.empty());
        // Re-allow Start() now that all workers have exited
        m_interrupt = false;
    }

    /**
     * @brief Enqueues a new task for asynchronous execution.
     *
     * @param  fn Callable to execute asynchronously.
     * @return true if the task was accepted, false if the pool is not running,
     *         was interrupted or the work queue is full.
     *
     * Thread-safe: Can be called from any thread, including within the provided 'fn' callable.
     */
    bool Submit(std::function<void()> fn)
    {
        {
            LOCK(m_mutex);
            if (m_workers.empty()) return false;
            if (m_interrupt) return false;
            if (m_max_queue_depth > 0 && m_work_queue.size() >= m_max_queue_depth) return false;

            m_work_queue.emplace(std::move(fn));
        }
        m_cv.notify_one();
        return true;
    }

    /**
     * @brief Stop accepting new tasks and begin asynchronous shutdown.
     */
    void Interrupt()
    {
        {
            LOCK(m_mutex);
            m_interrupt = true;
        }
        m_cv.notify_all();
    }

    size_t WorkQueueSize()
    {
        LOCK(m_mutex);
        return m_work_queue.size();
    }

    size_t WorkersCount()
    {
        LOCK(m_mutex);
        return m_workers.size();
    }
};

#endif // DEFI_UTIL_THREADPOOL_H
