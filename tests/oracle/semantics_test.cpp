// Differential test of generated instruction semantics against Dolphin's
// hardware-verified interpreter. Each test function is one real Tekken
// instruction encoding followed by blr (see cafe-semgen); each runs on many
// random machine states and must match the oracle bit for bit.

#include "oracle.h"

#include "cafe/ppc_ops.h"
#include "ppc.h"

#include <sys/mman.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

struct SemanticsTest {
    uint32_t word;
    cafe::PPCFunc* function;
};
extern const SemanticsTest kSemanticsTests[];
extern const size_t kSemanticsTestCount;

// Hooks the generated code may reference.
extern "C" {
cafe::PPCFunc* cafe_ppc_lookup(uint32_t) { std::abort(); }
void cafe_ppc_trap(cafe::PPCContext&, uint32_t) { std::abort(); }
void cafe_ppc_illegal(cafe::PPCContext&, uint32_t a, uint32_t w, const char* why) {
    std::fprintf(stderr, "illegal %08X at %08X: %s\n", w, a, why);
    std::abort();
}
void cafe_ppc_null_call(cafe::PPCContext&, uint32_t) { std::abort(); }
uint64_t cafe_ppc_timebase(void) { return 0; }
}

namespace {

// Guest memory visible to the tests: the low window and the top of the
// space, so (rA|0)+d addressing with rA=0 stays mapped either way. Only the
// ranges instructions can reach are randomised and compared.
constexpr uint32_t kLowEnd = 0x00090000;
constexpr uint32_t kHighBegin = 0xFFFF0000;
constexpr uint32_t kHighSize = 0x10010;
struct Range { uint32_t begin, end; };
constexpr Range kRandomised[] = {{0x00000000, 0x00008000}, {0x00020000, 0x00058000}};

uint8_t* reserve_space() {
    void* p = mmap(nullptr, (uint64_t{1} << 32) + 0x10000, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) std::abort();
    auto* base = static_cast<uint8_t*>(p);
    mprotect(base, kLowEnd, PROT_READ | PROT_WRITE);
    mprotect(base + kHighBegin, 0x20000, PROT_READ | PROT_WRITE);
    return base;
}

struct Rng {
    uint64_t s;
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint32_t u32() { return static_cast<uint32_t>(next()); }
    uint32_t below(uint32_t n) { return static_cast<uint32_t>(next() % n); }
};

uint32_t random_gpr(Rng& rng) {
    switch (rng.below(8)) {
    case 0: return 0;
    case 1: return 1;
    case 2: return 0xFFFFFFFFu;
    case 3: return 0x80000000u;
    case 4: return 0x7FFFFFFFu;
    case 5: return rng.u32() & 0xFF;
    default: return rng.u32();
    }
}

uint64_t random_fpr(Rng& rng) {
    static const double specials[] = {
        0.0, -0.0, INFINITY, -INFINITY, NAN, 1.0, -1.0, 0.5, -0.5, 1.5, 2.5,
        DBL_MIN, DBL_TRUE_MIN, FLT_MIN, FLT_TRUE_MIN, FLT_MAX, -FLT_MAX, DBL_MAX,
        2147483648.0, -2147483648.0, 2147483647.5, 4294967296.0, 1e-40, 3.0e38,
    };
    switch (rng.below(10)) {
    case 0: return rng.next(); // any bit pattern, NaN payloads included
    case 1: return std::bit_cast<uint64_t>(cafe::ppc::load_single(rng.u32()));
    case 2: return 0x7FF4000000000000ull | (rng.next() & 0xFFFFFFFFFull); // signalling NaN
    case 3: return std::bit_cast<uint64_t>(specials[rng.below(std::size(specials))]);
    case 4: case 5: case 6: {
        // Typical game values: single-precision numbers of moderate size.
        const float f = std::ldexp(static_cast<float>(rng.below(1u << 24)) / (1u << 24),
                                   static_cast<int>(rng.below(48)) - 24) *
                        (rng.below(2) ? -1.0f : 1.0f);
        return std::bit_cast<uint64_t>(static_cast<double>(f));
    }
    default: {
        const double d = std::ldexp(static_cast<double>(rng.next() >> 11) / 9007199254740992.0,
                                    static_cast<int>(rng.below(80)) - 40) *
                         (rng.below(2) ? -1.0 : 1.0);
        return std::bit_cast<uint64_t>(d);
    }
    }
}

uint32_t random_gqr(Rng& rng) {
    static const uint32_t types[] = {0, 4, 5, 6, 7};
    return types[rng.below(5)] | (rng.below(64) << 8) | (types[rng.below(5)] << 16) |
           (rng.below(64) << 24);
}

bool is_memory_op(cafe::ppc::Op op) {
    using cafe::ppc::Op;
    switch (op) {
    case Op::lbz: case Op::lbzu: case Op::lbzx: case Op::lbzux: case Op::lhz: case Op::lhzu:
    case Op::lhzx: case Op::lhzux: case Op::lha: case Op::lhau: case Op::lhax: case Op::lhaux:
    case Op::lwz: case Op::lwzu: case Op::lwzx: case Op::lwzux: case Op::stb: case Op::stbu:
    case Op::stbx: case Op::stbux: case Op::sth: case Op::sthu: case Op::sthx: case Op::sthux:
    case Op::stw: case Op::stwu: case Op::stwx: case Op::stwux: case Op::lhbrx: case Op::lwbrx:
    case Op::sthbrx: case Op::stwbrx: case Op::lmw: case Op::stmw: case Op::lswi: case Op::lswx:
    case Op::stswi: case Op::stswx: case Op::lfs: case Op::lfsu: case Op::lfsx: case Op::lfsux:
    case Op::lfd: case Op::lfdu: case Op::lfdx: case Op::lfdux: case Op::stfs: case Op::stfsu:
    case Op::stfsx: case Op::stfsux: case Op::stfd: case Op::stfdu: case Op::stfdx: case Op::stfdux:
    case Op::stfiwx: case Op::psq_l: case Op::psq_lu: case Op::psq_lx: case Op::psq_lux:
    case Op::psq_st: case Op::psq_stu: case Op::psq_stx: case Op::psq_stux: case Op::dcbz:
    case Op::dcbz_l:
        return true;
    default:
        return false;
    }
}

void to_oracle(const cafe::PPCContext& c, OracleState& o) {
    for (int i = 0; i < 32; ++i) {
        o.gpr[i] = c.r[i];
        o.ps0[i] = std::bit_cast<uint64_t>(c.f[i].ps0);
        o.ps1[i] = std::bit_cast<uint64_t>(c.f[i].ps1);
    }
    for (int i = 0; i < 8; ++i) {
        const cafe::CRField& f = c.cr[i];
        o.cr[i] = static_cast<uint8_t>((f.lt << 3) | (f.gt << 2) | (f.eq << 1) | f.so);
        o.gqr[i] = c.gqr[i];
    }
    o.so = c.xer_so; o.ov = c.xer_ov; o.ca = c.xer_ca; o.bc = c.xer_bc;
    o.lr = c.lr; o.ctr = c.ctr;
}

std::string compare(const OracleState& expected, const OracleState& actual) {
    std::string diff;
    char line[160];
    for (int i = 0; i < 32; ++i) {
        if (expected.gpr[i] != actual.gpr[i]) {
            std::snprintf(line, sizeof line, " r%d want %08X got %08X;", i, expected.gpr[i], actual.gpr[i]);
            diff += line;
        }
        if (expected.ps0[i] != actual.ps0[i]) {
            std::snprintf(line, sizeof line, " f%d.ps0 want %016llX got %016llX;", i,
                          (unsigned long long)expected.ps0[i], (unsigned long long)actual.ps0[i]);
            diff += line;
        }
        if (expected.ps1[i] != actual.ps1[i]) {
            std::snprintf(line, sizeof line, " f%d.ps1 want %016llX got %016llX;", i,
                          (unsigned long long)expected.ps1[i], (unsigned long long)actual.ps1[i]);
            diff += line;
        }
    }
    for (int i = 0; i < 8; ++i) {
        if (expected.cr[i] != actual.cr[i]) {
            std::snprintf(line, sizeof line, " cr%d want %X got %X;", i, expected.cr[i], actual.cr[i]);
            diff += line;
        }
        if (expected.gqr[i] != actual.gqr[i]) {
            std::snprintf(line, sizeof line, " gqr%d want %08X got %08X;", i, expected.gqr[i], actual.gqr[i]);
            diff += line;
        }
    }
    const auto field = [&](const char* name, uint32_t want, uint32_t got) {
        if (want != got) {
            std::snprintf(line, sizeof line, " %s want %X got %X;", name, want, got);
            diff += line;
        }
    };
    field("xer.so", expected.so, actual.so);
    field("xer.ov", expected.ov, actual.ov);
    field("xer.ca", expected.ca, actual.ca);
    field("xer.bc", expected.bc, actual.bc);
    field("lr", expected.lr, actual.lr);
    field("ctr", expected.ctr, actual.ctr);
    return diff;
}

} // namespace

int main(int argc, char** argv) {
    const int trials = argc > 1 ? std::atoi(argv[1]) : 300;
    uint8_t* ours = reserve_space();
    uint8_t* theirs = reserve_space();
    struct Tally { int encodings = 0; long runs = 0; long skipped = 0; long failures = 0; };
    std::map<std::string, Tally> tallies;
    long total_failures = 0;

    for (size_t t = 0; t < kSemanticsTestCount; ++t) {
        const SemanticsTest& test = kSemanticsTests[t];
        const uint32_t w = test.word;
        const cafe::ppc::Op op = cafe::ppc::decode(w);
        std::string name = cafe::ppc::op_name(op);
        if (cafe::ppc::has_overflow_form(op) && cafe::ppc::oe(w)) name += 'o';
        if (cafe::ppc::has_record_form(op) && cafe::ppc::rc(w)) name += '.';
        Tally& tally = tallies[name];
        ++tally.encodings;
        int reported = 0;
        Rng rng{0x5EED0000ull + w};
        for (int trial = 0; trial < trials; ++trial) {
            cafe::PPCContext ctx{};
            for (auto& r : ctx.r) r = random_gpr(rng);
            for (auto& f : ctx.f) {
                f.ps0 = std::bit_cast<double>(random_fpr(rng));
                f.ps1 = std::bit_cast<double>(random_fpr(rng));
            }
            for (auto& c : ctx.cr) {
                const uint32_t v = rng.below(16);
                c = {static_cast<uint8_t>(v >> 3), static_cast<uint8_t>((v >> 2) & 1),
                     static_cast<uint8_t>((v >> 1) & 1), static_cast<uint8_t>(v & 1)};
            }
            for (auto& g : ctx.gqr) g = random_gqr(rng);
            ctx.xer_so = rng.below(2);
            ctx.xer_ov = rng.below(2);
            ctx.xer_ca = rng.below(2);
            ctx.xer_bc = static_cast<uint8_t>(rng.below(0x80));
            ctx.lr = rng.u32();
            ctx.ctr = rng.u32();
            if (is_memory_op(op)) {
                // Address registers point into the middle of the low window,
                // word aligned, as compiled code's are.
                const uint32_t ra = cafe::ppc::ra(w);
                if (ra != 0) ctx.r[ra] = 0x00048000u + (rng.below(0x4000) & ~31u);
                const uint32_t rb = cafe::ppc::rb(w);
                const bool indexed = cafe::ppc::opcd(w) == 31 || (cafe::ppc::opcd(w) == 4);
                if (indexed && rb == ra && ra != 0) {
                    ctx.r[ra] = 0x00024000u + (rng.below(0x2000) & ~31u); // rA + rA
                } else if (indexed) {
                    // With rA=0 the index register is the whole address.
                    ctx.r[rb] = ra == 0 ? 0x00048000u + (rng.below(0x4000) & ~31u)
                                        : rng.below(0x400) & ~3u;
                }
            }
            for (const Range& range : kRandomised) {
                for (uint32_t a = range.begin; a < range.end; a += 8) {
                    const uint64_t v = rng.next();
                    std::memcpy(ours + a, &v, 8);
                }
                std::memcpy(theirs + range.begin, ours + range.begin, range.end - range.begin);
            }
            std::memset(ours + kHighBegin, 0, kHighSize);
            std::memset(theirs + kHighBegin, 0, kHighSize);

            OracleState expected;
            to_oracle(ctx, expected);
            const OracleState before = expected;
            if (!oracle_execute(expected, theirs, w)) {
                ++tally.skipped;
                continue;
            }
            // Dolphin derives addme/subfme's carry as Carry(a, CA - 1), which
            // is 0 whenever CA was 1. Architecturally the carry is that of
            // a + 0xFFFFFFFF + CA, which is always 1 when CA is 1; Cemu's
            // Espresso interpreter agrees (CA = a || ca). Correct the oracle.
            if ((op == cafe::ppc::Op::addme || op == cafe::ppc::Op::subfme) && before.ca) {
                expected.ca = 1;
            }
            test.function(ctx, ours);
            OracleState actual;
            to_oracle(ctx, actual);
            std::string diff = compare(expected, actual);
            bool memory_same =
                std::memcmp(ours + kHighBegin, theirs + kHighBegin, kHighSize) == 0;
            for (const Range& range : kRandomised) {
                memory_same = memory_same && std::memcmp(ours + range.begin, theirs + range.begin,
                                                         range.end - range.begin) == 0;
            }
            if (!memory_same) {
                diff += " memory differs;";
            }
            ++tally.runs;
            if (!diff.empty()) {
                ++tally.failures;
                ++total_failures;
                if (reported++ < 2) {
                    std::printf("FAIL %08X %-28s%s\n", w, cafe::ppc::disassemble(w, 0).c_str(),
                                diff.c_str());
                    const uint32_t b = cafe::ppc::rb(w), a = cafe::ppc::ra(w), c = cafe::ppc::frc(w);
                    std::printf("     inputs: rA=%08X rB=%08X fA=%016llX/%016llX fB=%016llX/%016llX "
                                "fC=%016llX/%016llX\n",
                                before.gpr[a], before.gpr[b], (unsigned long long)before.ps0[a],
                                (unsigned long long)before.ps1[a], (unsigned long long)before.ps0[b],
                                (unsigned long long)before.ps1[b], (unsigned long long)before.ps0[c],
                                (unsigned long long)before.ps1[c]);
                }
            }
        }
    }

    std::printf("\n%-14s %5s %9s %8s %8s\n", "variant", "enc", "runs", "skipped", "FAILED");
    for (const auto& [name, t] : tallies) {
        std::printf("%-14s %5d %9ld %8ld %8ld%s\n", name.c_str(), t.encodings, t.runs, t.skipped,
                    t.failures, t.failures ? "  <--" : "");
    }
    std::printf("\n%zu encodings, %ld mismatching runs\n", kSemanticsTestCount, total_failures);
    return total_failures == 0 ? 0 : 1;
}
