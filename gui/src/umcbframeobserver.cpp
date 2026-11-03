// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "umcbframeobserver.h"

#include <QDebug>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
constexpr int kW = 320;
constexpr int kH = 180;
constexpr int64_t kSampleIntervalUs = 500000;  // At most 2 fps.
constexpr int64_t kReportIntervalUs = 10000000; // Aggregate logs every 10 s.
using GrayFrame = std::array<unsigned char, kW * kH>;

bool formatSupported(int fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_GRAY8:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_BGRA:
    case AV_PIX_FMT_RGBA:
    case AV_PIX_FMT_BGR24:
    case AV_PIX_FMT_RGB24:
        return true;
    default:
        return false;
    }
}
}

int64_t UMCBFrameObserver::monotonicUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

UMCBFrameObserver::UMCBFrameObserver()
    : worker_(&UMCBFrameObserver::workerLoop, this)
{
    qInfo() << "[UMCB-M1.1] Observer enabled (2 fps, 320x180 grayscale, RAM only)";
}

UMCBFrameObserver::~UMCBFrameObserver()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (pending_)
            av_frame_free(&pending_);
    }
    wake_.notify_one();
    if (worker_.joinable())
        worker_.join();
    qInfo() << "[UMCB-M1.1] Observer stopped";
}

void UMCBFrameObserver::clearPending()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++generation_;
        if (pending_)
            av_frame_free(&pending_);
        pendingAtUs_ = 0;
    }
    nextSampleUs_.store(0, std::memory_order_relaxed);
}

void UMCBFrameObserver::sample(const AVFrame *frame)
{
    const int64_t now_us = monotonicUs();
    const int64_t next = nextSampleUs_.load(std::memory_order_relaxed);
    if (now_us < next)
        return;
    // This callback is owned by Chiaki's single frame-delivery thread.
    nextSampleUs_.store(now_us + kSampleIntervalUs, std::memory_order_relaxed);

    if (!frame || frame->hw_frames_ctx || !formatSupported(frame->format) ||
        !frame->data[0] || !frame->buf[0] || frame->width <= 0 || frame->height <= 0 ||
        frame->width > 8192 || frame->height > 8192 ||
        frame->linesize[0] == 0) {
        skippedUnsupported_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        skippedLock_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (stopping_ || workerBusy_ || pending_) {
        skippedBusy_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    AVFrame *clone = av_frame_clone(frame); // Reference, not full-resolution pixel copy.
    if (!clone) {
        cloneFailures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    pending_ = clone;
    pendingAtUs_ = now_us;
    lock.unlock();
    wake_.notify_one();
}

bool UMCBFrameObserver::downsampleGray(const AVFrame *frame, unsigned char *out)
{
    if (!frame || !out || !frame->data[0] || !formatSupported(frame->format))
        return false;
    const int fmt = frame->format;
    const bool is10 = fmt == AV_PIX_FMT_P010LE || fmt == AV_PIX_FMT_YUV420P10LE;
    const bool rgba = fmt == AV_PIX_FMT_RGBA || fmt == AV_PIX_FMT_BGRA;
    const bool rgb = fmt == AV_PIX_FMT_RGB24 || fmt == AV_PIX_FMT_BGR24;
    const int pixelBytes = is10 ? 2 : rgba ? 4 : rgb ? 3 : 1;
    const int64_t absStride = frame->linesize[0] < 0
        ? -static_cast<int64_t>(frame->linesize[0]) : frame->linesize[0];
    if (frame->width <= 0 || frame->height <= 0 || frame->width > 8192 ||
        frame->height > 8192 || absStride < static_cast<int64_t>(frame->width) * pixelBytes)
        return false;

    for (int y = 0; y < kH; ++y) {
        const int srcY = std::min(frame->height - 1,
                                   static_cast<int>((static_cast<int64_t>(2 * y + 1) * frame->height) / (2 * kH)));
        const unsigned char *row = frame->data[0] +
            static_cast<std::ptrdiff_t>(srcY) * frame->linesize[0];
        for (int x = 0; x < kW; ++x) {
            const int srcX = std::min(frame->width - 1,
                                       static_cast<int>((static_cast<int64_t>(2 * x + 1) * frame->width) / (2 * kW)));
            unsigned char value = 0;
            if (is10) {
                uint16_t v;
                std::memcpy(&v, row + static_cast<std::ptrdiff_t>(srcX) * 2, sizeof(v));
                value = static_cast<unsigned char>(fmt == AV_PIX_FMT_P010LE ? v >> 8 : v >> 2);
            } else if (rgba || rgb) {
                const unsigned char *pixel = row + static_cast<std::ptrdiff_t>(srcX) * pixelBytes;
                const bool bg = fmt == AV_PIX_FMT_BGRA || fmt == AV_PIX_FMT_BGR24;
                const int r = pixel[bg ? 2 : 0];
                const int g = pixel[1];
                const int b = pixel[bg ? 0 : 2];
                value = static_cast<unsigned char>((77 * r + 150 * g + 29 * b) >> 8);
            } else {
                value = row[srcX];
            }
            out[y * kW + x] = value;
        }
    }
    return true;
}

void UMCBFrameObserver::workerLoop()
{
    GrayFrame current{};
    GrayFrame previous{};
    bool havePrevious = false;
    uint64_t priorGeneration = 0;
    uint64_t processed = 0;
    uint64_t changed = 0;
    uint64_t invalid = 0;
    uint64_t sumProcessUs = 0;
    int64_t maxProcessUs = 0;
    int64_t maxEndToEndUs = 0;
    int64_t lastReportUs = monotonicUs();

    for (;;) {
        AVFrame *frame = nullptr;
        int64_t enqueuedAtUs = 0;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(1), [this] {
                return stopping_ || pending_ != nullptr;
            });
            if (stopping_)
                break;
            if (pending_) {
                frame = pending_;
                pending_ = nullptr;
                enqueuedAtUs = pendingAtUs_;
                pendingAtUs_ = 0;
                generation = generation_;
                workerBusy_ = true;
            }
        }
        if (frame) {
            if (generation != priorGeneration) {
                havePrevious = false;
                priorGeneration = generation;
            }
            const int64_t workStartUs = monotonicUs();
            const bool ok = downsampleGray(frame, current.data());
            av_frame_free(&frame);
            const int64_t workEndUs = monotonicUs();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                workerBusy_ = false;
                if (generation != generation_)
                    havePrevious = false;
                else if (ok) {
                    if (havePrevious) {
                        int different = 0;
                        for (size_t i = 0; i < current.size(); ++i) {
                            if (std::abs(int(current[i]) - int(previous[i])) >= 12)
                                ++different;
                        }
                        if (different > static_cast<int>(current.size() / 50))
                            ++changed; // >2% of sampled pixels have meaningful luma difference.
                    }
                    previous = current;
                    havePrevious = true;
                    ++processed;
                    const int64_t workUs = workEndUs - workStartUs;
                    sumProcessUs += static_cast<uint64_t>(std::max<int64_t>(0, workUs));
                    maxProcessUs = std::max(maxProcessUs, workUs);
                    maxEndToEndUs = std::max(maxEndToEndUs, workEndUs - enqueuedAtUs);
                } else {
                    ++invalid;
                }
            }
        }
        const int64_t nowUs = monotonicUs();
        if (nowUs - lastReportUs >= kReportIntervalUs) {
            const auto busy = skippedBusy_.exchange(0, std::memory_order_relaxed);
            const auto unsupported = skippedUnsupported_.exchange(0, std::memory_order_relaxed);
            const auto contention = skippedLock_.exchange(0, std::memory_order_relaxed);
            const auto cloneFailed = cloneFailures_.exchange(0, std::memory_order_relaxed);
            if (processed || invalid || busy || unsupported || contention || cloneFailed) {
                qInfo().noquote() << "[UMCB-M1.1] window_10s processed=" << processed
                    << "changed=" << changed << "invalid=" << invalid
                    << "busy=" << busy << "unsupported=" << unsupported
                    << "lock_miss=" << contention << "clone_failed=" << cloneFailed
                    << "avg_work_us=" << (processed ? sumProcessUs / processed : 0)
                    << "max_work_us=" << maxProcessUs
                    << "max_latency_us=" << maxEndToEndUs;
            }
            processed = changed = invalid = sumProcessUs = 0;
            maxProcessUs = maxEndToEndUs = 0;
            lastReportUs = nowUs;
        }
    }
}
