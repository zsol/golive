// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_dictionary.h"
#include "ppapi/cpp/media_stream_video_track.h"
#include "ppapi/cpp/media_stream_audio_track.h"
#include "ppapi/cpp/video_frame.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/utility/threading/simple_thread.h"
#include "ppapi/cpp/size.h"
#include "nacl_io/nacl_io.h"

extern "C" {
#include "libavformat/avio.h"
#include "libavutil/mem.h"
#include "libavformat/avformat.h"

#include <math.h>

// TODO: using private API here to speed up development cycle
struct URLProtocol;
int ffurl_register_protocol(URLProtocol*);
}

#include <sstream>

class GoLiveInstance;

namespace {

void log_callback(void* ctx, int level, const char* fmt, va_list args);

struct LogCtx {
  AVClass* av_class;
  GoLiveInstance* inst;
};

static LogCtx static_log_ctx = LogCtx();

}

class GoLiveInstance : public pp::Instance {
  pp::MediaStreamVideoTrack video_track_;
  pp::MediaStreamAudioTrack audio_track_;
  pp::CompletionCallbackFactory<GoLiveInstance> callback_factory_;

  std::string url_;

  AVFormatContext* av_fmt_octx_;
  bool octx_open_;
  bool is_encoding_;

  AVFrame* current_av_frame_;
  bool new_frame_available_;

  pp::SimpleThread bg_thread_;

 public:
  explicit GoLiveInstance(PP_Instance instance)
      : pp::Instance(instance),
        callback_factory_(this),
        octx_open_(false),
        is_encoding_(false),
        new_frame_available_(false),
        bg_thread_(this) {
          nacl_io_init_ppapi(instance, pp::Module::Get()->get_browser_interface());
          av_log_set_callback(&log_callback);
          static_log_ctx.inst = this;
          av_log_set_level(AV_LOG_DEBUG);
      #if 1
          extern AVOutputFormat ff_h264_muxer;
          av_register_output_format(&ff_h264_muxer);
          extern URLProtocol ff_rtmp_protocol;
          ffurl_register_protocol(&ff_rtmp_protocol);
          extern URLProtocol ff_tcp_protocol;
          ffurl_register_protocol(&ff_tcp_protocol);
          extern AVCodec ff_libx264_encoder;
          avcodec_register(&ff_libx264_encoder);
          extern AVCodec ff_mpeg4_encoder;
          avcodec_register(&ff_mpeg4_encoder);
          extern AVOutputFormat ff_mp4_muxer;
          av_register_output_format(&ff_mp4_muxer);
          extern AVOutputFormat ff_flv_muxer;
          av_register_output_format(&ff_flv_muxer);
      #else
          av_register_all();
      #endif
          avformat_network_init();
          pp::VarDictionary dict;
          dict.Set("type", "init");
          PostMessage(dict);
          bg_thread_.Start();
        }
  virtual ~GoLiveInstance() {
    StopConversion();
    avformat_network_deinit();
  }

  virtual void HandleMessage(const pp::Var& var_message) {
    // Ignore the message if it is not a string.
    if (!var_message.is_dictionary()) {
        Log(pp::Var("Invalid message"));
        return;
    }

    pp::VarDictionary var_dictionary_message(var_message);
    std::string command = var_dictionary_message.Get("command").AsString();

    if (command == "stream") {
      pp::Var var_track = var_dictionary_message.Get("video_track");
      if (!var_track.is_resource()) {
        Log("invalid video track");
        return;
      }
      video_track_ = pp::MediaStreamVideoTrack(var_track.AsResource());
      pp::Var var_url = var_dictionary_message.Get("url");
      if (!var_url.is_string()) {
        Log("invalid url");
        return;
      }
      url_ = var_url.AsString();
      video_track_.GetFrame(
        callback_factory_.NewCallbackWithOutput(
          &GoLiveInstance::OnFirstFrame
        )
      );
    } else if (command == "stop_stream") {
      StopConversion();
    } else {
      Log(pp::Var("Invalid command: " + command));
      return;
    }
  }

  void OnFirstFrame(int32_t res, const pp::VideoFrame frame) {
    pp::Size s;
    if (!frame.GetSize(&s)) {
      Log("failed getting size");
      video_track_.GetFrame(
        callback_factory_.NewCallbackWithOutput(
          &GoLiveInstance::OnFirstFrame
        )
      );
      return;
    }

    bg_thread_.message_loop().PostWork(
      callback_factory_.NewCallback(
        &GoLiveInstance::StartConversion,
        s.width(),
        s.height()
      )
    );
  }

  void StartConversion(int32_t, int width, int height) {
    current_av_frame_ = av_frame_alloc();

    InitializeRtmp(width, height, url_.c_str());

    is_encoding_ = true;
    video_track_.GetFrame(callback_factory_.NewCallbackWithOutput(
      &GoLiveInstance::OnFrame
    ));
  }

  void StopConversion() {
    is_encoding_ = false;
    while (new_frame_available_) {
      usleep(10 * 1000);
    }
    av_write_trailer(av_fmt_octx_);
    if (av_fmt_octx_->streams[0]) {
      AVCodecContext* cctx = av_fmt_octx_->streams[0]->codec;
      if (avcodec_is_open(cctx)) {
        avcodec_close(cctx);
      }
    }
    if (octx_open_) {
      avio_closep(&av_fmt_octx_->pb);
      avformat_free_context(av_fmt_octx_);
      av_fmt_octx_ = nullptr;
      octx_open_ = false;
    }
  }

  void InitializeRtmp(int width, int height, const char* url) {
    AVOutputFormat* ofmt = av_guess_format("flv", nullptr, nullptr);
    int ret = avformat_alloc_output_context2(
      &av_fmt_octx_,
      ofmt,
      nullptr,
      url
    );
    if (ret < 0) {
      AVLogError(ret);
      return;
    }

    ret = avio_open(
      &av_fmt_octx_->pb,
      url,
      AVIO_FLAG_READ_WRITE
    );
    if (ret < 0) {
      AVLogError(ret);
      LogError(errno, "errno");
      return;
    }
    octx_open_ = true;

    AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
      Log("Unable to find encoder");
      return;
    }

    AVStream* stream = avformat_new_stream(av_fmt_octx_, codec);
    stream->codec->codec_id = codec->id;
    stream->codec->bit_rate = 1000000;
    stream->codec->height = height;
    stream->codec->width = width;
    stream->codec->pix_fmt = AV_PIX_FMT_YUV420P;
    if (ofmt->flags & AVFMT_GLOBALHEADER) {
      stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    stream->codec->time_base = stream->time_base = (AVRational){1, 30};
    stream->codec->gop_size = 10;
    stream->codec->keyint_min = 5;
    av_fmt_octx_->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;
    ret = avcodec_open2(stream->codec, codec, nullptr);
    if (ret < 0) {
      Log("error during opening codec");
      AVLogError(ret);
      return;
    }
    avformat_write_header(av_fmt_octx_, nullptr);
    av_dump_format(av_fmt_octx_, 0, url, 1);

    return;
  }

  int32_t CopyVideoFrame(pp::VideoFrame src) {
    if (new_frame_available_) {
      return PP_OK;
    }
    if (current_av_frame_->buf[0] &&
        av_frame_is_writable(current_av_frame_) <= 0) {
      int ret = av_frame_make_writable(current_av_frame_);
      if (ret < 0) {
        AVLogError(ret);
        Log("dropped frame");
        return -1;
      }
    }
    current_av_frame_->format = AV_PIX_FMT_YUV420P;
    pp::Size frame_size;
    src.GetSize(&frame_size);
    current_av_frame_->width = frame_size.width();
    current_av_frame_->height = frame_size.height();
    int64_t ts_millisec = round(src.GetTimestamp() * 1000);
    const AVRational thousandth = {1, 1000};
    const AVRational spf = {1, 30};
    current_av_frame_->pts = av_rescale_q(ts_millisec, thousandth, spf);
    if (current_av_frame_->buf[0] == 0) {
      int ret = av_frame_get_buffer(current_av_frame_, 16);
      if (ret < 0) {
        AVLogError(ret);
        return -2;
      }
    }

    uint8_t* ppframe = reinterpret_cast<uint8_t*>(src.GetDataBuffer());
    size_t databuffersize = src.GetDataBufferSize();
    size_t height = frame_size.height();
    size_t width = frame_size.width();
    if (databuffersize != width * height + 2 * (width / 2) * (height / 2)) {
      Log("Unexpected Chrome frame buffer size, bad things will happen");
    }
    size_t y_stride = current_av_frame_->linesize[0];
    /* Y */
    uint8_t* dst = current_av_frame_->data[0];
    for (int i = 0; i < height; ++i) {
      memcpy(dst, ppframe, width);
      ppframe += width;
      dst += y_stride;
    }

    size_t uv_stride = current_av_frame_->linesize[1];
    /* U */
    dst = current_av_frame_->data[1];
    for (int i = 0; i < height / 2; ++i) {
      memcpy(dst, ppframe, width / 2);
      ppframe += width / 2;
      dst += uv_stride;
    }

    /* V */
    dst = current_av_frame_->data[2];
    for (int i = 0; i < height / 2; ++i) {
      memcpy(dst, ppframe, width / 2);
      ppframe += width / 2;
      dst += uv_stride;
    }

    new_frame_available_ = true;
    return PP_OK;
  }

  void EncodeFrame(int32_t) {
    AVPacket pkt = { 0 };
    av_init_packet(&pkt);
    int got_packet = 0;
    AVCodecContext* cctx = av_fmt_octx_->streams[0]->codec;
    int ret = avcodec_encode_video2(
      cctx,
      &pkt,
      current_av_frame_,
      &got_packet
    );
    if (ret < 0) {
      Log("Error during encoding frame");
      AVLogError(ret);
      new_frame_available_ = false;
      return;
    }
    if (got_packet != 0) {
      size_t stream_index = 0;
      pkt.pts = pkt.dts = current_av_frame_->pts;
      av_packet_rescale_ts(&pkt, cctx->time_base, av_fmt_octx_->streams[stream_index]->time_base);
      pkt.stream_index = stream_index;
      ret = av_interleaved_write_frame(av_fmt_octx_, &pkt);
      if (ret < 0) {
        Log("Error during writing encoded frame");
        AVLogError(ret);
        new_frame_available_ = false;
        return;
      }
    }
    av_free_packet(&pkt);
    new_frame_available_ = false;
  }

  void NoOp(int32_t) const {}

  void OnFrame(int32_t result, pp::VideoFrame frame) {
    if (!is_encoding_) {
      return;
    }
    CopyVideoFrame(frame);
    bg_thread_.message_loop().PostWork(
      callback_factory_.NewCallback(&GoLiveInstance::EncodeFrame)
    );
    video_track_.RecycleFrame(frame);
    video_track_.GetFrame(callback_factory_.NewCallbackWithOutput(
      &GoLiveInstance::OnFrame
    ));
  }

  void Log(const pp::Var& msg) {
    pp::VarDictionary d;
    d.Set("type", "log");
    d.Set("message", msg);
    PostMessage(d);
  }

  void AVLog(const pp::Var& msg) {
    pp::VarDictionary d;
    d.Set("type", "av_log");
    d.Set("message", msg);
    PostMessage(d);
  }

  template <typename T> void LogError(const T& error, const std::string& msg) {
    std::stringstream ss;
    ss << msg << ": " << error;
    Log(pp::Var(ss.str()));
  }

  void AVLogError(int64_t errnum) {
    std::stringstream ss;
    ss << av_err2str(errnum) << ": " << errnum;
    AVLog(pp::Var(ss.str()));
  }
};

class GoLiveModule : public pp::Module {
 public:
  GoLiveModule() : pp::Module() {}
  virtual ~GoLiveModule() {}

  virtual pp::Instance* CreateInstance(PP_Instance instance) {
    return new GoLiveInstance(instance);
  }
};

namespace pp {

Module* CreateModule() {
  return new GoLiveModule();
}

}  // namespace pp

namespace {
  void log_callback(void* ctx, int level, const char* fmt, va_list args) {
   char line[2048];
   int print_prefix;
   av_log_format_line(ctx, level, fmt, args, line, 2048, &print_prefix);
   static_log_ctx.inst->AVLog(pp::Var(std::string(line)));
 }
}
