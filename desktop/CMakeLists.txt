cmake_minimum_required(VERSION 3.16)
project(overtone_mandel C)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# pinned: the commit everything was tested with (master moves; MA_NO_* in main.c must match its raudio.c)
FetchContent_Declare(raylib GIT_REPOSITORY https://github.com/raysan5/raylib.git GIT_TAG 1825891abca17841467c861252d07bf7ccbf3fb7)
FetchContent_MakeAvailable(raylib)

set(SRC ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)
add_executable(overtone_mandel ${SRC}/main.c ${SRC}/voice.c ${SRC}/beat.c ${SRC}/physarum.c ${SRC}/video.c ${SRC}/expr.c ${SRC}/show.c ${SRC}/remote.c)
target_include_directories(overtone_mandel PRIVATE ${raylib_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})   # external/miniaudio.h, remote_html.h

# phone control page, embedded into the binary; editing it re-runs configure
file(READ ${SRC}/remote.html HTML_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," HTML_C "${HTML_HEX}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/remote_html.h "static const unsigned char REMOTE_HTML[] = {${HTML_C}0};\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SRC}/remote.html)
target_link_libraries(overtone_mandel raylib m pthread)
# video overlay runs ffmpeg as a child process; bake its absolute path in so it works outside `nix develop`
find_program(FFMPEG ffmpeg)
if(FFMPEG)
  target_compile_definitions(overtone_mandel PRIVATE FFMPEG_BIN="${FFMPEG}")
endif()
# QR code with the remote's address in the terminal
find_program(QRENCODE qrencode)
if(QRENCODE)
  target_compile_definitions(overtone_mandel PRIVATE QRENCODE_BIN="${QRENCODE}")
endif()

# miniaudio dlopens the audio backends at runtime; bake their dirs into RUNPATH so the binary
# finds them outside `nix develop` too (NixOS has no global /usr/lib)
foreach(lib pulse asound)
  find_library(AUDIO_${lib} ${lib})
  if(AUDIO_${lib})
    get_filename_component(dir ${AUDIO_${lib}} DIRECTORY)
    list(APPEND AUDIO_RPATH ${dir})
  endif()
endforeach()
if(AUDIO_RPATH)
  list(REMOVE_DUPLICATES AUDIO_RPATH)
  set_property(TARGET overtone_mandel APPEND PROPERTY BUILD_RPATH ${AUDIO_RPATH})
endif()
