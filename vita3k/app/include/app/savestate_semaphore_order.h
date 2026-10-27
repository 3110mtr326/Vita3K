// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <app/savestate_semaphore_wait.h>
#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace app {
struct SemaphoreOrderStep {
    size_t record_index;
    int32_t thread_id, uid, priority;
    uint32_t position, queue_size, attr;
};
struct SemaphoreOrderPlan {
    std::string error;
    std::vector<SemaphoreOrderStep> steps;
};
// Deterministic registration ordering only. No dispatch/restore permission:
// every step still needs validated entry CPU, host retirement and a registration
// barrier. Different semaphores are sorted by UID; no cross-object causality is
// inferred. Equal-priority ties follow saved queue position, never thread ID.
template<class Records>
SemaphoreOrderPlan prepare_semaphore_order(const Records &records) {
    SemaphoreOrderPlan result;
    auto fail = [&](const char *why) { result.error=why; result.steps.clear(); return result; };
    if (records.size()>4096) return fail("Too many semaphore order records");
    std::set<int32_t> ids;
    for (size_t i=0;i<records.size();++i) {
        const auto &r=records[i];const auto &w=r.semaphore_wait;
        if (!ids.insert(r.id).second || !valid_snapshot_semaphore_wait(w,r.wait_targets))
            return fail("Invalid semaphore order record");
        if (!w.uid) continue;
        if (r.id<=0 || w.uid<=0) return fail("Invalid semaphore order identity");
        result.steps.push_back({i,r.id,w.uid,w.priority,w.position,w.queue_size,w.attr});
    }
    std::sort(result.steps.begin(),result.steps.end(),[](const auto &a,const auto &b) {
        return a.uid!=b.uid?a.uid<b.uid:a.position<b.position;
    });
    for (size_t first=0;first<result.steps.size();) {
        size_t end=first+1;
        while(end<result.steps.size()&&result.steps[end].uid==result.steps[first].uid)++end;
        const auto &head=result.steps[first];
        if (end-first!=head.queue_size) return fail("Incomplete semaphore registration order");
        for(size_t i=first;i<end;++i) {
            const auto &step=result.steps[i];
            if(step.position!=i-first || step.queue_size!=head.queue_size || step.attr!=head.attr)
                return fail("Inconsistent semaphore registration order");
            if((head.attr&0x2000u) && i>first && result.steps[i-1].priority>step.priority)
                return fail("Saved semaphore priority order mismatch");
        }
        first=end;
    }
    return result;
}
} // namespace app
