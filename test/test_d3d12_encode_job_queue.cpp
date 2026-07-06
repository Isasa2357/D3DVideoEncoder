#include "async/D3D12EncodeJobQueue.hpp"

#include <iostream>
#include <stdexcept>

using namespace D3DVideoEncoderLib;

namespace {
void expect(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}
}

int main() {
    try {
        D3D12EncodeJobQueue queue;
        queue.initialize(2, EncoderQueueFullPolicy::DropOldest);

        D3D12EncodeJob a;
        a.timestamp100ns = 100;
        D3D12EncodeJob b;
        b.timestamp100ns = 200;
        D3D12EncodeJob c;
        c.timestamp100ns = 300;
        D3D12EncodeJob dropped;

        expect(queue.push(std::move(a)), "push a failed");
        expect(queue.push(std::move(b)), "push b failed");
        expect(queue.push(std::move(c), &dropped), "push c failed");
        expect(dropped.timestamp100ns == 100, "DropOldest did not return the oldest D3D12 job");

        D3D12EncodeJob out;
        expect(queue.pop(out), "pop b failed");
        expect(out.timestamp100ns == 200, "first remaining D3D12 job should be b");
        queue.jobDone();
        expect(queue.pop(out), "pop c failed");
        expect(out.timestamp100ns == 300, "second remaining D3D12 job should be c");
        queue.jobDone();
        queue.waitDrained();
        queue.close();
        expect(queue.empty(), "queue should be empty after drained close");

        D3D12EncodeJobQueue dropNewest;
        dropNewest.initialize(1, EncoderQueueFullPolicy::DropNewest);
        D3D12EncodeJob n0;
        n0.timestamp100ns = 1;
        D3D12EncodeJob n1;
        n1.timestamp100ns = 2;
        expect(dropNewest.push(std::move(n0)), "DropNewest first push failed");
        expect(!dropNewest.push(std::move(n1)), "DropNewest should reject the newest job when full");
        dropNewest.close();

        std::cout << "D3D12EncodeJobQueue tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D12EncodeJobQueue test failed: " << e.what() << "\n";
        return 1;
    }
}
