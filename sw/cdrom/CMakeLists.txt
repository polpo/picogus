add_library(cdrom INTERFACE)

target_sources(cdrom INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/cdrom.c
    ${CMAKE_CURRENT_LIST_DIR}/cdrom_image_backend.c
    ${CMAKE_CURRENT_LIST_DIR}/cdrom_image_manager.c
    ${CMAKE_CURRENT_LIST_DIR}/cdrom_image.c
    ${CMAKE_CURRENT_LIST_DIR}/86box_compat.c    
)

target_include_directories(cdrom INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/cdrom
)

target_link_libraries(cdrom INTERFACE
#    pico_multicore
) 
