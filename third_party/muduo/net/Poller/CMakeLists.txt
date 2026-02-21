file(GLOB POLLER_SRC *.cc)
add_library(poller ${POLLER_SRC})
target_include_directories(poller PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# poller 依赖 net
target_link_libraries(poller PUBLIC net)