// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
// UMCB M1.3: bounded user-labelled captures of decoded PS4 video (Windows test only).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <QString>

struct AVFrame;

class UMCBCalibrationCapture final
{
public:
    UMCBCalibrationCapture();
    ~UMCBCalibrationCapture();
    UMCBCalibrationCapture(const UMCBCalibrationCapture &) = delete;
    UMCBCalibrationCapture &operator=(const UMCBCalibrationCapture &) = delete;

    // Called from the GUI thread through a user-initiated, local-only command.
    QString request(const QString &label);
    QString status();
    void expireStalled();
    void cancelSession();
    // Called on Chiaki's existing decoded-frame delivery thread; never writes a file.
    void observe(const AVFrame *frame);

private:
    void workerLoop();
    static int64_t monotonicUs();
    QString folderPath() const;

    std::atomic<bool> active_{false};
    std::atomic<int64_t> nextDueUs_{0};
    std::atomic<uint64_t> skippedUnsupported_{0};
    std::mutex mutex_;
    std::condition_variable wake_;
    AVFrame *pending_ = nullptr;
    QString pendingLabel_;
    int pendingIndex_ = 0;
    uint64_t pendingGeneration_ = 0;
    QString label_;
    QString lastResult_ = "idle";
    uint64_t generation_ = 0;
    uint64_t savedBytes_ = 0;
    int queued_ = 0;
    int completed_ = 0;
    int expected_ = 0;
    int savedImages_ = 0;
    int64_t requestStartUs_ = 0;
    int64_t lastProgressUs_ = 0;
    bool workerBusy_ = false;
    bool stopping_ = false;
    // Construct last: worker must not see uninitialised members.
    std::thread worker_;
};
