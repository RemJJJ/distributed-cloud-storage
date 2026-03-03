# 收集common的源文件（如果common只有头文件，这行可以注释）
file(GLOB_RECURSE COMMON_SOURCES "*.cc")

# 编译为静态库（如果只有头文件，用INTERFACE库）
if(COMMON_SOURCES)
    add_library(common STATIC ${COMMON_SOURCES})
else()
    add_library(common INTERFACE)
endif()

# 让common的头文件可以被其他模块直接引用（<common/xxx.h>）
target_include_directories(common INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)