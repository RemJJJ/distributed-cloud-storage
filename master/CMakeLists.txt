# 收集 master 模块的所有源文件
file(GLOB MASTER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# 创建可执行文件
add_executable(master_node ${MASTER_SOURCES})

# 链接公共库（如果有）
target_link_libraries(master_node PRIVATE common_lib)

# 如果需要链接 muduo 库，添加：
# target_link_libraries(master_node PRIVATE muduo_base muduo_net)