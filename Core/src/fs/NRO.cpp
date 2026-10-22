#include "fs/NRO.h"

#include <cstdio>
#include <cstring>

#include "common/Log.h"

namespace pnx {

namespace {
constexpr uint8_t kNROMagic[4] = {'N', 'S', 'R', 'O'};
constexpr uint8_t kNSOMagic[4] = {'N', 'S', 'O', 0x00};

inline bool MagicMatches(const uint8_t* data, size_t size, const uint8_t* magic) {
    return size >= 4 && std::memcmp(data, magic, 4) == 0;
}
} // namespace

bool NROLoader::HasNROMagic(const uint8_t* data, size_t size) {
    return data && MagicMatches(data, size, kNROMagic);
}

bool NROLoader::HasNSOMagic(const uint8_t* data, size_t size) {
    return data && MagicMatches(data, size, kNSOMagic);
}

Result<NROImage> NROLoader::Load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        PNX_LOG_ERROR("FS.NRO", "cannot open '%s'", path.c_str());
        return Result<NROImage>::Err(NotFound);
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return Result<NROImage>::Err(InvalidArg);
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        return Result<NROImage>::Err(InvalidArg);
    }
    return Parse(buf.data(), buf.size());
}

Result<NROImage> NROLoader::Parse(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        return Result<NROImage>::Err(InvalidArg);
    }
    if (HasNROMagic(data, size)) {
        return ParseNRO(data, size, "<memory>");
    }
    if (HasNSOMagic(data, size)) {
        return ParseNSO(data, size, "<memory>");
    }
    PNX_LOG_ERROR("FS.NRO", "unknown container magic (0x%02x%02x%02x%02x)",
                  data[0], data[1], data[2], data[3]);
    return Result<NROImage>::Err(InvalidArg);
}

Result<NROImage> NROLoader::ParseNRO(const uint8_t* data, size_t size, const std::string& path) {
    (void)data; // full header field extraction lands in M1
    // NRO layout (after 4-byte magic): fixed 0x60-byte header describing the
    // embedded NSO, then the NSO payload. M0: verify structure exists and defer
    // full field extraction to M1.
    NROImage img;
    img.source_path = path;
    img.valid = size >= 0x64;
    PNX_LOG_INFO("FS.NRO", "NRO detected, size=%zu, struct-ok=%d", size, (int)img.valid);
    return Result<NROImage>::Ok(std::move(img));
}

Result<NROImage> NROLoader::ParseNSO(const uint8_t* data, size_t size, const std::string& path) {
    (void)data; // segment table + relocations extraction lands in M1
    // NSO layout (after 4-byte magic): 0x50-byte metadata header, then a
    // segment table (code / rodata / bss), then segment data, then relocations.
    // M0: detect + report; M1 extracts addresses, sizes, entry, and segment bytes.
    NROImage img;
    img.source_path = path;
    img.valid = size >= 0x54;
    PNX_LOG_INFO("FS.NRO", "NSO/NXO detected, size=%zu, struct-ok=%d", size, (int)img.valid);
    return Result<NROImage>::Ok(std::move(img));
}

} // namespace pnx
