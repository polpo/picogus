# Finds (or builds) the uf2create executable
#
# This will define the following variables
#
#    UF2CREATE_FOUND
#
# and the following imported targets
#
#     UF2CREATE
#

if (NOT UF2CREATE_FOUND)
    # todo we would like to use pckgconfig to look for it first
    # see https://pabloariasal.github.io/2018/02/19/its-time-to-do-cmake-right/

    include(ExternalProject)

    set(UF2CREATE_SOURCE_DIR ${CMAKE_SOURCE_DIR}/multifw/uf2create)
    set(UF2CREATE_BINARY_DIR ${CMAKE_BINARY_DIR}/uf2create)

    set(UF2CREATE_BUILD_TARGET UF2CREATEBuild)
    set(UF2CREATE_TARGET UF2CREATE)

    if (NOT TARGET ${UF2CREATE_BUILD_TARGET})
        pico_message_debug("UF2CREATE will need to be built")
        ExternalProject_Add(${UF2CREATE_BUILD_TARGET}
                PREFIX uf2create
                SOURCE_DIR ${UF2CREATE_SOURCE_DIR}
                BINARY_DIR ${UF2CREATE_BINARY_DIR}
                CMAKE_ARGS "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
                CMAKE_CACHE_ARGS "-DMULTIFW_INCLUDE:STRING=${CMAKE_BINARY_DIR}/generated/multifw"
                BUILD_ALWAYS 1 # force dependency checking
                INSTALL_COMMAND ""
                )
    endif()

    set(UF2CREATE_EXECUTABLE ${UF2CREATE_BINARY_DIR}/uf2create)
    if(NOT TARGET ${UF2CREATE_TARGET})
        add_executable(${UF2CREATE_TARGET} IMPORTED)
    endif()
    set_property(TARGET ${UF2CREATE_TARGET} PROPERTY IMPORTED_LOCATION
            ${UF2CREATE_EXECUTABLE})

    add_dependencies(${UF2CREATE_TARGET} ${UF2CREATE_BUILD_TARGET})
    set(UF2CREATE_FOUND 1)
endif()
