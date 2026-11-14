cmake_minimum_required(VERSION 3.28...3.30)

add_library(obs-replay-interface INTERFACE)
add_library(OBS::replay-interface ALIAS obs-replay-interface)

target_sources(obs-replay-interface INTERFACE obs-replay-interface.h)
target_include_directories(obs-replay-interface INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
