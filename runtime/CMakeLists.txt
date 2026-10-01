find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

# Linked as objects, not an archive: strong import implementations here must
# replace the weak defaults in the generated imports.cpp, and the export
# registrations must not be dropped by the linker.
add_library(cafe_runtime OBJECT
    src/memory.cpp
    src/loader.cpp
    src/dispatch.cpp
    src/diagnostics.cpp
    src/exports.cpp
    src/sysmem.cpp
    src/os/thread.cpp
    src/os/sync.cpp
    src/os/alarm.cpp
    src/os/heap.cpp
    src/os/dynload.cpp
    src/os/coreinit_misc.cpp
    src/os/guest_format.cpp
    src/os/nn_services.cpp
    src/os/proc_ui.cpp
    src/os/system_config.cpp
    src/os/input.cpp
    src/os/fs.cpp
    src/os/ax.cpp
    src/os/snd_user.cpp
    src/os/network.cpp
    src/os/swkbd.cpp
    src/os/erreula.cpp
    src/os/dmae.cpp
    src/os/hardware.cpp
    src/vfs.cpp
    src/gpu/command_processor.cpp
    src/gpu/display.cpp
    src/gpu/null_backend.cpp
    src/gx2/cbpool.cpp
    src/gx2/display.cpp
    src/gx2/draw.cpp
    src/gx2/format.cpp
    src/gx2/gx2r.cpp
    src/gx2/registers.cpp
    src/gx2/shader.cpp
    src/gx2/state.cpp
    src/gx2/surface.cpp
    src/gx2/texture.cpp)
target_include_directories(cafe_runtime PUBLIC include PRIVATE src)
target_compile_features(cafe_runtime PUBLIC cxx_std_20)
target_link_libraries(cafe_runtime PUBLIC cafe_rpx OpenSSL::Crypto ZArchive::zarchive amd_addrlib)
target_compile_options(cafe_runtime PRIVATE -Wall -Wextra -Wpedantic)
