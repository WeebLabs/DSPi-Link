# DSPi Link ESP32 bridge: ESP-IDF project file.
#
# Build with tools/build.sh, which runs the pinned ESP-IDF version
# (idf_version.txt) in Espressif's Docker image. See README.md.

cmake_minimum_required(VERSION 3.22)

# The bridge firmware's own version, reported as hub.version in hello
# (bridge spec 8.1). Follows semantic versioning.
set(PROJECT_VER "0.1.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dspi_bridge)
