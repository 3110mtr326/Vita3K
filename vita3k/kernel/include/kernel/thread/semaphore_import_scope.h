// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>

struct Semaphore;
struct SemaphoreWaitControl;

// Host-thread-local, stack-scoped evidence for one synchronous call_import.
// Nested imports mask their parents, including unsupported/CB imports.
// Destruction never acknowledges completion (in particular during unwinding).
class SemaphoreImportScope {
    inline static thread_local SemaphoreImportScope *current = nullptr;
    SemaphoreImportScope *previous;
    const void *kernel;
    int32_t thread;
    uint32_t nid;
    bool poisoned = false, finished = false;
    std::shared_ptr<Semaphore> semaphore;
    std::shared_ptr<SemaphoreWaitControl> control;
public:
    SemaphoreImportScope(const void *kernel, int32_t thread, uint32_t nid)
        : previous(current), kernel(kernel), thread(thread), nid(nid) { current = this; }
    ~SemaphoreImportScope() { current = previous; }
    SemaphoreImportScope(const SemaphoreImportScope &) = delete;
    SemaphoreImportScope &operator=(const SemaphoreImportScope &) = delete;
    static bool retain(const void *kernel, int32_t thread,
        const std::shared_ptr<Semaphore> &semaphore,
        const std::shared_ptr<SemaphoreWaitControl> &control) {
        auto *scope = current;
        if (!scope || scope->finished || scope->kernel != kernel || scope->thread != thread
            || (scope->nid != 0x0C7B834B && scope->nid != 0x45389B6B)
            || !semaphore || !control) return false;
        // More than one queued call in a single import is ambiguous.
        if (scope->control) { scope->poisoned = true; return false; }
        scope->semaphore = semaphore;
        scope->control = control;
        return true;
    }
    // Called explicitly after call_import and exclusive-state cleanup return.
    // Does not certify guest instruction resumption or saved-wait replay.
    bool finish(uint32_t cpu_result);
};
