#include "cpu/Loader.h"

#include <algorithm>
#include <cinttypes>
#include <cstring>

#include "common/Log.h"

#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
#include <sys/mman.h>
#endif

namespace pnx {

namespace {

constexpr size_t kPageSize = 0x1000;
inline size_t PageAlign(size_t v) { return (v + kPageSize - 1) & ~(kPageSize - 1); }

// ELF64 dynamic-section tags we care about.
enum DtTag : uint64_t {
    DT_NULL = 0,
    DT_PLTRELSZ = 2,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_STRSZ = 10,
    DT_JMPREL = 23,
};

// AArch64 dynamic relocation types (switchbrew / ARM ABI).
enum RelType : uint32_t {
    R_ABS64 = 257,
    R_ABS32 = 258,
    R_GLOB_DAT = 1025,
    R_JUMP_SLOT = 1026,
    R_RELATIVE = 1027,
};

// Mirror the on-disk ELF64 structs (little-endian, packed in this layout).
struct Elf64_Dyn {
    uint64_t tag;
    uint64_t value;
};
struct Elf64_Sym {
    uint32_t name;
    uint8_t info;
    uint8_t other;
    uint16_t shndx;
    uint64_t value;
    uint64_t size;
};
struct Elf64_Rela {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
};
static_assert(sizeof(Elf64_Dyn) == 0x10, "Elf64_Dyn must be 16 bytes");
static_assert(sizeof(Elf64_Sym) == 0x18, "Elf64_Sym must be 24 bytes");
static_assert(sizeof(Elf64_Rela) == 0x18, "Elf64_Rela must be 24 bytes");

} // namespace

Loader::Loader(LoadConfig cfg) : cfg_(cfg) {}

Loader::~Loader() = default;

void* Loader::MapRegion(uint64_t base, size_t size) {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    void* p = ::mmap(reinterpret_cast<void*>(base), size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        PNX_LOG_ERROR("Loader", "mmap(0x%" PRIx64 ", 0x%zx) failed", base, size);
        return nullptr;
    }
    if (reinterpret_cast<uint64_t>(p) != base) {
        PNX_LOG_WARN("Loader", "mmap hint 0x%" PRIx64 " not honored; using actual base 0x%" PRIx64,
                     base, reinterpret_cast<uint64_t>(p));
    }
    return p;
#else
    (void)base; (void)size;
    return nullptr;
#endif
}

bool Loader::FileToMem(const NROImage& img, uint64_t file_off, uint64_t& mem_off) const {
    auto check = [&](const NROImage::Segment& s) -> bool {
        if (!s.present) {
            return false;
        }
        const uint64_t extent = s.file_size ? s.file_size : s.size;
        if (file_off >= s.file_offset && file_off < s.file_offset + extent) {
            mem_off = s.mem_offset + (file_off - s.file_offset);
            return true;
        }
        return false;
    };
    if (check(img.text)) return true;
    if (check(img.rodata)) return true;
    if (check(img.data)) return true;
    return false;
}

void Loader::Unload(LoadedModule& mod) {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
    for (auto& r : mod.regions) {
        if (r.addr) {
            ::munmap(r.addr, r.size);
        }
    }
#endif
    mod.regions.clear();
    mod.base = 0;
    mod.entry_point = 0;
    mod.image_size = 0;
    mod.valid = false;
}

Result<LoadedModule> Loader::Load(const NROImage& img) {
    LoadedModule mod;
    if (!img.valid || !img.text.present) {
        PNX_LOG_ERROR("Loader", "image invalid or missing .text segment");
        return Result<LoadedModule>::Err(InvalidArg);
    }

    // Compute the top of the image: end of the last segment, plus BSS after data.
    uint64_t top = 0;
    auto bump = [&](const NROImage::Segment& s) {
        if (s.present) {
            top = std::max(top, s.mem_offset + s.size);
        }
    };
    bump(img.text);
    bump(img.rodata);
    bump(img.data);

    uint64_t bss_start = 0;
    if (img.data.present) {
        bss_start = img.data.mem_offset + img.data.size;
    } else if (img.rodata.present) {
        bss_start = img.rodata.mem_offset + img.rodata.size;
    } else {
        bss_start = img.text.mem_offset + img.text.size;
    }
    const uint64_t bss_end = bss_start + img.bss_size;
    top = std::max(top, bss_end);
    const size_t map_size = static_cast<size_t>(PageAlign(top));

    void* base = MapRegion(cfg_.base_address, map_size);
    if (!base) {
        return Result<LoadedModule>::Err(InvalidArg);
    }
    mod.base = reinterpret_cast<uint64_t>(base);
    mod.image_size = map_size;
    mod.regions.push_back({base, map_size, false});
    uint8_t* host = reinterpret_cast<uint8_t*>(base);

    // Copy each present segment into its memory location.
    auto place = [&](const NROImage::Segment& s, const char* name) {
        if (!s.present || s.bytes.empty()) {
            return;
        }
        if (s.mem_offset + s.bytes.size() > map_size) {
            PNX_LOG_ERROR("Loader", "segment %s (0x%" PRIx64 "+0x%zx) exceeds mapped size 0x%zx",
                          name, s.mem_offset, s.bytes.size(), map_size);
            return;
        }
        std::memcpy(host + s.mem_offset, s.bytes.data(), s.bytes.size());
    };
    place(img.text, ".text");
    place(img.rodata, ".rodata");
    place(img.data, ".data");

    // Zero the BSS (uninitialized) region that follows .data.
    if (img.bss_size && bss_end <= map_size) {
        std::memset(host + bss_start, 0, img.bss_size);
        PNX_LOG_INFO("Loader", "zeroed bss [0x%" PRIx64 "..0x%" PRIx64 ")",
                     mod.base + bss_start, mod.base + bss_end);
    }

    // Best-effort: mark code executable for the real runtime (needs JIT entitlement).
    if (cfg_.make_code_executable && img.text.present &&
        img.text.mem_offset + img.text.size <= map_size) {
#if defined(__APPLE__) || defined(__unix__) || defined(__linux__)
        if (::mprotect(host + img.text.mem_offset, PageAlign(static_cast<size_t>(img.text.size)),
                       PROT_READ | PROT_EXEC) == 0) {
            mod.regions.back().executable = true;
        } else {
            PNX_LOG_WARN("Loader", "mprotect(RX) failed — JIT entitlement likely required");
        }
#endif
    }

    Relocate(img, mod);

    mod.entry_point = mod.base + img.entry_offset;
    mod.valid = true;
    PNX_LOG_INFO("Loader",
                 "loaded base=0x%" PRIx64 " size=0x%zx entry=0x%" PRIx64
                 " rels_applied=%zu rels_skipped=%zu exports=%zu imports=%zu",
                 mod.base, map_size, mod.entry_point,
                 mod.applied_relocations, mod.skipped_relocations,
                 mod.exports.size(), mod.imports.size());
    return Result<LoadedModule>::Ok(std::move(mod));
}

void Loader::Relocate(const NROImage& img, LoadedModule& mod) {
    if (!img.mod.present) {
        PNX_LOG_WARN("Loader", "no MOD0; skipping relocations");
        return;
    }

    uint8_t* host = reinterpret_cast<uint8_t*>(mod.base);
    const auto in_image = [&](uint64_t off) { return off < mod.image_size; };
    const auto write64 = [&](uint64_t off, uint64_t v) { std::memcpy(host + off, &v, 8); };

    // MOD0 lives in a segment; map its file offset to a memory offset, then the
    // .dynamic section sits at (MOD0 + dynamic_offset), all relative to base.
    uint64_t mod_mem = 0;
    if (!FileToMem(img, img.mod.file_offset, mod_mem)) {
        PNX_LOG_WARN("Loader", "MOD0 file offset 0x%" PRIx64 " not in any segment",
                     img.mod.file_offset);
        return;
    }
    const uint64_t dyn_mem = mod_mem + img.mod.dynamic_offset;
    if (!in_image(dyn_mem)) {
        PNX_LOG_WARN("Loader", "dynamic section at 0x%" PRIx64 " out of image", dyn_mem);
        return;
    }

    // Walk the .dynamic table.
    std::unordered_map<uint64_t, uint64_t> dyn;
    {
        uint64_t off = dyn_mem;
        for (int guard = 0; guard < 4096 && in_image(off + sizeof(Elf64_Dyn)); ++guard) {
            Elf64_Dyn d;
            std::memcpy(&d, host + off, sizeof(d));
            off += sizeof(d);
            if (d.tag == DT_NULL) {
                break;
            }
            dyn[d.tag] = d.value;
        }
    }
    const auto has = [&](uint64_t t) { return dyn.find(t) != dyn.end(); };
    if (dyn.empty()) {
        // A DT_NULL-only .dynamic is a valid "no relocations" layout (the
        // M1 NRO builder emits this for self-contained homebrew).
        PNX_LOG_INFO("Loader", "dynamic: empty (DT_NULL only); no relocations");
        return;
    }
    PNX_LOG_INFO("Loader",
                 "dynamic: SYMTAB=%d STRTAB=%d STRSZ=%d RELA=%d RELASZ=%d JMPREL=%d PLTRELSZ=%d",
                 (int)has(DT_SYMTAB), (int)has(DT_STRTAB), (int)has(DT_STRSZ),
                 (int)has(DT_RELA), (int)has(DT_RELASZ), (int)has(DT_JMPREL), (int)has(DT_PLTRELSZ));
    if (!has(DT_SYMTAB) || !has(DT_STRTAB)) {
        PNX_LOG_WARN("Loader", "missing DT_SYMTAB/DT_STRTAB; no relocations applied");
        return;
    }
    const uint64_t symtab = dyn[DT_SYMTAB];
    const uint64_t strtab = dyn[DT_STRTAB];
    const uint64_t strsz = has(DT_STRSZ) ? dyn[DT_STRSZ] : 0;

    const auto sym_name = [&](uint32_t name_off) -> std::string {
        if (name_off == 0) {
            return {};
        }
        if (!in_image(strtab + name_off) || (strsz != 0 && name_off >= strsz)) {
            return {};
        }
        const char* s = reinterpret_cast<const char*>(host + strtab + name_off);
        return std::string(s);
    };
    struct Sym {
        uint32_t name = 0;
        uint64_t value = 0;
    };
    const auto read_sym = [&](uint32_t idx) -> Sym {
        Sym s;
        const uint64_t off = symtab + static_cast<uint64_t>(idx) * sizeof(Elf64_Sym);
        if (!in_image(off + sizeof(Elf64_Sym))) {
            return s;
        }
        Elf64_Sym e;
        std::memcpy(&e, host + off, sizeof(e));
        s.name = e.name;
        s.value = e.value;
        return s;
    };

    const auto apply_rela_range = [&](uint64_t rela_off, uint64_t rela_sz) {
        if (rela_sz == 0 || !in_image(rela_off + rela_sz)) {
            return;
        }
        for (uint64_t i = 0; i + sizeof(Elf64_Rela) <= rela_sz; i += sizeof(Elf64_Rela)) {
            Elf64_Rela r;
            std::memcpy(&r, host + rela_off + i, sizeof(r));
            const uint32_t rtype = static_cast<uint32_t>(r.info & 0xFFFFFFFFu);
            const uint32_t rsym = static_cast<uint32_t>(r.info >> 32);
            const uint64_t ea = r.offset; // relative to base
            if (!in_image(ea + sizeof(uint64_t))) {
                mod.skipped_relocations++;
                continue;
            }
            if (rtype == R_RELATIVE) {
                // *at(base+offset) = base + addend (no symbol involved).
                write64(ea, mod.base + static_cast<uint64_t>(r.addend));
                mod.applied_relocations++;
                continue;
            }
            const Sym sym = read_sym(rsym);
            const std::string name = sym_name(sym.name);
            if (sym.value != 0) {
                // Defined symbol: absolute address is base + value (+ addend for ABS64).
                uint64_t result = 0;
                bool handled = false;
                switch (rtype) {
                    case R_ABS64:
                        result = mod.base + sym.value + static_cast<uint64_t>(r.addend);
                        handled = true;
                        break;
                    case R_GLOB_DAT:
                    case R_JUMP_SLOT:
                        result = mod.base + sym.value;
                        handled = true;
                        break;
                    default:
                        break;
                }
                if (handled) {
                    write64(ea, result);
                    mod.applied_relocations++;
                    if (!name.empty()) {
                        mod.exports[name] = mod.base + sym.value;
                    }
                } else {
                    mod.skipped_relocations++;
                }
            } else {
                // Undefined symbol: an import to be resolved against SDK/OS later.
                mod.imports.push_back({mod.base + ea, rtype == R_ABS64 ? r.addend : 0, name});
                mod.skipped_relocations++;
            }
        }
    };

    if (has(DT_RELA)) {
        apply_rela_range(dyn[DT_RELA], has(DT_RELASZ) ? dyn[DT_RELASZ] : 0);
    }
    if (has(DT_JMPREL)) {
        apply_rela_range(dyn[DT_JMPREL], has(DT_PLTRELSZ) ? dyn[DT_PLTRELSZ] : 0);
    }
}

} // namespace pnx
