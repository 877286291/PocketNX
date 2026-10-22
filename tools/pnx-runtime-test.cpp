// End-to-end test for the M1 direct-execution Runtime pipeline:
//
//   NROImage ─▶ Loader (map RW) ─▶ ProgramRewriter (SVC→BRK)
//            ─▶ mprotect(.text, RX) ─▶ SyscallTrap + OS (wire)
//            ─▶ JumpToGuest (guest runs natively on this thread)
//
// Part A (always runs, self-contained):
//   Builds a synthetic NROImage in memory whose .text is a tiny hand-verified
//   AArch64 guest (disassembly below). The guest:
//     - reads its framebuffer pointer from the GuestFrameInfo passed as x0,
//     - writes 4 pixels,
//     - calls svc #0x1E (GetSystemTick) -> patched to BRK -> trap -> OS -> x0=ticks,
//     - stores the tick count into the frame counter,
//     - overwrites pixel[0] with a marker, then rets.
//   We assert the framebuffer was written, the tick came back > 0, and the
//   rewriter found exactly one SVC (0x1E). This proves the WHOLE C->D->E
//   pipeline works with live JIT memory.
//
// Part B (only if argv[1] is a .nro path):
//   Loads a real homebrew NRO, prepares and runs it. Used once the demo
//   homebrew (hello_counter.nro) is built.
//
// AArch64-only. On other hosts prints a skip and exits 0.
//
// Build (no CMake needed):
//   clang++ -std=c++20 -Wall -Wextra -Wpedantic -I Core/src \
//     tools/pnx-runtime-test.cpp Core/src/common/Log.cpp Core/src/common/Result.cpp \
//     Core/src/cpu/Loader.cpp Core/src/cpu/ProgramRewriter.cpp \
//     Core/src/cpu/SyscallTrap.cpp Core/src/cpu/GuestThread.cpp \
//     Core/src/cpu/GuestThreadTrampoline.S Core/src/os/OS.cpp \
//     Core/src/os/Services.cpp Core/src/os/SvcTable.cpp \
//     Core/src/fs/NRO.cpp Core/src/runtime/Runtime.cpp \
//     -o pnx-runtime-test -pthread

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <unistd.h>

#include "common/Log.h"
#include "fs/NRO.h"
#include "gpu/Framebuffer.h"
#include "os/SvcTable.h"
#include "runtime/Runtime.h"

using namespace pnx;

namespace {

bool Check(bool cond, const char* what) {
    std::printf("[check] %-52s %s\n", what, cond ? "PASS" : "FAIL");
    std::fflush(stdout);
    return cond;
}

// Diagnostic fault handler: for SIGILL (ILL_ILLOPC) and SIGSEGV (SEGV_MAPERR /
// SEGV_ACCERR) si_addr is the faulting instruction/data address, which tells us
// exactly where in the guest code we died. Runs in signal context; we only use
// the (async-signal-safe) write() and then re-raise to the default handler.
void FaultHandler(int signo, siginfo_t* info, void* /*uctx*/) {
    char buf[256];
    const uintptr_t addr = info && info->si_addr ? reinterpret_cast<uintptr_t>(info->si_addr) : 0;
    const int n = std::snprintf(buf, sizeof(buf),
                                "\n[FATAL] signal=%d code=%d fault_addr=0x%llx\n", signo,
                                info ? info->si_code : -1, (unsigned long long)addr);
    if (n > 0) {
        (void)!write(STDERR_FILENO, buf, (size_t)n);
    }
    signal(signo, SIG_DFL);
    raise(signo);
}

void InstallFaultHandlers() {
    struct sigaction sa{};
    sa.sa_sigaction = FaultHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
}

// The smoke guest, hand-verified via `clang -c smoke_guest.s && objdump -d`:
//
//   mov  x9, x0               // save &info
//   ldr  x10, [x9, #24]       // x10 = info->pixels
//   mov  x11, #0x2211
//   movk x11, #0xFF33, lsl #16  // x11 = 0xFF332211
//   str  w11, [x10]           // pixel 0
//   str  w11, [x10, #4]       // pixel 1
//   str  w11, [x10, #8]       // pixel 2
//   str  w11, [x10, #12]      // pixel 3
//   mov  x0, #0
//   svc  #0x1e                // GetSystemTick -> x0 = ticks (patched to BRK)
//   str  x0, [x9, #32]        // info->counter = ticks
//   mov  x0, #0xBEEF
//   movk x0, #0xDEAD, lsl #16  // x0 = 0xDEADBEEF
//   str  w0, [x10]            // pixel 0 = marker
//   ret
static constexpr uint32_t kSmokeGuestCode[] = {
    0xaa0003e9u, // mov  x9, x0
    0xf9400d2au, // ldr  x10, [x9, #24]
    0xd284422bu, // mov  x11, #0x2211
    0xf2bfe66bu, // movk x11, #0xff33, lsl #16
    0xb900014bu, // str  w11, [x10]
    0xb900054bu, // str  w11, [x10, #4]
    0xb900094bu, // str  w11, [x10, #8]
    0xb9000d4bu, // str  w11, [x10, #12]
    0xd2800000u, // mov  x0, #0
    0xd40003c1u, // svc  #0x1e   (GetSystemTick)
    0xf9001120u, // str  x0, [x9, #32]
    0xd297dde0u, // mov  x0, #0xbeef
    0xf2bbd5a0u, // movk x0, #0xdead, lsl #16
    0xb9000140u, // str  w0, [x10]
    0xd65f03c0u, // ret
};
static constexpr size_t kSmokeGuestSize = sizeof(kSmokeGuestCode);

void RunSmokeTest(bool& ok) {
    NROImage img{};
    img.valid = true;
    img.container = NROImage::Container::NRO;
    img.text.present = true;
    img.text.mem_offset = 0x100;
    img.text.file_offset = 0x100;
    img.text.size = kSmokeGuestSize;
    img.text.file_size = kSmokeGuestSize;
    // Byte-copy the code (assign from a uint32_t* would narrow each word to one
    // byte — we need all 60 bytes).
    img.text.bytes.resize(kSmokeGuestSize);
    std::memcpy(img.text.bytes.data(), kSmokeGuestCode, kSmokeGuestSize);
    img.entry_offset = 0x100;
    img.bss_size = 0;
    img.mod.present = false; // no relocations; Loader will skip Relocate

    Runtime rt;
    auto lr = rt.LoadImage(img);
    ok &= Check(lr.IsOk(), "LoadImage: synthetic NRO loads");

    auto pr = rt.Prepare();
    ok &= Check(pr.IsOk(), "Prepare: rewrite + mprotect + trap");

    ok &= Check(rt.Rewrite().patched_svcs == 1, "Rewriter found exactly 1 SVC");
    ok &= Check(rt.PCToSvc().size() == 1, "pc_to_svc has 1 entry");
    if (!rt.PCToSvc().empty()) {
        ok &= Check(rt.PCToSvc().begin()->second == 0x1E, "SVC is 0x1E (GetSystemTick)");
    }

    // The former SVC at word 9 must have been patched to BRK #0x504E
    // (0xD4200000 | (0x504E << 5) == 0xD42A09C0).
    if (const auto* mod = rt.Module()) {
        const uint32_t w = reinterpret_cast<const uint32_t*>(mod->base + 0x100)[9];
        ok &= Check(w == 0xd42a09c0, "SVC in-memory rewritten to BRK #0x504E");
    }

    auto rr = rt.Run();
    ok &= Check(rr.IsOk(), "Run: guest executed and returned");

    const uint8_t* px = rt.FramebufferPixels();
    uint32_t p0 = 0, p1 = 0, p3 = 0;
    std::memcpy(&p0, px + 0, 4);
    std::memcpy(&p1, px + 4, 4);
    std::memcpy(&p3, px + 12, 4);
    ok &= Check(p0 == 0xDEADBEEF, "pixel[0] == 0xDEADBEEF (marker)");
    ok &= Check(p1 == 0xFF332211, "pixel[1] == 0xFF332211 (color)");
    ok &= Check(p3 == 0xFF332211, "pixel[3] == 0xFF332211 (color)");

    const GuestFrameInfo* fi = rt.FrameInfo();
    ok &= Check(fi->counter > 0, "counter = GetSystemTick ticks > 0");

    ok &= Check(!rt.ProcessExited(), "smoke guest did not ExitProcess");
}

void RunRealNroTest(const std::string& path, bool& ok) {
    std::printf("\n[part B] loading real NRO: %s\n", path.c_str());
    Runtime rt;
    auto lr = rt.LoadPath(path);
    ok &= Check(lr.IsOk(), "LoadPath: real NRO parses + loads");
    if (!lr.IsOk()) {
        return;
    }
    auto pr = rt.Prepare();
    ok &= Check(pr.IsOk(), "Prepare: real NRO rewrites + mprotect + trap");
    if (!pr.IsOk()) {
        return;
    }
    ok &= Check(rt.Rewrite().patched_svcs > 0, "real NRO has at least 1 SVC");

    // NOTE: running a real homebrew may loop forever (max_frames=0). The test
    // harness for the demo will set a bounded frame count. Here we stop at
    // prepare success to keep the unit test non-blocking.
    std::printf("[info] real NRO prepared (patched %zu SVCs); run is left to the demo harness\n",
                rt.Rewrite().patched_svcs);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: survive crashes
    InstallFaultHandlers();
    Log::SetLevel(LogLevel::Warn); // quiet; test prints its own PASS/FAIL

    bool ok = true;
    RunSmokeTest(ok);

#if defined(__aarch64__)
    if (argc > 1) {
        RunRealNroTest(argv[1], ok);
    } else {
        std::printf("[check] %-52s SKIP (no NRO path given)\n", "part B: real homebrew NRO");
        std::printf("[hint]  build demo/hello_counter.nro and pass its path as argv[1]\n");
    }
#else
    std::printf("[check] %-52s SKIP (non-AArch64)\n", "part A: smoke guest pipeline");
#endif

    if (!ok) {
        std::printf("\nSOME CHECKS FAILED\n");
        return 1;
    }
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
