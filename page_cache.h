// created by wei on 2025-5-26

#ifndef PAGE_CACHE_H
#define PAGE_CACHE_H
#include <atomic>
#include <cstddef>
#include <map>
#include <span>
#include <optional>
#include <set>
#include <vector>
#include<mutex>

#include "utils.h"

namespace memory_pool
{
    class page_cache
    {
    public:
        static constexpr size_t PAGE_ALLOCATE_COUNT = 2048;
        static page_cache &GetInstance()
        {
            static page_cache instance;
            return instance;
        }

        // 申请指定页数的内存
        std::optional<memory_span> allocate_page(size_t page_count);

        // 回收指定页数的内存
        void deallocate_page(memory_span page);
        // 分配一个单元的内存，用于处理超大块内存
        std::optional<memory_span> allocate_unit(size_t memory_size);
        // 回收一个单元的内存，用于回收超大块内存
        void deallocate_unit(memory_span memories);

        // 关闭内存池
        void stop();

        ~page_cache();

    private:
        page_cache() = default;
        // 只申请，不回收，只有在销毁时回收
        std::optional<memory_span> system_allocate_memory(size_t page_count);

        // 回收内存，只有在析构函数中调用
        void system_deallocate_memory(memory_span page);

        //按页数存储空闲 span
        std::map<size_t, std::set<memory_span>> free_page_store = {};

        // 按起始地址存储空闲 span
        std::map<std::byte *, memory_span> free_page_map = {};

        // 用于回收时 munmap
        std::vector<memory_span> page_vector = {};
        
        // 表示当前的内存池是不是已经关闭了
        bool m_stop = false;
        // 并发控制
        std::mutex m_mutex;
    };
} // memory_pool
#endif