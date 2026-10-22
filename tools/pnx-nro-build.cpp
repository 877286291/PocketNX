// pnx-nro-build — pack a Mach-O arm64 relocatable object into a Switch NRO.
//
// M1 scope (kept deliberately narrow — see docs/ROADMAP.md):
//   * input : Mach-O 64-bit relocatable (MH_OBJECT), arm64, as produced by
//             `clang -c` on an Apple host
//   * output: a minimal, valid NRO with a fixed, page-aligned segment layout
//                 .text   @ 0x100
//                 .rodata @ 0x1000   (synthesized MOD0 + DT_NULL .dynamic + const)
//                 .data   @ 0x2000
//             Page-aligning every segment guarantees the runtime's W^X guard
//             never sees a .text page shared with writable .data/.bss.
//   * the entry symbol (main) must sit at offset 0 of __text, because the NRO
//     format fixes the entrypoint at the start of the .text segment.
//   * sections accepted:
//         __TEXT,__text              -> .text   (required, no relocations)
//         __TEXT,__const / __rodata
//         __TEXT,__cstring / __ustring -> .rodata (concatenated, file-order)
//         __DATA,__data / __DATA_CONST,__const -> .data
//         any S_ZEROFILL section (__common/__bss) -> .bss size
//       anything else is rejected so the builder's contract stays explicit.
//
// After emitting, the tool parses its own output back with NROLoader to
// self-check that the result is a structurally valid NRO.
//
// usage: pnx-nro-build <input.o> [-o output.nro]

#include <algorithm>
#include <cstdarg>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "common/Log.h"
#include "common/Result.h"
#include "fs/NRO.h"

namespace {

// printf-style formatting into a std::string for human-readable error messages.
std::string Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);
    std::string out(n > 0 ? static_cast<size_t>(n) : 0, '\0');
    if (n > 0) {
        std::vsnprintf(out.data(), out.size() + 1, fmt, ap);
    }
    va_end(ap);
    return out;
}

// ---------------------------------------------------------------------------
// Mach-O 64-bit layout (little-endian, arm64)
// ---------------------------------------------------------------------------
constexpr uint32_t kMachO64Magic = 0xFEEDFACF;
constexpr int32_t  kCpuArm64     = 0x0100000C;
constexpr uint32_t kMHObject     = 1;
constexpr uint32_t kLCSegment64  = 0x19;
constexpr uint32_t kLCSymtab     = 0x2;
constexpr uint32_t kSZerofill    = 0x2; // section type: flags & 3 == S_ZEROFILL

struct Section64 {
    std::string segname;
    std::string sectname;
    uint64_t    addr   = 0;   // section base address (== file offset in an .o)
    uint64_t    size   = 0;
    uint32_t    offset = 0;   // file offset of section content
    uint32_t    nreloc = 0;
    uint32_t    flags  = 0;
    std::vector<uint8_t> bytes; // content (empty for S_ZEROFILL)
};

bool ReadU32(const uint8_t* d, size_t sz, size_t off, uint32_t& o) {
    if (sz < off + 4) return false;
    o = static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
        (static_cast<uint32_t>(d[off + 2]) << 16) |
        (static_cast<uint32_t>(d[off + 3]) << 24);
    return true;
}
bool ReadU64(const uint8_t* d, size_t sz, size_t off, uint64_t& o) {
    if (sz < off + 8) return false;
    uint32_t lo = 0, hi = 0;
    if (!ReadU32(d, sz, off, lo)) return false;
    if (!ReadU32(d, sz, off + 4, hi)) return false;
    o = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}
void WriteU32(std::vector<uint8_t>& buf, size_t off, uint32_t v) {
    buf[off]     = static_cast<uint8_t>(v);
    buf[off + 1] = static_cast<uint8_t>(v >> 8);
    buf[off + 2] = static_cast<uint8_t>(v >> 16);
    buf[off + 3] = static_cast<uint8_t>(v >> 24);
}
std::string ReadCString(const uint8_t* d, size_t sz, size_t off, size_t max) {
    if (off >= sz) return {};
    size_t len = 0;
    while (len < max && off + len < sz && d[off + len] != '\0') ++len;
    return std::string(reinterpret_cast<const char*>(d + off), len);
}
uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// ---------------------------------------------------------------------------
// Fixed NRO segment layout (one page each, so W^X never conflicts)
// ---------------------------------------------------------------------------
constexpr uint64_t kTextMem   = 0x100;
constexpr uint64_t kRodataMem = 0x1000;
constexpr uint64_t kDataMem   = 0x2000;
constexpr uint64_t kPage      = 0x1000;
constexpr uint64_t kModOff    = 0x20;  // offset of .dynamic within .rodata

struct BuiltNRO {
    std::vector<uint8_t> bytes;
    uint64_t textSize = 0;
    uint64_t rodataSize = 0;
    uint64_t dataSize = 0;
    uint64_t bssSize = 0;
    bool dataPresent = false;
};

bool ParseMachO(const std::vector<uint8_t>& file,
                std::vector<Section64>& sections,
                uint32_t& symoff, uint32_t& nsyms,
                uint32_t& stroff, uint32_t& strsize,
                std::string& err) {
    if (file.size() < 32) {
        err = "file too small for a 64-bit Mach-O header";
        return false;
    }
    uint32_t magic = 0, filetype = 0, ncmds = 0, sizeofcmds = 0;
    ReadU32(file.data(), file.size(), 0, magic);
    uint32_t cputype_u = 0;
    ReadU32(file.data(), file.size(), 4, cputype_u);
    ReadU32(file.data(), file.size(), 12, filetype);
    ReadU32(file.data(), file.size(), 16, ncmds);
    ReadU32(file.data(), file.size(), 20, sizeofcmds);

    if (magic != kMachO64Magic) {
        err = Format("not a 64-bit Mach-O object (magic 0x%" PRIx32 ")", magic);
        return false;
    }
    if (static_cast<int32_t>(cputype_u) != kCpuArm64) {
        err = Format("expected arm64 (cputype 0x%" PRIx32 ")", cputype_u);
        return false;
    }
    if (filetype != kMHObject) {
        err = Format("expected a relocatable object (MH_OBJECT), got filetype %u", filetype);
        return false;
    }

    symoff = nsyms = stroff = strsize = 0;
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; ++i) {
        if (file.size() < off + 8) {
            err = "truncated load command table";
            return false;
        }
        uint32_t cmd = 0, cmdsize = 0;
        ReadU32(file.data(), file.size(), off, cmd);
        ReadU32(file.data(), file.size(), off + 4, cmdsize);
        if (cmdsize < 8 || file.size() < off + cmdsize) {
            err = Format("malformed load command (cmdsize %u)", cmdsize);
            return false;
        }

        if (cmd == kLCSegment64) {
            const size_t segname_off = off + 8;
            (void)segname_off;
            uint32_t nsects = 0;
            if (!ReadU32(file.data(), file.size(), off + 64, nsects)) {
                err = "truncated segment command";
                return false;
            }
            const size_t first = off + 72;
            for (uint32_t s = 0; s < nsects; ++s) {
                const size_t sec = first + static_cast<size_t>(s) * 80;
                if (file.size() < sec + 80) {
                    err = "truncated section entry";
                    return false;
                }
                Section64 sc;
                sc.segname = ReadCString(file.data(), file.size(), sec + 16, 16);
                sc.sectname = ReadCString(file.data(), file.size(), sec + 0, 16);
                ReadU64(file.data(), file.size(), sec + 32, sc.addr);
                ReadU64(file.data(), file.size(), sec + 40, sc.size);
                ReadU32(file.data(), file.size(), sec + 48, sc.offset);
                ReadU32(file.data(), file.size(), sec + 60, sc.nreloc);
                ReadU32(file.data(), file.size(), sec + 64, sc.flags);

                const bool zerofill = (sc.flags & 0x3) == kSZerofill;
                if (!zerofill && sc.size > 0) {
                    if (file.size() < sc.offset + sc.size) {
                        err = Format("section %s,%s out of bounds",
                                     sc.segname.c_str(), sc.sectname.c_str());
                        return false;
                    }
                    sc.bytes.assign(file.begin() + sc.offset,
                                    file.begin() + sc.offset + sc.size);
                }
                sections.push_back(std::move(sc));
            }
        } else if (cmd == kLCSymtab) {
            ReadU32(file.data(), file.size(), off + 8, symoff);
            ReadU32(file.data(), file.size(), off + 12, nsyms);
            ReadU32(file.data(), file.size(), off + 16, stroff);
            ReadU32(file.data(), file.size(), off + 20, strsize);
        }
        off += cmdsize;
    }
    (void)sizeofcmds;
    return true;
}

bool Build(const std::vector<uint8_t>& file, BuiltNRO& out, std::string& err) {
    std::vector<Section64> sections;
    uint32_t symoff = 0, nsyms = 0, stroff = 0, strsize = 0;
    if (!ParseMachO(file, sections, symoff, nsyms, stroff, strsize, err)) {
        return false;
    }
    if (sections.empty()) {
        err = "no sections found in object";
        return false;
    }

    const Section64* text = nullptr;
    std::vector<Section64> rodata_secs;
    std::vector<Section64> data_secs;
    uint64_t bss_size = 0;

    for (const Section64& sc : sections) {
        const bool zerofill = (sc.flags & 0x3) == kSZerofill;
        if (zerofill) {
            bss_size += sc.size;
            continue;
        }
        if (sc.segname == "__TEXT" && sc.sectname == "__text") {
            if (text) {
                err = "duplicate __TEXT,__text section";
                return false;
            }
            text = &sc;
        } else if (sc.segname == "__TEXT" &&
                   (sc.sectname == "__const" || sc.sectname == "__rodata" ||
                    sc.sectname == "__cstring" || sc.sectname == "__ustring")) {
            rodata_secs.push_back(sc);
        } else if ((sc.segname == "__DATA" || sc.segname == "__DATA_CONST") &&
                   (sc.sectname == "__data" || sc.sectname == "__const_data")) {
            data_secs.push_back(sc);
        } else {
            err = Format("unsupported section %s,%s (M1 builder is relocation-free)",
                         sc.segname.c_str(), sc.sectname.c_str());
            return false;
        }
    }

    if (!text) {
        err = "missing __TEXT,__text section";
        return false;
    }
    if (text->nreloc != 0) {
        err = Format("__text has %u relocations; M1 builder only supports relocation-free code",
                     text->nreloc);
        return false;
    }
    for (const Section64& sc : rodata_secs) {
        if (sc.nreloc != 0) {
            err = Format("%s,%s has %u relocations; M1 builder does not apply them",
                         sc.segname.c_str(), sc.sectname.c_str(), sc.nreloc);
            return false;
        }
    }
    for (const Section64& sc : data_secs) {
        if (sc.nreloc != 0) {
            err = Format("%s,%s has %u relocations; M1 builder does not apply them",
                         sc.segname.c_str(), sc.sectname.c_str(), sc.nreloc);
            return false;
        }
    }

    // Concatenate rodata / data sections in file order.
    std::sort(rodata_secs.begin(), rodata_secs.end(),
              [](const Section64& a, const Section64& b) { return a.offset < b.offset; });
    std::sort(data_secs.begin(), data_secs.end(),
              [](const Section64& a, const Section64& b) { return a.offset < b.offset; });

    std::vector<uint8_t> rodata_bytes;
    for (const Section64& sc : rodata_secs)
        rodata_bytes.insert(rodata_bytes.end(), sc.bytes.begin(), sc.bytes.end());
    std::vector<uint8_t> data_bytes;
    for (const Section64& sc : data_secs)
        data_bytes.insert(data_bytes.end(), sc.bytes.begin(), sc.bytes.end());

    // Locate the entry symbol; it must be at the very start of __text.
    if (nsyms == 0 || stroff == 0) {
        err = "no symbol table; cannot locate entry symbol 'main'";
        return false;
    }
    if (file.size() < symoff + static_cast<size_t>(nsyms) * 16 ||
        file.size() < stroff + strsize) {
        err = "truncated symbol/string table";
        return false;
    }
    bool found_main = false;
    uint64_t main_value = 0;
    for (uint32_t j = 0; j < nsyms; ++j) {
        const size_t nl = symoff + static_cast<size_t>(j) * 16;
        uint32_t n_strx = 0;
        uint64_t n_value = 0;
        ReadU32(file.data(), file.size(), nl, n_strx);
        ReadU64(file.data(), file.size(), nl + 8, n_value);
        if (n_strx >= strsize) continue;
        const std::string name = ReadCString(file.data(), file.size(), stroff + n_strx,
                                             strsize - n_strx);
        if (name == "_main" || name == "main") {
            found_main = true;
            main_value = n_value;
            break;
        }
    }
    if (!found_main) {
        err = "entry symbol 'main' not found in symbol table";
        return false;
    }
    // Offset of the entry within __text. For a Mach-O N_SECT symbol n_value is
    // the intra-section offset; some toolchains instead record the absolute
    // address (section->addr + n_value). Accept both, then require the entry to
    // sit at the very start of __text (the NRO fixes entry = start of .text).
    const uint64_t base = (text->addr != 0) ? text->addr : text->offset;
    uint64_t entry_off;
    if (main_value == 0) {
        entry_off = 0;
    } else if (base != 0 && main_value >= base) {
        entry_off = main_value - base;   // absolute-address convention
    } else {
        entry_off = main_value;          // intra-section convention
    }
    if (entry_off >= text->size) {
        err = Format("entry 'main' (n_value 0x%" PRIx64 ") is not inside __text (size 0x%" PRIx64 ")",
                     main_value, text->size);
        return false;
    }
    if (entry_off != 0) {
        err = Format("entry 'main' is at offset 0x%" PRIx64 " within __text; NRO requires it at 0",
                     entry_off);
        return false;
    }

    // ---- Assemble the NRO byte image --------------------------------------
    const uint64_t text_size   = AlignUp(text->size, 4);
    const uint64_t rodata_size = kModOff + 0x10 + AlignUp(rodata_bytes.size(), 4);
    const uint64_t data_size   = AlignUp(data_bytes.size(), 4);
    const bool data_present    = !data_bytes.empty();

    if (text_size > kRodataMem - kTextMem) {
        err = Format(".text (0x%" PRIx64 ") exceeds the M1 layout slot (0x%" PRIx64 ")",
                     text_size, kRodataMem - kTextMem);
        return false;
    }
    if (rodata_size > kDataMem - kRodataMem) {
        err = Format(".rodata (0x%" PRIx64 ") exceeds the M1 layout slot (0x%" PRIx64 ")",
                     rodata_size, kDataMem - kRodataMem);
        return false;
    }
    if (data_size > kPage) {
        err = Format(".data (0x%" PRIx64 ") exceeds the M1 layout slot (0x%" PRIx64 ")",
                     data_size, kPage);
        return false;
    }

    uint64_t end = kTextMem + text_size;
    end = std::max(end, kRodataMem + rodata_size);
    if (data_present) end = std::max(end, kDataMem + data_size);
    const uint64_t file_size = AlignUp(end, kPage);

    std::vector<uint8_t> nro(file_size, 0);

    // NroStart (16 bytes): [0]=0, [4]=mod file offset, [8..15]=0
    WriteU32(nro, 0x00, 0);
    WriteU32(nro, 0x04, static_cast<uint32_t>(kRodataMem));
    // NRO0 header @0x10
    std::memcpy(nro.data() + 0x10, "NRO0", 4);
    WriteU32(nro, 0x14, 2);                                     // version
    WriteU32(nro, 0x18, static_cast<uint32_t>(file_size));       // file size
    WriteU32(nro, 0x1C, 0);                                     // flags (uncompressed)
    WriteU32(nro, 0x20, static_cast<uint32_t>(kTextMem));        // text mem
    WriteU32(nro, 0x24, static_cast<uint32_t>(text_size));       // text size
    WriteU32(nro, 0x28, static_cast<uint32_t>(kRodataMem));      // rodata mem
    WriteU32(nro, 0x2C, static_cast<uint32_t>(rodata_size));     // rodata size
    WriteU32(nro, 0x30, data_present ? static_cast<uint32_t>(kDataMem) : 0);  // data mem
    WriteU32(nro, 0x34, data_present ? static_cast<uint32_t>(data_size) : 0); // data size
    WriteU32(nro, 0x38, static_cast<uint32_t>(bss_size));        // bss size
    // build_id @0x40: 32 zero bytes (already zeroed)

    // .text
    std::memcpy(nro.data() + kTextMem, text->bytes.data(), text->bytes.size());

    // .rodata = MOD0 + .dynamic(DT_NULL) + const bytes
    const size_t ro = kRodataMem;
    std::memcpy(nro.data() + ro, "MOD0", 4);
    // BSS follows .data (or .rodata when there is no .data), mirroring the loader.
    const uint64_t bss_start =
        data_present ? (kDataMem + data_size) : (kRodataMem + rodata_size);
    const uint64_t bss_end = bss_start + bss_size;
    WriteU32(nro, ro + 0x04, static_cast<uint32_t>(kModOff));   // dynamic_offset
    WriteU32(nro, ro + 0x08, static_cast<uint32_t>(bss_start));
    WriteU32(nro, ro + 0x0C, static_cast<uint32_t>(bss_end));
    WriteU32(nro, ro + 0x10, 0);                                // eh_frame_hdr_start
    WriteU32(nro, ro + 0x14, 0);                                // eh_frame_hdr_end
    // .dynamic @ ro + 0x20: a single DT_NULL entry (16 zero bytes, already zero).
    if (!rodata_bytes.empty()) {
        std::memcpy(nro.data() + ro + kModOff + 0x10, rodata_bytes.data(), rodata_bytes.size());
    }

    // .data
    if (data_present) {
        std::memcpy(nro.data() + kDataMem, data_bytes.data(), data_bytes.size());
    }

    out.bytes = std::move(nro);
    out.textSize = text_size;
    out.rodataSize = rodata_size;
    out.dataSize = data_size;
    out.bssSize = bss_size;
    out.dataPresent = data_present;
    return true;
}

int SelfCheck(const BuiltNRO& built, std::string& err) {
    pnx::NROLoader loader;
    auto result = loader.Parse(built.bytes.data(), built.bytes.size());
    if (!result.IsOk()) {
        err = Format("self-check: re-parse failed (code=0x%" PRIX32 ")", result.code);
        return 1;
    }
    const pnx::NROImage& img = result.value;
    if (!img.valid) {
        err = "self-check: parsed NRO is not valid (text/mod missing)";
        return 1;
    }
    if (!img.text.present || img.text.size != built.textSize) {
        err = Format("self-check: .text size mismatch (0x%" PRIx64 " vs 0x%" PRIx64 ")",
                     img.text.size, built.textSize);
        return 1;
    }
    if (!img.mod.present) {
        err = "self-check: MOD0 not found in emitted NRO";
        return 1;
    }
    if (img.entry_offset != kTextMem) {
        err = Format("self-check: entry_offset 0x%" PRIx64 " != 0x%" PRIx64,
                     img.entry_offset, kTextMem);
        return 1;
    }
    // .text bytes must round-trip unchanged.
    const size_t cmp = std::min<size_t>(img.text.bytes.size(), 64);
    if (img.text.bytes.size() < cmp ||
        std::memcmp(img.text.bytes.data(), built.bytes.data() + kTextMem, cmp) != 0) {
        err = "self-check: .text bytes did not round-trip";
        return 1;
    }
    // Count SVCs so the user can sanity-check the instruction stream.
    uint32_t svcs = 0;
    for (size_t i = 0; i + 4 <= img.text.bytes.size(); i += 4) {
        const uint32_t insn = static_cast<uint32_t>(img.text.bytes[i]) |
                              (static_cast<uint32_t>(img.text.bytes[i + 1]) << 8) |
                              (static_cast<uint32_t>(img.text.bytes[i + 2]) << 16) |
                              (static_cast<uint32_t>(img.text.bytes[i + 3]) << 24);
        if ((insn & 0xFFE0001Fu) == 0xD4000001u) ++svcs;
    }

    std::printf("wrote %zu-byte NRO: text=0x%" PRIx64 " rodata=0x%" PRIx64
                " data=0x%" PRIx64 " bss=0x%" PRIx64 " svcs=%u entry=0x%" PRIx64 "\n",
                built.bytes.size(), built.textSize, built.rodataSize,
                built.dataPresent ? built.dataSize : 0, built.bssSize, svcs,
                img.entry_offset);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string input, output;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            output = argv[++i];
        } else if (a == "-h" || a == "--help") {
            std::printf("usage: %s <input.o> [-o output.nro]\n", argv[0]);
            return 0;
        } else if (input.empty()) {
            input = a;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            return 1;
        }
    }
    if (input.empty()) {
        std::fprintf(stderr, "usage: %s <input.o> [-o output.nro]\n", argv[0]);
        return 1;
    }
    if (output.empty()) {
        size_t dot = input.find_last_of('.');
        output = (dot == std::string::npos) ? (input + ".nro") : (input.substr(0, dot) + ".nro");
    }

    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open '%s'\n", input.c_str());
        return 1;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    in.close();

    pnx::Log::SetLevel(pnx::LogLevel::Error);
    BuiltNRO built;
    std::string err;
    if (!Build(file, built, err)) {
        std::fprintf(stderr, "build failed: %s\n", err.c_str());
        return 1;
    }

    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "cannot open '%s' for writing\n", output.c_str());
        return 1;
    }
    out.write(reinterpret_cast<const char*>(built.bytes.data()),
              static_cast<std::streamsize>(built.bytes.size()));
    out.close();
    if (!out) {
        std::fprintf(stderr, "failed to write '%s'\n", output.c_str());
        return 1;
    }

    return SelfCheck(built, err);
}
