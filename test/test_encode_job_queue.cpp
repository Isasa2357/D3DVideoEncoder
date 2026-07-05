#include "async/EncodeJobQueue.hpp"

#include <cstdlib>
#include <iostream>
#include <thread>

using namespace D3DVideoEncoderLib;

namespace {
void require_true(bool cond, const char* message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}
}

int main() {
    EncodeJobQueue queue;
    queue.initialize(2, EncoderQueueFullPolicy::DropNewest);

    EncodeJob a; a.timestamp100ns = 0;
    EncodeJob b; b.timestamp100ns = 1;
    EncodeJob c; c.timestamp100ns = 2;

    require_true(queue.push(a), "push a");
    require_true(queue.push(b), "push b");
    require_true(!queue.push(c), "drop newest when full");

    EncodeJob out;
    require_true(queue.pop(out), "pop a");
    require_true(out.timestamp100ns == 0, "first timestamp");
    queue.jobDone();

    require_true(queue.pop(out), "pop b");
    require_true(out.timestamp100ns == 1, "second timestamp");
    queue.jobDone();
    queue.waitDrained();
    queue.close();
    require_true(!queue.pop(out), "closed pop drains");

    EncodeJobQueue dropOldestQueue;
    dropOldestQueue.initialize(1, EncoderQueueFullPolicy::DropOldest);
    EncodeJob oldJob; oldJob.timestamp100ns = 10; oldJob.surface.poolIndex = 7;
    EncodeJob newJob; newJob.timestamp100ns = 20;
    EncodeJob dropped;
    require_true(dropOldestQueue.push(oldJob, nullptr), "drop oldest push old");
    require_true(dropOldestQueue.push(newJob, &dropped), "drop oldest accepts new");
    require_true(dropped.timestamp100ns == 10, "drop oldest returns dropped job");
    require_true(dropped.surface.poolIndex == 7, "drop oldest preserves dropped surface bookkeeping");
    require_true(dropOldestQueue.pop(out), "drop oldest pop new");
    require_true(out.timestamp100ns == 20, "drop oldest new timestamp");
    dropOldestQueue.jobDone();
    dropOldestQueue.close();

    return 0;
}
