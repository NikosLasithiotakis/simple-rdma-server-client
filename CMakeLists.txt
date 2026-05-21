cmake_minimum_required(VERSION 3.10)
project(SimpleRDMA C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wunused-result -O2")

find_package(Threads REQUIRED)

include_directories(include)

add_library(utils src/utils.c)

add_executable(rdma_server src/rdma_server.c)
target_link_libraries(rdma_server utils ibverbs rdmacm Threads::Threads)

add_executable(rdma_client src/rdma_client.c)
target_link_libraries(rdma_client utils ibverbs rdmacm Threads::Threads)
