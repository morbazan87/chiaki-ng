// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// UMCB M1: video-only, PS4-encoded-sample recorder. No desktop capture or PS4 input.
#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

class UMCBVideoRecorder final
{
public:
    UMCBVideoRecorder(unsigned width, unsigned height, unsigned fps);
    ~UMCBVideoRecorder();
    QString start(int seconds);
    void stop();
    void feed(const uint8_t *data, size_t size);
    bool active() const { return active_.load(); }
    QString status() const;

private:
    struct VideoSample {
        QByteArray bytes;
        int64_t capture_us = 0;
        bool idr = false;
    };
    static int64_t monotonicUs();
    static bool containsNalType(const uint8_t *data, size_t size, unsigned type);
    void writeRecording(QByteArray header, QString output_path);

    const unsigned width_;
    const unsigned height_;
    const unsigned fps_;
    mutable std::mutex mutex_;
    std::condition_variable event_;
    std::deque<VideoSample> queue_;
    QByteArray header_;
    QString output_path_;
    QString status_ = QStringLiteral("Idle");
    size_t queued_bytes_ = 0;
    bool stop_requested_ = false;
    int duration_seconds_ = 120;
    int64_t start_us_ = 0;
    std::atomic<bool> active_{false};
    std::thread worker_;
};
