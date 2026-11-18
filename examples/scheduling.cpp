#include "universal_thread_pool/thread_pool.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace universal_thread_pool;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

thread_pool_options single_worker_options() {
    thread_pool_options options;
    options.initial_threads = 1;
    options.min_threads = 1;
    options.max_threads = 3;
    options.worker_batch_size = 1;
    options.max_queue_size = 1;
    return options;
}

void bounded_queues() {
    auto options = single_worker_options();
    options.queue = queue_policy::bounded_reject;
    {
        thread_pool pool(options);
        pool.pause();
        require(pool.detach([] {}), "第一个任务应成功入队");
        require(!pool.detach([] {}), "拒绝策略下，队列满时应拒绝新任务");
        pool.resume();
        require(pool.wait_for_idle(2s), "拒绝策略队列未按时清空");
        std::cout << "拒绝策略：队列已满，第二个任务被拒绝\n";
    }

    options.queue = queue_policy::bounded_block;
    {
        thread_pool pool(options);
        pool.pause();
        auto first = pool.submit([] { return 1; });
        // A timed submission bounds the producer's wait for queue space.
        auto second = pool.try_submit_for(10ms, [] { return 2; });
        require(!second, "阻塞策略下，满队列的限时提交应超时");
        pool.resume();
        require(first.get() == 1, "排队任务返回结果不符");
        std::cout << "阻塞策略：等待队列空位超时，本次提交结束\n";
    }

    options.queue = queue_policy::bounded_caller_runs;
    {
        thread_pool pool(options);
        pool.pause();
        auto first = pool.submit([] { return 1; });
        const auto caller = std::this_thread::get_id();
        auto second = pool.submit([] { return std::this_thread::get_id(); });
        require(second.get() == caller, "溢出任务应由提交线程执行");
        pool.resume();
        require(first.get() == 1, "排队任务返回结果不符");
        std::cout << "调用者执行策略：溢出任务已在提交线程中执行\n";
    }

    options.queue = queue_policy::bounded_drop_oldest;
    {
        thread_pool pool(options);
        pool.pause();
        auto oldest = pool.submit([] { return 1; });
        auto newest = pool.submit([] { return 2; });
        bool cancelled = false;
        try {
            (void)oldest.get();
        } catch (const task_cancelled&) {
            cancelled = true;
        }
        pool.resume();
        require(cancelled, "最旧的排队任务应被取消");
        require(newest.get() == 2, "新任务应被保留在队列中");
        std::cout << "丢弃最旧任务策略：旧任务已取消，新任务已保留\n";
    }
}

void priority_and_pause() {
    auto options = single_worker_options();
    options.scheduler = schedule_policy::priority;
    options.enable_priority = true;
    thread_pool pool(options);
    pool.pause();
    std::vector<int> order;
    require(pool.detach_with_priority(task_priority::low, [&] { order.push_back(1); }),
            "低优先级任务提交失败");
    require(pool.detach_with_priority(task_priority::critical, [&] { order.push_back(3); }),
            "紧急优先级任务提交失败");
    require(pool.detach_with_priority(task_priority::normal, [&] { order.push_back(2); }),
            "普通优先级任务提交失败");
    require(order.empty(), "暂停期间新任务应保留在队列中");
    pool.resume();
    require(pool.wait_for_idle(2s), "优先级任务未按时完成");
    require(order == std::vector<int>({3, 2, 1}), "优先级执行顺序不符");
    std::cout << "暂停恢复与优先级：执行顺序为紧急、普通、低\n";
}

void resize_workers() {
    auto options = single_worker_options();
    thread_pool pool(options);
    pool.resize(3);
    require(pool.thread_count() == 3, "线程池未成功扩容");
    pool.resize(1);
    // Shrinking is asynchronous: workers retire after finishing current work.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (pool.thread_count() != 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(pool.thread_count() == 1, "线程池未成功缩容");
    require(pool.submit([] { return 42; }).get() == 42, "调整大小后的线程池无法执行任务");
    std::cout << "调整工作线程数：1 -> 3 -> 1\n";
}

} // namespace

int main() {
    try {
        bounded_queues();
        priority_and_pause();
        resize_workers();
    } catch (const std::exception& error) {
        std::cerr << "调度示例失败：" << error.what() << '\n';
        return 1;
    }
}
