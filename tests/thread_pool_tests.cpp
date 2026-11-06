#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

#include "universal_thread_pool/thread_pool.hpp"

using namespace universal_thread_pool;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        std::cerr << file << ':' << line << ": check failed: " << expression << std::endl;
        std::abort();
    }
}

// Unlike assert, CHECK evaluates its expression in Release builds as well.
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

thread_pool_options base_options(std::size_t threads = 4) {
    thread_pool_options options;
    options.initial_threads = threads;
    options.max_threads = std::max<std::size_t>(threads, 8);
    return options;
}

void test_submit_and_future() {
    CHECK(version_major >= 0);
    CHECK(std::string(version_string).size() > 0);

    thread_pool pool(base_options(2));

    auto a = pool.submit([] { return 20; });
    auto b = pool.submit([](int x) { return x + 2; }, 20);

    CHECK(a.get() == 20);
    CHECK(b.get() == 22);
    CHECK(pool.wait_for_idle(2s));

    const auto metrics = pool.metrics();
    CHECK(metrics.submitted_tasks_total == 2);
    CHECK(metrics.completed_tasks_total == 2);
    CHECK(metrics.failed_tasks_total == 0);
}

void test_dispatch_inline_counts_submission() {
    thread_pool pool(base_options(1));

    auto dispatched = pool.submit([&pool] {
        bool ran_inline = false;
        const bool ok = pool.dispatch([&ran_inline] {
            ran_inline = true;
        });

        CHECK(ran_inline);
        return ok;
    });

    CHECK(dispatched.get());
    CHECK(pool.wait_for_idle(2s));

    const auto metrics = pool.metrics();
    CHECK(metrics.submitted_tasks_total == 2);
    CHECK(metrics.completed_tasks_total == 2);
    CHECK(metrics.failed_tasks_total == 0);
}

void test_detach_and_wait_for_idle() {
    thread_pool pool(base_options(4));
    std::atomic<int> counter{0};

    for (int i = 0; i < 1000; ++i) {
        CHECK(pool.detach([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    CHECK(pool.wait_for_idle(5s));
    CHECK(counter.load(std::memory_order_relaxed) == 1000);
}

void test_exception_propagates_from_submit() {
    thread_pool pool(base_options(2));

    auto future = pool.submit([]() -> int {
        throw std::runtime_error("boom");
    });

    bool threw = false;
    try {
        (void)future.get();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(pool.wait_for_idle(2s));

    const auto metrics = pool.metrics();
    CHECK(metrics.completed_tasks_total == 1);
    CHECK(metrics.failed_tasks_total == 0);
}

void test_detached_exception_is_counted() {
    thread_pool pool(base_options(2));

    CHECK(pool.detach([] {
        throw std::runtime_error("detached boom");
    }));

    CHECK(pool.wait_for_idle(2s));

    const auto metrics = pool.metrics();
    CHECK(metrics.failed_tasks_total == 1);
}

void test_pause_and_resume() {
    thread_pool_options options = base_options(2);
    options.queue = queue_policy::bounded_reject;
    options.max_queue_size = 8;

    thread_pool pool(options);
    pool.pause();

    std::atomic<int> counter{0};
    CHECK(pool.detach([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));

    std::this_thread::sleep_for(100ms);
    CHECK(counter.load(std::memory_order_relaxed) == 0);

    pool.resume();
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 1);
}

void test_bounded_reject() {
    thread_pool_options options = base_options(1);
    options.queue = queue_policy::bounded_reject;
    options.max_queue_size = 1;

    thread_pool pool(options);
    pool.pause();

    CHECK(pool.detach([] {}));
    CHECK(!pool.detach([] {}));

    pool.resume();
    CHECK(pool.wait_for_idle(2s));
}

void test_try_submit_and_try_detach() {
    thread_pool_options options = base_options(1);
    options.queue = queue_policy::bounded_block;
    options.max_queue_size = 1;

    thread_pool pool(options);
    pool.pause();

    auto accepted = pool.try_submit([] {
        return 1;
    });
    CHECK(accepted);

    auto rejected = pool.try_submit([] {
        return 2;
    });
    CHECK(!rejected);

    auto timed_rejected = pool.try_submit_for(1ms, [] {
        return 5;
    });
    CHECK(!timed_rejected);

    task_options named;
    named.name = "try-submit-with-options";
    auto rejected_with_options = pool.try_submit_with_options(named, [] {
        return 3;
    });
    CHECK(!rejected_with_options);

    auto timed_options = pool.try_submit_with_options_for(named, 1ms, [] {
        return 6;
    });
    CHECK(!timed_options);

    CHECK(!pool.try_detach([] {}));
    CHECK(!pool.try_detach_for(1ms, [] {}));

    pool.resume();
    CHECK(accepted->get() == 1);
    CHECK(pool.wait_for_idle(2s));

    auto executor_future = pool.get_executor().try_submit_for(1s, [] {
        return 4;
    });
    CHECK(executor_future);
    CHECK(executor_future->get() == 4);

    std::atomic<int> detached{0};
    CHECK(pool.get_executor().try_detach_for(1s, [&detached] {
        detached.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(pool.wait_for_idle(2s));
    CHECK(detached.load(std::memory_order_relaxed) == 1);
}

void test_executor_task_options() {
    thread_pool_options options = base_options(2);
    std::mutex names_mutex;
    std::vector<std::string> names;
    std::vector<task_priority> priorities;
    options.on_task_start = [&](const task_metadata& metadata) {
        if (!metadata.name.empty()) {
            std::lock_guard<std::mutex> lock(names_mutex);
            names.push_back(metadata.name);
            priorities.push_back(metadata.priority);
        }
    };

    thread_pool pool(options);
    auto ex = pool.get_executor(task_priority::high);

    task_options submit_options;
    submit_options.name = "executor-submit";
    submit_options.source = UNIVERSAL_THREAD_POOL_SOURCE_LOCATION;
    auto value = ex.submit_with_options(submit_options, [] {
        return 9;
    });

    task_options detach_options;
    detach_options.name = "executor-detach";
    detach_options.priority = task_priority::critical;
    detach_options.source = UNIVERSAL_THREAD_POOL_SOURCE_LOCATION;
    CHECK(ex.detach_with_options(detach_options, [] {}));

    CHECK(value.get() == 9);
    CHECK(pool.wait_for_idle(2s));

    std::lock_guard<std::mutex> lock(names_mutex);
    CHECK(std::find(names.begin(), names.end(), "executor-submit") != names.end());
    CHECK(std::find(names.begin(), names.end(), "executor-detach") != names.end());
    CHECK(std::find(priorities.begin(), priorities.end(), task_priority::high) != priorities.end());
    CHECK(std::find(priorities.begin(), priorities.end(), task_priority::critical) != priorities.end());
}

void test_bulk_options_and_executor_bulk() {
    thread_pool_options options = base_options(2);
    std::mutex names_mutex;
    std::vector<std::string> names;
    std::vector<task_priority> priorities;
    options.on_task_start = [&](const task_metadata& metadata) {
        if (!metadata.name.empty()) {
            std::lock_guard<std::mutex> lock(names_mutex);
            names.push_back(metadata.name);
            priorities.push_back(metadata.priority);
        }
    };

    thread_pool pool(options);

    task_options bulk_options;
    bulk_options.name = "bulk-options";
    bulk_options.priority = task_priority::high;
    auto futures = pool.bulk_submit_with_options(bulk_options, 4, [](std::size_t index) {
        return static_cast<int>(index * 3);
    });

    auto ex = pool.get_executor(task_priority::low);
    task_options executor_bulk_options;
    executor_bulk_options.name = "executor-bulk-options";
    auto executor_futures = ex.bulk_submit_with_options(executor_bulk_options, 2, [](std::size_t index) {
        return static_cast<int>(index + 10);
    });

    std::atomic<int> detached{0};
    task_options detach_options;
    detach_options.name = "bulk-detach-options";
    detach_options.priority = task_priority::critical;
    CHECK(pool.bulk_detach_with_options(detach_options, 3, [&detached](std::size_t) {
        detached.fetch_add(1, std::memory_order_relaxed);
    }) == 3);

    task_options executor_detach_options;
    executor_detach_options.name = "executor-bulk-detach-options";
    CHECK(ex.bulk_detach_with_options(executor_detach_options, 2, [&detached](std::size_t) {
        detached.fetch_add(1, std::memory_order_relaxed);
    }) == 2);

    CHECK(futures.back().get() == 9);
    CHECK(executor_futures.back().get() == 11);
    CHECK(pool.wait_for_idle(2s));
    CHECK(detached.load(std::memory_order_relaxed) == 5);

    std::lock_guard<std::mutex> lock(names_mutex);
    CHECK(std::find(names.begin(), names.end(), "bulk-options") != names.end());
    CHECK(std::find(names.begin(), names.end(), "executor-bulk-options") != names.end());
    CHECK(std::find(names.begin(), names.end(), "bulk-detach-options") != names.end());
    CHECK(std::find(priorities.begin(), priorities.end(), task_priority::high) != priorities.end());
    CHECK(std::find(priorities.begin(), priorities.end(), task_priority::low) != priorities.end());
    CHECK(std::find(priorities.begin(), priorities.end(), task_priority::critical) != priorities.end());
}

void test_caller_runs() {
    thread_pool_options options = base_options(1);
    options.queue = queue_policy::bounded_caller_runs;
    options.max_queue_size = 1;

    thread_pool pool(options);
    pool.pause();

    std::atomic<int> counter{0};
    CHECK(pool.detach([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));
    CHECK(pool.detach([&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));

    CHECK(counter.load(std::memory_order_relaxed) == 1);

    pool.resume();
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 2);

    const auto metrics = pool.metrics();
    CHECK(metrics.caller_runs_total == 1);
}

void test_priority_order() {
    thread_pool_options options = base_options(1);
    options.scheduler = schedule_policy::priority;
    options.enable_priority = true;
    options.max_queue_size = 8;

    thread_pool pool(options);
    pool.pause();

    std::vector<int> order;
    std::mutex order_mutex;

    CHECK(pool.detach_with_priority(task_priority::low, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(1);
    }));
    CHECK(pool.detach_with_priority(task_priority::critical, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(2);
    }));
    CHECK(pool.detach_with_priority(task_priority::normal, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(3);
    }));

    pool.resume();
    CHECK(pool.wait_for_idle(2s));

    CHECK(order.size() == 3);
    CHECK(order[0] == 2);
    CHECK(order[1] == 3);
    CHECK(order[2] == 1);
}

void test_priority_fairness_interval() {
    thread_pool_options options = base_options(1);
    options.scheduler = schedule_policy::priority;
    options.priority_fairness_interval = 1;
    options.max_queue_size = 8;

    thread_pool pool(options);
    pool.pause();

    std::vector<int> order;
    std::mutex order_mutex;

    CHECK(pool.detach_with_priority(task_priority::low, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(1);
    }));
    CHECK(pool.detach_with_priority(task_priority::critical, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(2);
    }));
    CHECK(pool.detach_with_priority(task_priority::critical, [&] {
        std::lock_guard<std::mutex> lock(order_mutex);
        order.push_back(3);
    }));

    pool.resume();
    CHECK(pool.wait_for_idle(2s));

    CHECK(order.size() == 3);
    CHECK(order[0] == 2);
    CHECK(order[1] == 1);
    CHECK(order[2] == 3);
    CHECK(pool.metrics().priority_fairness_picks_total >= 1);
}

void test_resize_grow_and_shrink() {
    thread_pool pool(base_options(1));
    CHECK(pool.thread_count() == 1);

    pool.resize(3);
    for (int i = 0; i < 50 && pool.thread_count() < 3; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pool.thread_count() == 3);

    pool.resize(1);
    for (int i = 0; i < 100 && pool.thread_count() > 1; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pool.thread_count() == 1);
}

void test_resize_shrink_is_idempotent() {
    thread_pool_options options = base_options(6);
    options.initial_threads = 6;
    options.min_threads = 1;
    options.max_threads = 6;
    options.worker_batch_size = 1;

    thread_pool pool(options);
    CHECK(pool.thread_count() == 6);

    for (int i = 0; i < 8; ++i) {
        pool.resize(2);
    }

    for (int i = 0; i < 100 && pool.thread_count() > 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pool.thread_count() == 2);

    for (int i = 0; i < 8; ++i) {
        pool.resize(2);
    }
    std::this_thread::sleep_for(50ms);
    CHECK(pool.thread_count() == 2);
}

void test_resize_grow_cancels_pending_retirements() {
    thread_pool_options options = base_options(6);
    options.initial_threads = 6;
    options.min_threads = 1;
    options.max_threads = 6;
    options.worker_batch_size = 1;

    thread_pool pool(options);
    std::atomic<bool> release{false};
    std::atomic<int> started{0};

    for (int i = 0; i < 6; ++i) {
        CHECK(pool.detach([&] {
            started.fetch_add(1, std::memory_order_relaxed);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(1ms);
            }
        }));
    }

    for (int i = 0; i < 100 && started.load(std::memory_order_relaxed) < 6; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(started.load(std::memory_order_relaxed) == 6);

    pool.resize(2);
    pool.resize(6);

    release.store(true, std::memory_order_release);
    CHECK(pool.wait_for_idle(2s));

    for (int i = 0; i < 100 && pool.thread_count() < 6; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pool.thread_count() == 6);
}

void test_parallel_for() {
    thread_pool pool(base_options(4));
    std::vector<int> values(1000, 0);

    loop_options options;
    options.block_size = 17;

    pool.parallel_for<std::size_t>(0, values.size(), [&](std::size_t i) {
        values[i] = static_cast<int>(i);
    }, options);

    CHECK(values[0] == 0);
    CHECK(values[999] == 999);
    CHECK(std::accumulate(values.begin(), values.end(), 0) == 499500);

    std::vector<int> shifted(10, 0);
    pool.parallel_for<int>(5, 15, [&](int i) {
        shifted[static_cast<std::size_t>(i - 5)] = i;
    }, options);
    CHECK(shifted.front() == 5);
    CHECK(shifted.back() == 14);
}

void test_parallel_for_each_and_transform() {
    thread_pool pool(base_options(4));
    std::vector<int> values(100, 1);
    loop_options options;
    options.block_size = 5;

    pool.parallel_for_each(values.begin(), values.end(), [](int& value) {
        value += 1;
    }, options);

    std::vector<int> transformed(values.size(), 0);
    pool.parallel_transform(values.begin(), values.end(), transformed.begin(), [](int value) {
        return value * 3;
    }, options);

    CHECK(values.front() == 2);
    CHECK(values.back() == 2);
    CHECK(transformed.front() == 6);
    CHECK(transformed.back() == 6);
}

void test_parallel_schedules_and_reduce() {
    thread_pool pool(base_options(4));
    std::vector<int> dynamic_values(1000, 0);
    std::vector<int> guided_values(1000, 0);

    loop_options dynamic;
    dynamic.schedule = loop_schedule::dynamic_blocks;
    dynamic.block_size = 13;

    pool.parallel_for<std::size_t>(0, dynamic_values.size(), [&](std::size_t i) {
        dynamic_values[i] = static_cast<int>(i);
    }, dynamic);

    loop_options guided;
    guided.schedule = loop_schedule::guided_blocks;
    guided.block_size = 7;

    pool.parallel_for<std::size_t>(0, guided_values.size(), [&](std::size_t i) {
        guided_values[i] = static_cast<int>(i);
    }, guided);

    CHECK(std::accumulate(dynamic_values.begin(), dynamic_values.end(), 0) == 499500);
    CHECK(std::accumulate(guided_values.begin(), guided_values.end(), 0) == 499500);

    const auto reduced = pool.parallel_reduce<int>(
        0,
        1000,
        0,
        [](int a, int b) {
            return a + b;
        },
        guided);
    CHECK(reduced == 499500);
}

void test_parallel_3d_and_nd() {
    thread_pool pool(base_options(4));
    std::vector<int> grid(4 * 3 * 2, 0);

    pool.parallel_for_3d<int>(0, 4, 0, 3, 0, 2, [&](int x, int y, int z) {
        grid[static_cast<std::size_t>((x * 3 + y) * 2 + z)] = x + y + z;
    });

    CHECK(grid.front() == 0);
    CHECK(grid.back() == 3 + 2 + 1);

    std::atomic<int> count{0};
    pool.parallel_for_nd<int, 4>(
        std::array<int, 4>{0, 0, 0, 0},
        std::array<int, 4>{2, 2, 2, 2},
        [&](int, int, int, int) {
            count.fetch_add(1, std::memory_order_relaxed);
        });

    CHECK(count.load(std::memory_order_relaxed) == 16);
}

void test_cached_pool_growth_and_worker_context() {
    auto options = make_cached_pool_options(0, 4);
    options.queue_expand_threshold = 1;
    options.idle_timeout = 50ms;

    thread_pool pool(options);
    CHECK(pool.thread_count() == 0);

    std::vector<std::future<bool>> futures;
    for (int i = 0; i < 4; ++i) {
        futures.push_back(pool.submit([&pool] {
            std::this_thread::sleep_for(20ms);
            return pool.is_worker_thread() &&
                thread_pool::current_worker_id() != static_cast<std::size_t>(-1);
        }));
    }

    for (auto& future : futures) {
        CHECK(future.get());
    }

    CHECK(pool.wait_for_idle(2s));
    for (int i = 0; i < 50 && pool.thread_count() != 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(pool.thread_count() == 0);

    const auto metrics = pool.metrics();
    CHECK(metrics.total_threads == 0);
    CHECK(metrics.idle_threads == 0);
}

void test_task_group() {
    thread_pool pool(base_options(4));
    task_group group(pool.get_executor());
    std::atomic<int> counter{0};

    auto grouped_value = group.submit([] {
        return 42;
    });
    auto maybe_grouped = group.try_submit_for(1s, [] {
        return 7;
    });
    CHECK(maybe_grouped);
    auto token_value = group.submit_with_token(
        [](cancellation_token token, int value) {
            return token.stop_requested() ? -1 : value + 1;
        },
        8);
    auto maybe_token_value = group.try_submit_with_token_for(
        1s,
        [](cancellation_token token) {
            return token.stop_requested() ? -1 : 10;
        });
    CHECK(maybe_token_value);

    for (int i = 0; i < 100; ++i) {
        group.run([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
    }
    group.run_with_token([&counter](cancellation_token token) {
        if (!token.stop_requested()) {
            counter.fetch_add(1, std::memory_order_relaxed);
        }
    });

    group.wait();
    CHECK(group.pending() == 0);
    CHECK(counter.load(std::memory_order_relaxed) == 101);
    CHECK(grouped_value.get() == 42);
    CHECK(maybe_grouped->get() == 7);
    CHECK(token_value.get() == 9);
    CHECK(maybe_token_value->get() == 10);

    task_group cleanup_group(pool.get_executor());
    cleanup_group.run([] {});
    CHECK(pool.wait_for_idle(2s));
    CHECK(cleanup_group.clear_ready() == 1);
    CHECK(cleanup_group.pending() == 0);

    cleanup_group.cancel();
    CHECK(cleanup_group.stop_requested());
    CHECK(cleanup_group.token().stop_requested());
}

void test_scoped_task_group() {
    thread_pool pool(base_options(2));
    std::atomic<int> counter{0};

    {
        scoped_task_group scope(pool.get_executor());
        scope.run([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        });
        auto value = scope.submit_with_token([](cancellation_token token) {
            return token.stop_requested() ? -1 : 11;
        });
        scope.wait();
        CHECK(value.get() == 11);
    }

    CHECK(counter.load(std::memory_order_relaxed) == 1);

    {
        scoped_task_group scope(
            pool.get_executor(),
            scoped_task_group_policy::cancel_and_wait);
        scope.cancel();
        auto cancelled = scope.submit_with_token([](cancellation_token token) {
            return token.stop_requested() ? -1 : 12;
        });
        bool wait_threw = false;
        try {
            scope.wait();
        } catch (const task_cancelled&) {
            wait_threw = true;
        }
        CHECK(wait_threw);
        bool threw = false;
        try {
            (void)cancelled.get();
        } catch (const task_cancelled&) {
            threw = true;
        }
        CHECK(threw);
    }
}

void test_submit_after() {
    thread_pool pool(base_options(2));

    const auto start = std::chrono::steady_clock::now();
    auto future = pool.submit_after(80ms, [] {
        return 77;
    });

    CHECK(future.get() == 77);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed >= 60ms);

    const auto metrics = pool.metrics();
    CHECK(metrics.scheduled_tasks_total == 1);
}

void test_schedule_submit_after_and_cancel() {
    thread_pool pool(base_options(2));

    auto scheduled = pool.schedule_submit_after(40ms, [] {
        return 78;
    });
    CHECK(scheduled);
    CHECK(scheduled.future.get() == 78);

    auto cancelled = pool.schedule_submit_after(40ms, [] {
        return 79;
    });
    CHECK(cancelled.cancel());
    CHECK(cancelled.cancelled());

    bool threw = false;
    try {
        (void)cancelled.future.get();
    } catch (const task_cancelled&) {
        threw = true;
    }
    CHECK(threw);

    task_options options;
    options.name = "scheduled-options";
    options.priority = task_priority::high;
    auto with_options = pool.schedule_submit_after_with_options(options, 1ms, [] {
        return 80;
    });
    CHECK(with_options.future.get() == 80);
}

void test_detach_after_and_cancel() {
    thread_pool pool(base_options(2));
    std::atomic<int> counter{0};

    auto handle = pool.detach_after(40ms, [&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    });
    CHECK(handle);

    std::this_thread::sleep_for(120ms);
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 1);

    auto cancelled = pool.detach_after(40ms, [&counter] {
        counter.fetch_add(100, std::memory_order_relaxed);
    });
    CHECK(cancelled.cancel());
    CHECK(cancelled.cancelled());

    std::this_thread::sleep_for(120ms);
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 1);

    const auto metrics = pool.metrics();
    CHECK(metrics.scheduled_tasks_total == 2);
    CHECK(metrics.scheduled_tasks_cancelled_total >= 1);
}

void test_cancellable_tasks() {
    thread_pool pool(base_options(2));
    pool.pause();

    cancellation_source source;
    std::atomic<int> counter{0};

    auto future = pool.submit_cancellable(source.token(), [] {
        return 42;
    });
    CHECK(pool.detach_cancellable(source.token(), [&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));

    source.request_stop();
    pool.resume();

    bool threw = false;
    try {
        (void)future.get();
    } catch (const task_cancelled&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 0);

    const auto metrics = pool.metrics();
    CHECK(metrics.cancelled_tasks_total >= 2);
}

void test_cancel_pending_sets_task_cancelled() {
    thread_pool_options options = base_options(1);
    options.queue = queue_policy::bounded_reject;
    options.max_queue_size = 4;
    std::atomic<int> cancelled_hook_count{0};
    options.on_task_cancel = [&cancelled_hook_count](const task_metadata&) {
        cancelled_hook_count.fetch_add(1, std::memory_order_relaxed);
    };

    thread_pool pool(options);
    pool.pause();

    auto future = pool.submit([] {
        return 42;
    });

    pool.shutdown(shutdown_policy::cancel_pending);

    bool threw = false;
    try {
        (void)future.get();
    } catch (const task_cancelled&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(cancelled_hook_count.load(std::memory_order_relaxed) >= 1);

    const auto metrics = pool.metrics();
    CHECK(metrics.cancelled_tasks_total >= 1);
}

void test_drop_oldest_sets_task_cancelled() {
    thread_pool_options options = base_options(1);
    options.queue = queue_policy::bounded_drop_oldest;
    options.max_queue_size = 1;

    thread_pool pool(options);
    pool.pause();

    auto dropped = pool.submit([] {
        return 1;
    });
    auto kept = pool.submit([] {
        return 2;
    });

    bool threw = false;
    try {
        (void)dropped.get();
    } catch (const task_cancelled&) {
        threw = true;
    }
    CHECK(threw);

    pool.resume();
    CHECK(kept.get() == 2);
    CHECK(pool.wait_for_idle(2s));
}

void test_deadline_tasks() {
    thread_pool pool(base_options(2));
    pool.pause();

    std::atomic<int> counter{0};
    auto future = pool.submit_with_timeout(10ms, [] {
        return 42;
    });
    CHECK(pool.detach_with_timeout(10ms, [&counter] {
        counter.fetch_add(1, std::memory_order_relaxed);
    }));

    std::this_thread::sleep_for(40ms);
    pool.resume();

    bool threw = false;
    try {
        (void)future.get();
    } catch (const task_cancelled&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(pool.wait_for_idle(2s));
    CHECK(counter.load(std::memory_order_relaxed) == 0);

    const auto metrics = pool.metrics();
    CHECK(metrics.cancelled_tasks_total >= 2);
}

void test_submit_then() {
    thread_pool pool(base_options(2));

    auto first = pool.submit([] {
        return 41;
    });
    auto second = pool.submit_then(std::move(first), [](int value) {
        return value + 1;
    });
    CHECK(second.get() == 42);

    auto void_first = pool.submit([] {});
    auto after_void = pool.get_executor().submit_then(std::move(void_first), [] {
        return 9;
    });
    CHECK(after_void.get() == 9);
}

void test_submit_then_does_not_occupy_worker_while_waiting() {
    thread_pool pool(base_options(1));
    std::promise<int> predecessor;

    auto continued = pool.submit_then(predecessor.get_future(), [](int value) {
        return value + 1;
    });

    CHECK(pool.detach([predecessor = std::move(predecessor)]() mutable {
        predecessor.set_value(41);
    }));

    CHECK(continued.get() == 42);
    CHECK(pool.wait_for_idle(2s));
}

void test_continue_with_and_dataflow() {
    thread_pool pool(base_options(2));

    auto first = pool.submit([] {
        return 21;
    });
    auto continued = pool.continue_with(std::move(first), [](int value) {
        return value * 2;
    });
    CHECK(continued.get() == 42);

    auto void_first = pool.submit([] {});
    auto after_void = pool.get_executor().continue_with(std::move(void_first), [] {
        return 7;
    });
    CHECK(after_void.get() == 7);

    std::vector<std::future<int>> inputs;
    for (int i = 1; i <= 4; ++i) {
        inputs.push_back(pool.submit([i] {
            return i;
        }));
    }

    auto summed = pool.dataflow(std::move(inputs), [](std::vector<int> values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });
    CHECK(summed.get() == 10);
}

void test_continuation_scheduler_and_numa_hooks() {
    thread_pool_options options = base_options(4);
    options.continuation_threads = 2;
    options.numa_nodes = 2;

    std::mutex mutex;
    std::vector<std::size_t> affinity_nodes;
    std::vector<std::size_t> task_nodes;
    options.on_thread_affinity = [&](std::size_t, std::size_t numa_node) {
        std::lock_guard<std::mutex> lock(mutex);
        affinity_nodes.push_back(numa_node);
    };
    options.on_task_start = [&](const task_metadata& metadata) {
        if (metadata.name == "numa-preferred") {
            std::lock_guard<std::mutex> lock(mutex);
            task_nodes.push_back(metadata.numa_node);
        }
    };

    thread_pool pool(options);

    auto base = pool.submit([] {
        return 20;
    });
    auto continued = pool.continue_with(std::move(base), [](int value) {
        return value + 2;
    });
    CHECK(continued.get() == 22);

    task_options task;
    task.name = "numa-preferred";
    task.numa_node = 1;
    auto value = pool.submit_with_options(task, [] {
        return 23;
    });
    CHECK(value.get() == 23);
    CHECK(pool.wait_for_idle(2s));

    std::lock_guard<std::mutex> lock(mutex);
    CHECK(!affinity_nodes.empty());
    CHECK(std::find(task_nodes.begin(), task_nodes.end(), 1) != task_nodes.end());
}

void test_completion_executor_worker_metrics_and_globals() {
    thread_pool waiter(base_options(2));
    thread_pool completion(base_options(2));

    auto first = waiter.submit([] {
        return 20;
    });
    auto continued = waiter.continue_on(
        completion.get_executor(task_priority::high),
        std::move(first),
        [](int value) {
            return value + 22;
        });
    CHECK(continued.get() == 42);

    std::vector<std::future<int>> inputs;
    inputs.push_back(waiter.submit([] {
        return 2;
    }));
    inputs.push_back(waiter.submit([] {
        return 3;
    }));
    auto flow = waiter.get_executor().dataflow_on(
        completion.get_executor(),
        std::move(inputs),
        [](std::vector<int> values) {
            return std::accumulate(values.begin(), values.end(), 1, std::multiplies<int>());
        });
    CHECK(flow.get() == 6);

    auto global_value = global_thread_pool().submit([] {
        return 5;
    });
    CHECK(global_value.get() == 5);

    auto runtime_value = global_runtime().continue_on(
        task_kind::blocking_io,
        task_kind::cpu_bound,
        global_runtime().submit_io([] {
            return 7;
        }),
        [](int value) {
            return value + 1;
        });
    CHECK(runtime_value.get() == 8);

    CHECK(waiter.wait_for_idle(2s));
    CHECK(completion.wait_for_idle(2s));
    const auto metrics = completion.metrics();
    CHECK(!metrics.workers.empty());
    CHECK(metrics.completed_tasks_total >= 2);
    CHECK(metrics.average_wait_time_ns() >= 0.0);
    CHECK(metrics.average_run_time_ns() >= 0.0);
    CHECK(metrics.wait_time_sample_count() >= 2);
    CHECK(metrics.run_time_sample_count() >= 2);
    CHECK(metrics.wait_time_percentile_estimate_ns(0.95) >= 0.0);
    CHECK(metrics.run_time_percentile_estimate_ns(0.95) >= 0.0);
    CHECK(metrics.pending_tasks_total() == 0);

    const auto prometheus = to_prometheus(metrics, "completion");
    CHECK(prometheus.find("universal_thread_pool_tasks_submitted_total") != std::string::npos);
    CHECK(prometheus.find("universal_thread_pool_task_wait_time_ns_bucket") != std::string::npos);
}

void test_when_all_values() {
    thread_pool pool(base_options(4));
    std::vector<std::future<int>> futures;

    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool.submit([i] {
            return i * i;
        }));
    }

    auto all = pool.when_all(std::move(futures));
    const auto values = all.get();

    CHECK(values.size() == 5);
    CHECK(values[0] == 0);
    CHECK(values[1] == 1);
    CHECK(values[2] == 4);
    CHECK(values[3] == 9);
    CHECK(values[4] == 16);
}

void test_when_all_does_not_occupy_worker_while_waiting() {
    thread_pool pool(base_options(1));
    std::promise<int> first;
    std::promise<int> second;
    std::vector<std::future<int>> futures;
    futures.push_back(first.get_future());
    futures.push_back(second.get_future());

    auto all = pool.when_all(std::move(futures));
    CHECK(pool.detach(
        [first = std::move(first), second = std::move(second)]() mutable {
            first.set_value(20);
            second.set_value(22);
        }));

    const auto values = all.get();
    CHECK(values.size() == 2);
    CHECK(values[0] == 20);
    CHECK(values[1] == 22);
    CHECK(pool.wait_for_idle(2s));
}

void test_when_all_void() {
    thread_pool pool(base_options(4));
    std::vector<std::future<void>> futures;
    std::atomic<int> counter{0};

    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit([&counter] {
            counter.fetch_add(1, std::memory_order_relaxed);
        }));
    }

    auto all = pool.get_executor().when_all(std::move(futures));
    all.get();
    CHECK(counter.load(std::memory_order_relaxed) == 8);
}

void test_when_any_values() {
    thread_pool pool(base_options(4));
    std::promise<int> first;
    std::promise<int> second;
    std::promise<int> third;
    std::vector<std::future<int>> futures;

    futures.push_back(first.get_future());
    futures.push_back(second.get_future());
    futures.push_back(third.get_future());

    auto any = pool.when_any(std::move(futures));
    second.set_value(2);
    const auto result = any.get();

    CHECK(result.index == 1);
    CHECK(result.value == 2);
}

void test_when_any_does_not_occupy_worker_while_waiting() {
    thread_pool pool(base_options(1));
    std::promise<int> winner;
    std::vector<std::future<int>> futures;
    futures.push_back(winner.get_future());

    auto any = pool.when_any(std::move(futures));
    CHECK(pool.detach([winner = std::move(winner)]() mutable {
        winner.set_value(7);
    }));

    const auto result = any.get();
    CHECK(result.index == 0);
    CHECK(result.value == 7);
    CHECK(pool.wait_for_idle(2s));
}

void test_when_any_void_and_empty() {
    thread_pool pool(base_options(4));
    std::promise<void> first;
    std::promise<void> second;
    std::vector<std::future<void>> futures;
    std::atomic<int> marker{0};

    futures.push_back(first.get_future());
    futures.push_back(second.get_future());

    auto any = pool.get_executor().when_any(std::move(futures));
    pool.detach([&] {
        marker.store(2, std::memory_order_release);
        second.set_value();
    });

    const auto result = any.get();

    CHECK(result.index == 1);
    CHECK(marker.load(std::memory_order_acquire) == 2);

    bool threw = false;
    try {
        (void)pool.when_any(std::vector<std::future<int>>{});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

void test_wait_for_idle_ready_work_semantics() {
    thread_pool pool(base_options(1));

    auto delayed = pool.submit_after(200ms, [] {
        return 1;
    });

    CHECK(pool.wait_for_idle(20ms));
    CHECK(delayed.wait_for(20ms) == std::future_status::timeout);
}

void test_work_stealing_local_tasks() {
    thread_pool_options options = base_options(4);
    options.scheduler = schedule_policy::work_stealing;
    options.enable_work_stealing = true;
    options.initial_threads = 4;
    options.max_threads = 4;

    thread_pool pool(options);
    std::atomic<int> counter{0};

    auto root = pool.submit([&] {
        for (int i = 0; i < 200; ++i) {
            CHECK(pool.detach([&counter] {
                counter.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(1ms);
            }));
        }

        std::this_thread::sleep_for(150ms);
    });

    root.get();
    CHECK(pool.wait_for_idle(5s));
    CHECK(counter.load(std::memory_order_relaxed) == 200);

    const auto metrics = pool.metrics();
    CHECK(metrics.local_tasks_total >= 200);
    CHECK(metrics.steal_success_total > 0);
}

void test_task_graph_dependencies() {
    thread_pool pool(base_options(4));
    task_graph graph;

    std::atomic<bool> a_done{false};
    std::atomic<bool> b_done{false};
    std::atomic<bool> c_done{false};
    std::atomic<bool> d_done{false};

    const auto a = graph.emplace([&] {
        std::this_thread::sleep_for(10ms);
        a_done.store(true, std::memory_order_release);
    });
    const auto b = graph.emplace([&] {
        b_done.store(true, std::memory_order_release);
    });
    const auto c = graph.emplace([&] {
        CHECK(a_done.load(std::memory_order_acquire));
        CHECK(b_done.load(std::memory_order_acquire));
        c_done.store(true, std::memory_order_release);
    });
    const auto d = graph.emplace([&] {
        CHECK(c_done.load(std::memory_order_acquire));
        d_done.store(true, std::memory_order_release);
    });

    graph.precede(a, c);
    graph.precede(b, c);
    graph.precede(c, d);

    auto future = graph.run(pool.get_executor());
    future.get();

    CHECK(a_done.load(std::memory_order_acquire));
    CHECK(b_done.load(std::memory_order_acquire));
    CHECK(c_done.load(std::memory_order_acquire));
    CHECK(d_done.load(std::memory_order_acquire));
}

void test_task_graph_exception() {
    thread_pool pool(base_options(2));
    task_graph graph;
    std::atomic<bool> dependent_ran{false};

    const auto a = graph.emplace([] {
        throw std::runtime_error("graph boom");
    });
    const auto b = graph.emplace([&] {
        dependent_ran.store(true, std::memory_order_release);
    });
    graph.precede(a, b);

    auto future = graph.run(pool.get_executor());

    bool threw = false;
    try {
        future.get();
    } catch (const std::runtime_error&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(!dependent_ran.load(std::memory_order_acquire));
}

void test_task_graph_cycle_detection() {
    thread_pool pool(base_options(2));
    task_graph graph;

    const auto a = graph.emplace([] {});
    const auto b = graph.emplace([] {});
    graph.precede(a, b);
    graph.precede(b, a);

    bool threw = false;
    try {
        (void)graph.run(pool.get_executor());
    } catch (const std::runtime_error&) {
        threw = true;
    }

    CHECK(threw);
}

void test_thread_pool_runtime_routing() {
    thread_pool_runtime runtime;
    std::atomic<int> background_counter{0};

    auto cpu = runtime.submit_cpu([] {
        return 21 * 2;
    });
    auto io = runtime.submit_io([] {
        return 7;
    });
    auto latency = runtime.submit(task_kind::latency_sensitive, [] {
        return 11;
    });
    auto bulk = runtime.bulk_submit_cpu(4, [](std::size_t index) {
        return static_cast<int>(index + 1);
    });
    task_options runtime_bulk_options;
    runtime_bulk_options.name = "runtime-options-bulk";
    runtime_bulk_options.kind = task_kind::latency_sensitive;
    auto routed_bulk = runtime.bulk_submit_with_options(runtime_bulk_options, 3, [](std::size_t index) {
        return static_cast<int>(index + 20);
    });
    auto continued = runtime.continue_cpu(std::move(cpu), [](int value) {
        return value + 1;
    });
    auto runtime_try = runtime.try_submit_cpu([] {
        return 12;
    });
    auto runtime_try_for = runtime.try_submit_cpu_for(1s, [] {
        return 13;
    });
    task_options io_options;
    io_options.name = "runtime-options-io";
    io_options.kind = task_kind::blocking_io;
    auto routed_io = runtime.submit_with_options(io_options, [] {
        return 14;
    });

    task_options background_options;
    background_options.name = "runtime-options-background";
    background_options.kind = task_kind::background;
    auto routed_background_try = runtime.try_submit_with_options_for(background_options, 1s, [] {
        return 15;
    });
    auto runtime_scheduled = runtime.schedule_submit_background_after(1ms, [] {
        return 16;
    });
    std::vector<int> runtime_values{1, 2, 3, 4};
    runtime.parallel_for_each(runtime_values.begin(), runtime_values.end(), [](int& value) {
        value += 2;
    });

    std::vector<std::future<int>> flow_inputs;
    flow_inputs.push_back(runtime.submit_cpu([] {
        return 3;
    }));
    flow_inputs.push_back(runtime.submit_cpu([] {
        return 4;
    }));
    auto flow = runtime.dataflow_cpu(std::move(flow_inputs), [](std::vector<int> values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });

    CHECK(runtime.detach_background([&background_counter] {
        background_counter.fetch_add(1, std::memory_order_relaxed);
    }));

    CHECK(io.get() == 7);
    CHECK(latency.get() == 11);
    CHECK(bulk.back().get() == 4);
    CHECK(routed_bulk.back().get() == 22);
    CHECK(continued.get() == 43);
    CHECK(runtime_try);
    CHECK(runtime_try->get() == 12);
    CHECK(runtime_try_for);
    CHECK(runtime_try_for->get() == 13);
    CHECK(routed_io.get() == 14);
    CHECK(routed_background_try);
    CHECK(routed_background_try->get() == 15);
    CHECK(runtime_scheduled.future.get() == 16);
    CHECK(flow.get() == 7);
    CHECK(runtime_values.back() == 6);
    CHECK(runtime.wait_for_idle_for(2s));
    CHECK(runtime.background_pool().wait_for_idle(2s));
    CHECK(background_counter.load(std::memory_order_relaxed) == 1);

    const auto metrics = runtime.metrics();
    CHECK(metrics.cpu.submitted_tasks_total >= 2);
    CHECK(metrics.io.submitted_tasks_total >= 1);
    CHECK(metrics.background.submitted_tasks_total >= 1);
    CHECK(metrics.submitted_tasks_total() >= 4);
    CHECK(metrics.total_threads() >= 1);
    CHECK(metrics.healthy());

    const auto prometheus = metrics.to_prometheus("runtime_test");
    CHECK(prometheus.find("runtime_test_cpu") != std::string::npos);
    CHECK(prometheus.find("runtime_test_io") != std::string::npos);
    CHECK(prometheus.find("runtime_test_background") != std::string::npos);
}

void test_shutdown_rejects_new_work() {
    thread_pool pool(base_options(2));
    pool.shutdown(shutdown_policy::cancel_pending);

    bool threw = false;
    try {
        (void)pool.submit([] { return 1; });
    } catch (const thread_pool_closed&) {
        threw = true;
    }

    CHECK(threw);
    CHECK(!pool.detach([] {}));
}

} // namespace

int main() {
    test_submit_and_future();
    test_dispatch_inline_counts_submission();
    test_detach_and_wait_for_idle();
    test_exception_propagates_from_submit();
    test_detached_exception_is_counted();
    test_pause_and_resume();
    test_bounded_reject();
    test_try_submit_and_try_detach();
    test_executor_task_options();
    test_bulk_options_and_executor_bulk();
    test_caller_runs();
    test_priority_order();
    test_priority_fairness_interval();
    test_resize_grow_and_shrink();
    test_resize_shrink_is_idempotent();
    test_resize_grow_cancels_pending_retirements();
    test_parallel_for();
    test_parallel_for_each_and_transform();
    test_parallel_schedules_and_reduce();
    test_parallel_3d_and_nd();
    test_cached_pool_growth_and_worker_context();
    test_task_group();
    test_scoped_task_group();
    test_submit_after();
    test_schedule_submit_after_and_cancel();
    test_detach_after_and_cancel();
    test_cancellable_tasks();
    test_cancel_pending_sets_task_cancelled();
    test_drop_oldest_sets_task_cancelled();
    test_deadline_tasks();
    test_submit_then();
    test_submit_then_does_not_occupy_worker_while_waiting();
    test_continue_with_and_dataflow();
    test_continuation_scheduler_and_numa_hooks();
    test_completion_executor_worker_metrics_and_globals();
    test_when_all_values();
    test_when_all_does_not_occupy_worker_while_waiting();
    test_when_all_void();
    test_when_any_values();
    test_when_any_does_not_occupy_worker_while_waiting();
    test_when_any_void_and_empty();
    test_wait_for_idle_ready_work_semantics();
    test_work_stealing_local_tasks();
    test_task_graph_dependencies();
    test_task_graph_exception();
    test_task_graph_cycle_detection();
    test_thread_pool_runtime_routing();
    test_shutdown_rejects_new_work();
    return 0;
}
