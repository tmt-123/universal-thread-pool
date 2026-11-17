#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace benchmark_support {

inline std::size_t positive_number(const std::string& input, std::size_t maximum) {
    if (input.empty()) throw std::invalid_argument("empty benchmark argument");
    std::size_t value = 0;
    for (char ch : input) {
        if (ch < '0' || ch > '9') throw std::invalid_argument("arguments must be decimal integers");
        const auto digit = static_cast<std::size_t>(ch - '0');
        if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) {
            throw std::out_of_range("benchmark argument exceeds allowed range");
        }
        value = value * 10 + digit;
    }
    if (value == 0) throw std::invalid_argument("arguments must be positive");
    return value;
}

struct summary {
    std::int64_t minimum;
    double median;
    std::int64_t p95;
    std::int64_t maximum;
};

inline summary summarize(std::vector<std::int64_t> values) {
    if (values.empty()) throw std::invalid_argument("no benchmark samples");
    std::sort(values.begin(), values.end());
    const auto count = values.size();
    const auto middle = count / 2;
    const double median = count % 2 ? static_cast<double>(values[middle])
        : static_cast<double>(values[middle - 1]) / 2 + static_cast<double>(values[middle]) / 2;
    // Nearest-rank P95 across repeated whole-run times, not task latencies.
    return {values.front(), median, values[(count * 95 + 99) / 100 - 1], values.back()};
}

} // namespace benchmark_support
