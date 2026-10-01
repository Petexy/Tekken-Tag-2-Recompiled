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
    src/gpu/shader_dump.cpp
    src/gpu/tiling.cpp
    src/gpu/vulkan/context.cpp
    src/gpu/vulkan/draw.cpp
    src/gpu/vulkan/formats.cpp
    src/gpu/vulkan/png.cpp
    src/gpu/vulkan/renderer.cpp
    src/gpu/vulkan/targets.cpp
    src/gpu/vulkan/textures.cpp
    src/host/audio.cpp
    src/host/window.cpp
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
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
# GLSL -> SPIR-V at run time.
find_library(SHADERC_LIBRARY NAMES shaderc_shared REQUIRED)
target_link_libraries(cafe_runtime PUBLIC cafe_rpx OpenSSL::Crypto ZArchive::zarchive amd_addrlib cafe_latte
    Vulkan::Vulkan SDL3::SDL3 ${SHADERC_LIBRARY})
# Vulkan structures are initialised with only their sType.
target_compile_options(cafe_runtime PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

# Latte shader decoding and translation, shared by the runtime and tools.
add_library(cafe_latte STATIC
    src/latte/isa.cpp
    src/latte/decode.cpp
    src/latte/disassemble.cpp
    src/latte/environment.cpp
    src/latte/translate.cpp)
target_include_directories(cafe_latte PUBLIC src)
target_compile_features(cafe_latte PUBLIC cxx_std_20)
target_compile_options(cafe_latte PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cafe-shader src/latte/cafe_shader_main.cpp)
target_link_libraries(cafe-shader PRIVATE cafe_latte ${SHADERC_LIBRARY})
target_compile_options(cafe-shader PRIVATE -Wall -Wextra -Wpedantic)
