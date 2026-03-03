# 收集datanode的所有源文件
file(GLOB_RECURSE DATANODE_SOURCES "src/*.cc")

# 生成可执行文件
add_executable(datanode ${DATANODE_SOURCES})

# 链接头文件目录：common的include + muduo的include
target_include_directories(datanode PRIVATE 
    ${CMAKE_SOURCE_DIR}/common/include
    ${MUDUO_INCLUDE_DIRS}
)

# 链接库：common公共库 + muduo库
target_link_libraries(datanode PRIVATE common ${MUDUO_LIBRARIES})