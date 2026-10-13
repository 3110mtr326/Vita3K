// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace app {
// Registry membership only; queue order, stack pointers, cancellation flags
// and deadlines are deliberately not serialized or reconstructed here.
struct SnapshotWaitTarget {
    uint32_t kind = 0; // 1 sema, 2 mutex, 3 lwmutex, 4 eventflag,
                       // 5 condvar, 6 lwcondvar, 7 simple event, 8 thread end
    int32_t uid = 0;
    bool operator==(const SnapshotWaitTarget &) const = default;
};
struct SnapshotWaitTargets {
    uint32_t count = 0;
    std::array<SnapshotWaitTarget,16> targets{};
    bool operator==(const SnapshotWaitTargets &) const = default;
};
static_assert(sizeof(SnapshotWaitTargets) == 132);
inline bool valid_snapshot_wait_targets(const SnapshotWaitTargets &v) {
    if (v.count > v.targets.size()) return false;
    for (size_t i=0;i<v.targets.size();++i) {
        const auto &t=v.targets[i];
        if (i>=v.count) {if (t.kind || t.uid) return false;continue;}
        if (!t.kind || t.kind>8 || t.uid<=0) return false;
        if (i && (v.targets[i-1].kind>t.kind
            || (v.targets[i-1].kind==t.kind && v.targets[i-1].uid>=t.uid))) return false;
    }
    return true;
}
// Caller continuously owns kernel, primitive and thread locks. Unknown wait
// types may have no entries; an empty result never proves no host wait exists.
template<class Kernel,class ThreadPtr>
std::optional<SnapshotWaitTargets> collect_snapshot_wait_targets(Kernel &kernel,const ThreadPtr &thread) {
    SnapshotWaitTargets result;
    const auto add=[&](uint32_t kind,int32_t uid) {
        if (result.count==result.targets.size()) return false;
        result.targets[result.count++]={kind,uid};return true;
    };
    const auto scan=[&](auto &objects,uint32_t kind) {
        for (const auto &[uid,obj]:objects) {
            if (!obj) return false;
            if (obj->waiting_threads && obj->waiting_threads->find(thread)!=obj->waiting_threads->end())
                if (!add(kind,uid)) return false;
        }
        return true;
    };
    if (!thread || !scan(kernel.semaphores,1) || !scan(kernel.mutexes,2)
        || !scan(kernel.lwmutexes,3) || !scan(kernel.eventflags,4)
        || !scan(kernel.condvars,5) || !scan(kernel.lwcondvars,6)
        || !scan(kernel.simple_events,7)) return std::nullopt;
    for (const auto &[uid,target]:kernel.threads) {
        if (!target) return std::nullopt;
        if (std::find(target->waiting_threads.begin(),target->waiting_threads.end(),thread)!=target->waiting_threads.end())
            if (!add(8,uid)) return std::nullopt;
    }
    std::sort(result.targets.begin(),result.targets.begin()+result.count,[](const auto &a,const auto &b) {
        return a.kind<b.kind || (a.kind==b.kind && a.uid<b.uid);
    });
    if (!valid_snapshot_wait_targets(result)) return std::nullopt;
    return result;
}
} // namespace app
