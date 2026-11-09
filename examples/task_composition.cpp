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
    require(left.get() + right.get() == 42, "group results mismatch");

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
    require(sum == 42, "graph dependency result mismatch");
    std::cout << "task_group + task_graph: independent inputs merged into 42\n";
}

void runtime_pipeline() {
    thread_pool_runtime_options options;
    options.cpu = small_pool(2);
    options.io = small_pool(1);
    options.background = small_pool(1);
    thread_pool_runtime runtime(options);

    // Simulate loading input on IO workers, then transform it on CPU workers.
    auto loaded = runtime.submit_io([&] {
        require(runtime.io_pool().is_worker_thread(), "input was not routed to IO");
        return 21;
    });
    auto transformed = runtime.continue_on(
        task_kind::blocking_io, task_kind::cpu_bound, std::move(loaded),
        [&](int value) {
            require(runtime.cpu_pool().is_worker_thread(), "continuation was not routed to CPU");
            return value * 2;
        });
    auto saved = runtime.continue_on(
        task_kind::cpu_bound, task_kind::background, std::move(transformed),
        [&](int value) {
            require(runtime.background_pool().is_worker_thread(), "output was not routed to background");
            return value;
        });
    require(saved.get() == 42, "pipeline result mismatch");

    std::vector<std::future<int>> inputs;
    for (int value = 1; value <= 4; ++value) {
        inputs.push_back(runtime.submit_cpu([value] { return value; }));
    }
    auto total = runtime.dataflow_cpu(std::move(inputs), [&](std::vector<int> values) {
        require(runtime.cpu_pool().is_worker_thread(), "dataflow did not execute on CPU");
        return std::accumulate(values.begin(), values.end(), 0);
    });
    require(total.get() == 10, "dataflow aggregation mismatch");

    // A predecessor failure propagates through the returned continuation future.
    auto failed = runtime.submit_io([]() -> int {
        throw std::runtime_error("input failure");
    });
    auto propagated = runtime.continue_on(
        task_kind::blocking_io, task_kind::cpu_bound, std::move(failed),
        [](int) -> int { throw std::logic_error("continuation must not run"); });
    bool saw_input_error = false;
    try {
        (void)propagated.get();
    } catch (const std::runtime_error& error) {
        saw_input_error = std::string(error.what()) == "input failure";
    }
    require(saw_input_error, "predecessor error was not propagated");
    std::cout << "runtime: IO -> CPU -> background = 42, dataflow = 10\n";
}

} // namespace

int main() {
    try {
        grouped_tasks_and_graph();
        runtime_pipeline();
    } catch (const std::exception& error) {
        std::cerr << "task composition example failed: " << error.what() << '\n';
        return 1;
    }
}
