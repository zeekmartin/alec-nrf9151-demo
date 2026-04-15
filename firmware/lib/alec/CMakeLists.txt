# ALEC FFI static library integration for Zephyr
#
# Pre-built libalec_ffi.a (thumbv8m.main-none-eabi, alec-ffi v1.3.5 zephyr feature)
# Critical section symbols provided by firmware/src/critical_section.c (Zephyr irq_lock)

add_library(alec_ffi STATIC IMPORTED GLOBAL)
set_target_properties(alec_ffi PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libalec_ffi.a
)
target_include_directories(alec_ffi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)