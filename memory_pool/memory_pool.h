// created by wei on 2025-5-27

#ifndef MEMORY_POOL_H
#define MEMORY_POOL_H

#include <optional>

#include "thread_cache.h"
namespace memory_pool
{

    class memory_pool
    {
    public:
        static std::optional<void *> allocate(size_t memory_size)
        {
            return thread_cache::GetInstance().allocate(memory_size);
        }

        static void deallocate(void *start_p, size_t memory_size)
        {
            thread_cache::GetInstance().deallocate(start_p, memory_size);
        }
    };
} // memory_pool
#endif