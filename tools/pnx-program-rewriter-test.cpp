// Self-contained test for the A/B program rewriter.
//
// Builds an in-memory AArch64 code buffer containing SVC, BRK, and ordinary
// instructions, runs ProgramRewriter, and asserts:
//   * every SVC is patched to the BRK marker
//   * the side table maps each patched PC to the original SVC number
//   * BRK and ordinary instructions are left untouched
//
// No execution is required: the rewriter only inspects and patches bytes, so it
// works on any host (the AArch64 encoding is data, not host code).
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -I Core/src tools/pnx-program-rewriter-test.cpp \
//     Core/src/common/Log.cpp Core/src/common/Result.cpp Core/src/cpu/ProgramRewriter.cpp \
//     -o pnx-program-rewriter-test -pthread
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/Log.h"
#include "cpu/ProgramRewriter.h"

using namespace pnx;

namespace {

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/mman.h>
#endif

void Put32(uint8_t* p, size_t off, uint32_t v) { std::memcpy(p + off, &v, 4); }
uint32_t Get32(const uint8_t* p, size_t off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }

bool Check(bool cond, const char* what) {
    std::printf("[check] %-30s %s\n", what, cond ? "PASS" : "FAIL");
    return cond;
}

} // namespace

int main() {
    Log::SetLevel(LogLevel::Info);

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    const size_t kPageSize = 0x1000;
    uint8_t* mem = static_cast<uint8_t*>(
        ::mmap(nullptr, kPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    assert(mem != MAP_FAILED && "mmap failed");

    // 5 instructions, 20 bytes total.
    const uint32_t ret_insn   = 0xD65F03C0u;                       // ret
    const uint32_t svc_0x80   = 0xD4000001u | (0x80u << 5);         // svc #0x80
    const uint32_t svc_1      = 0xD4000001u | (1u << 5);            // svc #1
    const uint32_t brk_0x1234 = 0xD4200000u | (0x1234u << 5);       // brk #0x1234
    const uint32_t mov_nzcv   = 0xD503201F;                          // hint/nop-ish

    Put32(mem, 0,  ret_insn);
    Put32(mem, 4,  svc_0x80);
    Put32(mem, 8,  svc_1);
    Put32(mem, 12, brk_0x1234);
    Put32(mem, 16, mov_nzcv);

    const size_t kScanSize = 20; // 5 instructions
    const uint64_t base = reinterpret_cast<uint64_t>(mem);

    ProgramRewriter rewriter;
    const uint32_t expect_brk = ProgramRewriter::EncodeBRK(rewriter.BrkImmediate());

    auto res = rewriter.Rewrite(mem, kScanSize);
    assert(res.IsOk() && "Rewrite should succeed");
    const ProgramRewriteResult& r = res.value;

    bool ok = true;
    ok &= Check(r.patched_svcs == 2, "patched exactly 2 SVC");
    ok &= Check(r.scanned_instructions == 5, "scanned 5 instructions");
    ok &= Check(r.non_svc_instructions == 3, "3 non-SVC instructions");
    ok &= Check(Get32(mem, 4)  == expect_brk, "SVC@+4 -> BRK marker");
    ok &= Check(Get32(mem, 8)  == expect_brk, "SVC@+8 -> BRK marker");
    ok &= Check(Get32(mem, 0)  == ret_insn,   "RET@+0 untouched");
    ok &= Check(Get32(mem, 12) == brk_0x1234, "BRK@+12 untouched");
    ok &= Check(Get32(mem, 16) == mov_nzcv,   "HINT@+16 untouched");

    auto it1 = r.pc_to_svc.find(base + 4);
    auto it2 = r.pc_to_svc.find(base + 8);
    ok &= Check(it1 != r.pc_to_svc.end() && it1->second == 0x80, "side table PC+4 -> svc 0x80");
    ok &= Check(it2 != r.pc_to_svc.end() && it2->second == 1,    "side table PC+8 -> svc 1");

    // Decoder sanity: the patched bytes must now read back as our BRK.
    uint16_t brk_imm = 0;
    ok &= Check(ProgramRewriter::IsBRK(expect_brk, brk_imm) &&
               brk_imm == rewriter.BrkImmediate(), "patched bytes decode as expected BRK");

    // A real SVC should decode to the right number.
    uint16_t svc = 0;
    ok &= Check(ProgramRewriter::IsSVC(svc_0x80, svc) && svc == 0x80, "IsSVC decodes 0x80");
    ok &= Check(!ProgramRewriter::IsSVC(ret_insn, svc), "RET is not SVC");

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        ::munmap(mem, kPageSize);
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    ::munmap(mem, kPageSize);
    return 0;
#else
    std::printf("skipped: mmap not available on this host\n");
    return 0;
#endif
}
