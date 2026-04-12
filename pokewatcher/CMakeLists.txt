cmake_minimum_required(VERSION 3.16)

# Point to the Watcher SDK components
set(EXTRA_COMPONENT_DIRS
    "/tmp/SenseCAP-Watcher-Firmware/components"
)

# Exclude components we don't need (rlottie, byte_track)
set(EXCLUDE_COMPONENTS "rlottie" "byte_track")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pokewatcher)
