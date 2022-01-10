# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nonmesh_internal_communication)

target_add_binary_data(nonmesh_internal_communication.elf "certs/mqtt_eclipseprojects_io.pem" TEXT)