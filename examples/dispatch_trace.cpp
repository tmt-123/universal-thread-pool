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
    case task_priority::low: return "低";
    case task_priority::high: return "高";
    case task_priority::critical: return "紧急";
    default: return "普通";
    }
}

int main(int argc, char** argv) {
    try {
        const std::size_t workers = argc > 1 ? std::stoul(argv[1]) : 2;
        const std::string mode = argc > 2 ? argv[2] : "fifo";
        if (workers < 1 || workers > 8 || (mode != "fifo" && mode != "priority")) {
            std::cerr << "用法：thread_pool_dispatch_trace [工作线程数:1..8] [fifo|priority]\n";
            return 1;
        }
        // Declare logging state before pool so callbacks can use it until join.
        std::mutex output_mutex;
        const auto beginning = std::chrono::steady_clock::now();
        auto log = [&](const std::string& message) {
            std::lock_guard<std::mutex> lock(output_mutex);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - beginning).count();
            std::cout << '[' << elapsed << " 毫秒] " << message << std::endl;
        };
        auto event = [&](const char* stage, const task_metadata& task) {
            std::ostringstream line;
            line << stage << " 任务=" << task.name << " 编号=" << task.id
                 << " 优先级=" << priority_name(task.priority);
            if (task.worker_id != static_cast<std::size_t>(-1)) {
                line << " 工作线程=" << task.worker_id;
            }
            line << " 系统线程ID=" << std::this_thread::get_id();
            log(line.str());
        };

        thread_pool_options options;
        options.initial_threads = options.min_threads = options.max_threads = workers;
        options.worker_batch_size = 1;
        options.scheduler = mode == "priority" ? schedule_policy::priority : schedule_policy::fifo;
        options.on_thread_start = [&](std::size_t id) { log("线程启动 工作线程=" + std::to_string(id)); };
        options.on_thread_stop = [&](std::size_t id) { log("线程退出 工作线程=" + std::to_string(id)); };
        options.on_task_queued = [&](const task_metadata& task) { event("任务入队", task); };
        options.on_task_start = [&](const task_metadata& task) { event("开始执行", task); };
        options.on_task_finish = [&](const task_metadata& task) { event("执行完成", task); };
        thread_pool pool(options);
        pool.pause();
        log(std::string("暂停取任务：先提交六个任务，调度模式=") + (mode == "priority" ? "优先级" : "先进先出"));
        const task_priority priorities[] = {
            task_priority::low, task_priority::high, task_priority::normal,
            task_priority::critical, task_priority::low, task_priority::high};
        std::vector<std::future<int>> results;
        for (int index = 0; index < 6; ++index) {
            task_options task;
            task.name = "job-" + std::to_string(index + 1);
            task.priority = mode == "priority" ? priorities[index] : task_priority::normal;
            log("主线程提交任务 " + task.name);
            results.push_back(pool.submit_with_options(task, [&, index] {
                log("执行任务函数 job-" + std::to_string(index + 1) + "：模拟耗时 250 毫秒");
                std::this_thread::sleep_for(250ms);
                return (index + 1) * 10;
            }));
        }
        log("恢复调度：允许工作线程从队列取任务");
        pool.resume();
        // Reading futures in submission order does not change dispatch order.
        for (std::size_t index = 0; index < results.size(); ++index) {
            auto value = results[index].get();
            log("主线程取得结果 job-" + std::to_string(index + 1) + " = " + std::to_string(value));
        }
        if (!pool.wait_for_idle(2s)) throw std::runtime_error("线程池未在规定时间内进入空闲状态");
        const auto metrics = pool.metrics();
        log("线程池空闲 已完成=" + std::to_string(metrics.completed_tasks_total)
            + " 排队中=" + std::to_string(metrics.queued_tasks)
            + " 执行中=" + std::to_string(metrics.running_tasks));
        pool.shutdown(shutdown_policy::drain);
        pool.join();
        log("等待退出完成：所有工作线程已停止");
    } catch (const std::exception& error) {
        std::cerr << "派发过程示例失败：" << error.what() << '\n';
        return 1;
    }
}
