if (PICO_SDK)
    add_library(opl INTERFACE)
    target_sources(opl INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/emu8950.c
            ${CMAKE_CURRENT_LIST_DIR}/slot_render.cpp
            ${CMAKE_CURRENT_LIST_DIR}/opl_pico.c)
    target_compile_options(opl INTERFACE -fms-extensions) # want OPL_SLOT_RENDER to be unnamed within OPL_SLOT
    target_compile_definitions(opl INTERFACE
            EMU8950_NO_RATECONV
    )
    target_include_directories(opl INTERFACE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(opl INTERFACE hardware_sync hardware_timer)
    if (PICO_ON_DEVICE)
        target_sources(opl INTERFACE
                ${CMAKE_CURRENT_LIST_DIR}/slot_render_pico.S
        )
        target_link_libraries(opl INTERFACE hardware_interp)
    endif()
else()
    message(FATAL_ERROR "This library only works in the Pico SDK") 
endif()
