#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>

// One writer, any number of readers. Fixed storage, no locks, allocations or
// retry loops on the producer. Lifetime telemetry, never reset concurrently.
class RealtimeTiming {
public:
    static constexpr std::array<uint64_t, 16> edgesNs{
        10000,25000,50000,100000,200000,500000,1000000,2000000,
        4000000,8000000,16000000,32000000,64000000,128000000,512000000,2000000000};
    static_assert(std::atomic<uint64_t>::is_always_lock_free, "Realtime counters must be lock-free");
    struct Snapshot {
        std::array<uint64_t, edgesNs.size()+1> buckets{};
        uint64_t count=0, maxNs=0, overQuarter=0, overBudget=0;
        double p99Ms() const {
            if (!count) return 0;
            const uint64_t target=count-count/100;
            uint64_t seen=0;
            for(size_t i=0;i<buckets.size();++i){
                seen+=buckets[i];
                if(seen>=target)return double(i<edgesNs.size()?std::min(edgesNs[i],maxNs):maxNs)/1e6;
            }
            return double(maxNs)/1e6;
        }
    };
    static uint64_t nowNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    void record(uint64_t ns, uint64_t budgetNs=0) {
        size_t i=0;while(i<edgesNs.size() && ns>edgesNs[i])++i;
        m_buckets[i].fetch_add(1,std::memory_order_relaxed);
        if(ns>m_max.load(std::memory_order_relaxed))m_max.store(ns,std::memory_order_relaxed);
        if(budgetNs){
            if(ns>budgetNs/4)m_overQuarter.fetch_add(1,std::memory_order_relaxed);
            if(ns>budgetNs)m_overBudget.fetch_add(1,std::memory_order_relaxed);
        }
    }
    Snapshot snapshot() const {
        Snapshot s;
        for(size_t i=0;i<s.buckets.size();++i)s.count+=s.buckets[i]=m_buckets[i].load(std::memory_order_relaxed);
        s.maxNs=m_max.load(std::memory_order_relaxed);
        s.overQuarter=m_overQuarter.load(std::memory_order_relaxed);
        s.overBudget=m_overBudget.load(std::memory_order_relaxed);
        return s;
    }
private:
    std::array<std::atomic<uint64_t>,edgesNs.size()+1> m_buckets{};
    std::atomic<uint64_t> m_max{0},m_overQuarter{0},m_overBudget{0};
};
