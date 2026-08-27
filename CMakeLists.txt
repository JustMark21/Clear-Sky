cmake_minimum_required(VERSION 3.16)
project(l3s_leo_engine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# ---------------------------------------------------------------------------
# Dependencies: libzmq (C library) + cppzmq (header-only C++ binding).
# Both are expected as system packages (Debian/Ubuntu: libzmq3-dev,
# cppzmq-dev). We locate libzmq via pkg-config, since upstream ZeroMQ does
# not reliably ship a CMake config on all distros.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED libzmq)

find_path(CPPZMQ_INCLUDE_DIR zmq.hpp
    PATHS /usr/include /usr/local/include
)
if(NOT CPPZMQ_INCLUDE_DIR)
    message(FATAL_ERROR "cppzmq (zmq.hpp) not found. Install the 'cppzmq-dev' package.")
endif()

# ---------------------------------------------------------------------------
# l3s_engine: the standalone SciML data-prep backend.
# ---------------------------------------------------------------------------
add_executable(l3s_engine src/main.cpp)

target_include_directories(l3s_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${ZMQ_INCLUDE_DIRS}
    ${CPPZMQ_INCLUDE_DIR}
)
target_link_directories(l3s_engine PRIVATE ${ZMQ_LIBRARY_DIRS})
target_link_libraries(l3s_engine PRIVATE ${ZMQ_LIBRARIES})
target_compile_options(l3s_engine PRIVATE ${ZMQ_CFLAGS_OTHER} -Wall -Wextra)

install(TARGETS l3s_engine RUNTIME DESTINATION bin)
