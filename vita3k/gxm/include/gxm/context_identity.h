// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>

struct SceGxmContext;

namespace gxm {

// Kept outside guest RAM: restoring old guest bytes must not roll an identity
// back. IDs are unique across registries/sessions within this host process and key type.
// They are NOT a persistent session UUID and must not validate cross-process
// saves without a separate, future session identity check.
template <typename Key>
class LifetimeIdentityRegistry {
    mutable std::mutex mutex;
    std::map<Key, uint64_t> identities;

    static uint64_t next_identity() {
        static std::atomic<uint64_t> next{ 1 };
        auto value = next.load(std::memory_order_relaxed);
        while (value != std::numeric_limits<uint64_t>::max()) {
            if (next.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
                return value;
        }
        throw std::overflow_error("GXM context identity space exhausted");
    }

public:
    void created(Key context) {
        if (!context)
            throw std::invalid_argument("Null GXM context identity");
        std::lock_guard<std::mutex> lock(mutex);
        identities.insert_or_assign(context, next_identity());
    }
    void destroyed(Key context) {
        std::lock_guard<std::mutex> lock(mutex);
        identities.erase(context);
    }
    uint64_t find(Key context) const {
        std::lock_guard<std::mutex> lock(mutex);
        const auto it = identities.find(context);
        return it == identities.end() ? 0 : it->second;
    }
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return identities.size();
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        identities.clear(); // do not reset the process-wide ID sequence
    }
};

using ContextIdentityRegistry = LifetimeIdentityRegistry<const SceGxmContext *>;
using GuestObjectIdentityRegistry = LifetimeIdentityRegistry<uint32_t>;

} // namespace gxm
