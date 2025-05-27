cmake_minimum_required(VERSION 3.20)

# 项目名称
project(memory_pool VERSION 1.0)

# 设置C++标准为23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 添加编译器标志
add_compile_options(-std=c++2b)

# 添加所有源文件
set(SOURCES
    memory_pool.cpp
    utils.cpp
    page_cache.cpp
    central_cache.cpp
    thread_cache.cpp
)

# 添加所有头文件
set(HEADERS
    memory_pool.h
    utils.h
    page_cache.h
    central_cache.h
    thread_cache.h
)

# 创建静态库
add_library(memory_pool_lib STATIC ${SOURCES} ${HEADERS})

# 设置包含目录
target_include_directories(memory_pool_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 如果是Debug模式，添加调试标志
target_compile_options(memory_pool_lib PRIVATE
    $<$<CONFIG:Debug>:-g -O0>
    $<$<CONFIG:Release>:-O3>
)

# 添加测试可执行文件（可选，如果你需要的话）
# add_executable(memory_pool_test test/main.cpp)
# target_link_libraries(memory_pool_test PRIVATE memory_pool_lib) 