#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <numeric>
#include <vector>

#include "universal_thread_pool/thread_pool.hpp"

int main() {
    using namespace universal_thread_pool;

    std::cout << "通用线程池 版本：" << version_string << '\n';

    thread_pool_options options;
    options.name = "example_pool";
    options.thread_name_prefix = "utp-example";
    options.initial_threads = 4;
    options.max_threads = 4;
    options.scheduler = schedule_policy::priority;
    options.priority_fairness_interval = 16;
    options.continuation_threads = 2;
    options.numa_nodes = 2;
    options.queue = queue_policy::bounded_caller_runs;
    options.max_queue_size = 1024;
    options.on_task_start = [](const task_metadata& task) {
        if (!task.name.empty()) {
            std::cout << "开始执行任务 编号=" << task.id << " " << task.name << '\n';
        }
    };

    thread_pool pool(options);

    auto future = pool.submit([] {
        return 42;
    });

    task_options named;
    named.name = "named-example";
    named.kind = task_kind::cpu_bound;
    named.numa_node = 0;
    named.source = UNIVERSAL_THREAD_POOL_SOURCE_LOCATION;
    auto named_future = pool.submit_with_options(named, [] {
        return 100;
    });

    task_options executor_named;
    executor_named.name = "executor-named-example";
    executor_named.kind = task_kind::cpu_bound;
    executor_named.source = UNIVERSAL_THREAD_POOL_SOURCE_LOCATION;
    auto executor_named_future = pool.get_executor().submit_with_options(executor_named, [] {
        return 101;
    });

    auto try_future = pool.try_submit([] {
        return 5;
    });
    auto try_for_future = pool.try_submit_for(std::chrono::milliseconds(1), [] {
        return 6;
    });
    auto scheduled_future = pool.schedule_submit_after(std::chrono::milliseconds(1), [] {
        return 15;
    });

    std::vector<int> values(1000, 0);
    loop_options loop;
    loop.schedule = loop_schedule::guided_blocks;
    loop.block_size = 32;

    pool.parallel_for<std::size_t>(0, values.size(), [&](std::size_t i) {
        values[i] = static_cast<int>(i);
    }, loop);

    pool.parallel_for_each(values.begin(), values.end(), [](int& value) {
        value += 1;
    }, loop);

    std::vector<int> doubled(values.size(), 0);
    pool.parallel_transform(values.begin(), values.end(), doubled.begin(), [](int value) {
        return value * 2;
    }, loop);

    const auto reduced = pool.parallel_reduce<int>(
        0,
        1000,
        0,
        [](int a, int b) {
            return a + b;
        });

    auto bulk = pool.bulk_submit(4, [](std::size_t index) {
        return static_cast<int>(index * index);
    });
    task_options bulk_options;
    bulk_options.name = "bulk-options-example";
    bulk_options.priority = task_priority::high;
    auto option_bulk = pool.bulk_submit_with_options(bulk_options, 4, [](std::size_t index) {
        return static_cast<int>(index + 10);
    });

    auto continued = pool.continue_with(pool.submit([] {
        return 40;
    }), [](int value) {
        return value + 2;
    });

    thread_pool_options completion_options;
    completion_options.initial_threads = 2;
    completion_options.max_threads = 2;
    thread_pool completion_pool(completion_options);
    auto cross_pool_continued = pool.continue_on(
        completion_pool.get_executor(task_priority::high),
        pool.submit([] {
            return 30;
        }),
        [](int value) {
            return value + 12;
        });

    std::vector<std::future<int>> flow_inputs;
    for (int i = 1; i <= 4; ++i) {
        flow_inputs.push_back(pool.submit([i] {
            return i;
        }));
    }
    auto flow = pool.dataflow(std::move(flow_inputs), [](std::vector<int> values) {
        return std::accumulate(values.begin(), values.end(), 0);
    });

    std::vector<std::future<int>> cross_flow_inputs;
    cross_flow_inputs.push_back(pool.submit([] {
        return 2;
    }));
    cross_flow_inputs.push_back(pool.submit([] {
        return 3;
    }));
    auto cross_pool_flow = pool.get_executor().dataflow_on(
        completion_pool.get_executor(),
        std::move(cross_flow_inputs),
        [](std::vector<int> values) {
            return std::accumulate(values.begin(), values.end(), 1, std::multiplies<int>());
        });

    task_group group(pool.get_executor());
    auto group_value = group.submit([] {
        return 8;
    });
    auto cancellable_group_value = group.submit_with_token(
        [](cancellation_token token, int value) {
            return token.stop_requested() ? -1 : value + 1;
        },
        8);
    group.wait();

    scoped_task_group scoped(pool.get_executor(), scoped_task_group_policy::cancel_and_wait);
    scoped.run_with_token([](cancellation_token token) {
        if (!token.stop_requested()) {
            std::cout << "作用域任务已执行\n";
        }
    });
    scoped.wait();

    std::vector<int> grid(4 * 4 * 4, 0);
    pool.parallel_for_3d<int>(0, 4, 0, 4, 0, 4, [&](int x, int y, int z) {
        grid[static_cast<std::size_t>((x * 4 + y) * 4 + z)] = x + y + z;
    }, loop);

    thread_pool_runtime runtime;
    auto runtime_bulk = runtime.bulk_submit_cpu(4, [](std::size_t index) {
        return static_cast<int>(index + 10);
    });
    task_options runtime_bulk_options;
    runtime_bulk_options.kind = task_kind::latency_sensitive;
    runtime_bulk_options.name = "runtime-bulk-options-example";
    auto runtime_option_bulk = runtime.bulk_submit_with_options(
        runtime_bulk_options,
        4,
        [](std::size_t index) {
            return static_cast<int>(index + 20);
        });
    auto runtime_continued = runtime.continue_cpu(runtime.submit_cpu([] {
        return 20;
    }), [](int value) {
        return value + 1;
    });
    auto runtime_cross_continued = runtime.continue_on(
        task_kind::blocking_io,
        task_kind::cpu_bound,
        runtime.submit_io([] {
            return 17;
        }),
        [](int value) {
            return value + 1;
        });
    auto runtime_try = runtime.try_submit_cpu([] {
        return 12;
    });
    auto runtime_try_for = runtime.try_submit_cpu_for(std::chrono::milliseconds(1), [] {
        return 13;
    });
    task_options runtime_named;
    runtime_named.name = "runtime-io-example";
    runtime_named.kind = task_kind::blocking_io;
    runtime_named.source = UNIVERSAL_THREAD_POOL_SOURCE_LOCATION;
    auto runtime_named_future = runtime.submit_with_options(runtime_named, [] {
        return 14;
    });
    auto runtime_scheduled = runtime.schedule_submit_background_after(
        std::chrono::milliseconds(1),
        [] {
            return 16;
        });
    std::vector<int> runtime_values{1, 2, 3, 4};
    runtime.parallel_for_each(runtime_values.begin(), runtime_values.end(), [](int& value) {
        value *= 3;
    });

    auto cached_options = make_cached_pool_options(1, 8);
    cached_options.idle_timeout = std::chrono::seconds(5);
    thread_pool cached_pool(cached_options);
    auto cached_future = cached_pool.submit([] {
        return 7;
    });
    auto global_future = global_thread_pool().submit([] {
        return 18;
    });

    const auto sum = std::accumulate(values.begin(), values.end(), 0);
    const auto metrics = pool.metrics();
    const auto prometheus = to_prometheus(metrics, "example_pool");
    const auto runtime_idle = runtime.wait_for_idle_for(std::chrono::seconds(1));

    std::cout << "普通任务结果=" << future.get()
              << " 命名任务结果=" << named_future.get()
              << " 执行器命名任务结果=" << executor_named_future.get()
              << " 尝试提交结果=" << (try_future ? try_future->get() : -1)
              << " 限时提交结果=" << (try_for_future ? try_for_future->get() : -1)
              << " 延迟任务结果=" << scheduled_future.future.get()
              << " 数组求和=" << sum
              << " 并行归约结果=" << reduced
              << " 批量任务末项=" << bulk.back().get()
              << " 带配置批量任务末项=" << option_bulk.back().get()
              << " 后续任务结果=" << continued.get()
              << " 跨池后续任务结果=" << cross_pool_continued.get()
              << " 数据流结果=" << flow.get()
              << " 跨池数据流结果=" << cross_pool_flow.get()
              << " 任务组结果=" << group_value.get()
              << " 带令牌任务组结果=" << cancellable_group_value.get()
              << " 三维循环末项=" << grid.back()
              << " 并行转换末项=" << doubled.back()
              << " 运行时批量末项=" << runtime_bulk.back().get()
              << " 运行时带配置批量末项=" << runtime_option_bulk.back().get()
              << " 运行时后续任务=" << runtime_continued.get()
              << " 运行时跨池后续任务=" << runtime_cross_continued.get()
              << " 运行时尝试提交=" << (runtime_try ? runtime_try->get() : -1)
              << " 运行时限时提交=" << (runtime_try_for ? runtime_try_for->get() : -1)
              << " 运行时命名任务=" << runtime_named_future.get()
              << " 运行时延迟任务=" << runtime_scheduled.future.get()
              << " 运行时是否空闲=" << runtime_idle
              << " 运行时遍历末项=" << runtime_values.back()
              << " 缓存模式任务结果=" << cached_future.get()
              << " 全局线程池任务结果=" << global_future.get()
              << " 已完成任务数=" << metrics.completed_tasks_total
              << " 工作线程数=" << metrics.workers.size()
              << " 平均等待时间_纳秒=" << metrics.average_wait_time_ns()
              << " P95等待时间估计_纳秒=" << metrics.wait_time_percentile_estimate_ns(0.95)
              << " Prometheus指标字节数=" << prometheus.size()
              << '\n';

    return 0;
}
