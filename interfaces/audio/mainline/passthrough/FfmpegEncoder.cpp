/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

// IEC 61937 packing of DTS-HD and Dolby TrueHD through FFmpeg (libavformat's
// "spdif" muxer, fed by libavcodec's DCA / MLP parsers). Only built with the
// ffmpeg_passthrough Soong config option; FFmpeg is linked as LGPL shared
// libraries.

#define LOG_TAG "MainlineAudio_Passthrough"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include <android-base/logging.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}

#include "passthrough/EncoderBackend.h"

namespace aidl::android::hardware::audio::core::mainline::passthrough {

namespace {

constexpr int kIoBufferSize = 64 * 1024;

// FFmpeg logs to stderr by default, which goes nowhere in a service.
void LogToLogcat(void* avcl, int level, const char* fmt, va_list args) {
    if (level > AV_LOG_WARNING) return;
    char line[512];
    int print_prefix = 1;
    av_log_format_line(avcl, level, fmt, args, line, sizeof(line), &print_prefix);
    if (level <= AV_LOG_ERROR) {
        LOG(ERROR) << "ffmpeg: " << line;
    } else {
        LOG(WARNING) << "ffmpeg: " << line;
    }
}

class FfmpegBackend final : public EncoderBackend {
  public:
    FfmpegBackend(AVCodecID codec_id, int dtshd_rate, Encoder::Output output)
        : codec_id_(codec_id), dtshd_rate_(dtshd_rate), output_(std::move(output)) {}
    ~FfmpegBackend() override { Destroy(); }

    bool Init() {
        parser_ = av_parser_init(codec_id_);
        parser_context_ = avcodec_alloc_context3(nullptr);
        packet_ = av_packet_alloc();
        const AVOutputFormat* spdif = av_guess_format("spdif", nullptr, nullptr);
        if (parser_ == nullptr || parser_context_ == nullptr || packet_ == nullptr ||
            spdif == nullptr ||
            avformat_alloc_output_context2(&muxer_, spdif, nullptr, nullptr) < 0) {
            LOG(ERROR) << __func__ << ": FFmpeg lacks the parser for codec " << codec_id_
                       << " or the spdif muxer";
            return false;
        }
        auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferSize));
        io_ = buffer != nullptr ? avio_alloc_context(buffer, kIoBufferSize, 1 /*write*/, this,
                                                     nullptr, &FfmpegBackend::WriteOutput, nullptr)
                                : nullptr;
        if (io_ == nullptr) {
            av_free(buffer);
            return false;
        }
        muxer_->pb = io_;
        muxer_->flags |= AVFMT_FLAG_CUSTOM_IO;
        AVStream* stream = avformat_new_stream(muxer_, nullptr);
        if (stream == nullptr) return false;
        stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        stream->codecpar->codec_id = codec_id_;
        // Rate of the IEC 60958 stream in two channel frames (768000 for high
        // bit rate); 0 would strip DTS-HD down to its core.
        if (dtshd_rate_ != 0 &&
            av_opt_set_int(muxer_->priv_data, "dtshd_rate", dtshd_rate_, 0) < 0) {
            return false;
        }
        if (const int err = avformat_write_header(muxer_, nullptr); err < 0) {
            LOG(ERROR) << __func__ << ": avformat_write_header: " << err;
            return false;
        }
        return true;
    }

    void Write(const void* data, size_t bytes) override {
        const auto* in = static_cast<const uint8_t*>(data);
        while (bytes > 0) {
            uint8_t* frame = nullptr;
            int frame_size = 0;
            const int used =
                    av_parser_parse2(parser_, parser_context_, &frame, &frame_size, in,
                                     static_cast<int>(bytes), AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (used < 0) break;
            in += used;
            bytes -= static_cast<size_t>(used);
            if (frame_size > 0) {
                packet_->data = frame;
                packet_->size = frame_size;
                packet_->stream_index = 0;
                // Failures (e.g. an overlong frame) drop that frame only.
                if (const int err = av_write_frame(muxer_, packet_); err < 0) {
                    LOG(WARNING) << __func__ << ": av_write_frame: " << err;
                }
                avio_flush(io_);
            } else if (used == 0) {
                break;
            }
        }
    }

    void Reset() override {
        // Neither the parser nor the muxer (TrueHD MAT frame in progress) can
        // be reset, so start over.
        Destroy();
        if (!Init()) LOG(ERROR) << __func__ << ": could not recreate the packer";
    }

  private:
    static int WriteOutput(void* opaque, const uint8_t* buffer, int size) {
        static_cast<FfmpegBackend*>(opaque)->output_(buffer, static_cast<size_t>(size));
        return size;
    }

    void Destroy() {
        if (muxer_ != nullptr) {
            avformat_free_context(muxer_);
            muxer_ = nullptr;
        }
        if (io_ != nullptr) {
            av_freep(&io_->buffer);
            avio_context_free(&io_);
        }
        if (parser_ != nullptr) {
            av_parser_close(parser_);
            parser_ = nullptr;
        }
        avcodec_free_context(&parser_context_);
        av_packet_free(&packet_);
    }

    const AVCodecID codec_id_;
    const int dtshd_rate_;
    const Encoder::Output output_;
    AVCodecParserContext* parser_ = nullptr;
    AVCodecContext* parser_context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFormatContext* muxer_ = nullptr;
    AVIOContext* io_ = nullptr;
};

}  // namespace

std::unique_ptr<EncoderBackend> CreateFfmpegBackend(EncodedFormat format, const IecStream& stream,
                                                    Encoder::Output output) {
    static std::once_flag log_once;
    std::call_once(log_once, [] { av_log_set_callback(&LogToLogcat); });

    AVCodecID codec_id;
    int dtshd_rate = 0;
    switch (format) {
        case EncodedFormat::kDtsHd:
        case EncodedFormat::kDtsHdMa:
            codec_id = AV_CODEC_ID_DTS;
            dtshd_rate = static_cast<int>(stream.pcm_rate * stream.pcm_channels / 2);
            break;
        case EncodedFormat::kTrueHd:
            codec_id = AV_CODEC_ID_TRUEHD;
            break;
        default:
            return nullptr;
    }
    auto backend = std::make_unique<FfmpegBackend>(codec_id, dtshd_rate, std::move(output));
    if (!backend->Init()) return nullptr;
    return backend;
}

}  // namespace aidl::android::hardware::audio::core::mainline::passthrough
