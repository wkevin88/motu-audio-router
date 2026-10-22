cmake_minimum_required(VERSION 3.20)
project(motu_audio_router CXX)

set(CMAKE_CXX_STANDARD 17)
add_compile_options(/utf-8)
set(ASIOSDK_DIR ${CMAKE_SOURCE_DIR}/third_party/asiosdk)

add_executable(spike spike/spike.cpp)
target_include_directories(spike PRIVATE ${ASIOSDK_DIR}/common)
target_link_libraries(spike PRIVATE ole32 advapi32)

add_executable(motu-audio-router WIN32 src/main.cpp src/audio.cpp src/config.cpp src/startup.cpp)
target_compile_definitions(motu-audio-router PRIVATE UNICODE _UNICODE)
target_include_directories(motu-audio-router PRIVATE ${ASIOSDK_DIR}/common)
target_link_libraries(motu-audio-router PRIVATE ole32 advapi32 avrt shell32 user32)
