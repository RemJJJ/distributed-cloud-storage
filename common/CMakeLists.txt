# 收集common的源文件
file(GLOB_RECURSE COMMON_SOURCES "*.cc")

# 编译为静态库或 INTERFACE 库
if(COMMON_SOURCES)
    add_library(common STATIC ${COMMON_SOURCES})
    # 【修改1】对于 STATIC 库，用 PUBLIC 让依赖者继承路径
    target_include_directories(common PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MUDUO_INCLUDE_DIRS}   # 新增：传递 muduo 路径
        ${MYSQL_INCLUDE_DIRS}    # 新增：传递 MySQL 路径
    )
else()
    add_library(common INTERFACE)
    # 【修改2】对于 INTERFACE 库，用 INTERFACE 传递路径
    target_include_directories(common INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${MUDUO_INCLUDE_DIRS}   # 新增：传递 muduo 路径
        ${MYSQL_INCLUDE_DIRS}    # 新增：传递 MySQL 路径
    )
endif()