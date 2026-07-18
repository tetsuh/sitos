cmake_minimum_required(VERSION 3.20)
project(sitos_package_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(sitos 0.1 CONFIG REQUIRED)

add_executable(sitos_package_consumer main.cpp)
target_link_libraries(sitos_package_consumer PRIVATE sitos::sitos)
