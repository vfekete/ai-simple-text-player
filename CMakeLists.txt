cmake_minimum_required(VERSION 3.16)
project(ansi_audio_player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(NCURSESW REQUIRED ncursesw)
pkg_check_modules(GST REQUIRED gstreamer-1.0)
pkg_check_modules(FFMPEG REQUIRED libavformat libavcodec libavutil)

add_executable(ansi_audio_player main.cpp)

target_include_directories(ansi_audio_player PRIVATE
    ${NCURSESW_INCLUDE_DIRS}
    ${GST_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)

target_compile_options(ansi_audio_player PRIVATE
    ${NCURSESW_CFLAGS_OTHER}
    ${GST_CFLAGS_OTHER}
    ${FFMPEG_CFLAGS_OTHER}
)

target_link_libraries(ansi_audio_player PRIVATE
    ${NCURSESW_LIBRARIES}
    ${GST_LIBRARIES}
    ${FFMPEG_LIBRARIES}
)
