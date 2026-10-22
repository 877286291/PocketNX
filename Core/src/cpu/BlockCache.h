#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace pnx {

// One translated basic block.
struct TranslatedBlock {
    uint64_t guest_pc = 0;        // guest address the block starts at
    void*    host_entry = nullptr; // pointer into the executable translation buffer
    size_t   host_size = 0;        // bytes of generated host code
    uint32_t hit_count = 0;        // used by PPTC for hot-block profiling
};

// Caches translated blocks so a guest block is translated once and re-run many
// times. Sharded lock because the translator runs on multiple threads.
class BlockCache {
public:
    // Shards to reduce lock contention during concurrent translation.
    static constexpr int kNumShards = 16;

    // Find a block by guest start PC. Returns nullptr if not yet translated.
    TranslatedBlock* Find(uint64_t guest_pc);

    // Register a freshly translated block.
    void Insert(TranslatedBlock block);

    // Lookup-or-translate helper. `translate` is invoked (with the shard held
    // read-free) only when the block is absent. M0: translation is a stub.
    TranslatedBlock* GetOrTranslate(uint64_t guest_pc,
                                    std::function<TranslatedBlock()> translate);

    void Clear();
    size_t Size() const;

private:
    struct Shard {
        std::unordered_map<uint64_t, TranslatedBlock> blocks;
        std::recursive_mutex mu;
    };
    Shard shards_[kNumShards];

    int ShardIndex(uint64_t pc) const {
        return static_cast<int>((pc * 2654435761u) % kNumShards);
    }
    Shard& shard(uint64_t pc) { return shards_[ShardIndex(pc)]; }
    const Shard& shard(uint64_t pc) const { return shards_[ShardIndex(pc)]; }
};

} // namespace pnx
