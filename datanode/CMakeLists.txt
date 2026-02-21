# 收集 datanode 模块的所有源文件
file(GLOB DATANODE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

# 创建可执行文件
add_executable(datanode_node ${DATANODE_SOURCES})

# 链接公共库（如果有）
target_link_libraries(datanode_node PRIVATE common_lib)