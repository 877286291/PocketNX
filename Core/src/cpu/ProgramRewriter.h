#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "common/Result.h"

namespace pnx {

// One patched instruction. `pc` is the absolute host address of the instruction
// in the loaded guest image. Under direct execution the guest and host address
// spaces are the same for the loaded module, so this is also the PC the trap
// handler will observe.
struct SvcRewriteInfo {
    uint64_t pc = 0;
    uint32_t original = 0;  // original SVC encoding
    uint32_t patched = 0;   // BRK encoding written in its place
    uint16_t svc_number = 0;
};

// Result of scanning a code region for SVC instructions and patching them to
// BRK. The side table maps patched PCs to the original Switch service-call
// number; Phase C (SyscallTrap) uses this to route the trap into the OS layer.
struct ProgramRewriteResult {
    uint64_t code_base = 0;
    size_t scanned_bytes = 0;
    size_t scanned_instructions = 0;
    size_t patched_svcs = 0;
    size_t non_svc_instructions = 0;

    std::vector<SvcRewriteInfo> rewrites;
    std::unordered_map<uint64_t, uint16_t> pc_to_svc;
};

class ProgramRewriter {
public:
    struct Config {
        // Immediate embedded in the BRK used to replace SVC. The trap handler
        // identifies PocketNX traps by this value and then consults the side
        // table for the original SVC number.
        uint16_t brk_immediate;  // "PN"
        // Best-effort instruction-cache maintenance after patching. On Apple
        // silicon this uses the built-in cache clear; on other hosts it is a
        // no-op in this M1 implementation.
        bool flush_icache;
        Config() : brk_immediate(0x504E), flush_icache(true) {}
    };

    explicit ProgramRewriter(Config cfg = {});

    // Scan `code` for AArch64 SVC instructions and patch them in place to
    // BRK. The buffer must be writable and 4-byte aligned. `size` is rounded
    // down to a multiple of four.
    Result<ProgramRewriteResult> Rewrite(uint8_t* code, size_t size) const;

    // Decoders / encoder, exposed for tests and the trap handler.
    static bool IsSVC(uint32_t insn, uint16_t& svc_number);
    static bool IsBRK(uint32_t insn, uint16_t& brk_immediate);
    static uint32_t EncodeBRK(uint16_t brk_immediate);

    uint16_t BrkImmediate() const { return cfg_.brk_immediate; }

private:
    Config cfg_;
};

} // namespace pnx
