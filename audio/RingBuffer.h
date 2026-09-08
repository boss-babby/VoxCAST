// ============================================================================
//  VoxCast — audio/RingBuffer.h
//  Single-producer / single-consumer lock-free ring buffer. The CoreAudio /
//  WASAPI callback thread is the sole producer; the encoder and the waveform
//  sampler each own their own instance (fan-out is done by the AudioEngine).
//  No allocation, no locks, no syscalls on the real-time thread.
// ============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <algorithm>

namespace vox::audio {

template <typename T>
class SpscRingBuffer {
public:
    explicit SpscRingBuffer(size_t capacityPow2 = 1u << 15)
        : cap_(nextPow2(capacityPow2)), mask_(cap_ - 1), data_(cap_) {}

    /// Producer side. Returns number of items actually written (drops on full).
    size_t write(const T* src, size_t n) {
        const size_t w = write_.load(std::memory_order_relaxed);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t free = cap_ - (w - r) - 1;
        const size_t count = std::min(n, free);
        for (size_t i = 0; i < count; ++i) data_[(w + i) & mask_] = src[i];
        write_.store(w + count, std::memory_order_release);
        overrun_ += (n - count);
        return count;
    }

    /// Consumer side.
    size_t read(T* dst, size_t n) {
        const size_t r = read_.load(std::memory_order_relaxed);
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t avail = w - r;
        const size_t count = std::min(n, avail);
        for (size_t i = 0; i < count; ++i) dst[i] = data_[(r + i) & mask_];
        read_.store(r + count, std::memory_order_release);
        return count;
    }

    /// Non-destructive peek of the most recent `n` items (waveform renderer).
    size_t peekLatest(T* dst, size_t n) const {
        const size_t w = write_.load(std::memory_order_acquire);
        const size_t r = read_.load(std::memory_order_acquire);
        const size_t avail = w - r;
        const size_t count = std::min(n, avail);
        const size_t start = w - count;
        for (size_t i = 0; i < count; ++i) dst[i] = data_[(start + i) & mask_];
        return count;
    }

    size_t available() const {
        return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire);
    }
    size_t capacity() const { return cap_; }
    size_t overruns() const { return overrun_; }
    void   reset() { read_.store(0); write_.store(0); overrun_ = 0; }

private:
    static size_t nextPow2(size_t v) {
        size_t p = 1; while (p < v) p <<= 1; return p;
    }
    const size_t cap_, mask_;
    std::vector<T> data_;
    std::atomic<size_t> read_{0}, write_{0};
    size_t overrun_{0};
};

} // namespace vox::audio
