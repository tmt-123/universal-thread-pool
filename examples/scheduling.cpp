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
        require(pool.detach([] {}), "first task must fit");
        require(!pool.detach([] {}), "full reject queue must reject");
        pool.resume();
        require(pool.wait_for_idle(2s), "reject queue did not drain");
        std::cout << "bounded_reject: second task rejected\n";
    }

    options.queue = queue_policy::bounded_block;
    {
        thread_pool pool(options);
        pool.pause();
        auto first = pool.submit([] { return 1; });
        // A timed submission bounds the producer's wait for queue space.
        auto second = pool.try_submit_for(10ms, [] { return 2; });
        require(!second, "full blocking queue must time out");
        pool.resume();
        require(first.get() == 1, "queued task result mismatch");
        std::cout << "bounded_block: timed submission expired\n";
    }

    options.queue = queue_policy::bounded_caller_runs;
    {
        thread_pool pool(options);
        pool.pause();
        auto first = pool.submit([] { return 1; });
        const auto caller = std::this_thread::get_id();
        auto second = pool.submit([] { return std::this_thread::get_id(); });
        require(second.get() == caller, "overflow task must run on caller");
        pool.resume();
        require(first.get() == 1, "queued task result mismatch");
        std::cout << "bounded_caller_runs: overflow executed on caller\n";
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
        require(cancelled, "oldest queued task must be cancelled");
        require(newest.get() == 2, "newest task must remain queued");
        std::cout << "bounded_drop_oldest: newest task retained\n";
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
            "low priority submission failed");
    require(pool.detach_with_priority(task_priority::critical, [&] { order.push_back(3); }),
            "critical priority submission failed");
    require(pool.detach_with_priority(task_priority::normal, [&] { order.push_back(2); }),
            "normal priority submission failed");
    require(order.empty(), "paused pool must leave new tasks queued");
    pool.resume();
    require(pool.wait_for_idle(2s), "priority tasks did not finish");
    require(order == std::vector<int>({3, 2, 1}), "priority order mismatch");
    std::cout << "pause/resume + priority: critical, normal, low\n";
}

void resize_workers() {
    auto options = single_worker_options();
    thread_pool pool(options);
    pool.resize(3);
    require(pool.thread_count() == 3, "pool did not grow");
    pool.resize(1);
    // Shrinking is asynchronous: workers retire after finishing current work.
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (pool.thread_count() != 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    require(pool.thread_count() == 1, "pool did not shrink");
    require(pool.submit([] { return 42; }).get() == 42, "resized pool cannot execute");
    std::cout << "resize: 1 -> 3 -> 1 workers\n";
}

} // namespace

int main() {
    try {
        bounded_queues();
        priority_and_pause();
        resize_workers();
    } catch (const std::exception& error) {
        std::cerr << "scheduling example failed: " << error.what() << '\n';
        return 1;
    }
}
