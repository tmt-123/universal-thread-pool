#include "universal_thread_pool/thread_pool.hpp"
#include "benchmark_support.hpp"
#include <array>

#include <chrono>
#include <future>
#include <iostream>
#include <iomanip>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using clock_type = std::chrono::steady_clock;

template <class F>
void run_case(const char* name, std::size_t count, std::size_t repeat, std::vector<std::int64_t>& samples, F&& work) {
    const auto start = clock_type::now();
    const auto sum = work();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock_type::now() - start).count();
    if (sum != count * (count + 1) / 2) {
        throw std::runtime_error("benchmark checksum mismatch");
    }
    samples.push_back(elapsed);
    // Printing and result validation are outside the measured interval.
    std::cout << name << ',' << repeat << ',' << count << ',' << elapsed << ','
              << static_cast<double>(elapsed) / count << ',' << sum << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 4) throw std::invalid_argument("usage: thread_pool_bench [tasks] [threads] [repeats]");
        const std::size_t count = argc > 1 ? benchmark_support::positive_number(argv[1], 10000) : 1000;
        const std::size_t threads = argc > 2 ? benchmark_support::positive_number(argv[2], 64) : 4;
        const std::size_t repeats = argc > 3 ? benchmark_support::positive_number(argv[3], 100) : 3;
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
        std::array<std::vector<std::int64_t>, 4> samples;
        for (std::size_t repeat = 1; repeat <= repeats; ++repeat) {
            run_case("submit_future", count, repeat, samples[0], [&] {
                std::vector<std::future<std::size_t>> futures;
                futures.reserve(count);
                for (std::size_t i = 0; i < count; ++i) {
                    futures.push_back(pool.submit([i] { return i + 1; }));
                }
                std::size_t sum = 0;
                for (auto& future : futures) sum += future.get();
                return sum;
            });
            run_case("bulk_submit", count, repeat, samples[1], [&] {
                auto futures = pool.bulk_submit(count, [](std::size_t i) { return i + 1; });
                std::size_t sum = 0;
                for (auto& future : futures) sum += future.get();
                return sum;
            });
            run_case("parallel_for", count, repeat, samples[2], [&] {
                std::vector<std::size_t> values(count);
                universal_thread_pool::loop_options loop;
                loop.schedule = universal_thread_pool::loop_schedule::guided_blocks;
                pool.parallel_for<std::size_t>(0, count, [&](std::size_t i) {
                    values[i] = i + 1;
                }, loop);
                return std::accumulate(values.begin(), values.end(), std::size_t{0});
            });
            run_case("std_async", count, repeat, samples[3], [&] {
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
        const char* names[] = {"submit_future", "bulk_submit", "parallel_for", "std_async"};
        std::cout << std::setprecision(17);
        std::cout << "# summary_case,samples,min_total_ns,median_total_ns,p95_total_ns,max_total_ns\n";
        for (std::size_t i = 0; i < samples.size(); ++i) {
            const auto stats = benchmark_support::summarize(samples[i]);
            std::cout << "# " << names[i] << ',' << samples[i].size() << ','
                      << stats.minimum << ',' << stats.median << ',' << stats.p95
                      << ',' << stats.maximum << '\n';
        }
        pool.shutdown();
        pool.join();
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
