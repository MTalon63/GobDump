#include "task_scheduler.h"
#include "core/plugin.h"
#include <chrono>
#include <limits>
#include <thread>

namespace satdump
{
    void TaskScheduler::thread_func()
    {
        while (running)
        {
            // Lock the mutex and set up
            std::unique_lock<std::mutex> lock(task_mtx);
            time_t sleep_for = std::numeric_limits<time_t>::max();
            time_t wait_began = time(0);

            // Store task data locally to avoid use-after-free if map is modified during wait
            std::shared_ptr<void> task_evt;
            std::string task_evt_name;
            std::string task_name_to_run;

            // Look for next task to run
            for (auto &this_task : scheduled_tasks)
            {
                // This task is already due! Run it
                time_t next_run = this_task.second.last_run + this_task.second.run_interval;
                if (next_run <= wait_began)
                {
                    task_evt = this_task.second.evt;
                    task_evt_name = this_task.second.evt_name;
                    task_name_to_run = this_task.first;
                    sleep_for = 0;
                    break;
                }

                // This task is the next one to run
                else if (sleep_for > wait_began - next_run)
                {
                    sleep_for = next_run - wait_began;
                    task_evt = this_task.second.evt;
                    task_evt_name = this_task.second.evt_name;
                    task_name_to_run = this_task.first;
                }
            }

            // Sleep until next task, or we are woken up
            needs_update = false;
            if (sleep_for > 0)
                cv.wait_for(lock, std::chrono::seconds(sleep_for), [this] { return !running || needs_update; });
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // TODOREWORK. Hogs CPU otherwise...

            // Stop/Restart loop if needed
            if (!running || needs_update ||          // We were woken up because of a change in the program
                task_evt == nullptr ||               // Spurious wake when no task scheduled
                wait_began + sleep_for > time(NULL)) // Spurious wake
                continue;

            // Verify task still exists and wasn't modified (re-lookup by name)
            auto it = scheduled_tasks.find(task_name_to_run);
            if (it == scheduled_tasks.end())
                continue; // Task was removed

            // Run the task that should be due now, and mark it for next run
            eventBus->fire_event(task_evt.get(), task_evt_name);
            it->second.last_run = time(NULL);
        }
    }

    TaskScheduler::TaskScheduler() {}

    void TaskScheduler::start_thread() { task_thread = std::thread(&TaskScheduler::thread_func, this); }

    TaskScheduler::~TaskScheduler()
    {
        running = false;
        cv.notify_one();
        if (task_thread.joinable())
            task_thread.join();
    }
} // namespace satdump