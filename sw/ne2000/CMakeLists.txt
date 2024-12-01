add_library(ne2000 INTERFACE)

# if(NOT PICO_BOARD STREQUAL "PICO_W")
#     message(FATAL_ERROR "NE2000 Requires Pico W")
# endif()



target_include_directories(ne2000 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ne2000
)

target_sources(ne2000 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/ne2000.c    
)

target_link_libraries(ne2000 INTERFACE
    pico_cyw43_arch_none
) 

if(PROJECT_TYPE STREQUAL "NE2K")
    target_compile_definitions(pico_cyw43_arch_none_headers INTERFACE
        PICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1
    )
    target_compile_definitions(ne2000 INTERFACE
        PICO_CYW43_ARCH_THREADSAFE_BACKGROUND=1
    )
endif()
