#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "fs/NRO.h"
#include "common/Log.h"

namespace {

const char* ContainerName(pnx::NROImage::Container container) {
    switch (container) {
        case pnx::NROImage::Container::NRO: return "NRO";
        case pnx::NROImage::Container::NSO: return "NSO";
        case pnx::NROImage::Container::NXO: return "NXO";
    }
    return "?";
}

void PrintHex(const uint8_t* data, size_t size, size_t max_bytes = 32) {
    const size_t count = std::min(size, max_bytes);
    for (size_t i = 0; i < count; ++i) {
        std::printf("%02x%s", data[i], i + 1 == count ? "\n" : " ");
    }
    if (size == 0) {
        std::printf("(empty)\n");
    } else if (size > max_bytes) {
        std::printf("... +%zu bytes\n", size - max_bytes);
    }
}

void PrintSegment(const char* name, const pnx::NROImage::Segment& seg) {
    std::printf("  %-8s present=%d file_off=0x%" PRIx64 " mem_off=0x%" PRIx64
                " size=0x%" PRIx64 " file_size=0x%" PRIx64 " compressed=%d\n",
                name, seg.present ? 1 : 0,
                seg.file_offset, seg.mem_offset, seg.size, seg.file_size,
                seg.compressed ? 1 : 0);
    if (!seg.bytes.empty()) {
        std::printf("    bytes: ");
        PrintHex(seg.bytes.data(), seg.bytes.size());
    }
}

void PrintBuildId(const std::array<uint8_t, 32>& build_id) {
    for (uint8_t byte : build_id) {
        std::printf("%02x", byte);
    }
    std::printf("\n");
}

int Run(const char* path) {
    pnx::NROLoader loader;
    auto result = loader.Load(path);
    if (!result.IsOk()) {
        std::fprintf(stderr, "failed to parse %s: code=0x%" PRIX32 "\n", path, result.code);
        return 1;
    }

    const pnx::NROImage& img = result.value;
    std::printf("File: %s\n", img.source_path.c_str());
    std::printf("Container: %s\n", ContainerName(img.container));
    std::printf("Valid: %d\n", img.valid ? 1 : 0);
    std::printf("Version: 0x%" PRIX32 "\n", img.version);
    std::printf("Flags: 0x%" PRIX32 "\n", img.flags);
    std::printf("Entry offset: 0x%" PRIX64 "\n", img.entry_offset);
    std::printf("BSS size: 0x%" PRIX64 "\n", img.bss_size);
    std::printf("Build ID: ");
    PrintBuildId(img.build_id);
    if (!img.module_name.empty()) {
        std::printf("Module: %s\n", img.module_name.c_str());
    }

    PrintSegment("text", img.text);
    PrintSegment("rodata", img.rodata);
    PrintSegment("data", img.data);

    std::printf("MOD0: present=%d file_off=0x%" PRIx64
                " dynamic=0x%x bss_start=0x%x bss_end=0x%x eh_start=0x%x eh_end=0x%x\n",
                img.mod.present ? 1 : 0,
                img.mod.file_offset,
                img.mod.dynamic_offset,
                img.mod.bss_start_offset,
                img.mod.bss_end_offset,
                img.mod.eh_frame_hdr_start,
                img.mod.eh_frame_hdr_end);

    return img.valid ? 0 : 2;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <nro|nso|nxo>\n", argv[0]);
        return 1;
    }

    pnx::Log::SetLevel(pnx::LogLevel::Info);
    return Run(argv[1]);
}
