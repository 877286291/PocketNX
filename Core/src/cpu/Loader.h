#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/Result.h"
#include "fs/NRO.h"

namespace pnx {

// One mapped memory region, tracked so it can be released on unload.
struct MemoryRegion {
    void* addr = nullptr;
    size_t size = 0;
    bool executable = false;
};

// A symbol referenced by the module but not defined in it (would be provided by
// the SDK NSO / OS on a real device). Kept for diagnostics and import resolving.
struct Import {
    uint64_t target_address = 0;  // absolute address of the relocation slot
    int64_t addend = 0;
    std::string name;
};

// Result of loading an NRO/NSO into host memory with relocations applied.
// Memory is mapped RW at load time so relocations can be written; the
// direct-execution runtime will mprotect code to RX later (needs JIT entitlement).
struct LoadedModule {
    bool valid = false;
    uint64_t base = 0;              // host virtual address the image was mapped at
    uint64_t entry_point = 0;       // base + image entry offset (or nnMain)
    size_t image_size = 0;          // total mapped size (segments + bss), page-aligned
    size_t applied_relocations = 0;
    size_t skipped_relocations = 0;

    std::unordered_map<std::string, uint64_t> exports;  // name -> absolute address
    std::vector<Import> imports;                        // unresolved references
    std::vector<MemoryRegion> regions;
};

// Load configuration.
struct LoadConfig {
    // Host virtual address hint for the image. On a direct-execution target this
    // should sit in a clean user-space range away from host mappings. The actual
    // mapped base is recorded in LoadedModule::base and used for all relocation
    // math, so this is a hint, not a requirement.
    uint64_t base_address = 0x10000000; // 256 MiB, classic Switch main-module base
    // Best-effort: mark .text readable+executable after relocation. Off by default
    // so relocation can be verified on any host; turn on for the real runtime.
    bool make_code_executable = false;
};

// Loads a parsed NRO/NSO into host memory and applies its dynamic relocations.
// This is the bridge between "we parsed the file" (NROLoader) and "we can jump to
// an entry point" (direct-execution runtime).
class Loader {
public:
    explicit Loader(LoadConfig cfg = {});
    ~Loader();

    // Map the image, copy segments, zero BSS, apply relocations, set entry point.
    Result<LoadedModule> Load(const NROImage& img);

    // Release host memory held by a module.
    void Unload(LoadedModule& mod);

private:
    // Map `size` bytes near `base` (RW). Returns the actual mapped address.
    void* MapRegion(uint64_t base, size_t size);

    // Memory offset (from image base) corresponding to a file offset, via segments.
    bool FileToMem(const NROImage& img, uint64_t file_off, uint64_t& mem_off) const;

    // Apply all dynamic relocations in place on the mapped image.
    void Relocate(const NROImage& img, LoadedModule& mod);

    LoadConfig cfg_;
};

} // namespace pnx
