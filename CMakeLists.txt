cmake_minimum_required (VERSION 2.6)
project (LDEngine)

set(SOURCES ldengine.c )

set(CMAKE_BUILD_TYPE Debug FORCE)
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0")

find_package(OpenGL REQUIRED)
include_directories(${OPENGL_INCLUDE_DIR})

add_executable(${PROJECT_NAME} ${SOURCES})

set(CMAKE_EXE_LINKER_FLAGS "-framework Cocoa -framework OpenGL -framework SDL2")
set(BUILD_DIR ".")