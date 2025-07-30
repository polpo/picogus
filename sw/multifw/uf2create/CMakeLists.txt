cmake_minimum_required(VERSION 3.22)
project(uf2create)

set(CMAKE_C_STANDARD 11)

add_executable(uf2create uf2create.c)
target_include_directories(uf2create PRIVATE ${MULTIFW_INCLUDE})
