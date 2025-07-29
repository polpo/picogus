

add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/taps.hpp
	#WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
	COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/taps.py ${CMAKE_CURRENT_BINARY_DIR}/taps.hpp
	DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/taps.py
)

add_custom_target(taps_hpp DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/taps.hpp)


add_library(taps OBJECT IMPORTED GLOBAL)
add_dependencies(taps taps_hpp)
target_include_directories(taps INTERFACE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(taps INTERFACE cxx_std_17)
		
add_library(resampler INTERFACE)
target_include_directories(resampler INTERFACE ${CMAKE_CURRENT_LIST_DIR})

target_link_libraries(resampler
	INTERFACE
		taps
)
