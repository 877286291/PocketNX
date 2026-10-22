#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/Result.h"

namespace pnx {

// Parsed NRO (Nintendo Runtime Object). NRO = header + NSO.
// This is the homebrew executable container used by M1 (no encryption).
struct NROImage {
    // Parsed NSO metadata (the loadable payload).
    struct NsoInfo {
        uint64_t text_address = 0;
        uint64_t text_size = 0;
        uint64_t rodata_address = 0;
        uint64_t rodata_size = 0;
        uint64_t bss_address = 0;
        uint64_t bss_size = 0;
        uint64_t entry_point = 0;
        uint64_t main_thread_stack_size = 0;
        uint32_t page_size = 0x10000; // Switch standard 64KB pages
        std::vector<uint8_t> code;     // raw text+rodata bytes
    };

    NsoInfo nso;
    std::string source_path;
    bool valid = false;
};

// Parses NRO / NSO / NXO files. M0: structure + header magic check.
class NROLoader {
public:
    // Load and parse an NRO (or raw NSO/NXO) from a file path.
    Result<NROImage> Load(const std::string& path);

    // Parse an in-memory NRO buffer.
    Result<NROImage> Parse(const uint8_t* data, size_t size);

    // Validate the NRO magic ("NSRO") / NSO magic ("NSO\0" or NXO "NSO\x00").
    static bool HasNROMagic(const uint8_t* data, size_t size);
    static bool HasNSOMagic(const uint8_t* data, size_t size);

private:
    Result<NROImage> ParseNRO(const uint8_t* data, size_t size, const std::string& path);
    Result<NROImage> ParseNSO(const uint8_t* data, size_t size, const std::string& path);
};

} // namespace pnx
