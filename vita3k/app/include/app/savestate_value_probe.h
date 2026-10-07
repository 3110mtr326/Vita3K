// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include <functional>
#include <array>
#include <cstring>
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
    template<class T>struct ObjectBytes final:Field {
        T &target;
        std::array<unsigned char,sizeof(T)> before{},after{};
        ObjectBytes(T &t,const void *saved):target(t){
            std::memcpy(before.data(),&target,sizeof(T));
            std::memcpy(after.data(),saved,sizeof(T));
        }
        void apply()noexcept override{std::memcpy(&target,after.data(),sizeof(T));}
        void undo()noexcept override{std::memcpy(&target,before.data(),sizeof(T));}
        bool matches(bool saved)const noexcept override{
            return std::memcmp(&target,saved?after.data():before.data(),sizeof(T))==0;
        }
    };
    template<class T>struct VectorBytes final:Field {
        std::vector<T>&target;
        std::vector<T> before,after,exchange;
        const T*original_data;size_t original_capacity;
        VectorBytes(std::vector<T>&t,const std::vector<T>&saved):target(t),before(t),after(saved),exchange(saved),original_data(t.data()),original_capacity(t.capacity()){}
        void apply()noexcept override{target.swap(exchange);}
        void undo()noexcept override{target.swap(exchange);}
        bool matches(bool saved)const noexcept override{
            const auto&expected=saved?after:before;
            return target.size()==expected.size()
                && (saved || (target.data()==original_data && target.capacity()==original_capacity))
                && (target.empty() || std::memcmp(target.data(),expected.data(),target.size()*sizeof(T))==0);
        }
    };
    template<class T>struct AccessBytes final:Field {
        std::function<bool(T&)> read;
        std::function<bool(const T&)> write;
        T before{},after{};
        bool applied_ok=false,restored_ok=false;
        AccessBytes(const void*saved,std::function<bool(T&)>r,std::function<bool(const T&)>w):read(std::move(r)),write(std::move(w)){
            std::memcpy(&after,saved,sizeof(T));
            if(!read(before))throw std::runtime_error("Snapshot runtime history backup failed");
        }
        void apply()noexcept override{try{applied_ok=write(after);}catch(...){applied_ok=false;}}
        void undo()noexcept override{try{restored_ok=write(before);}catch(...){restored_ok=false;}}
        bool matches(bool saved)const noexcept override{
            if(!(saved?applied_ok:restored_ok))return false;
            try{T current{};return read(current)&&std::memcmp(&current,saved?&after:&before,sizeof(T))==0;}catch(...){return false;}
        }
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
    // Caller supplies a full object representation and non-overlapping targets.
    // Intended only for pointer-free, trivially-copyable codec history records.
    template<class T>void stage_object_bytes(T &target,const void *saved) {
        static_assert(std::is_trivially_copyable_v<T>);
        if(!saved || applied || finished || !addresses.insert(&target).second)
            throw std::invalid_argument("Duplicate or closed snapshot object");
        fields.push_back(std::make_unique<ObjectBytes<T>>(target,saved));
    }
    template<class T>void stage_vector_bytes(std::vector<T>&target,const std::vector<T>&saved){
        static_assert(std::is_trivially_copyable_v<T> && !std::is_same_v<T,bool>);
        if(applied||finished||!addresses.insert(&target).second)
            throw std::invalid_argument("Duplicate or closed snapshot vector");
        fields.push_back(std::make_unique<VectorBytes<T>>(target,saved));
    }
    template<class T>void stage_access_bytes(const void*identity,const void*saved,
        std::function<bool(T&)>read,std::function<bool(const T&)>write){
        static_assert(std::is_trivially_copyable_v<T>);
        if(!identity||!saved||applied||finished||!addresses.insert(identity).second)
            throw std::invalid_argument("Duplicate or closed snapshot runtime");
        fields.push_back(std::make_unique<AccessBytes<T>>(saved,std::move(read),std::move(write)));
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
