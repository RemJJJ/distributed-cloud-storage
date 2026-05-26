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

# -------------------------- 配置相关 ----------------------------
set(CONFIG_SOURCE_DIR ${CMAKE_SOURCE_DIR})
set(CONFIG_FILE "config.json")

# -------------------------- muduo库配置 --------------------------
# 定义muduo的根目录（相对于项目根目录）
set(MUDUO_ROOT ${CMAKE_SOURCE_DIR}/third_party/muduo)

# muduo头文件路径（因为muduo头文件是<muduo/base/...>，所以包含到MUDUO_ROOT）
set(MUDUO_INCLUDE_DIRS ${MUDUO_ROOT})

# 直接把仿写的 muduo 源码纳入当前工程构建图。
# 这样每次编译 master / datanode 时，若 muduo 源码有变更，也会自动重新编译，
# 不再依赖 third_party/muduo/build 里的旧静态库。
file(GLOB MUDUO_BASE_SOURCES CONFIGURE_DEPENDS
    ${MUDUO_ROOT}/base/*.cc
)
file(GLOB MUDUO_NET_SOURCES CONFIGURE_DEPENDS
    ${MUDUO_ROOT}/net/*.cc
    ${MUDUO_ROOT}/net/Poller/*.cc
)

add_library(muduo_base STATIC ${MUDUO_BASE_SOURCES})
target_include_directories(muduo_base PUBLIC ${MUDUO_ROOT})
target_link_libraries(muduo_base PUBLIC pthread)

add_library(muduo_net STATIC ${MUDUO_NET_SOURCES})
target_include_directories(muduo_net PUBLIC ${MUDUO_ROOT})
target_link_libraries(muduo_net PUBLIC muduo_base pthread)

set(MUDUO_LIBRARIES muduo_net muduo_base pthread)

# 将muduo和MySQL的头文件路径加入全局搜索路径
include_directories(
    ${MUDUO_INCLUDE_DIRS}
    ${MYSQL_INCLUDE_DIRS}
)

#-------------------------- MySQL库配置 --------------------------
set(MYSQL_INCLUDE_DIRS "/usr/include/mysql")

set(MYSQL_LIBRARIES "mysqlclient")

#-------------------------- UUID库配置 ---------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(UUID REQUIRED uuid)

# 将 uuid 的头文件路径加入全局包含目录
include_directories(${UUID_INCLUDE_DIRS})

set(UUID_LIBRARIES ${UUID_LIBRARIES}) 

#-------------------------- JWT 库配置 ---------------------------
set(JWT_CPP_INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/third_party/jwt-cpp/include)

find_package(OpenSSL REQUIRED)

include_directories(
    ${JWT_CPP_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
)

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
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/html)
file(COPY ${CMAKE_SOURCE_DIR}/web/html DESTINATION ${CMAKE_BINARY_DIR})
