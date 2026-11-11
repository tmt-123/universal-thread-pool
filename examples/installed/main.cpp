#include "universal_thread_pool/thread_pool.hpp"

int main() {
    universal_thread_pool::thread_pool_options options;
    options.initial_threads = options.max_threads = 1;
    universal_thread_pool::thread_pool pool(options);
    return pool.submit([] { return 42; }).get() == 42 ? 0 : 1;
}
