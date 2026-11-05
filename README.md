# Universal Thread Pool

基于 C++17 的通用线程池，支持多种调度策略、工作窃取、任务依赖图、任务取消和运行时任务路由。

## 功能

- 固定大小、动态缓存与工作窃取模式。
- FIFO、优先级调度，以及有界队列的阻塞、拒绝、调用者执行和丢弃最旧任务策略。
- 基于 `std::future` 的结果获取、异常传递、任务延续与数据流组合。
- 延迟任务、取消令牌、暂停恢复、线程数调整及关闭策略。
- 并行循环、任务组、任务依赖图，以及 CPU、IO 和后台任务的运行时路由。
- 任务指标及 JSON、Prometheus、OpenTelemetry JSON 导出接口。
- C 接口，以及按编译标准启用的 C++20 协程和 `std::stop_token` 接口。

项目处于早期开发阶段，接口仍可能调整。C++20 接口、跨平台行为及性能数据将逐步补充验证。

当前 MSVC 19.44 构建在 continuation 相关代码中遇到 `std::future` 复制错误，Windows 支持尚未完成验证。现阶段建议使用 Linux GCC 构建。

基础版本已在 Linux、GCC 9.4.0、CMake 3.31.6 的 Debug 配置下构建，功能测试连续运行 10 次通过，基础示例运行通过。这些检查不代表已完成压力测试或 C++20 接口验证。

## 构建与测试

需要 CMake 3.16 或更新版本，以及支持 C++17 的编译器。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

Visual Studio 等多配置生成器通过 `--config Debug` 选择构建配置。
当前测试使用 `assert`，部分断言中包含操作调用，请先使用 Debug 运行测试；Release 测试检查机制将在后续批次修复。

运行示例：单配置生成器执行 `build/thread_pool_basic`，Visual Studio 执行 `build/Debug/thread_pool_basic.exe`。

## 最小示例

```cpp
#include <iostream>
#include "universal_thread_pool/thread_pool.hpp"

int main() {
    universal_thread_pool::thread_pool_options options;
    options.initial_threads = 4;
    options.max_threads = 4;
    universal_thread_pool::thread_pool pool(options);
    auto result = pool.submit([] { return 42; });
    std::cout << result.get() << '\n';
}
```

在 CMake 中链接 `universal_thread_pool::universal_thread_pool`。项目支持 `add_subdirectory`；也提供安装及 CMake package 配置：

```sh
cmake --install build --config Debug --prefix install
```

安装后的消费项目通过 `find_package(universal_thread_pool CONFIG REQUIRED)` 查找库，并将安装目录加入 `CMAKE_PREFIX_PATH`。

## 构建选项

| 选项 | 默认值 | 用途 |
| --- | --- | --- |
| `UNIVERSAL_THREAD_POOL_BUILD_TESTS` | `ON` | 构建测试和示例 |
| `UNIVERSAL_THREAD_POOL_BUILD_BENCHMARKS` | `OFF` | 构建基准程序（后续批次提供） |
| `UNIVERSAL_THREAD_POOL_SANITIZER` | 空 | GCC/Clang 下可选 `address` 或 `thread` |

## 目录

- `include/universal_thread_pool/`：公共头文件与模板实现。
- `src/`：线程池、执行器、运行时、任务图、任务组和 C 接口实现。
- `examples/`：使用示例。
- `tests/`：功能测试。
- `cmake/`：安装包配置。

## 发布计划

基础版本之后依次完善测试与 CI、调度策略示例、任务生命周期示例、组合任务示例、接口与安装验证、基准程序及发布说明。各批次以实际提交为准。
