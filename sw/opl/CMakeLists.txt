add_library(opl INTERFACE)
target_sources(opl INTERFACE
        # ${CMAKE_CURRENT_LIST_DIR}/opl_api.c
        ${CMAKE_CURRENT_LIST_DIR}/emu8950.c
        ${CMAKE_CURRENT_LIST_DIR}/tll_table_flash.c
        ${CMAKE_CURRENT_LIST_DIR}/slot_render.cpp
        ${CMAKE_CURRENT_LIST_DIR}/opl_pico.c)
target_compile_options(opl INTERFACE -fms-extensions) # want OPL_SLOT_RENDER to be unnamed within OPL_SLOT
target_compile_definitions(opl INTERFACE
        EMU8950_NO_RATECONV
)
target_include_directories(opl INTERFACE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(opl INTERFACE pico_audio_i2s hardware_gpio)
target_sources(opl INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/slot_render_pico.S
)
target_link_libraries(opl INTERFACE hardware_interp)

# ymfm OPL3 backend — alternative to the emu8950 library above.
# Link opl_ymfm instead of opl to use ymf262 (OPL3) emulation.
add_library(opl_ymfm INTERFACE)
target_sources(opl_ymfm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/opl_ymfm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_opl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_misc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_adpcm.cpp
)
target_include_directories(opl_ymfm INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src
)
target_compile_options(opl_ymfm INTERFACE -O3 -Wno-stringop-overflow)
target_link_libraries(opl_ymfm INTERFACE pico_audio_i2s hardware_gpio hardware_timer)

# ymfm OPL2 backend — uses ym3812 instead of ymf262.
# 9 channels / 18 operators / 1 output → ~3-4x faster than ymf262.
# Games detect OPL2 (status 0x06). Identical output to ymf262 for OPL2 music.
add_library(opl_ym3812 INTERFACE)
target_sources(opl_ym3812 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/opl_ymfm.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_opl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_misc.cpp
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src/ymfm_adpcm.cpp
)
target_include_directories(opl_ym3812 INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/ymfm/src
)
target_compile_definitions(opl_ym3812 INTERFACE USE_YMF3812=1)
target_compile_options(opl_ym3812 INTERFACE -O3 -Wno-stringop-overflow)
target_link_libraries(opl_ym3812 INTERFACE pico_audio_i2s hardware_gpio hardware_timer)

# DOSBox OPL3 backend
add_library(opl_dbopl INTERFACE)
target_sources(opl_dbopl INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/opl_dbopl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/dbopl/dbopl.cpp
)
target_include_directories(opl_dbopl INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/dbopl
)
target_compile_options(opl_dbopl INTERFACE -O3 -Wno-stringop-overflow)
target_link_libraries(opl_dbopl INTERFACE pico_audio_i2s hardware_gpio hardware_timer)
