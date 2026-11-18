# AirPlay speaker (classic RAOP receiver). alac.c is David Hammerton's ALAC decoder (MIT).
idf_component_register(
    SRCS "airplay.cpp" "alac.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_timer esp_wifi mdns mbedtls lwip log heap
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable)
