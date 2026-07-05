#include "async/EncodeJobQueue.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace D3DVideoEncoderLib {

void EncodeJobQueue::initialize(uint32_t capacity, EncoderQueueFullPolicy policy) {
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = std::max<uint32_t>(capacity, 1u);
    policy_ = policy;
    closed_ = false;
    activeJobs_ = 0;
    queue_.clear();
}

bool EncodeJobQueue::push(EncodeJob job) {
    return push(std::move(job), nullptr);
}

bool EncodeJobQueue::push(EncodeJob job, EncodeJob* droppedJob) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        throw std::runtime_error("EncodeJobQueue::push called after close.");
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

bool EncodeJobQueue::pop(EncodeJob& outJob) {
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

void EncodeJobQueue::jobDone() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeJobs_ > 0) --activeJobs_;
    if (queue_.empty() && activeJobs_ == 0) {
        cvDrained_.notify_all();
    }
}

void EncodeJobQueue::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cvNotEmpty_.notify_all();
    cvNotFull_.notify_all();
    cvDrained_.notify_all();
}

std::vector<EncodeJob> EncodeJobQueue::cancelPending() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;

    std::vector<EncodeJob> pending;
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

void EncodeJobQueue::waitDrained() {
    std::unique_lock<std::mutex> lock(mutex_);
    cvDrained_.wait(lock, [&]() { return queue_.empty() && activeJobs_ == 0; });
}

bool EncodeJobQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && activeJobs_ == 0;
}

} // namespace D3DVideoEncoderLib
