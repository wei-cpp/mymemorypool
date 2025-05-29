// created by wei on 2025-5-26
#include "central_cache.h"

#include <cassert>
#include <cstdlib>
#include <sys/mman.h>
#include <cstring>
#include <iostream>
#include <thread>

#include "page_cache.h"
#include "thread_cache.h"

namespace memory_pool {
    std::optional<std::byte*> central_cache::allocate(const size_t memory_size, const size_t block_count) {
        // 内存的传入应该一定是8的倍数
        assert(memory_size % 8 == 0);
        // 一次性申请的空间只可以小于512，如果出错了，则一定是代码写错了，所以使用assert
        assert(block_count <= page_span::MAX_UNIT_COUNT);

        if (memory_size == 0 || block_count == 0) {
            return std::nullopt;
        }
        //大内存，直接分配给page_cache管理
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            return page_cache::GetInstance().allocate_unit(memory_size).transform([this](memory_span memory) {
                return memory.data();
            });
        }

        // 小内存，从中心缓存中分配
        const size_t index = size_utils::get_index(memory_size);
        std::byte* result = nullptr;
        //给对应的桶加锁
        atomic_flag_guard guard(m_status[index]);

        try {
            // 如果当前缓存的个数小于申请的块数，则向页分配器申请
            if (m_free_array_size[index] < block_count) {

                // 一共要申请的大小
                //size_t total_size = block_count * memory_size;
                // 原本使用这个，只分配适量的空间
                //size_t allocate_page_count = size_utils::align(total_size, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
                // 现在改成直接分配能分配的最大的大小
                // 要申请的页面个数
                size_t allocate_page_count = get_page_allocate_count(memory_size);
                auto ret = get_page_from_page_cache(allocate_page_count);
                if (!ret.has_value()) {
                    return std::nullopt;
                }
                memory_span memory = ret.value();

                // 用于管理这个页面
                page_span page_span(memory, memory_size);

#ifndef NDEBUG
                // 如果使用的page_span是固定大小管理的，则可分配的个数也是固定的
                size_t allocate_unit_count = page_span::MAX_UNIT_COUNT;
#else
                // 否则就是不同个数的
                size_t allocate_unit_count = memory.size() / memory_size;
#endif

                // 将页面中的内存块分配出去
                for (size_t i = 0; i < block_count; i++) {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    assert((index + 1) * 8 == split_memory.size());

                    *(reinterpret_cast<std::byte**>(split_memory.data())) = result;
                    result = split_memory.data();

                    // 这个页面已经被分配出去了
                    page_span.allocate(split_memory);
                }

                // 完成页面分配的管理
                auto start_addr = page_span.data();
                //emplace返回类型为pair<iterator, bool>，第一个是迭代器，第二个是bool
                auto [_, succeed] = m_page_set[index].emplace(start_addr, std::move(page_span));
                // 如果插入失败了，说明代码写的有问题
                assert(succeed == true);

                // 多余的值存到空闲列表中
                allocate_unit_count -= block_count;
                for (size_t i = 0; i < allocate_unit_count; i++) {
                    memory_span split_memory = memory.subspan(0, memory_size);
                    memory = memory.subspan(memory_size);
                    assert((index + 1) * 8 == split_memory.size());

                    *(reinterpret_cast<std::byte**>(split_memory.data())) = m_free_array[index];
                    m_free_array[index] = split_memory.data();
                    m_free_array_size[index] ++;
                }
            } else {// 如果当前缓存的个数大于等于申请的块数，则直接从空闲链表中取
                auto& target_list = m_free_array[index];
                assert(m_free_array_size[index] >= block_count);
                // 直接从中心缓存区中分配内存
                for (size_t i = 0; i < block_count; i++) {
                    assert(m_free_array[index] != nullptr);
                    //头插法
                    std::byte* node = m_free_array[index];
                    m_free_array[index] = *(reinterpret_cast<std::byte**>(node));
                    m_free_array_size[index] --;
                    // 在页管理中记录分配的内存块
                    record_allocated_memory_span(node, memory_size);

                    *(reinterpret_cast<std::byte**>(node)) = result;
                    result = node;
                }
            }
        } catch (...) {
            throw std::runtime_error("central_cache::allocate Memory allocation failed");
            return std::nullopt;
        }


        assert(check_ptr_length(result) == block_count);
        return result;
    }

    void central_cache::deallocate(std::byte* memory_list, size_t memory_size) {
        assert(memory_list != nullptr);

        // 如果是大内存块，则直接返回给page_cache管理
        if (memory_size > size_utils::MAX_CACHED_UNIT_SIZE) {
            page_cache::GetInstance().deallocate_unit(memory_span(memory_list, memory_size));
            return;
        }

        // 小内存，从中心缓存中释放
        const size_t index = size_utils::get_index(memory_size);
        atomic_flag_guard guard(m_status[index]);

        std::byte* current_memory = memory_list;
        while (current_memory != nullptr) {
            std::byte* next_node_to_add = *(reinterpret_cast<std::byte**>(current_memory));
            // 先归还到数组中,空闲链表中，使用头插法
            assert((index + 1) * 8 == memory_size);

            *(reinterpret_cast<std::byte**>(current_memory)) = m_free_array[index];
            m_free_array[index] = current_memory;
            m_free_array_size[index] ++;


            // 然后再还给页面管理器中
            auto it = m_page_set[index].upper_bound(current_memory);
            assert(it != m_page_set[index].begin());
            -- it;
            assert(it->second.is_valid_unit_span(memory_span(current_memory, memory_size)));
            it->second.deallocate(memory_span(current_memory, memory_size));
            // 同时判断需不需要返回给页面管理器
            if (it->second.is_empty()) {
                // 如果已经还清内存了，则将这块内存还给页面管理器(page_cache)
                auto page_start_addr = it->second.data();
                auto page_end_addr = page_start_addr + it->second.size();
                assert(it->second.unit_size() == memory_size);

                std::byte* current = m_free_array[index];
                std::byte* prev = nullptr;
                // 遍历这个空闲链表，将在这个page_span中的块从空闲链表中剔除
                while (current != nullptr) {
                    std::byte* next = *(reinterpret_cast<std::byte**>(current));
                    bool should_remove = false;
                    auto memory_start_addr = current;
                    auto memory_end_addr = memory_start_addr + memory_size;

                    if (memory_start_addr >= page_start_addr && memory_end_addr <= page_end_addr) {
                        // 如果这个内存在这个范围内，则说明是正确的
                        // 一定是满足要求的，如果不满足，则说明代码写错了
                        assert(it->second.is_valid_unit_span(memory_span(current, memory_size)));
                        should_remove = true;
                    }
                    // 只有在不需要删除的时候才会更新prev
                    if (should_remove) {
                        // 从链表中移除 current
                        if (prev == nullptr) { // 移除的是头节点
                            m_free_array[index] = next;
                        } else { // 移除的是中间或尾部节点
                            *(reinterpret_cast<std::byte**>(prev)) = next;
                        }
                        m_free_array_size[index]--;
                        // 注意：当移除 current 时，prev 保持不变，因为它仍然是 next 的前一个节点
                    } else {
                        // current 未被移除，它成为下一次迭代的 prev
                        prev = current;
                    }
                    current = next;
                }
                memory_span page_memory = it->second.get_memory_span();
                m_page_set[index].erase(it);
                // 如果是动态分配申请页面的
#ifdef NDEBUG
                // 如果回收了指定的页面，则说明当前这个空间分配的过多了，下一次申请内存的时候要少一点申请
                m_next_allocate_memory_group_count[index] /= 2;
#endif

                page_cache::GetInstance().deallocate_page(page_memory);
            }
            current_memory = next_node_to_add;
        }
    }

    size_t central_cache::get_page_allocate_count(size_t memory_size) {
#ifndef NDEBUG//debug模式下，一次性分配管理上限个的页面
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
        size_t next_allocate_page_count = result + 1;
        m_next_allocate_memory_group_count[index] = next_allocate_page_count;
        return size_utils::align(result * thread_cache::MAX_FREE_BYTES_PER_LISTS, size_utils::PAGE_SIZE) / size_utils::PAGE_SIZE;
#endif
    }

    void central_cache::record_allocated_memory_span(std::byte* memory, const size_t memory_size) {
        const size_t index = size_utils::get_index(memory_size);
        auto it = m_page_set[index].upper_bound(memory);
        assert(it != m_page_set[index].begin());
        --it;
        it->second.allocate(memory_span(memory, memory_size));
    }

    std::optional<memory_span> central_cache::get_page_from_page_cache(size_t page_allocate_count) {
        return page_cache::GetInstance().allocate_page(page_allocate_count);
    }
}