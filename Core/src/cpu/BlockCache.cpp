#include "cpu/BlockCache.h"

#include "common/Log.h"

namespace pnx {

TranslatedBlock* BlockCache::Find(uint64_t guest_pc) {
    auto& s = shard(guest_pc);
    std::lock_guard lock(s.mu);
    auto it = s.blocks.find(guest_pc);
    return it == s.blocks.end() ? nullptr : &it->second;
}

void BlockCache::Insert(TranslatedBlock block) {
    auto& s = shard(block.guest_pc);
    std::lock_guard lock(s.mu);
    s.blocks[block.guest_pc] = std::move(block);
}

TranslatedBlock* BlockCache::GetOrTranslate(uint64_t guest_pc,
                                            std::function<TranslatedBlock()> translate) {
    if (auto* existing = Find(guest_pc)) {
        return existing;
    }
    // M0: the real DBT emitter lives in CPU.cpp. The callback is honored so the
    // flow is exercisable, but no host code is emitted yet.
    TranslatedBlock block;
    block.guest_pc = guest_pc;
    if (translate) {
        block = translate();
        if (block.guest_pc == 0) {
            block.guest_pc = guest_pc;
        }
    }
    Insert(block);
    return Find(guest_pc);
}

void BlockCache::Clear() {
    for (auto& s : shards_) {
        std::lock_guard lock(s.mu);
        s.blocks.clear();
    }
}

size_t BlockCache::Size() const {
    size_t total = 0;
    for (auto& s : shards_) {
        std::lock_guard lock(const_cast<std::recursive_mutex&>(s.mu));
        total += s.blocks.size();
    }
    return total;
}

} // namespace pnx
