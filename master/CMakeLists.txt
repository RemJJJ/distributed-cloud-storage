file(GLOB_RECURSE MASTER_SOURCES "src/*.cc")
add_executable(master ${MASTER_SOURCES})

target_include_directories(master PRIVATE 
    ${CMAKE_SOURCE_DIR}/common/include
    ${MUDUO_INCLUDE_DIRS}
    ${MYSQL_INCLUDE_DIRS}
)

target_link_libraries(master PRIVATE common ${MUDUO_LIBRARIES} ${MYSQL_LIBRARIES})