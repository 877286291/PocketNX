#include "cpu/ProgramRewriter.h"

#include <cstring>

#include "common/Log.h"

namespace pnx {

namespace {

// AArch64 exception-generation encodings:
//   SVC #imm16 : 0xD4000001 | (imm16 << 5)
//   BRK #imm16 : 0xD4200000 | (imm16 << 5)
// The low five bits are fixed, so the masks clear only the 16-bit immediate.
constexpr uint32_t kSVCBase = 0xD4000001u;
constexpr uint32_t kSVCMask = 0xFFE0001Fu;
constexpr uint32_t kBRKBase = 0xD4200000u;
constexpr uint32_t kBRKMask = 0xFFE00000u;

void FlushICache(uint8_t* begin, size_t len) {
#if defined(__APPLE__) && defined(__aarch64__)
    __builtin___clear_cache(reinterpret_cast<char*>(begin),
                            reinterpret_cast<char*>(begin + len));
#else
    (void)begin;
    (void)len;
#endif
}

} // namespace

ProgramRewriter::ProgramRewriter(Config cfg) : cfg_(cfg) {}

bool ProgramRewriter::IsSVC(uint32_t insn, uint16_t& svc_number) {
    if ((insn & kSVCMask) != kSVCBase) {
        return false;
    }
    svc_number = static_cast<uint16_t>((insn >> 5) & 0xFFFFu);
    return true;
}

bool ProgramRewriter::IsBRK(uint32_t insn, uint16_t& brk_immediate) {
    if ((insn & kBRKMask) != kBRKBase) {
        return false;
    }
    brk_immediate = static_cast<uint16_t>((insn >> 5) & 0xFFFFu);
    return true;
}

uint32_t ProgramRewriter::EncodeBRK(uint16_t brk_immediate) {
    return kBRKBase | (static_cast<uint32_t>(brk_immediate) << 5);
}

Result<ProgramRewriteResult> ProgramRewriter::Rewrite(uint8_t* code, size_t size) const {
    ProgramRewriteResult result;

    if (!code || size < 4) {
        PNX_LOG_ERROR("ProgramRewriter", "invalid code buffer: ptr=%p size=%zu",
                      static_cast<void*>(code), size);
        return Result<ProgramRewriteResult>::Err(InvalidArg);
    }
    if ((reinterpret_cast<uintptr_t>(code) & 3u) != 0) {
        PNX_LOG_ERROR("ProgramRewriter", "code buffer is not 4-byte aligned: %p",
                      static_cast<void*>(code));
        return Result<ProgramRewriteResult>::Err(InvalidArg);
    }

    result.code_base = reinterpret_cast<uint64_t>(code);
    result.scanned_bytes = size & ~static_cast<size_t>(3);
    if (result.scanned_bytes == 0) {
        return Result<ProgramRewriteResult>::Ok(std::move(result));
    }

    const size_t insn_count = result.scanned_bytes / 4;
    const uint32_t replacement = EncodeBRK(cfg_.brk_immediate);
    const uint32_t* begin = reinterpret_cast<const uint32_t*>(code);
    const uint32_t* end = begin + insn_count;

    for (const uint32_t* p = begin; p != end; ++p) {
        uint32_t insn;
        std::memcpy(&insn, p, sizeof(insn));

        uint16_t svc_number = 0;
        if (!IsSVC(insn, svc_number)) {
            ++result.non_svc_instructions;
            continue;
        }

        SvcRewriteInfo info;
        info.pc = reinterpret_cast<uint64_t>(p);
        info.original = insn;
        info.patched = replacement;
        info.svc_number = svc_number;

        std::memcpy(const_cast<uint32_t*>(p), &replacement, sizeof(replacement));
        result.rewrites.push_back(info);
        result.pc_to_svc[info.pc] = info.svc_number;
        ++result.patched_svcs;

        if (result.patched_svcs <= 8) {
            PNX_LOG_INFO("ProgramRewriter", "patched SVC #0x%x at pc=0x%llx",
                         svc_number, (unsigned long long)info.pc);
        }
    }

    result.scanned_instructions = insn_count;
    if (cfg_.flush_icache) {
        FlushICache(code, result.scanned_bytes);
    }

    PNX_LOG_INFO("ProgramRewriter",
                 "scanned 0x%zx bytes (%zu instructions), patched %zu SVC -> BRK #0x%04x",
                 result.scanned_bytes, insn_count, result.patched_svcs, cfg_.brk_immediate);
    return Result<ProgramRewriteResult>::Ok(std::move(result));
}

} // namespace pnx
