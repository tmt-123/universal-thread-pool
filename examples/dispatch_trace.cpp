#include "universal_thread_pool/thread_pool.hpp"

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace universal_thread_pool;
using namespace std::chrono_literals;

const char* priority_name(task_priority priority) {
    switch (priority) {
    case task_priority::low: return "low";
    case task_priority::high: return "high";
    case task_priority::critical: return "critical";
    default: return "normal";
    }
}

int main(int argc, char** argv) {
    try {
        const std::size_t workers = argc > 1 ? std::stoul(argv[1]) : 2;
        const std::string mode = argc > 2 ? argv[2] : "fifo";
        if (workers < 1 || workers > 8 || (mode != "fifo" && mode != "priority")) {
            std::cerr << "Usage: thread_pool_dispatch_trace [workers:1..8] [fifo|priority]\n";
            return 1;
        }
        // Declare logging state before pool so callbacks can use it until join.
        std::mutex output_mutex;
        const auto beginning = std::chrono::steady_clock::now();
        auto log = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(output_mutex);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginning).count();
            std::cout << '[' << elapsed << " ms] " << message << std::endl;
        };
        auto event = [&](const char* stage, const task_metadata& task) {
            std::ostringstream line;
            line << stage << " task=" << task.name << " id=" << task.id
                 << " priority=" << priority_name(task.priority);
            if (task.worker_id != static_cast<std::size_t>(-1)) {
                line << " worker=" << task.worker_id;
            }
            line << " thread=" << std::this_thread::get_id();
            log(line.str());
        };

        thread_pool_options options;
        options.initial_threads = options.min_threads = options.max_threads = workers;
        options.worker_batch_size = 1;
        options.scheduler = mode == "priority" ? schedule_policy::priority : schedule_policy::fifo;
        options.on_thread_start = [&](std::size_t id) { log("WORKER_UP worker=" + std::to_string(id)); };
        options.on_thread_stop = [&](std::size_t id) { log("WORKER_DOWN worker=" + std::to_string(id)); };
        options.on_task_queued = [&](const task_metadata& task) { event("QUEUED", task); };
        options.on_task_start = [&](const task_metadata& task) { event("START", task); };
        options.on_task_finish = [&](const task_metadata& task) { event("FINISH", task); };
        thread_pool pool(options);
        pool.pause();
        log("PAUSE: queue six tasks before starting workers; mode=" + mode);
        const task_priority priorities[] = {
            task_priority::low, task_priority::high, task_priority::normal,
            task_priority::critical, task_priority::low, task_priority::high};
        std::vector<std::future<int>> results;
        for (int index = 0; index < 6; ++index) {
            task_options task;
            task.name = "job-" + std::to_string(index + 1);
            task.priority = mode == "priority" ? priorities[index] : task_priority::normal;
            log("SUBMIT " + task.name);
            results.push_back(pool.submit_with_options(task, [&, index] {
                log("BODY job-" + std::to_string(index + 1) + ": simulated work 250 ms");
                std::this_thread::sleep_for(250ms);
                return (index + 1) * 10;
            }));
        }
        log("RESUME: workers may now take tasks");
        pool.resume();
        // Reading futures in submission order does not change dispatch order.
        for (std::size_t index = 0; index < results.size(); ++index) {
            auto value = results[index].get();
            log("RESULT job-" + std::to_string(index + 1) + " = " + std::to_string(value));
        }
        if (!pool.wait_for_idle(2s)) throw std::runtime_error("pool did not become idle");
        const auto metrics = pool.metrics();
        log("IDLE completed=" + std::to_string(metrics.completed_tasks_total)
            + " queued=" + std::to_string(metrics.queued_tasks)
            + " running=" + std::to_string(metrics.running_tasks));
        pool.shutdown(shutdown_policy::drain);
        pool.join();
        log("JOIN: all workers stopped");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
