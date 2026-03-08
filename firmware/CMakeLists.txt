cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(alec_nbiot_demo)

target_sources(app PRIVATE
    src/main.c
    src/critical_section.c
)

# ALEC compression library (pre-built static lib from alec-ffi crate)
add_subdirectory(lib/alec)
target_link_libraries(app PRIVATE alec_ffi)