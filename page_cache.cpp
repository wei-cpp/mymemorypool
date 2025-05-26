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
        if(page_count==0){
            return std::nullopt;
        }
        std::unique_lock<std::mutex> guard(m_mutex);

        auto it = free_page_store.lower_bound(page_count);
        // 如果存在一个页面，这个页面的大小是大于或等于要分配的页面的
        while(it != free_page_store.end()){
            if(!it->second.empty()){

                auto mem_iter = it->second.begin();
                memory_span free_memory = *mem_iter;
                it->second.erase(mem_iter);
                free_page_map.erase(free_memory.data());

                // 开始分割获取出来的空闲空间
                size_t memory_to_use = page_count*size_utils::PAGE_SIZE;
                memory_span memory = free_memory.subspan(0,memory_to_use);
                free_memory = free_memory.subspan(memory_to_use);

                if(free_memory.size()){
                    // 如果还有空间，则插回到缓存中
                    free_page_store[free_memory.size()/size_utils::PAGE_SIZE].emplace(free_memory);
                    free_page_map.emplace(free_memory.data(),free_memory);
                }
                return memory;

            }
            ++it;
        }

        // 如果已经没有足够大的页面了，则向系统申请
        // 一次性分配8MB的大小，为2048个页面，而批量申请的全都取最大是4mb，-> 16KB(缓存最大大小) * 512(一次性管理最大个数) = 4MB
        size_t page_to_allocate = std::max(PAGE_ALLOCATE_COUNT,page_count);
        return system_allocate_memory(page_to_allocate).transform([this,page_count](memory_span memory){
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
            return result;
        });
    }

    // 回收指定页数的内存
    void page_cache::deallocate_page(memory_span page)
    {
    }

    // 分配一个单元的内存，用于处理超大块内存
    std::optional<memory_span> page_cache::allocate_unit(size_t memory_size)
    {
    }

    // 回收一个单元的内存，用于回收超大块内存
    void page_cache::deallocate_unit(memory_span memories)
    {
    }

    // 关闭内存池
    void page_cache::stop()
    {
    }

    page_cache::~page_cache()
    {
        stop();
    }

    // 只申请，不回收，只有在销毁时回收
    std::optional<memory_span> page_cache::system_allocate_memory(size_t page_count)
    {
    }

    // 回收内存，只有在析构函数中调用
    void page_cache::system_deallocate_memory(memory_span page)
    {
    }
}