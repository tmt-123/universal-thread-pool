#include "universal_thread_pool/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace universal_thread_pool;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class T>
void expect_cancelled(std::future<T>& future) {
    require(future.wait_for(2s) == std::future_status::ready, "cancelled result not ready");
    try {
        (void)future.get();
    } catch (const task_cancelled&) {
        return;
    }
    throw std::runtime_error("expected task_cancelled");
}

thread_pool_options options() {
    thread_pool_options result;
    result.initial_threads = result.min_threads = result.max_threads = 1;
    result.worker_batch_size = 1;
    return result;
}

void cancellation_and_deadlines() {
    std::atomic<int> calls{0};
    thread_pool pool(options());
    pool.pause();
    cancellation_source source;
    auto cancelled = pool.submit_cancellable(source.token(), [&] { ++calls; });
    source.request_stop();
    auto expired = pool.submit_with_deadline(
        std::chrono::steady_clock::now() - 1ms, [&] { ++calls; });
    auto zero_timeout = pool.submit_with_timeout(0ms, [&] { ++calls; });
    pool.resume();
    expect_cancelled(cancelled);
    expect_cancelled(expired);
    expect_cancelled(zero_timeout);
    require(calls == 0, "cancelled or expired callable ran");
    std::cout << "cancellation/deadline: queued callables skipped\n";
}

void delayed_tasks() {
    std::atomic<int> calls{0};
    thread_pool pool(options());
    const auto start = std::chrono::steady_clock::now();
    auto delayed = pool.schedule_submit_after(20ms, [] {
        return std::chrono::steady_clock::now();
    });
    require(delayed.future.get() >= start + 20ms, "delayed task ran too early");

    // Pause workers so cancellation wins even if the timer has already queued it.
    pool.pause();
    auto cancelled = pool.schedule_submit_after(20ms, [&] { ++calls; });
    require(cancelled.cancel(), "scheduled cancellation was not accepted");
    pool.resume();
    expect_cancelled(cancelled.future);
    require(calls == 0, "cancelled delayed callable ran");

    auto far_future = pool.submit_after(1h, [&] { ++calls; });
    require(pool.wait_for_idle(100ms), "future timer should not prevent idle");
    pool.shutdown(shutdown_policy::drain);
    expect_cancelled(far_future);
    pool.join();
    require(calls == 0, "shutdown executed future timer");
    std::cout << "delay: due time respected, cancellation and shutdown checked\n";
}

void shutdown_policy_example(shutdown_policy policy) {
    std::atomic<int> queued_calls{0};
    std::promise<void> started;
    std::promise<void> release;
    auto release_signal = release.get_future().share();
    thread_pool pool(options());
    // A running task stays alive until the producer explicitly releases it.
    auto running = pool.submit([&] {
        started.set_value();
        release_signal.wait();
        return 7;
    });
    started.get_future().wait();
    auto queued = pool.submit([&] { ++queued_calls; return 8; });
    pool.shutdown(policy);
    const bool still_running = running.wait_for(0ms) == std::future_status::timeout;
    bool rejected = false;
    try {
        (void)pool.submit([] {});
    } catch (const thread_pool_closed&) {
        rejected = true;
    }
    // Release before checking so a failed check cannot leave the worker blocked.
    release.set_value();
    require(still_running, "shutdown must not forcibly finish a running callable");
    require(rejected, "shutdown must reject new submissions");
    require(running.get() == 7, "running task result lost");
    if (policy == shutdown_policy::drain) {
        require(queued.get() == 8 && queued_calls == 1, "drain lost queued task");
    } else {
        expect_cancelled(queued);
        require(queued_calls == 0, "shutdown executed pending task");
    }
    pool.join();
}

} // namespace

int main() {
    try {
        cancellation_and_deadlines();
        delayed_tasks();
        shutdown_policy_example(shutdown_policy::drain);
        shutdown_policy_example(shutdown_policy::cancel_pending);
        shutdown_policy_example(shutdown_policy::stop_immediately);
        std::cout << "shutdown: drain, cancel_pending, stop_immediately checked\n";
    } catch (const std::exception& error) {
        std::cerr << "task lifecycle example failed: " << error.what() << '\n';
        return 1;
    }
}
