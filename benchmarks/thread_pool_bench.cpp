#include "universal_thread_pool/thread_pool.hpp"

#include <chrono>
#include <future>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

template <class F>
void run_case(const char* name, std::size_t count, std::size_t repeat, F&& work) {
    const auto start = clock_type::now();
    const auto sum = work();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock_type::now() - start).count();
    if (sum != count * (count + 1) / 2) {
        throw std::runtime_error("benchmark checksum mismatch");
    }
    // Printing and result validation are outside the measured interval.
    std::cout << name << ',' << repeat << ',' << count << ',' << elapsed << ','
              << static_cast<double>(elapsed) / count << ',' << sum << '\n';
}

std::size_t positive_number(const char* text) {
    const std::string input(text);
    if (input.empty() || input.find_first_not_of("0123456789") != std::string::npos) {
        throw std::invalid_argument("arguments must be positive decimal integers");
    }
    const auto value = std::stoull(input);
    if (value == 0) throw std::invalid_argument("arguments must be positive");
    return static_cast<std::size_t>(value);
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::size_t count = argc > 1 ? positive_number(argv[1]) : 1000;
        const std::size_t threads = argc > 2 ? positive_number(argv[2]) : 4;
        const std::size_t repeats = argc > 3 ? positive_number(argv[3]) : 3;
        if (argc > 4 || count > 10000 || threads > 64 || repeats > 100) {
            throw std::invalid_argument("usage: thread_pool_bench [tasks:1..10000] [threads:1..64] [repeats:1..100]");
        }
        auto options = universal_thread_pool::make_cpu_pool_options(threads);
        options.name = "bench_pool";
        options.worker_batch_size = 8;
        options.max_threads = threads;
        universal_thread_pool::thread_pool pool(options);
        pool.submit([] {}).get();
        if (!pool.wait_for_idle(std::chrono::seconds(2))) {
            throw std::runtime_error("warmup did not finish");
        }
        std::cout << "# threads=" << threads << " repeats=" << repeats
                  << " pool_warmup=1 metrics=on batch_size=8\n";
        std::cout << "case,repeat,operations,total_ns,ns_per_op,checksum\n";
        for (std::size_t repeat = 1; repeat <= repeats; ++repeat) {
            run_case("submit_future", count, repeat, [&] {
                std::vector<std::future<std::size_t>> futures;
                futures.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    futures.push_back(pool.submit([i] { return i + 1; }));
                }
                std::size_t sum = 0;
                for (auto& future : futures) sum += future.get();
                return sum;
            });
            run_case("bulk_submit", count, repeat, [&] {
                auto futures = pool.bulk_submit(count, [](std::size_t i) { return i + 1; });
                std::size_t sum = 0;
                for (auto& future : futures) sum += future.get();
                return sum;
            });
            run_case("parallel_for", count, repeat, [&] {
                std::vector<std::size_t> values(count);
                universal_thread_pool::loop_options loop;
                loop.schedule = universal_thread_pool::loop_schedule::guided_blocks;
                pool.parallel_for<std::size_t>(0, count, [&](std::size_t i) {
                    values[i] = i + 1;
                }, loop);
                return std::accumulate(values.begin(), values.end(), std::size_t{0});
            });
            run_case("std_async", count, repeat, [&] {
                std::vector<std::future<std::size_t>> futures;
                futures.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    futures.push_back(std::async(std::launch::async, [i] { return i + 1; }));
                }
                std::size_t sum = 0;
                for (auto& future : futures) sum += future.get();
                return sum;
            });
        }
        pool.shutdown();
        pool.join();
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
