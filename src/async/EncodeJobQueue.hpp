#pragma once

#include "surface/EncodeSurface.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace D3DVideoEncoderLib {

struct EncodeJob {
    EncodeSurface surface;
    int64_t timestamp100ns = 0;
    int64_t duration100ns = 0;
};

class EncodeJobQueue {
public:
    void initialize(uint32_t capacity, EncoderQueueFullPolicy policy);

    // Returns false when DropNewest rejected the new job.
    bool push(EncodeJob job);

    // When policy is DropOldest and the queue is full, the dropped job is moved
    // into droppedJob so the caller can release any surface owned by that job.
    // Returns false when DropNewest rejected the new job.
    bool push(EncodeJob job, EncodeJob* droppedJob);

    // Returns false when closed and drained. Each successful pop must be paired
    // with jobDone() after the worker has finished processing the job.
    bool pop(EncodeJob& outJob);
    void jobDone();

    void close();

    // Close the queue and return all jobs that have not yet been popped by the worker.
    // The caller owns and must release the surfaces carried by the returned jobs.
    std::vector<EncodeJob> cancelPending();

    void waitDrained();
    bool empty() const;

private:
    uint32_t capacity_ = 4;
    EncoderQueueFullPolicy policy_ = EncoderQueueFullPolicy::Block;
    bool closed_ = false;
    uint32_t activeJobs_ = 0;
    std::deque<EncodeJob> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cvNotEmpty_;
    std::condition_variable cvNotFull_;
    std::condition_variable cvDrained_;
};

} // namespace D3DVideoEncoderLib
