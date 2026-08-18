#include "task_queue.h"

namespace satdump
{
    TaskQueue::TaskQueue() {}

    TaskQueue::~TaskQueue()
    {
        if (task_thread.joinable())
            task_thread.join();
    }

    // The exit decision and the flag must be published under one lock. Previously the queue was tested,
    // the lock dropped, and the flag set after - so a push() in that window saw thread_exited still
    // false, queued a task nobody would run, and a later push() assigned over a joinable thread.
    void TaskQueue::threadFunc()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lck(queue_mtx);
            if (task_queue.size() == 0)
            {
                thread_exited = true; // Set before releasing, so push() can never miss it
                return;
            }

            auto task = task_queue.front();
            task_queue.pop();
            lck.unlock();

            try
            {
                task();
            }
            catch (std::exception &)
            {
                // TODOREWORK?
            }
        }
    }

    void TaskQueue::push(std::function<void()> task)
    {
        std::lock_guard<std::mutex> lck(queue_mtx);

        task_queue.push(task);

        if (thread_exited)
        {
            if (task_thread.joinable())
                task_thread.join();
            thread_exited = false;
            task_thread = std::thread(&TaskQueue::threadFunc, this);
        }
    }
} // namespace satdump