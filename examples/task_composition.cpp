#include "universal_thread_pool/thread_pool.hpp"

#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace universal_thread_pool;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

thread_pool_options small_pool(std::size_t threads) {
    thread_pool_options options;
    options.initial_threads = options.min_threads = options.max_threads = threads;
    return options;
}

void grouped_tasks_and_graph() {
    thread_pool pool(small_pool(2));
    task_group group(pool.get_executor());
    auto left = group.submit([] { return 20; });
    auto right = group.submit([] { return 22; });
    group.wait();
    require(left.get() + right.get() == 42, "任务组结果不符");

    // The graph orders independent inputs before their shared consumer.
    int first = 0;
    int second = 0;
    int sum = 0;
    task_graph graph;
    const auto a = graph.emplace_named("input-a", [&] { first = 20; });
    const auto b = graph.emplace_named("input-b", [&] { second = 22; });
    const auto merge = graph.emplace_named("merge", [&] { sum = first + second; });
    graph.precede(a, merge);
    graph.precede(b, merge);
    graph.run(pool.get_executor()).get();
    require(sum == 42, "任务图依赖计算结果不符");
    std::cout << "任务组与依赖图：独立输入任务完成后，合并结果为 42\n";
}

void runtime_pipeline() {
    thread_pool_runtime_options options;
    options.cpu = small_pool(2);
    options.io = small_pool(1);
    options.background = small_pool(1);
    thread_pool_runtime runtime(options);

    // Simulate loading input on IO workers, then transform it on CPU workers.
    auto loaded = runtime.submit_io([&] {
        require(runtime.io_pool().is_worker_thread(), "输入任务未路由到IO线程池");
        return 21;
    });
    auto transformed = runtime.continue_on(
        task_kind::blocking_io, task_kind::cpu_bound, std::move(loaded),
        [&](int value) {
            require(runtime.cpu_pool().is_worker_thread(), "后续任务未路由到CPU线程池");
            return value * 2;
        });
    auto saved = runtime.continue_on(
        task_kind::cpu_bound, task_kind::background, std::move(transformed),
        [&](int value) {
            require(runtime.background_pool().is_worker_thread(), "输出任务未路由到后台线程池");
            return value;
        });
    require(saved.get() == 42, "流水线结果不符");

    std::vector<std::future<int>> inputs;
    for (int value = 1; value <= 4; ++value) {
        inputs.push_back(runtime.submit_cpu([value] { return value; }));
    }
    auto total = runtime.dataflow_cpu(std::move(inputs), [&](std::vector<int> values) {
        require(runtime.cpu_pool().is_worker_thread(), "数据流聚合未在CPU线程池执行");
        return std::accumulate(values.begin(), values.end(), 0);
    });
    require(total.get() == 10, "数据流聚合结果不符");

    // A predecessor failure propagates through the returned continuation future.
    auto failed = runtime.submit_io([]() -> int {
        throw std::runtime_error("输入任务失败");
    });
    auto propagated = runtime.continue_on(
        task_kind::blocking_io, task_kind::cpu_bound, std::move(failed),
        [](int) -> int { throw std::logic_error("后续任务不应执行"); });
    bool saw_input_error = false;
    try {
        (void)propagated.get();
    } catch (const std::runtime_error& error) {
        saw_input_error = std::string(error.what()) == "输入任务失败";
    }
    require(saw_input_error, "前置任务异常未正确传递");
    std::cout << "运行时流水线：IO池 -> CPU池 -> 后台池，结果为 42；数据流聚合结果为 10\n";
}

} // namespace

int main() {
    try {
        grouped_tasks_and_graph();
        runtime_pipeline();
    } catch (const std::exception& error) {
        std::cerr << "任务组合示例失败：" << error.what() << '\n';
        return 1;
    }
}
