cmake_minimum_required(VERSION 3.16)

# Point to the Watcher SDK components
set(EXTRA_COMPONENT_DIRS
    "../SenseCAP-Watcher-Firmware/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pokewatcher)
