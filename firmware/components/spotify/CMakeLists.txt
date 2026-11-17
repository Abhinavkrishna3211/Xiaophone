# Spotify Connect receiver: thin bridge around cspot (GPL-3, firmware/external/spotifyesp).
idf_component_register(
    SRCS "spotify_bridge.cpp"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s nvs_flash mdns esp_timer
)

# cspot's vendored libraries trip warnings that newer GCC treats as errors
idf_build_set_property(COMPILE_OPTIONS "-Wno-error;-Wno-sizeof-array-div;-Wno-overloaded-virtual" APPEND)

set(BUILD_SHARED_LIBS OFF)
set(BUILD_TESTING OFF)
set(BELL_DISABLE_SINKS ON CACHE BOOL "" FORCE)
set(CSPOT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../external/spotifyesp/cspot/cspot")

# PlatformIO builds from the CMake code model and skips custom commands, so the nanopb
# sources cspot expects are generated here at configure time instead.
idf_build_get_property(python PYTHON)
file(GLOB protos RELATIVE "${CSPOT_DIR}" "${CSPOT_DIR}/protobuf/*.proto")
execute_process(
    COMMAND ${python} bell/external/nanopb/generator/nanopb_generator.py -q -I. -Iprotobuf
            -D "${CMAKE_CURRENT_BINARY_DIR}/cspot" ${protos}
    WORKING_DIRECTORY "${CSPOT_DIR}"
    RESULT_VARIABLE nanopb_rc)
if(NOT nanopb_rc EQUAL 0)
    message(FATAL_ERROR "nanopb failed: pip install protobuf grpcio-tools \"setuptools<81\" into the ESP-IDF Python")
endif()

add_subdirectory("${CSPOT_DIR}" ${CMAKE_CURRENT_BINARY_DIR}/cspot)

target_link_libraries(${COMPONENT_LIB} PRIVATE cspot)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++20)
