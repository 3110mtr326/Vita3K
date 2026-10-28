// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <memory>

struct Semaphore;
struct SemaphoreWaitControl;

// One pending diagnostic per run_loop frame, never shared with nested callbacks.
// A CPU run returning without a fresh SVC is NOT proof of guest progress.
class SemaphoreGuestProgress {
    friend class SemaphoreImportScope;
    const void *kernel = nullptr;
    int32_t thread = 0;
    std::shared_ptr<Semaphore> semaphore;
    std::shared_ptr<SemaphoreWaitControl> control;
public:
    SemaphoreGuestProgress() = default;
    SemaphoreGuestProgress(const SemaphoreGuestProgress &) = delete;
    SemaphoreGuestProgress &operator=(const SemaphoreGuestProgress &) = delete;
    void reset() { control.reset(); semaphore.reset(); kernel = nullptr; thread = 0; }
    // Called only after run/step reports a NEW SVC in this same run_loop frame,
    // before its import is dispatched. Error/abort/exit paths discard evidence.
    bool observe_next_svc(const void *owner, int32_t thread_id, uint32_t pc, uint32_t nid);
};

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
    bool finish(uint32_t cpu_result, SemaphoreGuestProgress *progress = nullptr);
};
