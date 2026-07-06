#include "async/D3D12EncodeJobQueue.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace D3DVideoEncoderLib {

void D3D12EncodeJobQueue::initialize(uint32_t capacity, EncoderQueueFullPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = std::max<uint32_t>(capacity, 1u);
    policy_ = policy;
    closed_ = false;
    activeJobs_ = 0;
    queue_.clear();
}

bool D3D12EncodeJobQueue::push(D3D12EncodeJob job) {
    return push(std::move(job), nullptr);
}

bool D3D12EncodeJobQueue::push(D3D12EncodeJob job, D3D12EncodeJob* droppedJob) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::runtime_error("D3D12EncodeJobQueue::push called after close.");
    }

    if (queue_.size() >= capacity_) {
        switch (policy_) {
        case EncoderQueueFullPolicy::Block:
            cvNotFull_.wait(lock, [&]() { return closed_ || queue_.size() < capacity_; });
            if (closed_) return false;
            break;
        case EncoderQueueFullPolicy::DropNewest:
            return false;
        case EncoderQueueFullPolicy::DropOldest:
            if (!queue_.empty()) {
                if (droppedJob) {
                    *droppedJob = std::move(queue_.front());
                }
                queue_.pop_front();
            }
            break;
        }
    }

    queue_.push_back(std::move(job));
    cvNotEmpty_.notify_one();
    return true;
}

bool D3D12EncodeJobQueue::pop(D3D12EncodeJob& outJob) {
    std::unique_lock<std::mutex> lock(mutex_);
    cvNotEmpty_.wait(lock, [&]() { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
        return false;
    }

    outJob = std::move(queue_.front());
    queue_.pop_front();
    ++activeJobs_;
    cvNotFull_.notify_one();
    return true;
}

void D3D12EncodeJobQueue::jobDone() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeJobs_ > 0) --activeJobs_;
    if (queue_.empty() && activeJobs_ == 0) {
        cvDrained_.notify_all();
    }
}

void D3D12EncodeJobQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cvNotEmpty_.notify_all();
    cvNotFull_.notify_all();
    cvDrained_.notify_all();
}

std::vector<D3D12EncodeJob> D3D12EncodeJobQueue::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;

    std::vector<D3D12EncodeJob> pending;
    pending.reserve(queue_.size());
    while (!queue_.empty()) {
        pending.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }

    cvNotEmpty_.notify_all();
    cvNotFull_.notify_all();
    if (activeJobs_ == 0) {
        cvDrained_.notify_all();
    }
    return pending;
}

void D3D12EncodeJobQueue::waitDrained() {
    std::unique_lock<std::mutex> lock(mutex_);
    cvDrained_.wait(lock, [&]() { return queue_.empty() && activeJobs_ == 0; });
}

bool D3D12EncodeJobQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && activeJobs_ == 0;
}

} // namespace D3DVideoEncoderLib
