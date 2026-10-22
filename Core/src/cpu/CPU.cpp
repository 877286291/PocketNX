#include "cpu/CPU.h"

#include <cstring>
#include <cstdlib>

#include "common/Log.h"

namespace pnx {

// ---------------------------------------------------------------------------
// MemoryManager
// ---------------------------------------------------------------------------
void MemoryManager::Initialize(size_t size) {
    size_ = size;
    base_ = std::malloc(size_ ? size_ : 1);
    PNX_LOG_INFO("CPU.Mem", "guest view reserved: %zu bytes @ host %p", size_, base_);
}

void MemoryManager::Shutdown() {
    if (base_) {
        std::free(base_);
        base_ = nullptr;
        size_ = 0;
    }
}

void* MemoryManager::MapRegion(uint64_t guest_addr, size_t size, int /*prot*/) {
    // M0: simple linear offset mapping within the reserved guest view.
    if (!base_ || guest_addr + size > size_) {
        PNX_LOG_ERROR("CPU.Mem", "map region out of bounds: 0x%llx + %zu",
                      (unsigned long long)guest_addr, size);
        return nullptr;
    }
    return static_cast<uint8_t*>(base_) + guest_addr;
}

void MemoryManager::UnmapRegion(uint64_t /*guest_addr*/, size_t /*size*/) {
    // M0: no per-region teardown; the whole view is freed in Shutdown.
}

void* MemoryManager::GuestToHost(uint64_t guest_addr) {
    if (!base_ || guest_addr >= size_) {
        return nullptr;
    }
    return static_cast<uint8_t*>(base_) + guest_addr;
}

uint8_t MemoryManager::Read8(uint64_t a)   { return *static_cast<uint8_t*>(GuestToHost(a)); }
uint16_t MemoryManager::Read16(uint64_t a) { return *static_cast<uint16_t*>(GuestToHost(a)); }
uint32_t MemoryManager::Read32(uint64_t a) { return *static_cast<uint32_t*>(GuestToHost(a)); }
uint64_t MemoryManager::Read64(uint64_t a) { return *static_cast<uint64_t*>(GuestToHost(a)); }
void MemoryManager::Write8(uint64_t a, uint8_t v)   { *static_cast<uint8_t*>(GuestToHost(a)) = v; }
void MemoryManager::Write16(uint64_t a, uint16_t v) { *static_cast<uint16_t*>(GuestToHost(a)) = v; }
void MemoryManager::Write32(uint64_t a, uint32_t v) { *static_cast<uint32_t*>(GuestToHost(a)) = v; }
void MemoryManager::Write64(uint64_t a, uint64_t v) { *static_cast<uint64_t*>(GuestToHost(a)) = v; }

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
CPU::CPU() : CPU(Config()) {}
CPU::CPU(Config cfg) : cfg_(cfg) {}

CPU::~CPU() {
    Stop();
}

void CPU::Run() {
    running_.store(true, std::memory_order_release);
    PNX_LOG_INFO("CPU", "run started, entry=0x%llx cores=%u",
                 (unsigned long long)regs_.pc, cfg_.core_count);

    // M0 stub main loop. The real implementation fetches a guest block,
    // looks it up in the cache (translating on miss), then jumps to
    // `block.host_entry`. Here we just advance PC once to demonstrate the loop
    // shape and stop, so the skeleton is testable without an emitter.
    while (running_.load(std::memory_order_acquire)) {
        uint64_t pc = regs_.pc;
        TranslatedBlock* block = cache_.GetOrTranslate(pc, [this, pc]() {
            return TranslateBlock(pc);
        });
        if (!block || !block->host_entry) {
            PNX_LOG_DEBUG("CPU", "no translation for 0x%llx; halting (M0 stub)",
                          (unsigned long long)pc);
            break;
        }
        // Real path would be: ((void(*)())block->host_entry)(this);
        regs_.pc += 4; // stub step
    }

    PNX_LOG_INFO("CPU", "run stopped, translated blocks=%zu", cache_.Size());
}

TranslatedBlock CPU::TranslateBlock(uint64_t pc) {
    // M0 stub: no host code emission yet. The real DBT pipeline is:
    //   fetch guest block -> decode AArch64 -> emit host ARM64 -> link block.
    PNX_LOG_DEBUG("CPU.DB", "translate requested at 0x%llx (stub, no emit)",
                  (unsigned long long)pc);
    TranslatedBlock b;
    b.guest_pc = pc;
    b.host_entry = nullptr;
    b.host_size = 0;
    return b;
}

ResultCode CPU::DispatchSVC(uint32_t svc_num) {
    // M0 stub: every service call is unimplemented. As OS services land in
    // M1+, this becomes a dispatch table keyed by (module, command id).
    PNX_LOG_WARN("CPU.SVC", "unimplemented SVC 0x%x", svc_num);
    return Unimplemented;
}

} // namespace pnx
