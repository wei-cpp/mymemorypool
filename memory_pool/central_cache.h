// created by wei on 2025-5-26

#ifndef CENTRAL_CACHE_H
#define CENTRAL_CAHCE_H
#include <atomic>
#include <list>
#include <map>
#include <span>
#include <optional>
#include <mutex>
#include <set>
#include <unordered_map>

#include "utils.h"

namespace memory_pool
{

    class central_cache
    {
    public:
        static central_cache &GetInstance()
        {
            static central_cache instance;
            return instance;
        }

        // 用于分配指定个数指定大小的空间
        std::optional<std::byte *> allocate(size_t memory_size, size_t block_count);

        void deallocate(std::byte *memory_list, size_t memory_size);

    private:
        central_cache() = default;

        // 将分配出去的内存块记录下来
        void record_allocated_memory_span(std::byte* memory, const size_t memory_size);

        std::optional<memory_span> get_page_from_page_cache(size_t page_allocate_count);

        size_t get_page_allocate_count(size_t memory_size);
    private:
        // 按不同长度单位存放的空闲链表
        std::array<std::byte *, size_utils::CACHE_LINE_SIZE> m_free_array = {};
        // 每个空闲链表的长度有多少
        std::array<size_t, size_utils::CACHE_LINE_SIZE> m_free_array_size = {};
        // 各个长度单位的锁
        std::array<std::atomic_flag, size_utils::CACHE_LINE_SIZE> m_status;
        // 用于页面的管理
        std::array<std::map<std::byte *, page_span>, size_utils::CACHE_LINE_SIZE> m_page_set;
    };
}
#endif // CENTRAL_CAHCE_H