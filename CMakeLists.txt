cmake_minimum_required(VERSION 3.16)
project(l3s_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(cppzmq REQUIRED)

add_executable(l3s_engine src/main.cpp)
target_link_libraries(l3s_engine PRIVATE cppzmq)
