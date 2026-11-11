#include "universal_thread_pool/thread_pool.hpp"
#include <iostream>
#include <stdexcept>

#if !UNIVERSAL_THREAD_POOL_HAS_COROUTINE || !UNIVERSAL_THREAD_POOL_HAS_STOP_TOKEN
#error These tests require C++20 coroutine and stop_token support
#endif

using namespace universal_thread_pool;

void check(bool ok) { if (!ok) throw std::runtime_error("C++20 check failed"); }

// Self-destroying frame; the returned future owns only the result state.
struct async_result {
    struct promise_type {
        std::promise<int> value;
        async_result get_return_object() { return {value.get_future()}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_value(int result) { value.set_value(result); }
        void unhandled_exception() { value.set_exception(std::current_exception()); }
    };
    std::future<int> result;
};

async_result compute(thread_pool& pool) {
    auto ex = pool.get_executor();
    co_await ex.schedule();
    check(pool.is_worker_thread());
    int value = co_await ex.submit_awaitable([] { return 21; });
    // Use a ready future here so awaiting it does not occupy a worker waiting.
    std::promise<int> ready;
    ready.set_value(value);
    co_return value + co_await ex.await_future(ready.get_future());
}

int main() {
    try {
        thread_pool_options options;
        options.initial_threads = options.max_threads = 2;
        thread_pool pool(options);
        check(compute(pool).result.get() == 42);
        std::stop_source source;
        source.request_stop();
        auto stopped = pool.submit_stop_token(source.get_token(), [] { return 1; });
        bool cancelled = false;
        try { (void)stopped.get(); } catch (const task_cancelled&) { cancelled = true; }
        check(cancelled);
        std::stop_source active;
        check(pool.submit_stop_token(active.get_token(), [] { return 7; }).get() == 7);
        // Wait for the coroutine's final frame cleanup before destroying pool.
        check(pool.wait_for_idle(std::chrono::seconds(2)));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
