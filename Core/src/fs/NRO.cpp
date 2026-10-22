#include "fs/NRO.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "common/Log.h"

namespace pnx {

namespace {

constexpr uint8_t kNROMagic[4] = {'N', 'R', 'O', '0'};
constexpr uint8_t kNSOMagic[4] = {'N', 'S', 'O', '0'};
constexpr uint8_t kNSOMagicLegacy[4] = {'N', 'S', 'O', 0x00};
constexpr uint8_t kNXOMagic[4] = {'N', 'X', 'O', '0'};
constexpr uint8_t kNXOMagicLegacy[4] = {'N', 'X', 'O', 0x00};

bool ReadU32LE(const uint8_t* data, size_t size, size_t offset, uint32_t& out) {
    if (data == nullptr || size < offset + sizeof(uint32_t)) {
        return false;
    }
    out = static_cast<uint32_t>(data[offset]) |
          (static_cast<uint32_t>(data[offset + 1]) << 8) |
          (static_cast<uint32_t>(data[offset + 2]) << 16) |
          (static_cast<uint32_t>(data[offset + 3]) << 24);
    return true;
}

bool ReadBytes(const uint8_t* data, size_t size, size_t offset, size_t count,
               std::vector<uint8_t>& out) {
    if (data == nullptr || size < offset + count) {
        return false;
    }
    out.assign(data + offset, data + offset + count);
    return true;
}

bool ReadBuildId(const uint8_t* data, size_t size, size_t offset,
                 std::array<uint8_t, 32>& out) {
    if (data == nullptr || size < offset + out.size()) {
        return false;
    }
    std::memcpy(out.data(), data + offset, out.size());
    return true;
}

bool LooksLikeMod0(const uint8_t* data, size_t size, size_t offset) {
    if (data == nullptr || size < offset + 0x20) {
        return false;
    }
    return std::memcmp(data + offset, "MOD0", 4) == 0;
}

bool ParseModAt(const uint8_t* data, size_t size, size_t offset, NROImage::ModInfo& mod) {
    if (!LooksLikeMod0(data, size, offset)) {
        return false;
    }
    if (!ReadU32LE(data, size, offset + 0x04, mod.dynamic_offset)) return false;
    if (!ReadU32LE(data, size, offset + 0x08, mod.bss_start_offset)) return false;
    if (!ReadU32LE(data, size, offset + 0x0C, mod.bss_end_offset)) return false;
    if (!ReadU32LE(data, size, offset + 0x10, mod.eh_frame_hdr_start)) return false;
    if (!ReadU32LE(data, size, offset + 0x14, mod.eh_frame_hdr_end)) return false;

    // Sanity: BSS bounds should be inside the executable module image.
    if (mod.bss_end_offset < mod.bss_start_offset) {
        return false;
    }

    mod.file_offset = offset;
    mod.present = true;
    return true;
}

bool ParseMod(NROImage& img, const uint8_t* data, size_t size, uint64_t hint) {
    if (ParseModAt(data, size, static_cast<size_t>(hint), img.mod)) {
        return true;
    }

    const size_t scan_limit = std::min(size, static_cast<size_t>(0x100000));
    for (size_t offset = 0; offset + 0x20 <= scan_limit; ++offset) {
        if (ParseModAt(data, size, offset, img.mod)) {
            return true;
        }
    }
    return false;
}

void FillSegment(const uint8_t* data, size_t size, uint32_t file_offset,
                 uint64_t mem_offset, uint32_t decompressed_size,
                 uint32_t compressed_file_size, bool compressed,
                 NROImage::Segment& seg, const char* name) {
    seg.present = decompressed_size != 0;
    seg.file_offset = file_offset;
    seg.mem_offset = mem_offset;
    seg.size = decompressed_size;
    seg.compressed = compressed;

    if (!seg.present) {
        return;
    }

    const uint64_t in_file_size = compressed
                                      ? (compressed_file_size != 0 ? compressed_file_size : decompressed_size)
                                      : decompressed_size;
    if (static_cast<uint64_t>(file_offset) + in_file_size > size) {
        PNX_LOG_WARN("FS.NRO", "segment %s out of bounds: file_off=0x%x size=0x%" PRIu64 " file_size=0x%zx",
                     name, file_offset, in_file_size, size);
        seg.present = false;
        seg.file_size = 0;
        return;
    }

    seg.file_size = in_file_size;
    if (!ReadBytes(data, size, file_offset, static_cast<size_t>(in_file_size), seg.bytes)) {
        PNX_LOG_WARN("FS.NRO", "failed to copy segment %s", name);
        seg.present = false;
    } else if (compressed) {
        PNX_LOG_WARN("FS.NRO", "segment %s is compressed; LZ4/ZBIC decompression is not implemented yet", name);
    }
}

std::string ReadCString(const uint8_t* data, size_t size, uint32_t offset, uint32_t max_size) {
    if (data == nullptr || offset >= size || max_size == 0) {
        return {};
    }
    const size_t available = std::min(static_cast<size_t>(max_size), size - offset);
    const char* begin = reinterpret_cast<const char*>(data + offset);
    size_t length = 0;
    while (length < available && begin[length] != '\0') {
        ++length;
    }
    return std::string(begin, length);
}

} // namespace

bool NROLoader::IsNRO(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 0x14) {
        return false;
    }
    return std::memcmp(data + 0x10, kNROMagic, 4) == 0;
}

bool NROLoader::IsNSO(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4) {
        return false;
    }
    if (std::memcmp(data, kNSOMagic, 4) == 0 || std::memcmp(data, kNSOMagicLegacy, 4) == 0) {
        return true;
    }
    return std::memcmp(data, kNXOMagic, 4) == 0 || std::memcmp(data, kNXOMagicLegacy, 4) == 0;
}

Result<NROImage> NROLoader::Load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        PNX_LOG_ERROR("FS.NRO", "cannot open '%s'", path.c_str());
        return Result<NROImage>::Err(NotFound);
    }

    if (std::fseek(f, 0, SEEK_END) != 0) {
        std::fclose(f);
        return Result<NROImage>::Err(InvalidArg);
    }
    const long file_size = std::ftell(f);
    if (file_size <= 0) {
        std::fclose(f);
        return Result<NROImage>::Err(InvalidArg);
    }
    if (std::fseek(f, 0, SEEK_SET) != 0) {
        std::fclose(f);
        return Result<NROImage>::Err(InvalidArg);
    }

    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        PNX_LOG_ERROR("FS.NRO", "short read for '%s'", path.c_str());
        return Result<NROImage>::Err(InvalidArg);
    }

    auto result = Parse(buf.data(), buf.size());
    if (result.IsOk()) {
        result.value.source_path = path;
    }
    return result;
}

Result<NROImage> NROLoader::Parse(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 4) {
        return Result<NROImage>::Err(InvalidArg);
    }

    if (IsNRO(data, size)) {
        return ParseNRO(data, size, "<memory>");
    }
    if (IsNSO(data, size)) {
        return ParseNSO(data, size, "<memory>");
    }

    PNX_LOG_ERROR("FS.NRO", "unknown container magic (0x%02x%02x%02x%02x)",
                  data[0], data[1], data[2], data[3]);
    return Result<NROImage>::Err(InvalidArg);
}

Result<NROImage> NROLoader::ParseNRO(const uint8_t* data, size_t size, const std::string& path) {
    // NRO layout:
    //   0x00: NroStart (16 bytes, contains mod_offset at 0x04)
    //   0x10: NRO0 header
    if (!IsNRO(data, size) || size < 0x60) {
        return Result<NROImage>::Err(InvalidArg);
    }

    NROImage img;
    img.source_path = path;
    img.container = NROImage::Container::NRO;

    uint32_t mod_hint = 0;
    if (!ReadU32LE(data, size, 0x04, mod_hint)) {
        return Result<NROImage>::Err(InvalidArg);
    }

    uint32_t declared_file_size = 0;
    if (!ReadU32LE(data, size, 0x14, img.version)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x18, declared_file_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x1C, img.flags)) return Result<NROImage>::Err(InvalidArg);

    uint32_t text_mem = 0, text_size = 0;
    uint32_t rodata_mem = 0, rodata_size = 0;
    uint32_t data_mem = 0, data_size = 0;
    if (!ReadU32LE(data, size, 0x20, text_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x24, text_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x28, rodata_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x2C, rodata_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x30, data_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x34, data_size)) return Result<NROImage>::Err(InvalidArg);
    uint32_t bss32 = 0;
    if (!ReadU32LE(data, size, 0x38, bss32)) return Result<NROImage>::Err(InvalidArg);
    img.bss_size = bss32;
    if (!ReadBuildId(data, size, 0x40, img.build_id)) return Result<NROImage>::Err(InvalidArg);

    // For NRO, the segments are laid out so file offset == memory offset from
    // the image base. This is the common homebrew layout.
    FillSegment(data, size, text_mem, text_mem, text_size, text_size, false, img.text, ".text");
    FillSegment(data, size, rodata_mem, rodata_mem, rodata_size, rodata_size, false, img.rodata, ".rodata");
    FillSegment(data, size, data_mem, data_mem, data_size, data_size, false, img.data, ".data");

    if (declared_file_size != static_cast<uint32_t>(size)) {
        PNX_LOG_WARN("FS.NRO", "declared NRO size 0x%x differs from file size 0x%" PRIu64, declared_file_size,
                     static_cast<uint64_t>(size));
    }

    if (!ParseMod(img, data, size, mod_hint)) {
        PNX_LOG_WARN("FS.NRO", "MOD0 structure not found in NRO");
    }

    img.entry_offset = img.text.mem_offset;
    img.valid = img.text.present && img.mod.present;
    PNX_LOG_INFO("FS.NRO",
                 "NRO parsed: version=0x%x flags=0x%x text=0x%x+0x%x rodata=0x%x+0x%x data=0x%x+0x%x bss=0x%x entry=0x%x mod=0x%x",
                 img.version, img.flags,
                 static_cast<unsigned>(img.text.mem_offset), static_cast<unsigned>(img.text.size),
                 static_cast<unsigned>(img.rodata.mem_offset), static_cast<unsigned>(img.rodata.size),
                 static_cast<unsigned>(img.data.mem_offset), static_cast<unsigned>(img.data.size),
                 static_cast<unsigned>(img.bss_size), static_cast<unsigned>(img.entry_offset),
                 static_cast<unsigned>(img.mod.file_offset));
    return Result<NROImage>::Ok(std::move(img));
}

Result<NROImage> NROLoader::ParseNSO(const uint8_t* data, size_t size, const std::string& path) {
    if (!IsNSO(data, size) || size < 0x100) {
        return Result<NROImage>::Err(InvalidArg);
    }

    NROImage img;
    img.source_path = path;
    img.container = (data[1] == 'X') ? NROImage::Container::NXO : NROImage::Container::NSO;

    uint32_t value = 0;
    if (!ReadU32LE(data, size, 0x04, img.version)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x0C, img.flags)) return Result<NROImage>::Err(InvalidArg);

    uint32_t text_file = 0, text_mem = 0, text_size = 0;
    uint32_t name_offset = 0, name_size = 0;
    uint32_t rodata_file = 0, rodata_mem = 0, rodata_size = 0;
    uint32_t data_file = 0, data_mem = 0, data_size = 0;
    if (!ReadU32LE(data, size, 0x10, text_file)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x14, text_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x18, text_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x1C, name_offset)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x20, rodata_file)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x24, rodata_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x28, rodata_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x2C, name_size)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x30, data_file)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x34, data_mem)) return Result<NROImage>::Err(InvalidArg);
    if (!ReadU32LE(data, size, 0x38, data_size)) return Result<NROImage>::Err(InvalidArg);
    uint32_t bss32 = 0;
    if (!ReadU32LE(data, size, 0x3C, bss32)) return Result<NROImage>::Err(InvalidArg);
    img.bss_size = bss32;
    if (!ReadBuildId(data, size, 0x40, img.build_id)) return Result<NROImage>::Err(InvalidArg);

    uint32_t text_file_size = text_size;
    uint32_t rodata_file_size = rodata_size;
    uint32_t data_file_size = data_size;
    if (ReadU32LE(data, size, 0x60, value)) text_file_size = value;
    if (ReadU32LE(data, size, 0x64, value)) rodata_file_size = value;
    if (ReadU32LE(data, size, 0x68, value)) data_file_size = value;

    const bool text_compressed = (img.flags & 0x1) != 0;
    const bool rodata_compressed = (img.flags & 0x2) != 0;
    const bool data_compressed = (img.flags & 0x4) != 0;

    FillSegment(data, size, text_file, text_mem, text_size, text_file_size, text_compressed, img.text, ".text");
    FillSegment(data, size, rodata_file, rodata_mem, rodata_size, rodata_file_size, rodata_compressed, img.rodata, ".rodata");
    FillSegment(data, size, data_file, data_mem, data_size, data_file_size, data_compressed, img.data, ".data");

    img.module_name = ReadCString(data, size, name_offset, name_size);

    // MOD0 is normally near the start of .rodata, right after the module name.
    const uint64_t mod_hint = static_cast<uint64_t>(name_offset) + static_cast<uint64_t>(name_size);
    if (!ParseMod(img, data, size, mod_hint)) {
        if (!ParseMod(img, data, size, rodata_file)) {
            PNX_LOG_WARN("FS.NRO", "MOD0 structure not found in NSO/NXO");
        }
    }

    img.entry_offset = img.text.mem_offset;
    img.valid = img.text.present && img.mod.present;
    PNX_LOG_INFO("FS.NRO",
                 "NSO/NXO parsed: version=0x%x flags=0x%x text=0x%x+0x%x rodata=0x%x+0x%x data=0x%x+0x%x bss=0x%x entry=0x%x mod=0x%x name=%s",
                 img.version, img.flags,
                 static_cast<unsigned>(img.text.mem_offset), static_cast<unsigned>(img.text.size),
                 static_cast<unsigned>(img.rodata.mem_offset), static_cast<unsigned>(img.rodata.size),
                 static_cast<unsigned>(img.data.mem_offset), static_cast<unsigned>(img.data.size),
                 static_cast<unsigned>(img.bss_size), static_cast<unsigned>(img.entry_offset),
                 static_cast<unsigned>(img.mod.file_offset), img.module_name.c_str());
    return Result<NROImage>::Ok(std::move(img));
}

} // namespace pnx
