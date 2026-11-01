// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL
#include "umcbvideorecorder.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}

namespace {
constexpr int64_t kMaxFileBytes = 300000000; // 300 decimal MB
constexpr size_t kMaxQueuedBytes = 16 * 1024 * 1024;
constexpr int kMaxQueueSamples = 400;
}

UMCBVideoRecorder::UMCBVideoRecorder(unsigned width, unsigned height, unsigned fps)
    : width_(width), height_(height), fps_(fps ? fps : 60)
{
}

UMCBVideoRecorder::~UMCBVideoRecorder()
{
    stop();
}

int64_t UMCBVideoRecorder::monotonicUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool UMCBVideoRecorder::containsNalType(const uint8_t *p, size_t size, unsigned type)
{
    if (!p || size < 5)
        return false;
    for (size_t i = 0; i + 4 < size; ++i) {
        if (p[i] != 0 || p[i + 1] != 0)
            continue;
        const bool three = p[i + 2] == 1;
        const bool four = p[i + 2] == 0 && p[i + 3] == 1;
        if (!three && !four)
            continue;
        const size_t nal = i + (four ? 4 : 3);
        if (nal < size && (p[nal] & 0x1fu) == type)
            return true;
    }
    return false;
}

QString UMCBVideoRecorder::start(int seconds)
{
    if (seconds != 120 && seconds != 180)
        return QStringLiteral("ERROR: Choose 120 or 180 seconds");
    if (active_.load())
        return QStringLiteral("ERROR: Recording already active");
    // Reap an earlier auto-stopped recording before opening another file.
    stop();

    QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (movies.isEmpty())
        movies = QDir::homePath();
    QDir folder(movies);
    if (!folder.mkpath(QStringLiteral("UMCB Recordings")))
        return QStringLiteral("ERROR: Could not create recording directory");
    folder.cd(QStringLiteral("UMCB Recordings"));
    const QString basename = QStringLiteral("UMCB_Minecraft_FullSequence_") +
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HHmmss_zzz")) +
        QStringLiteral(".mp4");
    const QString file_path = folder.filePath(basename);

    QByteArray header;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        header = header_;
        if (header.isEmpty())
            return QStringLiteral("ERROR: PS4 H.264 header not received yet; reconnect video and retry");
        queue_.clear();
        queued_bytes_ = 0;
        stop_requested_ = false;
        duration_seconds_ = seconds;
        start_us_ = monotonicUs();
        output_path_ = file_path;
        status_ = QStringLiteral("Recording: ") + file_path;
        active_.store(true);
    }
    worker_ = std::thread(&UMCBVideoRecorder::writeRecording, this, header, file_path);
    return file_path;
}

void UMCBVideoRecorder::stop()
{
    active_.store(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_requested_ = true;
    }
    event_.notify_one();
    if (worker_.joinable())
        worker_.join();
}

QString UMCBVideoRecorder::status() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void UMCBVideoRecorder::feed(const uint8_t *data, size_t size)
{
    if (!data || !size || size > static_cast<size_t>(std::numeric_limits<int>::max()))
        return;

    // Chiaki sends the H.264 SPS/PPS header as a separate sample upon profile setup.
    // Keep that tiny header even before the user starts recording.
    if (containsNalType(data, size, 7)) {
        std::lock_guard<std::mutex> lock(mutex_);
        header_ = QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(size));
        return;
    }
    if (!active_.load())
        return;

    const int64_t now = monotonicUs();
    bool notify = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_.load())
            return;
        if (now - start_us_ >= static_cast<int64_t>(duration_seconds_) * 1000000) {
            active_.store(false);
            stop_requested_ = true;
            notify = true;
        } else if (queued_bytes_ + size > kMaxQueuedBytes ||
                   queue_.size() >= kMaxQueueSamples) {
            active_.store(false);
            stop_requested_ = true;
            status_ = QStringLiteral("ERROR: Recording queue overloaded; check saved clip");
            notify = true;
        } else {
            VideoSample sample;
            sample.bytes = QByteArray(reinterpret_cast<const char *>(data), static_cast<int>(size));
            sample.capture_us = now;
            sample.idr = containsNalType(data, size, 5);
            queued_bytes_ += size;
            queue_.push_back(std::move(sample));
            notify = true;
        }
    }
    if (notify)
        event_.notify_one();
}

void UMCBVideoRecorder::writeRecording(QByteArray header, QString output_path)
{
    AVFormatContext *fmt = nullptr;
    AVPacket *packet = nullptr;
    bool header_written = false;
    bool failed = false;
    bool truncated = false;
    bool received_idr = false;
    int64_t first_us = -1;
    int64_t last_pts = -1;
    int written_frames = 0;
    QString error;

    do {
        const QByteArray path_utf8 = output_path.toUtf8();
        if (avformat_alloc_output_context2(&fmt, nullptr, "mp4", path_utf8.constData()) < 0 || !fmt) {
            error = QStringLiteral("FFmpeg could not initialise MP4 writer");
            failed = true;
            break;
        }
        AVStream *stream = avformat_new_stream(fmt, nullptr);
        if (!stream) {
            error = QStringLiteral("FFmpeg could not create video stream");
            failed = true;
            break;
        }
        stream->time_base = AVRational{1, 90000};
        stream->avg_frame_rate = AVRational{static_cast<int>(fps_), 1};
        AVCodecParameters *par = stream->codecpar;
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->codec_id = AV_CODEC_ID_H264;
        par->width = static_cast<int>(width_);
        par->height = static_cast<int>(height_);
        par->extradata = static_cast<uint8_t *>(av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) {
            error = QStringLiteral("FFmpeg ran out of memory for H.264 header");
            failed = true;
            break;
        }
        memcpy(par->extradata, header.constData(), static_cast<size_t>(header.size()));
        par->extradata_size = static_cast<int>(header.size());

        if (!(fmt->oformat->flags & AVFMT_NOFILE) &&
            avio_open(&fmt->pb, path_utf8.constData(), AVIO_FLAG_WRITE) < 0) {
            error = QStringLiteral("Cannot create MP4 recording on disk");
            failed = true;
            break;
        }
        if (avformat_write_header(fmt, nullptr) < 0) {
            error = QStringLiteral("FFmpeg rejected the H.264 stream header");
            failed = true;
            break;
        }
        header_written = true;
        packet = av_packet_alloc();
        if (!packet) {
            error = QStringLiteral("FFmpeg could not allocate video packet");
            failed = true;
            break;
        }
        for (;;) {
            VideoSample sample;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                event_.wait(lock, [this]() { return stop_requested_ || !queue_.empty(); });
                if (queue_.empty() && stop_requested_)
                    break;
                sample = std::move(queue_.front());
                queued_bytes_ -= static_cast<size_t>(sample.bytes.size());
                queue_.pop_front();
            }
            // An MP4 must begin with a keyframe. Skip P-frames until the next IDR.
            if (!received_idr) {
                if (!sample.idr)
                    continue;
                first_us = sample.capture_us;
                received_idr = true;
            }
            if (av_new_packet(packet, sample.bytes.size()) < 0) {
                error = QStringLiteral("Could not allocate compressed sample");
                failed = true;
                break;
            }
            memcpy(packet->data, sample.bytes.constData(), static_cast<size_t>(sample.bytes.size()));
            int64_t pts = av_rescale_q(sample.capture_us - first_us,
                                       AVRational{1, 1000000}, stream->time_base);
            pts = std::max(pts, last_pts + 1);
            last_pts = pts;
            packet->pts = pts;
            packet->dts = pts; // PS4 Remote Play H.264 uses low-latency, reordered-free frames.
            packet->duration = av_rescale_q(1000000 / static_cast<int64_t>(fps_),
                                            AVRational{1, 1000000}, stream->time_base);
            packet->stream_index = stream->index;
            if (sample.idr)
                packet->flags |= AV_PKT_FLAG_KEY;
            const int result = av_interleaved_write_frame(fmt, packet);
            av_packet_unref(packet);
            if (result < 0) {
                error = QStringLiteral("FFmpeg MP4 packet write failed");
                failed = true;
                break;
            }
            ++written_frames;
            if (fmt->pb && avio_tell(fmt->pb) >= kMaxFileBytes - 1048576) {
                truncated = true;
                active_.store(false);
                std::lock_guard<std::mutex> lock(mutex_);
                stop_requested_ = true;
                break;
            }
        }
    } while (false);

    if (header_written && av_write_trailer(fmt) < 0) {
        failed = true;
        error = QStringLiteral("MP4 finalisation failed");
    }
    if (packet)
        av_packet_free(&packet);
    if (fmt) {
        if (fmt->pb && !(fmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&fmt->pb);
        avformat_free_context(fmt);
    }
    if (!received_idr || written_frames == 0) {
        failed = true;
        error = QStringLiteral("No H.264 keyframe captured; recording could not be saved");
    }
    if (failed)
        QFile::remove(output_path);
    const qint64 bytes = failed ? 0 : QFile(output_path).size();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed)
            status_ = QStringLiteral("ERROR: ") + error;
        else
            status_ = QStringLiteral("Saved: ") + output_path +
                QStringLiteral(" (%1 MB)%2").arg(bytes / 1000000.0, 0, 'f', 1)
                    .arg(truncated ? QStringLiteral(" [300 MB limit]") : QString());
        active_.store(false);
        stop_requested_ = true;
    }
}
