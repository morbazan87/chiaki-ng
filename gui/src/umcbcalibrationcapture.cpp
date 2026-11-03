// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "umcbcalibrationcapture.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QStandardPaths>
#include <QDebug>

#include <algorithm>
#include <chrono>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

namespace {
constexpr int kMaxImages = 48;
constexpr uint64_t kMaxBytes = 100ULL * 1024ULL * 1024ULL;
constexpr int64_t kBurstIntervalUs = 125000; // Up to 8 images/sec; only user-triggered.
constexpr int64_t kRequestTimeoutUs = 15000000;

bool supportedFormat(int f)
{
    switch (f) {
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

// No external encoder or GPU readback. Preserve input-resolution geometry and
// luminance in an 8-bit grayscale reference PNG (10-bit sources down-converted).
QImage makeGrayImage(const AVFrame *frame)
{
    if (!frame || !supportedFormat(frame->format) || frame->hw_frames_ctx ||
        !frame->data[0] || frame->width < 1 || frame->height < 1 ||
        frame->width > 4096 || frame->height > 4096)
        return {};
    const bool tenBit = frame->format == AV_PIX_FMT_P010LE || frame->format == AV_PIX_FMT_YUV420P10LE;
    const bool four = frame->format == AV_PIX_FMT_RGBA || frame->format == AV_PIX_FMT_BGRA;
    const bool three = frame->format == AV_PIX_FMT_RGB24 || frame->format == AV_PIX_FMT_BGR24;
    const int bytesPerPixel = tenBit ? 2 : (four ? 4 : (three ? 3 : 1));
    const int64_t stride = frame->linesize[0];
    if (stride <= 0 || stride < static_cast<int64_t>(frame->width) * bytesPerPixel)
        return {};

    QImage out(frame->width, frame->height, QImage::Format_Grayscale8);
    if (out.isNull())
        return {};
    for (int y = 0; y < frame->height; ++y) {
        const auto *src = frame->data[0] + static_cast<ptrdiff_t>(y) * stride;
        auto *dst = out.scanLine(y);
        if (!tenBit && !four && !three) {
            std::memcpy(dst, src, static_cast<size_t>(frame->width));
            continue;
        }
        for (int x = 0; x < frame->width; ++x) {
            if (tenBit) {
                uint16_t v;
                std::memcpy(&v, src + static_cast<ptrdiff_t>(x) * 2, sizeof(v));
                dst[x] = static_cast<unsigned char>(frame->format == AV_PIX_FMT_P010LE ? (v >> 8) : (v >> 2));
            } else {
                const auto *p = src + static_cast<ptrdiff_t>(x) * bytesPerPixel;
                const bool bgr = frame->format == AV_PIX_FMT_BGRA || frame->format == AV_PIX_FMT_BGR24;
                const int r = p[bgr ? 2 : 0], g = p[1], b = p[bgr ? 0 : 2];
                dst[x] = static_cast<unsigned char>((77 * r + 150 * g + 29 * b) >> 8);
            }
        }
    }
    return out;
}
}

int64_t UMCBCalibrationCapture::monotonicUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

UMCBCalibrationCapture::UMCBCalibrationCapture()
    : worker_(&UMCBCalibrationCapture::workerLoop, this)
{
    qInfo() << "[UMCB-M1.3] Supervised calibration enabled (no automatic capture)";
}

UMCBCalibrationCapture::~UMCBCalibrationCapture()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        active_.store(false, std::memory_order_release);
        if (pending_)
            av_frame_free(&pending_);
    }
    wake_.notify_one();
    if (worker_.joinable())
        worker_.join();
    qInfo() << "[UMCB-M1.3] Calibration worker stopped";
}

QString UMCBCalibrationCapture::folderPath() const
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/Pictures";
    return QDir(base).filePath("UMCB Calibration");
}

QString UMCBCalibrationCapture::request(const QString &label)
{
    const bool burst = label == "CHAT_CURSOR_ACTIVE";
    if (label != "GAMEPLAY" && label != "CHAT_UNFOCUSED" && !burst && label != "PS4_KEYBOARD")
        return "ERROR invalid_label";
    const int count = burst ? 12 : 1;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_)
        return "ERROR stopping";
    if (active_.load(std::memory_order_acquire))
        return "ERROR busy";
    if (savedImages_ + count > kMaxImages || savedBytes_ >= kMaxBytes)
        return "ERROR session_limit";
    ++generation_;
    label_ = label;
    expected_ = count;
    completed_ = 0;
    queued_ = 0;
    requestStartUs_ = lastProgressUs_ = monotonicUs();
    nextDueUs_.store(0, std::memory_order_release);
    lastResult_ = "queued";
    active_.store(true, std::memory_order_release);
    qInfo().noquote() << "[UMCB-M1.3] User-labelled request" << label << "count=" << count;
    return QString("QUEUED %1 %2").arg(label).arg(count);
}

QString UMCBCalibrationCapture::status()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return QString("STATUS active=%1 queued=%2 done=%3 total_saved=%4 bytes=%5 last=%6 folder=%7")
        .arg(active_.load(std::memory_order_acquire) ? 1 : 0)
        .arg(queued_).arg(completed_).arg(savedImages_)
        .arg(static_cast<qulonglong>(savedBytes_)).arg(lastResult_, folderPath());
}

void UMCBCalibrationCapture::cancelSession()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    active_.store(false, std::memory_order_release);
    nextDueUs_.store(0, std::memory_order_release);
    if (pending_)
        av_frame_free(&pending_);
    lastResult_ = "session_stopped";
    qInfo() << "[UMCB-M1.3] Session stopped; capture request cancelled";
}

void UMCBCalibrationCapture::expireStalled()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_.load(std::memory_order_acquire) || stopping_)
        return;
    const int64_t now = monotonicUs();
    if (now - lastProgressUs_ < kRequestTimeoutUs)
        return;
    ++generation_;
    active_.store(false, std::memory_order_release);
    if (pending_)
        av_frame_free(&pending_);
    lastResult_ = "timeout_no_decodable_frame";
    qWarning() << "[UMCB-M1.3] Capture timed out; no usable decoded frame";
}

void UMCBCalibrationCapture::observe(const AVFrame *frame)
{
    if (!active_.load(std::memory_order_acquire))
        return;
    const int64_t now = monotonicUs();
    if (now < nextDueUs_.load(std::memory_order_relaxed))
        return;
    if (!frame || frame->hw_frames_ctx || !supportedFormat(frame->format) ||
        !frame->buf[0] || !frame->data[0] || frame->width < 1 || frame->height < 1 ||
        frame->width > 4096 || frame->height > 4096 || frame->linesize[0] <= 0) {
        skippedUnsupported_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock() || stopping_ || !active_.load(std::memory_order_acquire) ||
        workerBusy_ || pending_ || queued_ >= expected_)
        return;
    AVFrame *clone = av_frame_clone(frame);
    if (!clone)
        return;
    pending_ = clone;
    pendingLabel_ = label_;
    pendingIndex_ = ++queued_;
    pendingGeneration_ = generation_;
    nextDueUs_.store(now + kBurstIntervalUs, std::memory_order_release);
    lastProgressUs_ = now;
    lock.unlock();
    wake_.notify_one();
}

void UMCBCalibrationCapture::workerLoop()
{
    for (;;) {
        AVFrame *frame = nullptr;
        QString label;
        int index = 0;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || pending_ != nullptr; });
            if (stopping_)
                break;
            frame = pending_;
            pending_ = nullptr;
            label = pendingLabel_;
            index = pendingIndex_;
            generation = pendingGeneration_;
            workerBusy_ = true;
        }
        const QImage image = makeGrayImage(frame);
        av_frame_free(&frame);
        QByteArray png;
        bool encoded = false;
        if (!image.isNull()) {
            QBuffer buffer(&png);
            if (buffer.open(QIODevice::WriteOnly))
                encoded = image.save(&buffer, "PNG");
        }
        bool saved = false;
        QString detail = "decode_or_png_failed";
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != generation_ || stopping_ || !active_.load(std::memory_order_acquire)) {
                workerBusy_ = false;
                continue;
            }
            if (!encoded) {
                detail = "decode_or_png_failed";
            } else if (savedImages_ >= kMaxImages ||
                       savedBytes_ + static_cast<uint64_t>(png.size()) > kMaxBytes) {
                detail = "session_storage_limit";
            } else {
                // Only the worker writes files, and the folder is created on first save.
                const QString folder = folderPath();
                const QString run = QString("%1_%2")
                    .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz"))
                    .arg(static_cast<qlonglong>(QCoreApplication::applicationPid()));
                if (!QDir().mkpath(folder)) {
                    detail = "cannot_create_folder";
                } else {
                    // Timestamp + monotonic session count prevents collisions under normal use.
                    const QString filename = QString("%1_%2_%3.png")
                        .arg(label, run).arg(savedImages_ + 1, 3, 10, QChar('0'));
                    const QString path = QDir(folder).filePath(filename);
                    if (QFileInfo::exists(path)) {
                        detail = "filename_collision";
                    } else {
                        QSaveFile out(path);
                        if (out.open(QIODevice::WriteOnly) && out.write(png) == png.size() && out.commit()) {
                            saved = true;
                            ++savedImages_;
                            savedBytes_ += static_cast<uint64_t>(png.size());
                            detail = QString("saved_%1").arg(filename);
                        } else {
                            detail = "file_write_failed";
                        }
                    }
                }
            }
            ++completed_;
            workerBusy_ = false;
            lastProgressUs_ = monotonicUs();
            if (!saved || completed_ >= expected_) {
                lastResult_ = saved ? "complete" : detail;
                active_.store(false, std::memory_order_release);
            }
        }
        if (saved)
            qInfo().noquote() << "[UMCB-M1.3] Saved" << label << "frame=" << index << "bytes=" << png.size();
        else
            qWarning().noquote() << "[UMCB-M1.3] Capture did not save" << label << "frame=" << index << "reason=" << detail;
    }
}
