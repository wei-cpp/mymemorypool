cmake_minimum_required(VERSION 3.20)

# Set compilers before project() command
set(CMAKE_C_COMPILER "gcc-13")
set(CMAKE_CXX_COMPILER "g++-13")

# 项目名称
project(memory_pool_project VERSION 1.0)

# 设置C++标准为23
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 如果没有指定构建类型，设置为Release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 添加编译器标志
add_compile_options(-std=c++2b)

# 添加内存池库子目录
add_subdirectory(memory_pool)

# 添加可执行文件
add_executable(memory_pool_demo main.cpp)
add_executable(memory_pool_benchmark benchmark.cpp)
add_executable(memory_pool_performance performance.cpp)

# 链接内存池库
target_link_libraries(memory_pool_demo PRIVATE memory_pool_lib)
target_link_libraries(memory_pool_benchmark PRIVATE memory_pool_lib)
target_link_libraries(memory_pool_performance PRIVATE memory_pool_lib pthread)

# 设置包含目录，使main.cpp和benchmark.cpp能够找到内存池的头文件
target_include_directories(memory_pool_demo PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_pool
)
target_include_directories(memory_pool_benchmark PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_pool
)
target_include_directories(memory_pool_performance PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/memory_pool
)