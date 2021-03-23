cmake_minimum_required(VERSION 2.8)
project(ringcache)
set(project_name "ringcache")

option(TARGET_DEBUG_MODE "Build the project with debug mode" OFF)
set(CMAKE_CXX_FLAGS "-gdwarf-2 -pipe -std=c++0x -fno-omit-frame-pointer -fPIC")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__FILENAME__='\"$(notdir $<)\"'")
if (TARGET_DEBUG_MODE)
    set(GENERATE_TEST "OFF")
    set(CMAKE_BUILD_TYPE "Debug")
    message("debug")
    set(CMAKE_CXX_FLAGS_DEBUG "-W -Wall -O0 -g -ftest-coverage -fprofile-arcs -D__DEBUG_MODE__ -DLOG_DEBUG_ENABLE=true")
else (TARGET_DEBUG_MODE)
    set(CMAKE_BUILD_TYPE "Release")
    set(CMAKE_CXX_FLAGS_RELEASE "-DNDEBUG -O2")
    message("not debug")
endif (TARGET_DEBUG_MODE)

# work home
set(work_home ${CMAKE_CURRENT_SOURCE_DIR})
set(src_home ${work_home}/src/)
message("src_home=${src_home}")

# include file list
set(include_list "")
list(APPEND include_list ${src_home})
include_directories(${include_list})

# library file list
set(library_list -lpthread -lz -lrt -ldl)

add_executable(test ${work_home}/test.cpp)
target_link_libraries(test ${library_list})