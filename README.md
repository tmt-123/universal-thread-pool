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

## 项目结构

```text
universal-thread-pool/
├── CMakeLists.txt                     # 构建、测试与安装配置
├── cmake/
│   └── universal_thread_poolConfig.cmake.in
├── include/universal_thread_pool/
│   ├── thread_pool.hpp                # 统一入口头文件
│   ├── common.hpp                     # 公共类型、选项、取消令牌与指标
│   ├── thread_pool_core.hpp           # 线程池接口与任务提交模板
│   ├── executor.hpp                   # 执行器接口
│   ├── coroutine.hpp                  # C++20 协程适配
│   ├── task_group.hpp                 # 任务组与作用域任务管理
│   ├── task_graph.hpp                 # 任务依赖图
│   ├── runtime.hpp                    # 多线程池运行时与任务路由
│   └── c_api.h                        # C 语言接口
├── src/
│   ├── common.cpp                     # 公共工具与指标导出
│   ├── thread_pool.cpp                # 工作线程、队列调度与生命周期管理
│   ├── executor.cpp                   # 执行器实现
│   ├── task_group.cpp                 # 任务组实现
│   ├── task_graph.cpp                 # 依赖解析与任务图执行
│   ├── runtime.cpp                    # 多线程池运行时实现
│   └── c_api.cpp                      # C 接口实现
├── examples/
│   └── basic.cpp                      # 综合使用示例
└── tests/
    └── thread_pool_tests.cpp           # 功能测试
```

`thread_pool` 负责工作线程、任务队列和调度，是任务执行的核心。调用方通过 `submit` 获取结果，通过 `detach` 提交无需返回值的任务，也可以通过 `executor` 携带默认优先级提交任务。

`task_group` 将一组任务组织起来，提供统一等待和取消操作；`task_graph` 通过依赖关系组织任务，在前置任务完成后调度后继任务。任务延续与数据流接口用于连接异步结果。

`thread_pool_runtime` 管理 CPU、IO 和后台线程池，按照任务类型路由执行。指标接口用于查看任务计数、队列状态、工作线程状态和延迟分布，`c_api.h` 为 C 调用方提供创建、提交、等待、关闭和指标查询入口。

## 构建与测试

需要 CMake 3.16 或更新版本，以及支持 C++17 的编译器。以下命令用于 Linux 构建；协程和 `std::stop_token` 接口需要 C++20。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure
```

测试使用始终执行的 `CHECK` 检查，支持 Debug 和 Release 配置。使用 `-DCMAKE_BUILD_TYPE=Release` 可构建 Release 版本。CI 分别运行 Debug、Release、Address/UndefinedBehaviorSanitizer 和 ThreadSanitizer 检查。

构建后执行基础示例：

```sh
./build/thread_pool_basic
```

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
| `UNIVERSAL_THREAD_POOL_SANITIZER` | 空 | GCC/Clang 下可选 `address` 或 `thread` |
