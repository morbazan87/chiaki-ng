// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// UMCB M1.1: opt-in, read-only sampling of CPU-accessible decoded frames.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

struct AVFrame;

class UMCBFrameObserver final
{
public:
    UMCBFrameObserver();
    ~UMCBFrameObserver();
    UMCBFrameObserver(const UMCBFrameObserver &) = delete;
    UMCBFrameObserver &operator=(const UMCBFrameObserver &) = delete;

    // Called only after Chiaki has prepared a decoded frame for presentation.
    // Never consumes ownership of the original frame; never blocks on the worker.
    void sample(const AVFrame *frame);
    // Releases any frame waiting for the worker when a Remote Play session ends.
    void clearPending();

private:
    void workerLoop();
    static int64_t monotonicUs();
    static bool downsampleGray(const AVFrame *frame, unsigned char *out);

    std::atomic<int64_t> nextSampleUs_{0};
    std::atomic<uint64_t> skippedBusy_{0};
    std::atomic<uint64_t> skippedUnsupported_{0};
    std::atomic<uint64_t> skippedLock_{0};
    std::atomic<uint64_t> cloneFailures_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    AVFrame *pending_ = nullptr;
    int64_t pendingAtUs_ = 0;
    uint64_t generation_ = 0;
    bool workerBusy_ = false;
    bool stopping_ = false;
    // Construct the worker only after all state it can read.
    std::thread worker_;
};
