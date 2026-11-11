cmake_minimum_required(VERSION 3.16)
project(overtone_mandel C)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(raylib)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
add_executable(overtone_mandel ${SRC}/main.c ${SRC}/voice.c)
target_include_directories(overtone_mandel PRIVATE ${raylib_SOURCE_DIR}/src)   # external/miniaudio.h
target_link_libraries(overtone_mandel raylib m)
