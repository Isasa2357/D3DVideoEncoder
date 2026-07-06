#pragma once

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace D3DVideoEncoderLib {

struct D3D12EncodeJob {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
};

class D3D12EncodeJobQueue {
public:
    void initialize(uint32_t capacity, EncoderQueueFullPolicy policy);

    // Returns false when DropNewest rejected the new job.
    bool push(D3D12EncodeJob job);

    // When policy is DropOldest and the queue is full, the dropped job is moved
    // into droppedJob so any ComPtr-held resource is released immediately by the caller.
    // Returns false when DropNewest rejected the new job.
    bool push(D3D12EncodeJob job, D3D12EncodeJob* droppedJob);

    // Returns false when closed and drained. Each successful pop must be paired
    // with jobDone() after the worker has finished processing the job.
    bool pop(D3D12EncodeJob& outJob);
    void jobDone();

    void close();

    // Close the queue and return all jobs that have not yet been popped by the worker.
    std::vector<D3D12EncodeJob> cancelPending();

    void waitDrained();
    bool empty() const;

private:
    uint32_t capacity_ = 4;
    EncoderQueueFullPolicy policy_ = EncoderQueueFullPolicy::Block;
    bool closed_ = false;
    uint32_t activeJobs_ = 0;
    std::deque<D3D12EncodeJob> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::condition_variable cvDrained_;
};

} // namespace D3DVideoEncoderLib
