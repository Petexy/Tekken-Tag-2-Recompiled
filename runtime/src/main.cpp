#include "cafe/generated.h"
#include "cafe/layout.h"
#include "cafe/ppc_ops.h"
#include "cafe/runtime.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

// Main-thread stack and startup arguments live at the top of MEM2 until the
// coreinit heap implementation owns that memory.
constexpr uint32_t kStartupArea = 0x4FF00000;
constexpr uint32_t kStartupAreaSize = 0x00100000;

void usage(const char* argv0) {
    std::fprintf(stderr,
                 "usage: %s GAME_DIR\n"
                 "  GAME_DIR contains code/Tekken.rpx (and later content/ and meta/)\n",
                 argv0);
}

} // namespace

int main(int argc, char** argv) {
    using namespace cafe;
    if (argc != 2) {
        usage(argv[0]);
        return 2;
    }
    const std::filesystem::path game = argv[1];
    install_fault_handler();
    reserve_guest_memory();
    const LoadedImage image = load_image(game / "code" / "Tekken.rpx");
    build_dispatch_table();
    std::fprintf(stderr, "ttt2: loaded image, entry 0x%08X, %zu/%zu guest entries compiled\n",
                 image.entry_point, cafe_program_info.compiled_entries,
                 cafe_program_info.total_entries);

    commit_guest_memory(kStartupArea, kStartupAreaSize);
    uint8_t* base = guest_base();
    // argv[0] is the executable name, as the Cafe loader passes it.
    const char* name = "Tekken.rpx";
    const uint32_t argv_address = kStartupArea;
    const uint32_t name_address = kStartupArea + 0x10;
    std::memcpy(base + name_address, name, std::strlen(name) + 1);
    ppc::st32(base, argv_address, name_address);
    ppc::st32(base, argv_address + 4, 0);

    static PPCContext ctx{};
    const uint32_t stack_top = kStartupArea + kStartupAreaSize - 0x10;
    ctx.r[1] = stack_top;
    ppc::st32(base, stack_top, 0); // terminate the back chain
    ctx.r[2] = image.sda2_base;
    ctx.r[13] = image.sda_base;
    ctx.r[3] = 1;
    ctx.r[4] = argv_address;
    // Cafe OS starts every thread with these quantization formats.
    ctx.gqr[2] = 0x00040004;
    ctx.gqr[3] = 0x00050005;
    ctx.gqr[4] = 0x00060006;
    ctx.gqr[5] = 0x00070007;
    set_current_context(&ctx);

    cafe_ppc_lookup(image.entry_point)(ctx, base);
    std::fprintf(stderr, "ttt2: guest entry returned %d\n", static_cast<int32_t>(ctx.r[3]));
    return static_cast<int>(ctx.r[3]);
}
