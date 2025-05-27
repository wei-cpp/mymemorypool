// created by wei on 2025-5-26

#include <cassert>
#include <cstring>
#include <iostream>
#include <mutex>
#include <bits/ostream.tcc>
#include <sys/mman.h>

#include "page_cache.h"

namespace memory_pool
{

    // 申请指定页数的内存
    std::optional<memory_span> page_cache::allocate_page(size_t page_count)
    {
        if (page_count == 0)
        {
            return std::nullopt;
        }
        std::unique_lock<std::mutex> guard(m_mutex);

        auto it = free_page_store.lower_bound(page_count);
        // 如果存在一个页面，这个页面的大小是大于或等于要分配的页面的
        while (it != free_page_store.end())
        {
            if (!it->second.empty())
            {

                auto mem_iter = it->second.begin();
                memory_span free_memory = *mem_iter;
                it->second.erase(mem_iter);
                free_page_map.erase(free_memory.data());

                // 开始分割获取出来的空闲空间
                size_t memory_to_use = page_count * size_utils::PAGE_SIZE;
                memory_span memory = free_memory.subspan(0, memory_to_use);
                free_memory = free_memory.subspan(memory_to_use);

                if (free_memory.size())
                {
                    // 如果还有空间，则插回到缓存中
                    free_page_store[free_memory.size() / size_utils::PAGE_SIZE].emplace(free_memory);
                    free_page_map.emplace(free_memory.data(), free_memory);
                }
                return memory;
            }
            ++it;
        }

        // 如果已经没有足够大的页面了，则向系统申请
        // 一次性分配8MB的大小，为2048个页面，而批量申请的全都取最大是4mb，-> 16KB(缓存最大大小) * 512(一次性管理最大个数) = 4MB
        size_t page_to_allocate = std::max(PAGE_ALLOCATE_COUNT, page_count);
        return system_allocate_memory(page_to_allocate).transform([this, page_count](memory_span memory)
                                                                  {
            // 存入总的内存，用于结尾回收内存
            //result 返回申请的，多的放入空闲链中
            page_vector.push_back(memory);
            size_t memory_to_use = page_count* size_utils::PAGE_SIZE;
            memory_span result = memory.subspan(0,memory_to_use);
            memory_span free_memory = memory.subspan(memory_to_use);
            if(free_memory.size()){
                size_t index = free_memory.size()/size_utils::PAGE_SIZE;
                free_page_store[index].emplace(free_memory);
                free_page_map.emplace(free_memory.data(),free_memory);
            }
            return result; });
    }

    // 回收指定页数的内存
    void page_cache::deallocate_page(memory_span page)
    { // 上层调用必须传入页的整数倍
        assert(page.size() % size_utils::PAGE_SIZE == 0);
        std::unique_lock<std::mutex> guard(m_mutex);

        // 只有在集合不空的时候才会考虑合并
        // 合并头
        while (!free_page_map.empty())
        {
            assert(!free_page_map.contains(page.data()));
            auto it = free_page_map.upper_bound(page.data());
            if (it != free_page_map.begin())
            {
                --it;
                const memory_span &memory = it->second;
                // 如果前面一段的空间与当前的相邻，则合并
                if (memory.data() + memory.size() == page.data())
                {
                    page = memory_span(memory.data(), memory.size() + page.size());

                    // 删除合并了的页
                    free_page_store[memory.size() / size_utils::PAGE_SIZE].erase(memory);
                    free_page_map.erase(it);
                }
                else
                {
                    break;
                }
            }
            else
            {
                break;
            }
        }
        // 合并尾
        while (!free_page_map.empty())
        {
            assert(!free_page_map.contains(page.data()));
            if (free_page_map.contains(page.data() + page.size()))
            {
                auto it = free_page_map.find(page.data() + page.size());
                memory_span next_memory = it->second;
                free_page_store[next_memory.size() / size_utils::PAGE_SIZE].erase(next_memory);
                free_page_map.erase(it);
                page = memory_span(page.data(), page.size() + next_memory.size());
            }
            else
            {
                break;
            }
        }
        size_t index = page.size() / size_utils::PAGE_SIZE;
        free_page_store[index].emplace(page);
        free_page_map.emplace(page.data(), page);
    }

    // 分配一个单元的内存，用于处理超大块内存
    std::optional<memory_span> page_cache::allocate_unit(size_t memory_size)
    {
        auto ret = malloc(memory_size);
        if (ret != nullptr)
        {
            return memory_span{static_cast<std::byte *>(ret), memory_size};
        }
        return std::nullopt;
    }

    // 回收一个单元的内存，用于回收超大块内存
    void page_cache::deallocate_unit(memory_span memories)
    {
        free(memories.data());
    }

    // 关闭内存池
    void page_cache::stop()
    {
        std::unique_lock<std::mutex> guard(m_mutex);
        if (m_stop == false)
        {
            m_stop = true;
            for (auto &i : page_vector)
            {
                system_deallocate_memory(i);
            }
        }
    }

    page_cache::~page_cache()
    {
        stop();
    }

    // 只申请，不回收，只有在销毁时回收
    std::optional<memory_span> page_cache::system_allocate_memory(size_t page_count)
    {
        const size_t size = page_count * size_utils::PAGE_SIZE;

        // 使用mmap分配内存
        void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (ptr == MAP_FAILED)
        {
            return std::nullopt;
        }

        memset(ptr,0,size);
        return memory_span{static_cast<std::byte*>(ptr),size};
    }

    // 回收内存，只有在析构函数中调用
    void page_cache::system_deallocate_memory(memory_span page)
    {
        // 使用munmap系统调用，释放由 mmap 映射到进程地址空间的内存区域
        // 若映射时使用了 MAP_SHARED 标志，munmap 会将内存中的修改同步到磁盘文件
        // 若使用 MAP_PRIVATE，内存修改是私有的，munmap 不会影响原文件
        munmap(page.data(), page.size());
    }
}