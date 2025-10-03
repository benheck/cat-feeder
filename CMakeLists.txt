cmake_minimum_required(VERSION 3.10)

project(opnr LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Add include directory
include_directories(${CMAKE_SOURCE_DIR}/include)

# Find required packages
find_package(PkgConfig REQUIRED)
pkg_check_modules(GPIOD REQUIRED libgpiod)

# Add executable
add_executable(opnr 
    src/main.cpp
    src/MarlinController.cpp
    src/SSD1306.cpp
)

# Link libraries
target_link_libraries(opnr ${GPIOD_LIBRARIES} i2c)
target_include_directories(opnr PRIVATE ${GPIOD_INCLUDE_DIRS})
target_compile_options(opnr PRIVATE ${GPIOD_CFLAGS_OTHER})