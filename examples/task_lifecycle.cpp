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
    require(future.wait_for(2s) == std::future_status::ready, "取消结果未按时就绪");
    try {
        (void)future.get();
    } catch (const task_cancelled&) {
        return;
    }
    throw std::runtime_error("预期收到任务取消异常");
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
    require(calls == 0, "已取消或已过期的任务函数被意外执行");
    std::cout << "取消与截止时间：已取消或已过期的排队任务未执行\n";
}

void delayed_tasks() {
    std::atomic<int> calls{0};
    thread_pool pool(options());
    const auto start = std::chrono::steady_clock::now();
    auto delayed = pool.schedule_submit_after(20ms, [] {
        return std::chrono::steady_clock::now();
    });
    require(delayed.future.get() >= start + 20ms, "延迟任务提前执行");

    // Pause workers so cancellation wins even if the timer has already queued it.
    pool.pause();
    auto cancelled = pool.schedule_submit_after(20ms, [&] { ++calls; });
    require(cancelled.cancel(), "延迟任务取消请求未被接受");
    pool.resume();
    expect_cancelled(cancelled.future);
    require(calls == 0, "已取消的延迟任务被意外执行");

    auto far_future = pool.submit_after(1h, [&] { ++calls; });
    require(pool.wait_for_idle(100ms), "尚未到期的延迟任务不应阻止空闲等待完成");
    pool.shutdown(shutdown_policy::drain);
    expect_cancelled(far_future);
    pool.join();
    require(calls == 0, "关闭时意外执行了尚未到期的延迟任务");
    std::cout << "延迟任务：到期时间、取消和关闭行为均检查通过\n";
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
    require(still_running, "关闭不应强制终止正在执行的任务函数");
    require(rejected, "关闭后应拒绝新任务");
    require(running.get() == 7, "运行中任务的结果丢失");
    if (policy == shutdown_policy::drain) {
        require(queued.get() == 8 && queued_calls == 1, "排空策略丢失了排队任务");
    } else {
        expect_cancelled(queued);
        require(queued_calls == 0, "关闭时意外执行了待取消任务");
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
        std::cout << "关闭策略：排空任务、取消待执行任务、立即停止取任务，均检查通过\n";
    } catch (const std::exception& error) {
        std::cerr << "任务生命周期示例失败：" << error.what() << '\n';
        return 1;
    }
}
