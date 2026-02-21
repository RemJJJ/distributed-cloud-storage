cmake_minimum_required(VERSION 3.10)
project(DistributedCloudStorage)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================
# 【核心】添加全局头文件搜索路径
# ============================================================

# 1. 公共头文件目录 (FileStorage.h 所在目录)
include_directories(
    ${CMAKE_SOURCE_DIR}/common/include
)

# 2. 第三方库 muduo 头文件目录
include_directories(
    ${CMAKE_SOURCE_DIR}/third_party/muduo
)

# ============================================================
# 添加子目录（分别编译 master 和 datanode）
# ============================================================
add_subdirectory(master)
add_subdirectory(datanode)

# ============================================================
# 如果需要编译 common 的源代码为静态库
# ============================================================
# 先收集 common 的所有源文件
# file(GLOB COMMON_SOURCES "${CMAKE_SOURCE_DIR}/common/src/*.cpp")
# add_library(common_lib STATIC ${COMMON_SOURCES})
# target_include_directories(common_lib PUBLIC ${CMAKE_SOURCE_DIR}/common/include)