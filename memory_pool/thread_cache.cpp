// created by wei on 2025-5-26

#include <assert.h>
#include <iostream>
#include <bits/ostream.tcc>

#include "thread_cache.h"
#include "utils.h"
#include"central_cache.h"

namespace memory_pool
{
    std::optional<void *> thread_cache::allocate(size_t size)
    {
        if (size == 0)
        {
            return std::nullopt;
        }

        // 大小对齐到8byte
        size = size_utils::align(size);
        // 大内存直接向下传递
        if (size > size_utils::MAX_CACHED_UNIT_SIZE)
        {
            return allocate_from_central_cache(size).and_then([](std::byte *memory_addr)
                                                              { return std::optional<void *>(memory_addr); });
        }

        const size_t index = size_utils::get_index(size);
        if(m_free_cache[index]!=nullptr){
            std::byte* result =m_free_cache[index];
            //获取下一个节点，每个节点前半部分存储next指针信息
            m_free_cache[index]=*(reinterpret_cast<std::byte**>(result));

            m_free_cache_size[index]--;

            return result;
        }
        // 如果当前的桶中没有空闲内存，则从central_cache中申请
        return allocate_from_central_cache(size).and_then([](std::byte *memory_addr)
                                                              { return std::optional<void *>(memory_addr); });
    }

    // 参数：start_p:内存开始的地址, size_t：这片地址的大小
    void thread_cache::deallocate(void *start_p, size_t size)
    {
        if(size==0||start_p==nullptr){
            return;
        }
        size = size_utils::align(size);

        //大内存直接向下传递
        if(size>size_utils::MAX_CACHED_UNIT_SIZE){
            central_cache::GetInstance().deallocate(reinterpret_cast<std::byte*>(start_p),size);
            return;
        }

        // 小内存直接放入桶中
        const size_t index =size_utils::get_index(size);
        *(reinterpret_cast<std::byte**>(start_p))= m_free_cache[index];
        m_free_cache[index]=reinterpret_cast<std::byte*>(start_p);
        m_free_cache_size[index]++;

        // 检测一下需不需要回收
        // 如果当前的列表所维护的大小已经超过了阈值，则触发资源回收
        // 维护的大小 = 个数 × 单个空间的大小
        if(m_free_cache_size[index]*size>MAX_FREE_BYTES_PER_LISTS){
            // 如果超过了，则回收一半的多余的内存块
            size_t deallocate_block_size = m_free_cache_size[index]/2;
            std::byte* block_to_deallocate = m_free_cache[index];
            std::byte* last_node_to_remove = block_to_deallocate;

            for(int i=0;i<deallocate_block_size-1;i++){
                assert(last_node_to_remove!=nullptr);
                if(*(reinterpret_cast<std::byte**>(last_node_to_remove)) == nullptr){
                    // 如果链表提前结束，说明 m_free_cache_size[index] 计数有误
                    // 或者 deallocate_block_size 计算逻辑在这种边界条件下有问题
                    // 暂时返回，避免崩溃
                    return; 
                }
                last_node_to_remove = *(reinterpret_cast<std::byte**>(last_node_to_remove));
            }
            std::byte* new_head = *(reinterpret_cast<std::byte**>(last_node_to_remove));
            *(reinterpret_cast<std::byte**>(last_node_to_remove))=nullptr;
            m_free_cache[index]=new_head;
            m_free_cache_size[index]-=deallocate_block_size;

            // 检查当前的链表与要删除的链表的长度是不是一样的
            assert(check_ptr_length(m_free_cache[index]) == m_free_cache_size[index]);
            assert(check_ptr_length(block_to_deallocate) == deallocate_block_size);

             // 释放空间
            central_cache::GetInstance().deallocate(block_to_deallocate,size);
            // 在回收工作完成以后，还要调整这个空间大小的申请的个数
            // 减半下一次申请的个数
            m_next_allocate_count[index] /= 2;
        }
        
    }

    std::optional<std::byte*> thread_cache::allocate_from_central_cache(size_t memory_size){
        size_t block_cout = compute_allocate_count(memory_size);
        return central_cache::GetInstance().allocate(memory_size,block_cout).transform([this,memory_size,block_cout](std::byte* memory_list){
            size_t index = size_utils::get_index(memory_size);
            std::byte* list_end = memory_list;
            size_t list_size = 1;
            while(*(reinterpret_cast<std::byte**>(list_end))!=nullptr){
                list_end = *(reinterpret_cast<std::byte**>(list_end));
                list_size++;
            }

            assert(list_size==block_cout);
            *(reinterpret_cast<std::byte**>(list_end))=m_free_cache[index];

            // 将链表指向下一个结点，第一个结点要传出去
            m_free_cache[index]= *(reinterpret_cast<std::byte**>(list_end));
            m_free_cache_size[index]+=block_cout-1;
            return memory_list;
        });
    }

    size_t thread_cache::compute_allocate_count(size_t memory_size){
        size_t index = size_utils::get_index(memory_size);

        if(index>=size_utils::CACHE_LINE_SIZE){
            return 1;
        }

        // 最少申请4个块
        size_t result = std::max(m_next_allocate_count[index],static_cast<size_t>(4));

        // 计算下一次要申请的个数，默认乘2
        size_t next_allocate_count = result*2;
#ifndef NDEBUG
        // 要确保不会超过center_cache一次申请的最大个数
        next_allocate_count = std::min(next_allocate_count, page_span::MAX_UNIT_COUNT);
#endif
        // 同时也要确保不会超过一个列表维护的最大容量   给予冗余在极限的基础上除以2
        next_allocate_count = std::min(next_allocate_count,MAX_FREE_BYTES_PER_LISTS/memory_size/2);

        m_next_allocate_count[index] = next_allocate_count;
        return result;
    }
} // memory_pool