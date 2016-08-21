// Copyright (c) 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ppapi/cpp/instance.h"
#include "ppapi/cpp/module.h"
#include "ppapi/cpp/file_io.h"
#include "ppapi/cpp/file_ref.h"
#include "ppapi/cpp/file_system.h"
#include "ppapi/c/ppb_file_io.h"
#include "ppapi/cpp/var.h"
#include "ppapi/cpp/var_dictionary.h"
#include "ppapi/cpp/media_stream_video_track.h"
#include "ppapi/cpp/media_stream_audio_track.h"
#include "ppapi/cpp/video_frame.h"
#include "ppapi/utility/completion_callback_factory.h"
#include "ppapi/utility/threading/simple_thread.h"
#include "ppapi/cpp/video_encoder.h"
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

double clamp(double min, double max, double value) {
  return std::max(std::min(value, max), min);
}

void log_callback(void* ctx, int level, const char* fmt, va_list args);

struct LogCtx {
  AVClass* av_class;
  GoLiveInstance* inst;
};

static LogCtx static_log_ctx = LogCtx();

struct BitstreamBuf {
  static const size_t BUFSIZE = 300000;
  uint8_t* buf;
  size_t start_pos;
  size_t end_pos;
};

int read_packet(void* opaque, uint8_t* buf, int size) {
  BitstreamBuf* av_io_buf = static_cast<BitstreamBuf*>(opaque);
  size = FFMIN(size, av_io_buf->end_pos - av_io_buf->start_pos);

  memcpy(buf, av_io_buf->buf + av_io_buf->start_pos, size);
  av_io_buf->start_pos += size;

  return size;
}

}

class GoLiveInstance : public pp::Instance {
  pp::MediaStreamVideoTrack video_track_;
  pp::MediaStreamAudioTrack audio_track_;
  pp::CompletionCallbackFactory<GoLiveInstance> callback_factory_;

  pp::FileSystem file_system_;
  pp::FileIO file_io_;
  pp::FileRef file_ref_;
  uint64_t file_offset_;
  uint64_t nb_frames_;

  bool is_encode_ticking_;
  pp::VideoEncoder video_encoder_;
  pp::VideoFrame current_track_frame_;

  PP_Time last_encode_tick_;
  BitstreamBuf bitstream_buf_;

  uint8_t* av_io_ibuf_;
  AVIOContext* av_io_ictx_;
  AVFormatContext* av_fmt_ictx_;

  AVFormatContext* av_fmt_octx_;

  AVFrame* current_av_frame_;

  static const size_t BUFSIZE = 30000;

  pp::SimpleThread bg_thread_;

 public:
  explicit GoLiveInstance(PP_Instance instance)
      : pp::Instance(instance),
        callback_factory_(this),
        file_offset_(0),
        nb_frames_(0),
        is_encode_ticking_(false),
        bg_thread_(this) {
          bitstream_buf_.buf = new uint8_t[BitstreamBuf::BUFSIZE];
          bitstream_buf_.start_pos = bitstream_buf_.end_pos = 0;
          av_io_ibuf_ = static_cast<uint8_t*>(av_malloc(BUFSIZE));
          av_io_ictx_ = avio_alloc_context(
            av_io_ibuf_, BUFSIZE, 0,
            &this->bitstream_buf_, &read_packet, nullptr, nullptr
          );
          av_fmt_ictx_ = avformat_alloc_context();
          av_fmt_ictx_->pb = av_io_ictx_;

          nacl_io_init_ppapi(instance, pp::Module::Get()->get_browser_interface());

          Log("ready");
          bg_thread_.Start();
        }
  virtual ~GoLiveInstance() {
    delete [] bitstream_buf_.buf;
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
      Log(pp::Var("Got stream"));
      pp::Var var_track = var_dictionary_message.Get("video_track");
      if (!var_track.is_resource()) {
        return;
      }
      video_track_ = pp::MediaStreamVideoTrack(var_track.AsResource());
      StartConversion();
    } else {
      Log(pp::Var("Invalid command: " + command));
      return;
    }
  }

  void OnFileSystemOpen(int32_t open_result) {
    if (open_result != PP_OK) {
      std::stringstream ss;
      ss << "Failed to open file system: " << open_result;
      Log(pp::Var(ss.str()));
      return;
    }
    file_ref_ = pp::FileRef(file_system_, "/frame");
    file_io_ = pp::FileIO(this);

    if (file_system_.is_null()) {
      std::stringstream ss;
      ss << "File ref is null";
      Log(pp::Var(ss.str()));
      return;
    }

    if (file_ref_.is_null()) {
      std::stringstream ss;
      ss << "File ref is null";
      Log(pp::Var(ss.str()));
      return;
    }

    if (file_io_.is_null()) {
      std::stringstream ss;
      ss << "File io is null";
      Log(pp::Var(ss.str()));
      return;
    }

    file_io_.Open(
      file_ref_,
      PP_FILEOPENFLAG_WRITE | PP_FILEOPENFLAG_CREATE | PP_FILEOPENFLAG_TRUNCATE,
      callback_factory_.NewCallback(&GoLiveInstance::OnFileOpen)
    );
  }

  void OnFileOpen(int32_t open_result) {
    if (open_result != PP_OK) {
      std::stringstream ss;
      ss << "Failed to open file: " << open_result;
      Log(pp::Var(ss.str()));
    }
  }

  virtual void StartConversion() {
    av_log_set_callback(&log_callback);
    static_log_ctx.inst = this;
    av_log_set_level(AV_LOG_DEBUG);
#if 1
    extern AVOutputFormat ff_h264_muxer;
    av_register_output_format(&ff_h264_muxer);
    extern AVInputFormat ff_h264_demuxer;
    av_register_input_format(&ff_h264_demuxer);
    extern URLProtocol ff_rtmp_protocol;
    ffurl_register_protocol(&ff_rtmp_protocol);
    extern URLProtocol ff_tcp_protocol;
    ffurl_register_protocol(&ff_tcp_protocol);
    extern AVCodec ff_h264_decoder;
    avcodec_register(&ff_h264_decoder);
    extern AVCodecParser ff_h264_parser;
    av_register_codec_parser(&ff_h264_parser);
    extern AVOutputFormat ff_mpeg2video_muxer;
    av_register_output_format(&ff_mpeg2video_muxer);
    extern AVOutputFormat ff_flv_muxer;
    av_register_output_format(&ff_flv_muxer);
#else
    av_register_all();
#endif
    avformat_network_init(); // TODO: deinit

    current_av_frame_ = av_frame_alloc();
    av_frame_get_buffer(current_av_frame_, 32);

    file_system_ = pp::FileSystem(this, PP_FILESYSTEMTYPE_LOCALPERSISTENT);
    file_system_.Open(
      1024*1024*10,
      callback_factory_.NewCallback(&GoLiveInstance::OnFileSystemOpen)
    );
    video_encoder_ = pp::VideoEncoder(this);

    int32_t error = video_encoder_.Initialize(
        PP_VIDEOFRAME_FORMAT_I420, pp::Size(640, 480),
        PP_VIDEOPROFILE_H264BASELINE, 2000000,
        PP_HARDWAREACCELERATION_WITHFALLBACK,
        callback_factory_.NewCallback(
            &GoLiveInstance::OnInitializedEncoder));
    if (error != PP_OK_COMPLETIONPENDING) {
      std::stringstream ss;
      ss << "Cannot initialize encoder: " << error;
      Log(pp::Var(ss.str()));
      return;
    }

    /*bg_thread_.message_loop().PostWork(
      callback_factory_.NewCallback(&GoLiveInstance::InitializeRtmp)
    );*/

    video_track_.GetFrame(callback_factory_.NewCallbackWithOutput(
      &GoLiveInstance::OnFrame
    ));
  }

  void OnInitializedEncoder(int32_t res) {
    if (res != PP_OK) {
      LogError(res, "Encoder initialization failed: ");
      return;
    }

    video_encoder_.GetBitstreamBuffer(
      callback_factory_.NewCallbackWithOutput(
        &GoLiveInstance::OnGetBitstreamBuffer
      )
    );

    ScheduleNextEncode();
  }

  void OnGetBitstreamBuffer(int32_t result, PP_BitstreamBuffer buffer) {
    if (result != PP_OK) {
      LogError(result, "Cannot get bitstream buffer: ");
      return;
    }
    file_offset_ += buffer.size;
    if (bitstream_buf_.end_pos + buffer.size >= BitstreamBuf::BUFSIZE) {
      Log("Internal buffer full");
      return;
    }
    uint8_t* bufstart = bitstream_buf_.buf + bitstream_buf_.end_pos;
    memcpy(bufstart, buffer.buffer, buffer.size);
    bitstream_buf_.end_pos += buffer.size;


    if (nb_frames_++ == 0) {
      int ret = avformat_open_input(&av_fmt_ictx_, "", nullptr, nullptr);
      if (ret != 0) {
        LogError(ret, av_err2str(ret));
      }
    }

    if (nb_frames_ == 30) {
      int ret = avformat_find_stream_info(av_fmt_ictx_, nullptr);
      if (ret != 0) {
        LogError(ret, av_err2str(ret));
      } else {
        av_dump_format(av_fmt_ictx_, 0, "screen", 0);
        bg_thread_.message_loop().PostWork(
          callback_factory_.NewCallback(&GoLiveInstance::InitializeRtmp)
        );
      }
    }

    video_encoder_.RecycleBitstreamBuffer(buffer);
    video_encoder_.GetBitstreamBuffer(
      callback_factory_.NewCallbackWithOutput(
        &GoLiveInstance::OnGetBitstreamBuffer
      )
    );
  }

  void InitializeRtmp(int32_t res) {
    const char* url = "rtmp://localhost/live/test";
    if (res != PP_OK) {
      LogError(res, "error scheduling rtmp initialization");
      return;
    }
    Log("initializing rtmp");
    AVOutputFormat* ofmt = av_guess_format("flv", nullptr, nullptr);
    int ret = avformat_alloc_output_context2(
      &av_fmt_octx_,
      ofmt,
      nullptr,
      url
    );
    if (ret < 0) {
      LogError(ret, av_err2str(ret));
      return;
    }

    ret = avio_open(
      &av_fmt_octx_->pb,
      url,
      AVIO_FLAG_READ_WRITE
    );
    if (ret < 0) {
      LogError(ret, av_err2str(ret));
      LogError(errno, "errno: ");
      return;
    }

    AVStream* stream = avformat_new_stream(av_fmt_octx_, av_fmt_ictx_->video_codec);
    avcodec_copy_context(stream->codec, av_fmt_ictx_->streams[0]->codec);
    if (ofmt->flags & AVFMT_GLOBALHEADER) {
      stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
    }
    stream->codec->time_base = stream->time_base = (AVRational){1, 30};
    stream->codec->gop_size = 15;
    avformat_write_header(av_fmt_octx_, nullptr);
    av_dump_format(av_fmt_octx_, 0, url, 1);

    bg_thread_.message_loop().PostWork(
      callback_factory_.NewCallback(&GoLiveInstance::RtmpTick));
  }

  void RtmpTick(int32_t) {
    AVPacket pkt;
    av_init_packet(&pkt);
    int ret = av_read_frame(av_fmt_ictx_, &pkt);
    if (ret < 0) {
      av_free_packet(&pkt);
      if (ret == AVERROR_EOF) {
        bg_thread_.message_loop().PostWork(
          callback_factory_.NewCallback(&GoLiveInstance::RtmpTick), 100);
      } else {
        LogError(ret, av_err2str(ret));
      }
      return;
    }
    static size_t pts = 0;
    pkt.pts = pkt.dts = pts++;
    av_packet_rescale_ts(
      &pkt,
      av_fmt_ictx_->streams[pkt.stream_index]->codec->time_base,
      av_fmt_octx_->streams[pkt.stream_index]->codec->time_base
    );
    ret = av_interleaved_write_frame(av_fmt_octx_, &pkt);
    if (ret < 0) {
      LogError(ret, av_err2str(ret));
    }
    av_free_packet(&pkt);
    bg_thread_.message_loop().PostWork(
      callback_factory_.NewCallback(&GoLiveInstance::RtmpTick));
  }

  void ScheduleNextEncode() {
    // Avoid scheduling more than once at a time.
    if (is_encode_ticking_) {
      return;
    }

    PP_Time now = pp::Module::Get()->core()->GetTime();
    PP_Time tick = 1.0 / 30;
    // If the callback was triggered late, we need to account for that
    // delay for the next tick.
    PP_Time delta = tick - clamp(0, tick, now - last_encode_tick_ - tick);

    pp::Module::Get()->core()->CallOnMainThread(
        delta * 1000,
        callback_factory_.NewCallback(&GoLiveInstance::GetEncoderFrameTick),
        0);

    last_encode_tick_ = now;
    is_encode_ticking_ = true;
  }

  void GetEncoderFrameTick(int32_t result) {
    is_encode_ticking_ = false;

    if (!current_track_frame_.is_null()) {
      pp::VideoFrame frame = current_track_frame_;
      current_track_frame_.detach();
      GetEncoderFrame(frame);
    }
    ScheduleNextEncode();

  }

  void GetEncoderFrame(const pp::VideoFrame& track_frame) {
    video_encoder_.GetVideoFrame(callback_factory_.NewCallbackWithOutput(
        &GoLiveInstance::OnEncoderFrame, track_frame));
  }

  void OnEncoderFrame(
    int32_t result,
    pp::VideoFrame encoder_frame,
    pp::VideoFrame track_frame
  ) {
    if (result == PP_ERROR_ABORTED) {
      video_track_.RecycleFrame(track_frame);
      Log("Encoding aborted");
      return;
    }
    if (result != PP_OK) {
      video_track_.RecycleFrame(track_frame);
      LogError(result, "Cannot get video frame from video encoder");
      return;
    }

    if (CopyVideoFrame(encoder_frame, track_frame) == PP_OK)
      EncodeFrame(encoder_frame);
    video_track_.RecycleFrame(track_frame);
  }

  int32_t CopyVideoFrame(pp::VideoFrame dest, pp::VideoFrame src) {
    dest.SetTimestamp(src.GetTimestamp());
    /*int ret = av_frame_make_writable(current_av_frame_);
    if (ret < 0) {
      LogError(ret, av_err2str(ret));
      Log("dropped frame");
      return -1;
    }
    current_av_frame_->format = AV_PIX_FMT_YUV420P;
    pp::Size frame_size;
    src.GetSize(&frame_size);
    current_av_frame_->width = frame_size.width();
    current_av_frame_->height = frame_size.height();
    current_av_frame_->pts = lround(src.GetTimestamp() * 1000);
    memcpy(current_av_frame_->data, src.GetDataBuffer(), src.GetDataBufferSize());
*/
    memcpy(dest.GetDataBuffer(), src.GetDataBuffer(), src.GetDataBufferSize());
    return PP_OK;
  }

  void EncodeFrame(const pp::VideoFrame& frame) {
    /*AVPacket pkt;
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
      LogError(ret, av_err2str(ret));
      return;
    }
    if (got_packet != 0) {
      size_t stream_index = 0;
      av_packet_rescale_ts(&pkt, cctx->time_base, av_fmt_octx_->streams[stream_index]->time_base);
      pkt.stream_index = stream_index;
      ret = av_interleaved_write_frame(av_fmt_octx_, &pkt);
      if (ret < 0) {
        Log("Error during writing encoded frame");
        LogError(ret, av_err2str(ret));
        return;
      }
    }*/
    video_encoder_.Encode(
      frame, PP_FALSE,
      callback_factory_.NewCallback(&GoLiveInstance::OnEncodeDone)
    );
  }

  void OnEncodeDone(int32_t result) {
    if (result != PP_OK) {
      LogError(result, "Encode failed: ");
      return;
    }
  }

  void NoOp(int32_t) const {}

  void OnFrame(int32_t result, pp::VideoFrame frame) {
    if (!current_track_frame_.is_null()) {
      video_track_.RecycleFrame(current_track_frame_);
      current_track_frame_.detach();
    }

    current_track_frame_ = frame;
    video_track_.GetFrame(callback_factory_.NewCallbackWithOutput(
      &GoLiveInstance::OnFrame
    ));
  }

  void Log(const pp::Var& msg) {
    PostMessage(msg);
  }

  template <typename T> void LogError(const T& error, const std::string& msg) {
    std::stringstream ss;
    ss << msg << error;
    Log(pp::Var(ss.str()));
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
   static_log_ctx.inst->PostMessage(pp::Var(std::string(line)));
 }
}
