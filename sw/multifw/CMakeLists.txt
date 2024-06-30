cmake_minimum_required(VERSION 3.29)
project(multifw)

# add_executable(uf2create)
# target_sources(uf2create PRIVATE uf2create.c)

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. picogus)

add_custom_command(
    OUTPUT pg-multi.uf2
    COMMAND uf2create picogus.bin pg-multi.uf2
    DEPENDS picogus
)
add_custom_target(pg-multi
    DEPENDS pg-multi.uf2
)
