#include "../benchmarks/benchmark_support.hpp"
#include <iostream>
#include <limits>

void check(bool ok) { if (!ok) throw std::runtime_error("benchmark support check failed"); }

int main() {
    try {
        using namespace benchmark_support;
        check(positive_number("00064", 64) == 64);
        check(positive_number("1", 10000) == 1);
        check(positive_number("10000", 10000) == 10000);
        for (const auto& input : {"", "0", "-1", "+1", "1x", " 1", "1 ", "65", "184467440737095516160000"}) {
            bool rejected = false;
            try { (void)positive_number(input, 64); }
            catch (const std::exception&) { rejected = true; }
            check(rejected);
        }
        const auto one = summarize({7});
        check(one.minimum == 7 && one.median == 7 && one.p95 == 7 && one.maximum == 7);
        const auto even = summarize({40, 10, 30, 20});
        check(even.minimum == 10 && even.median == 25 && even.p95 == 40 && even.maximum == 40);
        check(summarize({9, 1, 5}).median == 5);
        std::vector<std::int64_t> values;
        for (int i = 20; i >= 1; --i) values.push_back(i);
        check(summarize(values).p95 == 19);
        check(values.front() == 20); // Raw samples must remain unchanged.
        const auto large = std::numeric_limits<std::int64_t>::max();
        check(summarize({large, large}).median > 0);
        bool rejected = false;
        try { (void)summarize({}); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
