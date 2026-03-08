# ALEC FFI static library integration for Zephyr
#
# Place the pre-built libalec_ffi.a (cross-compiled for thumbv8m.main-none-eabihf)
# in this directory. Build it from the alec-ffi crate (v1.2.1, bare-metal):
#
#   rustup target add thumbv8m.main-none-eabihf
#   cd alec-ffi
#   cargo build --release --target thumbv8m.main-none-eabihf
#   cp target/thumbv8m.main-none-eabihf/release/libalec_ffi.a \
#      <repo>/firmware/lib/alec/

add_library(alec_ffi STATIC IMPORTED GLOBAL)
set_target_properties(alec_ffi PROPERTIES
    IMPORTED_LOCATION ${CMAKE_CURRENT_SOURCE_DIR}/libalec_ffi.a
)
target_include_directories(alec_ffi INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
