#include <iostream>          // 用于输入输出流
#include <vector>            // 用于 std::vector 容器
#include <thread>            // 用于 std::thread 多线程支持
#include <chrono>            // 用于时间测量 (high_resolution_clock)
#include <random>            // 用于随机数生成
#include <numeric>           // 用于 std::accumulate (虽然此处未使用，但相关头文件可能有用)
#include <atomic>            // 用于原子操作，保证线程安全统计
#include <memory>            // 用于智能指针和 PMR (多态内存资源)
#include <mutex>             // 用于互斥锁 (std::mutex)
#include <algorithm>         // 用于 std::sort, std::min 等算法
#include <iomanip>           // 用于格式化输出 (setw, setprecision, fixed)
#include <list>              // 使用 std::list 方便随机移除元素
#include <stdexcept>         // 用于 std::bad_alloc 异常
#include <memory_resource>   // C++17/20/23 PMR 特性
#include "memory_pool/memory_pool.h"

// --- 配置参数 ---
const unsigned int NUM_THREADS = std::thread::hardware_concurrency(); // 线程数，使用硬件支持的最大并发数
const size_t NUM_OPERATIONS_PER_THREAD = 100000; // 每个线程执行的操作总数
const size_t MIN_ALLOC_SIZE = 8;                  // 最小分配内存块大小 (字节)
const size_t MAX_ALLOC_SIZE = 4 * 1024;           // 最大分配内存块大小 (字节)，覆盖常见的小对象大小
const int ALLOC_PERCENTAGE = 60;                  // 分配操作所占的百分比
const unsigned int RANDOM_SEED = 54321;           // 固定的随机种子，确保每次运行结果可复现
const size_t DEFAULT_ALIGNMENT = alignof(std::max_align_t);

// --- 统计数据结构 ---
struct Stats {
    std::atomic<size_t> total_allocs{0};         // 总尝试分配次数
    std::atomic<size_t> successful_allocs{0};    // 成功分配次数
    std::atomic<size_t> failed_allocs{0};        // 失败分配次数
    std::atomic<size_t> total_deallocs{0};       // 总尝试释放次数 (在基准测试计时内)
    std::atomic<long long> total_alloc_latency_ns{0}; // 总分配延迟 (纳秒)
    std::atomic<long long> total_dealloc_latency_ns{0};// 总释放延迟 (纳秒)
    std::atomic<size_t> peak_memory_usage{0};    // 峰值内存使用量 (各线程峰值之和，近似值)

    std::vector<long long> alloc_latencies;       // 存储所有单次分配延迟
    std::vector<long long> dealloc_latencies;     // 存储所有单次释放延迟
    std::mutex latency_mutex;                     // 保护延迟向量在合并时线程安全

    long long total_duration_ms = 0;              // 基准测试总运行时间 (毫秒)
    double ops_per_sec = 0.0;                     // 每秒操作数
    double p99_alloc_latency_ns = 0.0;           // P99 分配延迟 (纳秒)
    double p99_dealloc_latency_ns = 0.0;         // P99 释放延迟 (纳秒)

    void clear() {
        total_allocs = 0;
        successful_allocs = 0;
        failed_allocs = 0;
        total_deallocs = 0;
        total_alloc_latency_ns = 0;
        total_dealloc_latency_ns = 0;
        peak_memory_usage = 0;
        alloc_latencies.clear();
        dealloc_latencies.clear();
        total_duration_ms = 0;
        ops_per_sec = 0.0;
        p99_alloc_latency_ns = 0.0;
        p99_dealloc_latency_ns = 0.0;
    }

    Stats& operator+=(const Stats& other) {
        total_allocs += other.total_allocs.load();
        successful_allocs += other.successful_allocs.load();
        failed_allocs += other.failed_allocs.load();
        total_deallocs += other.total_deallocs.load();
        total_alloc_latency_ns += other.total_alloc_latency_ns.load();
        total_dealloc_latency_ns += other.total_dealloc_latency_ns.load();
        peak_memory_usage = std::max(peak_memory_usage.load(), other.peak_memory_usage.load());
        
        std::lock_guard<std::mutex> lock(latency_mutex);
        if (!alloc_latencies.empty() && !other.alloc_latencies.empty()) {
            alloc_latencies.insert(alloc_latencies.end(), 
                                 other.alloc_latencies.begin(), 
                                 other.alloc_latencies.end());
        }
        if (!dealloc_latencies.empty() && !other.dealloc_latencies.empty()) {
            dealloc_latencies.insert(dealloc_latencies.end(),
                                   other.dealloc_latencies.begin(),
                                   other.dealloc_latencies.end());
        }
        return *this;
    }
};

// --- 操作类型枚举 ---
enum class OpType { ALLOCATE, DEALLOCATE };

// --- 预生成的操作序列结构 ---
struct Operation {
    OpType type;
    size_t size;
};

// --- 工作线程函数模板 ---
template <typename AllocFunc, typename DeallocFunc>
void worker_thread(
    int thread_id,
    const std::vector<Operation>& operations,
    AllocFunc allocate_func,
    DeallocFunc deallocate_func,
    Stats& global_stats)
{
    size_t local_allocs = 0;
    size_t local_successful_allocs = 0;
    size_t local_failed_allocs = 0;
    size_t local_deallocs = 0;
    long long local_alloc_latency_ns = 0;
    long long local_dealloc_latency_ns = 0;
    size_t local_current_memory = 0;
    size_t local_peak_memory = 0;
    std::vector<long long> local_alloc_latencies_vec;
    std::vector<long long> local_dealloc_latencies_vec;
    local_alloc_latencies_vec.reserve(operations.size() * ALLOC_PERCENTAGE / 100 + 1);
    local_dealloc_latencies_vec.reserve(operations.size() * (100 - ALLOC_PERCENTAGE) / 100 + 1);

    std::list<std::pair<void*, size_t>> allocations;
    std::mt19937 local_rng(RANDOM_SEED + thread_id);

    for (const auto& op : operations) {
        if (op.type == OpType::ALLOCATE) {
            local_allocs++;
            void* ptr = nullptr;
            bool success = false;
            auto alloc_start = std::chrono::high_resolution_clock::now();
            try {
                ptr = allocate_func(op.size);
                if (ptr) {
                    success = true;
                }
            } catch (const std::bad_alloc&) {
                success = false;
                ptr = nullptr;
            } catch (const std::exception&) {
                success = false;
                ptr = nullptr;
            } catch (...) {
                success = false;
                ptr = nullptr;
            }
            auto alloc_end = std::chrono::high_resolution_clock::now();
            auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(alloc_end - alloc_start).count();
            local_alloc_latency_ns += latency;
            local_alloc_latencies_vec.push_back(latency);

            if (success && ptr) {
                local_successful_allocs++;
                allocations.push_back({ptr, op.size});
                local_current_memory += op.size;
                if (local_current_memory > local_peak_memory) {
                    local_peak_memory = local_current_memory;
                }
            } else {
                local_failed_allocs++;
            }
        } else {
            if (!allocations.empty()) {
                std::uniform_int_distribution<size_t> dist(0, allocations.size() - 1);
                size_t index_to_remove = dist(local_rng);
                auto it = allocations.begin();
                std::advance(it, index_to_remove);
                void* ptr_to_free = it->first;
                size_t size_to_free = it->second;

                local_deallocs++;
                auto dealloc_start = std::chrono::high_resolution_clock::now();
                try {
                    deallocate_func(ptr_to_free, size_to_free);
                } catch(const std::exception&) {
                } catch(...) {
                }
                auto dealloc_end = std::chrono::high_resolution_clock::now();
                auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(dealloc_end - dealloc_start).count();
                local_dealloc_latency_ns += latency;
                local_dealloc_latencies_vec.push_back(latency);

                local_current_memory -= size_to_free;
                allocations.erase(it);
            }
        }
    }

    // 更新全局统计
    global_stats.total_allocs += local_allocs;
    global_stats.successful_allocs += local_successful_allocs;
    global_stats.failed_allocs += local_failed_allocs;
    global_stats.total_deallocs += local_deallocs;
    global_stats.total_alloc_latency_ns += local_alloc_latency_ns;
    global_stats.total_dealloc_latency_ns += local_dealloc_latency_ns;
    
    size_t prev_peak = global_stats.peak_memory_usage.load(std::memory_order_relaxed);
    while (local_peak_memory > prev_peak) {
        if (global_stats.peak_memory_usage.compare_exchange_weak(prev_peak, local_peak_memory,
                                                               std::memory_order_relaxed)) {
            break;
        }
    }

    // 将本地延迟数据合并到全局
    {
        std::lock_guard<std::mutex> lock(global_stats.latency_mutex);
        global_stats.alloc_latencies.insert(
            global_stats.alloc_latencies.end(),
            local_alloc_latencies_vec.begin(),
            local_alloc_latencies_vec.end());
        global_stats.dealloc_latencies.insert(
            global_stats.dealloc_latencies.end(),
            local_dealloc_latencies_vec.begin(),
            local_dealloc_latencies_vec.end());
    }

    // 清理任何剩余的分配
    for (const auto& alloc : allocations) {
        try {
            deallocate_func(alloc.first, alloc.second);
        } catch (...) {
        }
    }
}

// 计算P99延迟
double calculate_p99_latency(std::vector<long long>& latencies) {
    if (latencies.empty()) return 0.0;
    std::sort(latencies.begin(), latencies.end());
    size_t p99_index = (latencies.size() * 99) / 100;
    return static_cast<double>(latencies[p99_index]);
}

// 运行基准测试
template <typename AllocFunc, typename DeallocFunc>
void run_benchmark(const std::string& name,
                  const std::vector<std::vector<Operation>>& ops_per_thread,
                  AllocFunc allocate_func,
                  DeallocFunc deallocate_func,
                  Stats& stats)
{
    std::vector<std::thread> threads;
    stats.clear();

    auto start_time = std::chrono::high_resolution_clock::now();

    // 启动所有工作线程
    for (size_t i = 0; i < ops_per_thread.size(); ++i) {
        threads.emplace_back(worker_thread<AllocFunc, DeallocFunc>,
                           static_cast<int>(i),
                           std::ref(ops_per_thread[i]),
                           allocate_func,
                           deallocate_func,
                           std::ref(stats));
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    stats.total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        end_time - start_time).count();

    // 计算最终统计数据
    stats.ops_per_sec = (stats.successful_allocs + stats.total_deallocs) * 1000.0 / stats.total_duration_ms;
    stats.p99_alloc_latency_ns = calculate_p99_latency(stats.alloc_latencies);
    stats.p99_dealloc_latency_ns = calculate_p99_latency(stats.dealloc_latencies);

    // 输出结果
    std::cout << "\n=== " << name << " Performance Report ===\n"
              << std::fixed << std::setprecision(2)
              << "Operations per Second: " << stats.ops_per_sec << " Ops/Sec\n"
              << "Average Allocation Latency: " 
              << (stats.successful_allocs > 0 ? static_cast<double>(stats.total_alloc_latency_ns.load()) / stats.successful_allocs : 0.0)
              << " ns\n"
              << "P99 Allocation Latency: " << stats.p99_alloc_latency_ns << " ns\n"
              << "Average Deallocation Latency: "
              << (stats.total_deallocs > 0 ? static_cast<double>(stats.total_dealloc_latency_ns.load()) / stats.total_deallocs : 0.0)
              << " ns\n"
              << "P99 Deallocation Latency: " << stats.p99_dealloc_latency_ns << " ns\n"
              << "Peak Memory: " << (stats.peak_memory_usage.load() / (1024.0 * 1024.0)) << " MB\n"
              << "Successful Allocations: " << stats.successful_allocs << "\n"
              << "Failed Allocations: " << stats.failed_allocs << "\n"
              << "Successful Deallocations: " << stats.total_deallocs << "\n";
}

// 打印对比结果表格
void print_comparison_line(const std::string& metric,
                          const std::string& unit,
                          double pool_val,
                          double malloc_val,
                          double pmr_val,
                          bool higher_is_better = true)
{
    std::cout << std::left << std::setw(25) << metric
              << std::right << std::setw(15) << std::fixed << std::setprecision(2) << pool_val
              << std::setw(15) << malloc_val
              << std::setw(15) << pmr_val << " " << unit;
    
    // 计算相对性能
    if (malloc_val > 0) {
        double pool_vs_malloc = (pool_val / malloc_val - 1.0) * 100.0;
        std::cout << std::setw(20)
                  << ((higher_is_better && pool_vs_malloc > 0) || (!higher_is_better && pool_vs_malloc < 0) ? "+" : "")
                  << pool_vs_malloc << "%";
    }
    std::cout << "\n";
}

void print_comparison_line(const std::string& metric,
                          const std::string& unit,
                          size_t pool_val,
                          size_t malloc_val,
                          size_t pmr_val,
                          bool higher_is_better = true)
{
    print_comparison_line(metric, unit,
                         static_cast<double>(pool_val),
                         static_cast<double>(malloc_val),
                         static_cast<double>(pmr_val),
                         higher_is_better);
}

int main() {
    try {
        std::cout << "\n=== Memory Allocator Performance Benchmark ===\n"
                  << "Threads: " << NUM_THREADS << "\n"
                  << "Operations per thread: " << NUM_OPERATIONS_PER_THREAD << "\n"
                  << "Allocation size range: " << MIN_ALLOC_SIZE << " - " << MAX_ALLOC_SIZE << " bytes\n"
                  << "Allocation percentage: " << ALLOC_PERCENTAGE << "%\n\n";

        // 为每个线程生成操作序列
        std::vector<std::vector<Operation>> ops_per_thread(NUM_THREADS);
        std::mt19937 master_rng(RANDOM_SEED);
        std::uniform_int_distribution<size_t> size_dist(MIN_ALLOC_SIZE, MAX_ALLOC_SIZE);

        // 生成每个线程的操作序列
        for (auto& thread_ops : ops_per_thread) {
            thread_ops.reserve(NUM_OPERATIONS_PER_THREAD);
            for (size_t i = 0; i < NUM_OPERATIONS_PER_THREAD; ++i) {
                bool should_allocate = (std::uniform_int_distribution<int>(1, 100)(master_rng) <= ALLOC_PERCENTAGE);
                thread_ops.push_back({
                    should_allocate ? OpType::ALLOCATE : OpType::DEALLOCATE,
                    size_dist(master_rng)
                });
            }
        }

        // 分别运行各个测试，每个测试都用try-catch包围
        Stats pool_stats, malloc_stats, pmr_stats;
        bool pool_success = false, malloc_success = false, pmr_success = false;

        // 测试内存池
        try {
            auto memory_pool_alloc = [](size_t size) -> void* {
                auto result = memory_pool::memory_pool::allocate(size);
                return result.has_value() ? result.value() : nullptr;
            };
            auto memory_pool_dealloc = [](void* p, size_t s) {
                if (p) memory_pool::memory_pool::deallocate(p, s);
            };
            run_benchmark("Memory Pool", ops_per_thread, memory_pool_alloc, memory_pool_dealloc, pool_stats);
            pool_success = true;
        } catch (const std::exception& e) {
            std::cerr << "Memory Pool test failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Memory Pool test failed with unknown error" << std::endl;
        }

        // 测试标准 malloc/free
        try {
            auto malloc_alloc = [](size_t size) -> void* {
                return malloc(size);
            };
            auto malloc_dealloc = [](void* p, size_t) {
                if (p) free(p);
            };
            run_benchmark("Standard malloc/free", ops_per_thread, malloc_alloc, malloc_dealloc, malloc_stats);
            malloc_success = true;
        } catch (const std::exception& e) {
            std::cerr << "Malloc test failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "Malloc test failed with unknown error" << std::endl;
        }

        // 测试PMR
        try {
            std::pmr::monotonic_buffer_resource upstream(1024 * 1024); // 1MB initial block
            std::pmr::synchronized_pool_resource resource(&upstream);
            
            auto pmr_alloc = [&resource](size_t size) -> void* {
                try {
                    return resource.allocate(size, DEFAULT_ALIGNMENT);
                } catch (...) {
                    return nullptr;
                }
            };
            auto pmr_dealloc = [&resource](void* p, size_t size) {
                if (p) resource.deallocate(p, size, DEFAULT_ALIGNMENT);
            };
            run_benchmark("PMR", ops_per_thread, pmr_alloc, pmr_dealloc, pmr_stats);
            pmr_success = true;
        } catch (const std::exception& e) {
            std::cerr << "PMR test failed: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "PMR test failed with unknown error" << std::endl;
        }

        // 如果至少有两个测试成功，打印比较结果
        if ((pool_success ? 1 : 0) + (malloc_success ? 1 : 0) + (pmr_success ? 1 : 0) >= 2) {
            std::cout << "\n=== Performance Comparison ===\n";
            std::cout << std::left << std::setw(25) << "Metric"
                      << std::right;
            if (pool_success) std::cout << std::setw(15) << "Memory Pool";
            if (malloc_success) std::cout << std::setw(15) << "malloc/free";
            if (pmr_success) std::cout << std::setw(15) << "PMR";
            std::cout << std::setw(25) << " (vs malloc)\n";
            std::cout << std::string(95, '-') << "\n";

            // 辅助lambda用于安全打印
            auto safe_print = [](bool success, double val) {
                if (success) std::cout << std::setw(15) << std::fixed << std::setprecision(2) << val;
                else std::cout << std::setw(15) << "N/A";
            };

            auto print_metric = [&](const std::string& metric, const std::string& unit,
                                  double pool_val, double malloc_val, double pmr_val,
                                  bool higher_is_better = true) {
                std::cout << std::left << std::setw(25) << metric;
                safe_print(pool_success, pool_val);
                safe_print(malloc_success, malloc_val);
                safe_print(pmr_success, pmr_val);
                std::cout << " " << unit;
                
                if (malloc_success && pool_success) {
                    double pool_vs_malloc = (pool_val / malloc_val - 1.0) * 100.0;
                    std::cout << std::setw(20)
                              << ((higher_is_better && pool_vs_malloc > 0) || 
                                 (!higher_is_better && pool_vs_malloc < 0) ? "+" : "")
                              << pool_vs_malloc << "%";
                }
                std::cout << "\n";
            };

            print_metric("Operations/sec", "ops",
                        pool_stats.ops_per_sec,
                        malloc_stats.ops_per_sec,
                        pmr_stats.ops_per_sec);

            print_metric("Avg alloc latency", "ns",
                        pool_stats.successful_allocs > 0 ? 
                        static_cast<double>(pool_stats.total_alloc_latency_ns.load()) / pool_stats.successful_allocs : 0.0,
                        malloc_stats.successful_allocs > 0 ? 
                        static_cast<double>(malloc_stats.total_alloc_latency_ns.load()) / malloc_stats.successful_allocs : 0.0,
                        pmr_stats.successful_allocs > 0 ? 
                        static_cast<double>(pmr_stats.total_alloc_latency_ns.load()) / pmr_stats.successful_allocs : 0.0,
                        false);

            print_metric("P99 alloc latency", "ns",
                        pool_stats.p99_alloc_latency_ns,
                        malloc_stats.p99_alloc_latency_ns,
                        pmr_stats.p99_alloc_latency_ns,
                        false);

            print_metric("Avg dealloc latency", "ns",
                        pool_stats.total_deallocs > 0 ? 
                        static_cast<double>(pool_stats.total_dealloc_latency_ns.load()) / pool_stats.total_deallocs : 0.0,
                        malloc_stats.total_deallocs > 0 ? 
                        static_cast<double>(malloc_stats.total_dealloc_latency_ns.load()) / malloc_stats.total_deallocs : 0.0,
                        pmr_stats.total_deallocs > 0 ? 
                        static_cast<double>(pmr_stats.total_dealloc_latency_ns.load()) / pmr_stats.total_deallocs : 0.0,
                        false);

            print_metric("P99 dealloc latency", "ns",
                        pool_stats.p99_dealloc_latency_ns,
                        malloc_stats.p99_dealloc_latency_ns,
                        pmr_stats.p99_dealloc_latency_ns,
                        false);

            print_metric("Peak memory", "MB",
                        pool_stats.peak_memory_usage.load() / (1024.0 * 1024.0),
                        malloc_stats.peak_memory_usage.load() / (1024.0 * 1024.0),
                        pmr_stats.peak_memory_usage.load() / (1024.0 * 1024.0),
                        false);

            print_metric("Successful allocs", "",
                        static_cast<double>(pool_stats.successful_allocs),
                        static_cast<double>(malloc_stats.successful_allocs),
                        static_cast<double>(pmr_stats.successful_allocs));

            print_metric("Failed allocs", "",
                        static_cast<double>(pool_stats.failed_allocs),
                        static_cast<double>(malloc_stats.failed_allocs),
                        static_cast<double>(pmr_stats.failed_allocs),
                        false);

            print_metric("Successful deallocs", "",
                        static_cast<double>(pool_stats.total_deallocs),
                        static_cast<double>(malloc_stats.total_deallocs),
                        static_cast<double>(pmr_stats.total_deallocs));
        } else {
            std::cerr << "\nNot enough successful tests to make meaningful comparisons.\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Program failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Program failed with unknown error" << std::endl;
        return 1;
    }

    return 0;
}