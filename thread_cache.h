// created by wei on 2025-5-26

#ifndef THREAD_CACHE_H
#define THREAD_CACHE_H
#include<array>
#include <list>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>

namespace memory_pool{

class thread_cache{
public:
    // 设置每个列表缓存的上限为256KB
    static constexpr size_t MAX_FREE_BYTES_PER_LISTS = 256 * 1024;
    
    static thread_cache& GetInstance(){
        static thread_local thread_cache instance;
        return instance;
    }

    std::optional<void*> allocate(size_t size);
    // 参数：start_p:内存开始的地址, size_t：这片地址的大小
    void deallocate(void* start_p, size_t size);

private:
    thread_cache()=default;
    // 向高层申请一块空间
    std::optional<std::byte*> allocate_from_central_cache(size_t memory_size);
    // 指定下标存放的大小
    std::array<size_t, size_utils::CACHE_LINE_SIZE> m_free_cache_size = {};
    // 当前还没有被分配的内存
    std::array<std::byte* , size_utils::CACHE_LINE_SIZE> m_free_cache = {};

    // 用于表示下一次再申请指定大小的内存时，会申请几个内存
    std::array<size_t, size_utils::CACHE_LINE_SIZE> m_next_allocate_count = {};

    // 动态分配内存
    size_t compute_allocate_count(size_t memory_size);
};

}//memory_pool
#endif