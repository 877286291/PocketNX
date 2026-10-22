#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "common/Result.h"
#include "cpu/BlockCache.h"

namespace pnx {

// Guest AArch64 register file. Switch cores are Cortex-A57 (AArch64).
// We model the architectural state the translator and syscall handler need.
struct RegFile {
    uint64_t x[31]{};   // general purpose (x30 = LR)
    uint64_t sp = 0;
    uint64_t pc = 0;
    uint64_t pstate = 0; // NZCV + misc flags we care about
    uint64_t fpsr = 0;
    uint64_t fpcr = 0;
    double d[32]{};     // SIMD/FP view (low 64 bits of V0..V31)
};

// Models the guest (Switch) address space. The Switch user mode sees a
// 32-bit-like space; we map it onto host virtual memory on demand.
class MemoryManager {
public:
    // Reserve the guest view. `size` in bytes.
    void Initialize(size_t size);
    void Shutdown();

    // Map a guest physical/guest-VA region backed by host memory.
    void* MapRegion(uint64_t guest_addr, size_t size, int prot);
    void  UnmapRegion(uint64_t guest_addr, size_t size);

    // Raw host pointer for a guest address (nullptr if not mapped).
    void* GuestToHost(uint64_t guest_addr);

    uint8_t  Read8(uint64_t a);
    uint16_t Read16(uint64_t a);
    uint32_t Read32(uint64_t a);
    uint64_t Read64(uint64_t a);
    void Write8(uint64_t a, uint8_t v);
    void Write16(uint64_t a, uint16_t v);
    void Write32(uint64_t a, uint32_t v);
    void Write64(uint64_t a, uint64_t v);

private:
    void* base_ = nullptr;
    size_t size_ = 0;
};

// The CPU: owns the register file, memory, translation cache and the main
// execution loop. M0 provides the structure; the actual DBT emit is a stub.
class CPU {
public:
    struct Config {
        uint32_t core_count = 4;      // Switch has 4 application cores
        uint64_t entry_point = 0;
        size_t   memory_size = 4ULL * 1024 * 1024 * 1024; // 4GB guest view
    };

    explicit CPU(Config cfg = {});
    ~CPU();

    void SetMemory(MemoryManager* mem) { mem_ = mem; }
    MemoryManager* Memory() const { return mem_; }
    RegFile& Regs() { return regs_; }

    // Begin execution from the current PC until `running_` is cleared.
    // M0: a single-step / stub loop that demonstrates the flow without emitting code.
    void Run();
    void Stop() { running_.store(false, std::memory_order_release); }
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }

    // Translate one guest block starting at `pc`. M0: stub returning empty block.
    TranslatedBlock TranslateBlock(uint64_t pc);

    // SVC / system-instruction dispatch. Routed here from the translator.
    ResultCode DispatchSVC(uint32_t svc_num);

    uint32_t CoreCount() const { return cfg_.core_count; }

    // Number of translated blocks currently cached (diagnostics / PPTC tuning).
    size_t TranslatedBlockCount() const { return cache_.Size(); }

private:
    Config cfg_;
    RegFile regs_;
    MemoryManager* mem_ = nullptr;
    BlockCache cache_;
    std::atomic<bool> running_{false};
};

} // namespace pnx
