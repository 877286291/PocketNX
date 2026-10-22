// Self-contained unit test for the NRO parser + Loader.
// Builds a synthetic NRO (with a real .dynamic + .rela.dyn) in memory, parses it,
// loads it, and asserts the relocations were applied correctly.
//
// Build (no CMake needed):
//   clang++ -std=c++20 -I Core/src tools/pnx-loader-test.cpp \
//     Core/src/common/Log.cpp Core/src/common/Result.cpp \
//     Core/src/fs/NRO.cpp Core/src/cpu/Loader.cpp -o /tmp/pnx-loader-test -pthread
#include <cassert>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common/Log.h"
#include "fs/NRO.h"
#include "cpu/Loader.h"

using namespace pnx;

namespace {

void W32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = v & 0xFF;
    b[off + 1] = (v >> 8) & 0xFF;
    b[off + 2] = (v >> 16) & 0xFF;
    b[off + 3] = (v >> 24) & 0xFF;
}

void W64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
    for (int i = 0; i < 8; ++i) b[off + i] = (v >> (i * 8)) & 0xFF;
}

// Build a synthetic 0x400-byte NRO with:
//   - NRO header (magic at 0x10)
//   - .text @0x100 (0x100B) with a branch instr
//   - .rodata @0x200 (0x100B) containing MOD0, .symtab, .strtab
//   - .data @0x300 (0x100B) containing .dynamic, .rela.dyn, and two reloc targets
//   - bss @0x400 (0x40B)
// Expected after load (base = actual mapped address):
//   host[0x3A0] == base + 0x100   (R_RELATIVE, addend 0x100)
//   host[0x3A8] == base + 0x108   (R_ABS64, sym=_start@0x100, addend 8)
//   host[0x400..0x440] == 0        (bss zeroed)
std::vector<uint8_t> BuildSyntheticNRO() {
    const size_t kSize = 0x400;
    std::vector<uint8_t> buf(kSize, 0);

    // --- NroStart (0x00-0x10): module_header_offset at 0x04 -> 0x200 (MOD0) ---
    W32(buf, 0x04, 0x200);

    // --- NRO0 header @0x10 ---
    buf[0x10] = 'N'; buf[0x11] = 'R'; buf[0x12] = 'O'; buf[0x13] = '0';
    W32(buf, 0x14, 0);          // version
    W32(buf, 0x18, 0x400);      // total file size
    W32(buf, 0x1C, 0);          // flags
    // segments (offset, size)
    W32(buf, 0x20, 0x100); W32(buf, 0x24, 0x100);  // text
    W32(buf, 0x28, 0x200); W32(buf, 0x2C, 0x100);  // rodata
    W32(buf, 0x30, 0x300); W32(buf, 0x34, 0x100);  // data
    W32(buf, 0x38, 0x40);                   // bss size
    for (int i = 0; i < 32; ++i) buf[0x40 + i] = static_cast<uint8_t>(i); // build id

    // --- .text @0x100: a branch instruction (b .) = 0x14000000 ---
    W32(buf, 0x100, 0x14000000);

    // --- .rodata @0x200 ---
    // MOD0 @0x200
    buf[0x200] = 'M'; buf[0x201] = 'O'; buf[0x202] = 'D'; buf[0x203] = '0';
    W32(buf, 0x204, 0x100);   // dynamic_offset (relative to MOD0): 0x200+0x100=0x300
    W32(buf, 0x208, 0x400);   // bss start
    W32(buf, 0x20C, 0x440);   // bss end
    W32(buf, 0x210, 0);       // eh start
    W32(buf, 0x214, 0);       // eh end

    // .symtab @0x240
    // sym[0]: null symbol (name=0, value=0)
    W64(buf, 0x240, 0);       // name=0, info/other/shndx=0
    W64(buf, 0x248, 0);       // value=0, size=0
    // sym[1]: _start, value=0x100
    W64(buf, 0x258, 1);       // name=1 (strtab index), rest 0
    W64(buf, 0x260, 0x100);   // value=0x100, size=0

    // .strtab @0x2C0: index0='\0', index1="_start\0"
    buf[0x2C0] = '\0';
    const char* s1 = "_start";
    for (size_t i = 0; i < std::strlen(s1); ++i) buf[0x2C1 + i] = s1[i];
    buf[0x2C1 + std::strlen(s1)] = '\0';

    // --- .data @0x300 ---
    // .dynamic @0x300 (6 entries, each 16B)
    W64(buf, 0x300, 6);  W64(buf, 0x308, 0x240);  // DT_SYMTAB
    W64(buf, 0x310, 5);  W64(buf, 0x318, 0x2C0);  // DT_STRTAB
    W64(buf, 0x320, 10); W64(buf, 0x328, 0x40);   // DT_STRSZ
    W64(buf, 0x330, 7);  W64(buf, 0x338, 0x360);  // DT_RELA
    W64(buf, 0x340, 8);  W64(buf, 0x348, 0x30);   // DT_RELASZ
    W64(buf, 0x350, 0);  W64(buf, 0x358, 0);      // DT_NULL

    // .rela.dyn @0x360 (2 entries, each 24B)
    // rela[0]: R_RELATIVE(1027) at 0x3A0, addend 0x100  -> base+0x100
    W64(buf, 0x360, 0x3A0);                // offset
    W64(buf, 0x368, (uint64_t(0) << 32) | 1027u);   // info: sym=0, type=R_RELATIVE
    W64(buf, 0x370, 0x100);                // addend
    // rela[1] starts at 0x360 + 24 = 0x378 (records are contiguous):
    // R_ABS64(257) at 0x3A8, sym=1 (_start), addend 8 -> base+0x108
    W64(buf, 0x378, 0x3A8);                // offset
    W64(buf, 0x380, (uint64_t(1) << 32) | 257u);    // info: sym=1, type=R_ABS64
    W64(buf, 0x388, 8);                    // addend

    // Relocation targets (pre-filled with link-time value 0x100)
    W64(buf, 0x3A0, 0x100);
    W64(buf, 0x3A8, 0x100);

    return buf;
}

} // namespace

int main() {
    pnx::Log::SetLevel(pnx::LogLevel::Info);
    auto buf = BuildSyntheticNRO();

    // --- Parse ---
    NROLoader loader;
    auto res = loader.Parse(buf.data(), buf.size());
    assert(res.IsOk() && "Parse should succeed");
    const NROImage& img = res.value;

    assert(img.container == NROImage::Container::NRO && "container should be NRO");
    assert(img.valid && "image should be valid");
    assert(img.text.present && "text segment present");
    assert(img.entry_offset == 0x100 && "entry should be text base (0x100)");
    assert(img.mod.present && "MOD0 should be found");
    assert(img.mod.file_offset == 0x200 && "MOD0 at 0x200");
    std::printf("[parse] OK: container=NRO entry=0x%llx mod@0x%" PRIX64
                " text=0x%" PRIX64 "+0x%" PRIX64 " bss=0x%" PRIX64 "\n",
                (unsigned long long)img.entry_offset, img.mod.file_offset,
                img.text.mem_offset, img.text.size, img.bss_size);

    // --- Load ---
    Loader ld;
    auto lres = ld.Load(img);
    assert(lres.IsOk() && "Load should succeed");
    LoadedModule& mod = lres.value;
    uint8_t* host = reinterpret_cast<uint8_t*>(mod.base);

    assert(mod.valid);
    assert(mod.applied_relocations == 2 && "should apply 2 relocations");
    assert(mod.skipped_relocations == 0 && "no relocations should be skipped");

    uint64_t got1 = 0, got2 = 0;
    std::memcpy(&got1, host + 0x3A0, 8);
    std::memcpy(&got2, host + 0x3A8, 8);

    const uint64_t expect1 = mod.base + 0x100;
    const uint64_t expect2 = mod.base + 0x108;
    std::printf("[load]  base=0x%llx applied=%zu skipped=%zu\n",
                (unsigned long long)mod.base, mod.applied_relocations, mod.skipped_relocations);
    std::printf("[check] R_RELATIVE  @0x3A0 got=0x%llx want=0x%llx  %s\n",
                (unsigned long long)got1, (unsigned long long)expect1,
                got1 == expect1 ? "PASS" : "FAIL");
    std::printf("[check] R_ABS64     @0x3A8 got=0x%llx want=0x%llx  %s\n",
                (unsigned long long)got2, (unsigned long long)expect2,
                got2 == expect2 ? "PASS" : "FAIL");

    // BSS must be zeroed
    bool bss_ok = true;
    for (uint64_t o = 0x400; o < 0x440; ++o) {
        if (host[o] != 0) { bss_ok = false; break; }
    }
    std::printf("[check] BSS zeroed  [0x400..0x440)  %s\n", bss_ok ? "PASS" : "FAIL");

    assert(got1 == expect1 && "R_RELATIVE result wrong");
    assert(got2 == expect2 && "R_ABS64 result wrong");
    assert(bss_ok && "BSS not zeroed");

    // Export should include _start at base+0x100
    auto it = mod.exports.find("_start");
    std::printf("[check] export _start = 0x%llx  %s\n",
                (unsigned long long)(it != mod.exports.end() ? it->second : 0),
                (it != mod.exports.end() && it->second == mod.base + 0x100) ? "PASS" : "FAIL");
    assert(it != mod.exports.end() && it->second == mod.base + 0x100);

    ld.Unload(mod);
    std::printf("\nALL TESTS PASSED\n");
    return 0;
}
