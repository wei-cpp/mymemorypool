// created by wei on 2025-5-26
#include <cassert>
#include <cstdlib>
#include <sys/mman.h>
#include <cstring>
#include <iostream>
#include <thread>

#include "central_cache.h"
#include "thread_cache.h"
#include "page_cache.h"

namespace memory_pool
{

    // 用于分配指定个数指定大小的空间
    std::optional<std::byte *> central_cache::allocate(size_t memory_size, size_t block_count)
    {
        // 这一层是由thread_cache调用，处理过 传入大小一定是8的倍数
        assert(memory_size % 8 == 0);
        assert(block_count <= page_span::MAX_UNIT_COUNT);

        if (memory_size == 0 || block_count == 0)
        {
            return std::nullopt;
        }

        // 处理大内存
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE)
        {
            return page_cache::GetInstance().allocate_unit(memory_size).transform([this](memory_span memory)
                                                                                  { return memory.data(); });
        }

        const size_t index = size_utils::get_index(memory_size);
        std::byte *result = nullptr;

        atomic_flag_guard guard(m_status[index]);

        try
        {
            // 如果当前缓存的个数小于申请的块数，则向页分配器申请
            if (m_free_array_size[index] < block_count)
            {

                // 一共要申请的大小
                // size_t total_size = block_count * memory_size;
                // 直接分配能分配的最大的大小
                size_t allocate_page_count = get_page_allocate_count(memory_size);
                auto ret = get_page_from_page_cache(allocate_page_count);
                if (!ret.has_value())
                {
                    return std::nullopt;
                }
                memory_span memory = ret.value();

                page_span page_span(memory, memory_size);
#ifndef NDEBUG
                // 如果使用的page_span是固定大小管理的，则可分配的个数也是固定的
                size_t allocate_unit_count = page_span::MAX_UNIT_COUNT;
#else
                // 否则就是可以直接分配的
                size_t allocate_unit_count = memory.size() / memory_size;
#endif
                for (size_t i = 0; i < block_count; i++)
                {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    assert((index + 1) * 8 == split_memory.size());

                    *(reinterpret_cast<std::byte **>(split_memory.data())) = result;
                    result = split_memory.data();

                    page_span.allocate(split_memory);
                }

                // 完成页面分配管理
                auto start_addr = page_span.data();
                auto [_, succeed] = m_page_set[index].emplace(start_addr, std::move(page_span));
                assert(succeed == true);

                // 多余的值存到空闲列表中
                allocate_unit_count -= block_count;
                for (size_t i = 0; i < allocate_unit_count; i++)
                {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    assert((index + 1) * 8 == split_memory.size());

                    *(reinterpret_cast<std::byte **>(split_memory.data())) = m_free_array[index];
                    m_free_array[index] = split_memory.data();
                    m_free_array_size[index]++;
                }
            }
            else
            {
                assert(m_free_array_size[index] >= block_count);
                auto &target_list = m_free_array[index];

                // 直接从空闲链表中分配
                for (size_t i = 0; i < block_count; i++)
                {
                    assert(m_free_array[index] != nullptr);

                    std::byte *node = m_free_array[index];
                    m_free_array[index] = *(reinterpret_cast<std::byte **>(node));
                    m_free_array_size[index]--;

                    record_allocated_memory_span(node, memory_size);

                    // 头插法  分配的链节点
                    *(reinterpret_cast<std::byte **>(node)) = result;
                    result = node;
                }
            }
        }
        catch (...)
        {
            throw std::runtime_error("central_cache allocation failed");
            return std::nullopt;
        }

        assert(check_ptr_length(result) == block_count);
        return result;
    }

    void central_cache::deallocate(std::byte *memory_list, size_t memory_size)
    {
        assert(memory_list != nullptr);

        // 如果是超大内存块，则直接返回给page_cache管理
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE)
        {
            page_cache::GetInstance().deallocate_unit(memory_span(memory_list, memory_size));
            return;
        }

        // 否则走central的链表
        const size_t index = size_utils::get_index(memory_size);
        atomic_flag_guard guard(m_status[index]);

        std::byte *current_memory = memory_list;
        while (current_memory != nullptr)
        {
            std::byte *next_node_to_add = *(reinterpret_cast<std::byte **>(current_memory));

            assert((index + 1) * 8 == memory_size);
            // 先放入空闲链表
            *(reinterpret_cast<std::byte **>(current_memory)) = m_free_array[index];
            m_free_array[index] = current_memory;
            m_free_array_size[index]++;

            // 再还给页面管理器
            // todo
            auto it = m_page_set[index].upper_bound(current_memory);
            assert(it != m_page_set[index].begin());
            --it;
            assert(it->second.is_valid_unit_span(memory_span(current_memory, memory_size)));
            it->second.deallocate(memory_span(current_memory, memory_size));

            // 同时判断需不需要返回给页面管理器
            if (it->second.is_empty())
            {
                // 如果已经还清内存了，则将这块内存还给页面管理器(page_cache)
                auto page_start_addr = it->second.data();
                auto page_end_addr = page_start_addr + it->second.size();
                assert(it->second.unit_size() == memory_size);

                std::byte *current = m_free_array[index];
                std::byte *prev = nullptr;

                // 遍历整个空闲链表
                while (current != nullptr)
                {
                    std::byte *next = *(reinterpret_cast<std::byte **>(current));
                    bool should_remove = false;
                    auto memory_start_addr = current;
                    auto memory_end_addr = memory_start_addr + memory_size;

                    if (memory_start_addr >= page_start_addr && memory_end_addr <= page_end_addr)
                    {
                        // 如果这个内存在这个范围内，则说明是正确的
                        assert(it->second.is_valid_unit_span(memory_span(current, memory_size)));
                        should_remove = true;
                    }

                    // 从链表中移除 current
                    if (should_remove)
                    {
                        // 移除的是头节点
                        if (prev == nullptr)
                        {
                            m_free_array[index] = next;
                        }
                        // 移除的是中间或尾部节点
                        else
                        {
                            *(reinterpret_cast<std::byte **>(prev)) = next;
                        }
                        m_free_array_size[index]--;
                    }
                    else
                    {
                        // current 未被移除，它成为下一次迭代的 prev
                        prev = current;
                    }
                    current = next;
                }
                memory_span page_memory = it->second.get_memory_span();
                m_page_set[index].erase(it);

#ifdef NDEBUG
                // 如果回收了指定的页面，则说明当前这个空间分配的过多了，下一次申请内存的时候要少一点申请
                m_next_allocate_memory_group_count[index] /= 2;
#endif
                page_cache::GetInstance().deallocate_page(page_memory);
            }
            current_memory = next_node_to_add;
        }
    }

    // 将分配出去的内存块记录下来
    void central_cache::record_allocated_memory_span(std::byte *memory, const size_t memory_size)
    {
        const size_t index = size_utils::get_index(memory_size);
        auto it = m_page_set[index].upper_bound(memory);
        assert(it != m_page_set[index].begin());
        --it;
        it->second.allocate(memory_span(memory, memory_size));
    }

    std::optional<memory_span> central_cache::get_page_from_page_cache(size_t page_allocate_count)
    {
        return page_cache::GetInstance().allocate_page(page_allocate_count);
    }

    size_t central_cache::get_page_allocate_count(size_t memory_size)
    {
#ifndef NDEBUG
        // 如果page_span一次性有最大的管理上限，那么就一次性分配管理上限个的页面
        size_t allocate_unit_count = page_span::MAX_UNIT_COUNT;
        size_t allocate_page_count = size_utils::align(memory_size * allocate_unit_count, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
        return allocate_page_count;
#else
        size_t index = size_utils::get_index(memory_size);
        size_t result = m_next_allocate_memory_group_count[index];

        // 最小要分配一组的数据
        result = std::max(result, static_cast<size_t>(1));

        // 下一次再请求分配的时候，就再加一组的数据
        size_t next_allocate_count = result + 1;
        m_next_allocate_memory_group_count[index] = next_allocate_page_count;
        return size_utils::align(result * thread_cache::MAX_FREE_BYTES_PER_LISTS, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
#endif
    }

}