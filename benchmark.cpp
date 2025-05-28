#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <thread>
#include <vector>
#include <malloc.h>
#include "memory_pool/memory_pool.h"

// 统计信息结构体
struct Statistics {
    std::vector<double> alloc_latencies;  // 分配延迟记录 (us)
    std::vector<double> free_latencies;   // 释放延迟记录 (us)
    std::atomic<size_t> success_allocs{0};  // 成功分配次数
    std::atomic<size_t> failed_allocs{0};   // 失败分配次数
    std::atomic<size_t> success_frees{0};   // 成功释放次数
    std::atomic<size_t> peak_memory{0};     // 峰值内存 (bytes)
    std::mutex latency_mutex;  // 用于保护延迟数据的互斥锁

    void add_alloc_latency(double latency) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        alloc_latencies.push_back(latency);
    }

    void add_free_latency(double latency) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        free_latencies.push_back(latency);
    }
};

// 计算百分位数
double calculate_percentile(std::vector<double>& data, double percentile) {
    if (data.empty()) return 0.0;
    std::sort(data.begin(), data.end());
    size_t index = static_cast<size_t>(data.size() * percentile / 100);
    return data[index];
}

// 计算平均值
double calculate_mean(const std::vector<double>& data) {
    if (data.empty()) return 0.0;
    return std::accumulate(data.begin(), data.end(), 0.0) / data.size();
}

// 存储分配的内存块信息
struct AllocatedBlock {
    void* ptr;
    size_t size;
};

// 标准malloc/free包装器
struct MallocAllocator {
    void* allocate(size_t size) {
        return malloc(size);
    }
    void deallocate(void* ptr) {
        free(ptr);
    }
};

// 内存池包装器
struct MemoryPoolAllocator {
    memory_pool::memory_pool pool;
    
    std::optional<void*> allocate(size_t size) {
        return pool.allocate(size);
    }
    
    void deallocate(void* ptr, size_t size) {
        memory_pool::memory_pool::deallocate(ptr, size);
    }
};

// 工作线程函数
void worker_thread_malloc(
    Statistics& stats,
    MallocAllocator& allocator,
    int duration_seconds,
    int min_alloc_size,
    int max_alloc_size) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(min_alloc_size, max_alloc_size);
    
    std::vector<AllocatedBlock> allocated_blocks;
    auto start_time = std::chrono::steady_clock::now();
    
    while (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count() < duration_seconds) {
        
        bool should_allocate = allocated_blocks.empty() || 
                             (std::uniform_real_distribution<>(0, 1)(gen) < 0.7);

        if (should_allocate) {
            size_t size = size_dist(gen);
            auto alloc_start = std::chrono::high_resolution_clock::now();
            void* ptr = allocator.allocate(size);
            auto alloc_end = std::chrono::high_resolution_clock::now();
            
            if (ptr) {
                allocated_blocks.push_back(AllocatedBlock{ptr, size});
                stats.success_allocs++;
                double latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    alloc_end - alloc_start).count();
                stats.add_alloc_latency(latency);
                
                size_t current_memory = allocated_blocks.size() * size;
                size_t peak = stats.peak_memory.load();
                while (current_memory > peak && 
                       !stats.peak_memory.compare_exchange_weak(peak, current_memory));
            } else {
                stats.failed_allocs++;
            }
        } else if (!allocated_blocks.empty()) {
            size_t index = std::uniform_int_distribution<size_t>(
                0, allocated_blocks.size() - 1)(gen);
            auto block = allocated_blocks[index];
            
            auto free_start = std::chrono::high_resolution_clock::now();
            allocator.deallocate(block.ptr);
            auto free_end = std::chrono::high_resolution_clock::now();
            
            double latency = std::chrono::duration_cast<std::chrono::microseconds>(
                free_end - free_start).count();
            
            stats.success_frees++;
            stats.add_free_latency(latency);
            
            allocated_blocks.erase(allocated_blocks.begin() + index);
        }
    }
    
    for (const auto& block : allocated_blocks) {
        allocator.deallocate(block.ptr);
        stats.success_frees++;
    }
}

void worker_thread_mempool(
    Statistics& stats,
    MemoryPoolAllocator& allocator,
    int duration_seconds,
    int min_alloc_size,
    int max_alloc_size) {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> size_dist(min_alloc_size, max_alloc_size);
    
    std::vector<AllocatedBlock> allocated_blocks;
    auto start_time = std::chrono::steady_clock::now();
    
    while (std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count() < duration_seconds) {
        
        bool should_allocate = allocated_blocks.empty() || 
                             (std::uniform_real_distribution<>(0, 1)(gen) < 0.7);

        if (should_allocate) {
            size_t size = size_dist(gen);
            auto alloc_start = std::chrono::high_resolution_clock::now();
            auto ptr_opt = allocator.allocate(size);
            auto alloc_end = std::chrono::high_resolution_clock::now();
            
            if (ptr_opt.has_value()) {
                allocated_blocks.push_back(AllocatedBlock{ptr_opt.value(), size});
                stats.success_allocs++;
                double latency = std::chrono::duration_cast<std::chrono::microseconds>(
                    alloc_end - alloc_start).count();
                stats.add_alloc_latency(latency);
                
                size_t current_memory = allocated_blocks.size() * size;
                size_t peak = stats.peak_memory.load();
                while (current_memory > peak && 
                       !stats.peak_memory.compare_exchange_weak(peak, current_memory));
            } else {
                stats.failed_allocs++;
            }
        } else if (!allocated_blocks.empty()) {
            size_t index = std::uniform_int_distribution<size_t>(
                0, allocated_blocks.size() - 1)(gen);
            auto block = allocated_blocks[index];
            
            auto free_start = std::chrono::high_resolution_clock::now();
            allocator.deallocate(block.ptr, block.size);
            auto free_end = std::chrono::high_resolution_clock::now();
            
            double latency = std::chrono::duration_cast<std::chrono::microseconds>(
                free_end - free_start).count();
            
            stats.success_frees++;
            stats.add_free_latency(latency);
            
            allocated_blocks.erase(allocated_blocks.begin() + index);
        }
    }
    
    for (const auto& block : allocated_blocks) {
        allocator.deallocate(block.ptr, block.size);
        stats.success_frees++;
    }
}

// 运行基准测试 - malloc版本
void run_benchmark_malloc(const std::string& allocator_name,
                         MallocAllocator& allocator,
                         int thread_count,
                         int duration_seconds,
                         int min_alloc_size,
                         int max_alloc_size,
                         Statistics& stats) {
    std::vector<std::thread> threads;
    auto start_time = std::chrono::steady_clock::now();

    // 创建工作线程
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker_thread_malloc,
                           std::ref(stats),
                           std::ref(allocator),
                           duration_seconds,
                           min_alloc_size,
                           max_alloc_size);
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time).count();

    // 输出结果
    std::cout << "\n=== " << allocator_name << " Performance Report ===\n"
              << "Operations per Second: "
              << std::fixed << std::setprecision(2)
              << (stats.success_allocs + stats.success_frees) / static_cast<double>(duration)
              << " Ops/Sec\n"
              << "Average Allocation Latency: "
              << calculate_mean(stats.alloc_latencies) << " us\n"
              << "P99 Allocation Latency: "
              << calculate_percentile(stats.alloc_latencies, 99) << " us\n"
              << "Average Free Latency: "
              << calculate_mean(stats.free_latencies) << " us\n"
              << "P99 Free Latency: "
              << calculate_percentile(stats.free_latencies, 99) << " us\n"
              << "Peak Memory: "
              << stats.peak_memory.load() / (1024.0 * 1024.0) << " MB\n"
              << "Successful Allocations: " << stats.success_allocs << "\n"
              << "Failed Allocations: " << stats.failed_allocs << "\n"
              << "Successful Frees: " << stats.success_frees << "\n";
}

// 运行基准测试 - 内存池版本
void run_benchmark_mempool(const std::string& allocator_name,
                         MemoryPoolAllocator& allocator,
                         int thread_count,
                         int duration_seconds,
                         int min_alloc_size,
                         int max_alloc_size,
                         Statistics& stats) {
    std::vector<std::thread> threads;
    auto start_time = std::chrono::steady_clock::now();

    // 创建工作线程
    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back(worker_thread_mempool,
                           std::ref(stats),
                           std::ref(allocator),
                           duration_seconds,
                           min_alloc_size,
                           max_alloc_size);
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(
        end_time - start_time).count();

    // 输出结果
    std::cout << "\n=== " << allocator_name << " Performance Report ===\n"
              << "Operations per Second: "
              << std::fixed << std::setprecision(2)
              << (stats.success_allocs + stats.success_frees) / static_cast<double>(duration)
              << " Ops/Sec\n"
              << "Average Allocation Latency: "
              << calculate_mean(stats.alloc_latencies) << " us\n"
              << "P99 Allocation Latency: "
              << calculate_percentile(stats.alloc_latencies, 99) << " us\n"
              << "Average Free Latency: "
              << calculate_mean(stats.free_latencies) << " us\n"
              << "P99 Free Latency: "
              << calculate_percentile(stats.free_latencies, 99) << " us\n"
              << "Peak Memory: "
              << stats.peak_memory.load() / (1024.0 * 1024.0) << " MB\n"
              << "Successful Allocations: " << stats.success_allocs << "\n"
              << "Failed Allocations: " << stats.failed_allocs << "\n"
              << "Successful Frees: " << stats.success_frees << "\n";
}

// 打印比较行
void print_comparison_line(const std::string& metric,
                          const std::string& unit,
                          double pool_val,
                          double malloc_val,
                          bool higher_is_better = true)
{
    std::cout << std::left << std::setw(25) << metric
              << std::right << std::setw(15) << std::fixed << std::setprecision(2) << pool_val
              << std::setw(15) << malloc_val
              << " " << unit;
    
    // 计算相对性能
    if (malloc_val > 0) {
        double pool_vs_malloc = (pool_val / malloc_val - 1.0) * 100.0;
        if (!higher_is_better) {
            pool_vs_malloc = -pool_vs_malloc;  // 翻转百分比，因为对于延迟来说，更低更好
        }
        std::cout << std::setw(20)
                  << ((pool_vs_malloc > 0) ? "+" : "")
                  << pool_vs_malloc << "%";
    }
    std::cout << "\n";
}

// 打印整数比较行
void print_comparison_line(const std::string& metric,
                          const std::string& unit,
                          size_t pool_val,
                          size_t malloc_val,
                          bool higher_is_better = true)
{
    print_comparison_line(metric, unit,
                         static_cast<double>(pool_val),
                         static_cast<double>(malloc_val),
                         higher_is_better);
}

int main() {
    std::cout << "\n=== Memory Allocator Benchmark ===\n"
              << "Duration: 30 seconds\n"
              << "Allocation size range: 8 - 4096 bytes\n"
              << "Allocation probability: 70%\n\n";

    Statistics malloc_stats;
    Statistics pool_stats;
    
    std::cout << "Testing standard malloc/free...\n\n";
    {
        MallocAllocator allocator;
        run_benchmark_malloc("Standard malloc/free", allocator, 1, 30, 8, 4096, malloc_stats);
    }

    std::cout << "Testing memory pool...\n\n";
    {
        MemoryPoolAllocator allocator;
        run_benchmark_mempool("Memory Pool", allocator, 1, 30, 8, 4096, pool_stats);
    }

    // 打印比较表格
    std::cout << "\n=== Performance Comparison ===\n";
    std::cout << std::left << std::setw(25) << "Metric"
              << std::right << std::setw(15) << "Memory Pool"
              << std::setw(15) << "malloc/free"
              << std::setw(25) << "(vs malloc)\n";
    std::cout << std::string(80, '-') << "\n";

    // 计算每秒操作数
    double pool_ops = (pool_stats.success_allocs + pool_stats.success_frees) / 30.0;
    double malloc_ops = (malloc_stats.success_allocs + malloc_stats.success_frees) / 30.0;

    // 打印所有指标
    print_comparison_line("Operations/sec", "ops", pool_ops, malloc_ops);
    print_comparison_line("Avg alloc latency", "us",
                         calculate_mean(pool_stats.alloc_latencies),
                         calculate_mean(malloc_stats.alloc_latencies),
                         false);
    print_comparison_line("P99 alloc latency", "us",
                         calculate_percentile(pool_stats.alloc_latencies, 99),
                         calculate_percentile(malloc_stats.alloc_latencies, 99),
                         false);
    print_comparison_line("Avg free latency", "us",
                         calculate_mean(pool_stats.free_latencies),
                         calculate_mean(malloc_stats.free_latencies),
                         false);
    print_comparison_line("P99 free latency", "us",
                         calculate_percentile(pool_stats.free_latencies, 99),
                         calculate_percentile(malloc_stats.free_latencies, 99),
                         false);
    print_comparison_line("Peak memory", "MB",
                         pool_stats.peak_memory.load() / (1024.0 * 1024.0),
                         malloc_stats.peak_memory.load() / (1024.0 * 1024.0),
                         false);
    print_comparison_line("Successful allocs", "",
                         pool_stats.success_allocs,
                         malloc_stats.success_allocs);
    print_comparison_line("Failed allocs", "",
                         pool_stats.failed_allocs,
                         malloc_stats.failed_allocs,
                         false);
    print_comparison_line("Successful frees", "",
                         pool_stats.success_frees,
                         malloc_stats.success_frees);

    return 0;
} 