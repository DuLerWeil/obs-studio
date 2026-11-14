cmake_minimum_required(VERSION 3.28...3.30)

add_library(obs-replay-buffer-interface INTERFACE)
add_library(OBS::replay-buffer-interface ALIAS obs-replay-buffer-interface)

target_sources(obs-replay-buffer-interface INTERFACE obs-replay-buffer-interface.h)
target_include_directories(
  obs-replay-buffer-interface
  INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}"
)
