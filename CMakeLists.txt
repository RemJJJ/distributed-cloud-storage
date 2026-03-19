# CMake最低版本要求（兼容主流环境）
cmake_minimum_required(VERSION 3.14)

# 项目名称与语言
project(distributed-fm CXX)

# -------------------------- 全局编译设置 --------------------------
# 设置C++标准（与你之前的.clangd配置一致）
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项：开启警告，优化调试信息
add_compile_options(-Wall -Wextra -Wpedantic -g)

# 生成compile_commands.json（给clangd用，必加！）
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# -------------------------- muduo库配置 --------------------------
# 定义muduo的根目录（相对于项目根目录）
set(MUDUO_ROOT ${CMAKE_SOURCE_DIR}/third_party/muduo)

# muduo头文件路径（因为muduo头文件是<muduo/base/...>，所以包含到MUDUO_ROOT）
set(MUDUO_INCLUDE_DIRS ${MUDUO_ROOT})

# muduo库文件路径（预编译的静态库在build目录）
set(MUDUO_LIBRARY_DIRS ${MUDUO_ROOT}/build)

# muduo需要链接的库（根据你的muduo编译结果调整，通常是net+base+pthread）
set(MUDUO_LIBRARIES muduo_net muduo_base pthread)

# 将muduo库路径加入链接搜索目录
link_directories(${MUDUO_LIBRARY_DIRS})

# 将muduo和MySQL的头文件路径加入全局搜索路径
include_directories(
    ${MUDUO_INCLUDE_DIRS}
    ${MYSQL_INCLUDE_DIRS}
)

#-------------------------- MySQL库配置 --------------------------
set(MYSQL_INCLUDE_DIRS "/usr/include/mysql")

set(MYSQL_LIBRARIES "mysqlclient")

# 在子目录管理之前，把config.json复制到build目录
file(COPY ${CMAKE_SOURCE_DIR}/config.json DESTINATION ${CMAKE_BINARY_DIR})

# -------------------------- 子目录管理 --------------------------
# 先编译common公共库
add_subdirectory(common)

# 再编译datanode和master（依赖common和muduo）
add_subdirectory(datanode)
add_subdirectory(master)

# -------------------------- 静态资源处理 --------------------------
# 将web/html复制到build目录，方便运行时直接访问
file(COPY ${CMAKE_SOURCE_DIR}/web/html DESTINATION ${CMAKE_BINARY_DIR})