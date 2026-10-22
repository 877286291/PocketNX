#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common/Result.h"

namespace pnx {

// Parsed NRO / NSO / NXO image — the unencrypted homebrew executable containers
// targeted by M1. Layouts follow SwitchBrew:
//   NRO : https://switchbrew.org/wiki/NRO0   (NroStart + NRO0 header)
//   NSO : https://switchbrew.org/wiki/NSO0   (NSO0 header, 0x100 bytes)
//
// Key facts baked in here:
//  * NRO file  = NroStart(16B) @0x00  +  NRO0 header @0x10 (magic "NRO0").
//  * NSO file  = NSO0 header @0x00    (magic "NSO0").
//  * NRO segments carry {file_off, size}; file_off == mem_offset (no holes).
//  * NSO segments carry {file_off, mem_off, size}; segments may be LZ4-compressed.
//  * Entrypoint sits at the start of the .text segment (a branch that skips the
//    header), i.e. image_base + text.mem_offset.
struct NROImage {
    enum class Container { NRO, NSO, NXO };

    // One loadable segment (.text / .rodata / .data).
    struct Segment {
        uint64_t file_offset = 0;   // offset within the file
        uint64_t mem_offset = 0;    // offset from image base where it is mapped
        uint64_t size = 0;          // decompressed size in bytes
        uint64_t file_size = 0;     // bytes stored in the file (== size if uncompressed)
        bool     present = false;
        bool     compressed = false;
        std::vector<uint8_t> bytes; // segment bytes (raw if compressed)
    };

    // MOD0 (nn::ro dynamic structure): holds relocation / bss / eh_frame bounds.
    struct ModInfo {
        bool     present = false;
        uint64_t file_offset = 0;   // file offset of the "MOD0" magic
        uint32_t dynamic_offset = 0;   // .dynamic, relative to MOD base
        uint32_t bss_start_offset = 0; // relative to MOD base
        uint32_t bss_end_offset = 0;
        uint32_t eh_frame_hdr_start = 0;
        uint32_t eh_frame_hdr_end = 0;
    };

    Container container = Container::NRO;
    uint32_t  version = 0;
    uint32_t  flags = 0;

    Segment   text;
    Segment   rodata;
    Segment   data;
    ModInfo   mod;

    uint64_t  bss_size = 0;
    uint64_t  entry_offset = 0;                 // offset from image base of entrypoint
    std::array<uint8_t, 32> build_id{};
    std::string module_name;                     // NSO: name from .rodata; NRO: usually empty

    std::string source_path;
    bool        valid = false;
};

// Parses NRO / NSO / NXO from a file path or an in-memory buffer.
class NROLoader {
public:
    // Load and parse from disk.
    Result<NROImage> Load(const std::string& path);

    // Parse an in-memory buffer (NRO, NSO or NXO).
    Result<NROImage> Parse(const uint8_t* data, size_t size);

    // Magic sniffers (kept public for callers / tests).
    static bool IsNRO(const uint8_t* data, size_t size);
    static bool IsNSO(const uint8_t* data, size_t size);
    // Back-compat names (still useful in logs / library UI).
    static bool HasNROMagic(const uint8_t* data, size_t size) { return IsNRO(data, size); }
    static bool HasNSOMagic(const uint8_t* data, size_t size) { return IsNSO(data, size); }

private:
    Result<NROImage> ParseNRO(const uint8_t* data, size_t size, const std::string& path);
    Result<NROImage> ParseNSO(const uint8_t* data, size_t size, const std::string& path);
};

} // namespace pnx
