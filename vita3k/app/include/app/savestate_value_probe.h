// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace app {
// Internal stopped-session transaction. Only stage scalar/shared ownership
// values whose assignment/equality cannot throw. Caller keeps object lifetime
// and every relevant lock from staging through destruction. Never signals waits.
class SnapshotValueProbe {
    struct Field {
        virtual ~Field()=default;
        virtual void apply() noexcept=0;
        virtual void undo() noexcept=0;
        virtual bool matches(bool saved) const noexcept=0;
    };
    template<class T>struct Value final:Field {
        T &target;T before,after;
        Value(T &t,const T &v):target(t),before(t),after(v){}
        void apply() noexcept override {target=after;}
        void undo() noexcept override {target=before;}
        bool matches(bool saved)const noexcept override {return target==(saved?after:before);}
    };
    std::vector<std::unique_ptr<Field>> fields;
    std::set<const void*> addresses;
    bool applied=false,finished=false;
    void undo()noexcept {if(applied){for(auto &f:fields)f->undo();applied=false;}}
public:
    enum class Result { Passed, ApplyMismatch, RollbackFailed, AlreadyFinished };
    SnapshotValueProbe()=default;
    SnapshotValueProbe(const SnapshotValueProbe&)=delete;
    SnapshotValueProbe&operator=(const SnapshotValueProbe&)=delete;
    ~SnapshotValueProbe(){undo();}
    template<class T>void stage(T &target,const T &saved) {
        static_assert(std::is_nothrow_copy_assignable_v<T>);
        static_assert(noexcept(target==saved));
        if(applied||finished||!addresses.insert(&target).second)
            throw std::invalid_argument("Duplicate or closed snapshot field");
        fields.push_back(std::make_unique<Value<T>>(target,saved));
    }
    size_t size()const noexcept{return fields.size();}
    template<class During>Result probe_with(During during)noexcept {
        if(finished)return Result::AlreadyFinished;
        finished=true;applied=true;
        for(auto &f:fields)f->apply();
        bool saved_ok=true;for(auto &f:fields)saved_ok=f->matches(true)&&saved_ok;
        if(saved_ok) {
            try {saved_ok=bool(during());}catch(...){saved_ok=false;}
            for(auto &f:fields)saved_ok=f->matches(true)&&saved_ok;
        }
        undo();
        bool original_ok=true;for(auto &f:fields)original_ok=f->matches(false)&&original_ok;
        return !original_ok?Result::RollbackFailed:saved_ok?Result::Passed:Result::ApplyMismatch;
    }
    Result probe()noexcept {return probe_with([]{return true;});}
};
}
