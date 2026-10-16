#include "RealtimeTiming.h"
#include <thread>
#include <iostream>
int main() {
    int failures = 0;
    auto check = [&](bool ok) { if (!ok) ++failures; };
    RealtimeTiming timing;
    check(timing.snapshot().p99Ms() == 0);
    for (int i=0;i<99;++i) timing.record(1000000,4000000);
    timing.record(5000000,4000000);
    auto s=timing.snapshot();
    check(s.count==100 && s.p99Ms()==1.0 && s.maxNs==5000000);
    check(s.overQuarter==1 && s.overBudget==1);
    timing.record(1000001,4000000);
    s=timing.snapshot();
    check(s.p99Ms()==2.0 && s.overQuarter==2 && s.overBudget==1);
    RealtimeTiming concurrent;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (int i=0;i<100000;++i) concurrent.record(4000001,4000000);
        done.store(true);
    });
    while(!done.load()) {
        auto current=concurrent.snapshot();
        check(current.count<=100000 && current.overBudget<=100000);
    }
    writer.join();
    s=concurrent.snapshot();
    check(s.count==100000 && s.overQuarter==100000 && s.overBudget==100000);
    check(s.maxNs==4000001 && s.p99Ms()==4.000001);
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
