#include <stdio.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <kj/debug.h>
#include <kj/io.h>
#include <syscall.h>
#include <linux/futex.h>
#include <kj/thread.h>
#include <kj/main.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <sys/ioctl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
}

#include "client.html.h"

namespace kvmonitor {

// =======================================================================================
// utility code for KJ+ffmpeg

const int LINESIZE_ALIGNMENT = 32;   // idk, internet told me so?

static const uint8_t OPUS_DEFAULT_EXTRADATA[19] = {
  'O', 'p', 'u', 's', 'H', 'e', 'a', 'd',
  1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

kj::String avError(int error) {
  char buffer[4096];
  av_strerror(error, buffer, sizeof(buffer));
  buffer[sizeof(buffer)-1] = 0;  // unsure if necessary, docs don't say
  return kj::str(buffer);
}

kj::StringPtr avError(const void*) {
  return "returned null"_kj;
}

bool isAvError(int e) { return e < 0; }
bool isAvError(const void* ptr) { return ptr == nullptr; }

#define KJ_AVCALL(code, ...) \
  [&]() { \
    auto r = code; \
    if (isAvError(r)) { \
      KJ_FAIL_ASSERT(#code, r, avError(r), ##__VA_ARGS__); \
    } \
    return r; \
  }()

template <typename T, void (*freeFunc)(T**)>
kj::Own<T> avOwn(T* obj) {
  class Disposer final: public kj::Disposer {
    void disposeImpl(void* pointer) const override {
      T* ptr = reinterpret_cast<T*>(pointer);
      freeFunc(&ptr);
    }
  };
  static const Disposer disposer;

  return kj::Own<T>(obj, disposer);
}

// These don't follow the usual pattern...
#define avformat_context_alloc avformat_alloc_context
inline void avformat_context_free(AVFormatContext** s) { avformat_free_context(*s); s = nullptr; }

#define KJ_AVALLOC(type, ...) \
  avOwn<kj::Decay<decltype(*type##_alloc())>, &type##_free>(KJ_AVCALL(type##_alloc(), ##__VA_ARGS__))


class AvBufferDisposer final: public kj::ArrayDisposer {
  void disposeImpl(void* firstElement, size_t elementSize, size_t elementCount,
                   size_t capacity, void (*destroyElement)(void*)) const override {
    av_free(firstElement);
  }
};
const AvBufferDisposer avBufferDisposer KJ_UNUSED;

template <typename T>
kj::Array<T> avAllocBuffer(size_t size) {
  T* result = reinterpret_cast<T*>(av_malloc(size * sizeof(T)));
  return kj::Array<T>(result, size, avBufferDisposer);
}

// =======================================================================================

using kj::byte;

class RingBuffer {
  // A RingBuffer meant for pulling real-time data.

public:
  explicit RingBuffer(size_t size)
      : buffer(kj::heapArray<byte>(size)) {}

  uint64_t read(uint64_t offset, uint64_t maxLag, kj::ArrayPtr<byte> bytes, bool mix) {
    // Read starting no earlier than `minOffset`, and no later than the current end minus
    // `maxLag`. Fill the whole buffer before returning, blocking if necessary.

    uint64_t end = __atomic_load_n(&atomicEnd, __ATOMIC_ACQUIRE);

    // maxLag should be no more than half the buffer size, to avoid re-copying bytes that are
    // already been overwritten.
    maxLag = kj::min(maxLag, buffer.size() / 2);

    // Skip offset forward if it's too far behind.
    offset = kj::max(offset, end - kj::min(maxLag, end));

    size_t pos = 0;
    while (pos < bytes.size()) {
      end = __atomic_load_n(&atomicEnd, __ATOMIC_ACQUIRE);

      if (offset >= end) {
        // Wait for data to become available.
        //
        // Technically futex() only looks at 32-bit values. That's OK, there's no way the lower
        // 32 bits are going to wrap around in the amount of time it takes us to call futex().
        KJ_NONBLOCKING_SYSCALL(syscall(
            SYS_futex, &atomicEnd, FUTEX_WAIT, (uint32_t)end, nullptr, nullptr, 0));
      } else {
        // Data available in buffer. Copy it.

        // Amount to copy = rest of available data OR space in output buffer, whichever is less.
        size_t amountToCopy = kj::min(end - offset, bytes.size() - pos);

        // Calcualte starting point in the ring buffer.
        size_t startAt = offset % buffer.size();

        // Don't copy past the end of the current ring buffer. Clamp to end.
        amountToCopy = kj::min(buffer.size() - startAt, amountToCopy);

        if (mix) {
          float* __restrict__ outPtr = reinterpret_cast<float*>(bytes.begin() + pos);
          const float* __restrict__ inPtr = reinterpret_cast<const float*>(buffer.begin() + startAt);
          size_t n = amountToCopy / 4;

          for (auto i: kj::zeroTo(n)) {
            outPtr[i] += inPtr[i];
          }
        } else {
          memcpy(bytes.begin() + pos, buffer.begin() + startAt, amountToCopy);
        }

        offset += amountToCopy;
        pos += amountToCopy;
      }
    }

    return offset;
  }

  void write(kj::ArrayPtr<byte> bytes) {
    // Technically doesn't need to be an atomic read since no one else writes this.
    uint64_t offset = __atomic_load_n(&atomicEnd, __ATOMIC_RELAXED);

    // Shouldn't be writing an excessive amount at once...
    KJ_REQUIRE(bytes.size() < buffer.size() / 2);

    while (bytes.size() > 0) {
      size_t startAt = offset % buffer.size();
      size_t amountToCopy = kj::min(buffer.size() - startAt, bytes.size());

      memcpy(buffer.begin() + startAt, bytes.begin(), amountToCopy);

      offset += amountToCopy;
      bytes = bytes.slice(amountToCopy);
    }

    __atomic_store_n(&atomicEnd, offset, __ATOMIC_RELEASE);

    // Wake any waiters.
    KJ_SYSCALL(syscall(SYS_futex, &atomicEnd, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
  }

private:
  kj::Array<byte> buffer;
  uint64_t atomicEnd = 0;
};

// =======================================================================================

void readStream(kj::StringPtr inputUrl, RingBuffer& ringBuffer) {
  // Open RTSP stream
  // ANNOYING: avformat_open_input() wants to allocate the context for us. We can allocate our
  //   own but avformat_open_input() will FREE it on error, wtf.
  AVFormatContext* formatCtx = nullptr;
  KJ_AVCALL(avformat_open_input(
      &formatCtx, inputUrl.cStr(), NULL, NULL));
  KJ_DEFER(avformat_free_context(formatCtx));

  // Find the video substream.
  int stream_index = -1;
  for (auto stream: kj::arrayPtr(formatCtx->streams, formatCtx->nb_streams)) {
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO &&
        stream->codecpar->codec_id == AV_CODEC_ID_OPUS) {
      stream_index = stream->index;
    } else {
      stream->discard = AVDISCARD_ALL;
    }
  }
  KJ_ASSERT(stream_index != -1);
  auto stream = formatCtx->streams[stream_index];

//  KJ_AVCALL(avformat_find_stream_info(formatCtx, NULL));
  // avformat_find_stream_info() delays several seconds, but apparently the only thing we get out
  // of it is that stream->codecpar->format becomes initialized. Let's just do that ourselves?
  stream->codecpar->format = AV_SAMPLE_FMT_FLTP;

  // Start playing the RTSP stream.
  KJ_AVCALL(av_read_play(formatCtx));

  // Get codec for stream.
  const AVCodec *codec = KJ_AVCALL(avcodec_find_decoder(stream->codecpar->codec_id));

  // Create a decoding context, initialized with the same parameters (sample rate, etc.)
  AVCodecContext* codecCtx = KJ_AVCALL(avcodec_alloc_context3(codec));
  KJ_DEFER(avcodec_free_context(&codecCtx));

  KJ_AVCALL(avcodec_parameters_to_context(codecCtx, stream->codecpar));
  KJ_AVCALL(avcodec_open2(codecCtx, codec, NULL));

  // Allocate a frame buffer.
  kj::Own<AVFrame> frame = KJ_AVALLOC(av_frame);
  KJ_AVCALL(av_samples_alloc(
      frame->data, frame->linesize, stream->codecpar->ch_layout.nb_channels,
      stream->codecpar->sample_rate / 50,  // seems to be ignored?
      (AVSampleFormat)stream->codecpar->format, LINESIZE_ALIGNMENT));
  KJ_DEFER(av_freep(&frame->data[0]));

  KJ_DEFER(KJ_AVCALL(av_read_pause(formatCtx)));

  uint64_t totalSamples = 0;
  auto& clock = kj::systemCoarseMonotonicClock();
  auto startTime = clock.now();

  for (;;) {
    AVPacket* packet = av_packet_alloc();
    KJ_DEFER(av_packet_unref(packet));
    KJ_AVCALL(av_read_frame(formatCtx, packet));

    // Ignore the packet if it isn't from the video substream.
    if (packet->stream_index == stream_index) {
      // Deliver the packet to the codec for decoding.
      KJ_AVCALL(avcodec_send_packet(codecCtx, packet));

      for (;;) {
        // Ask codec to deliver a frame.
        int result = avcodec_receive_frame(codecCtx, frame);
        if (result == AVERROR(EAGAIN)) {
          // No more frames available.
          break;
        } else if (result < 0) {
          KJ_FAIL_ASSERT("avcodec_receive_frame()", result, avError(result));
        }

        auto samples = kj::arrayPtr(reinterpret_cast<float*>(frame->data[0]), frame->nb_samples);
        ringBuffer.write(samples.asBytes());

        totalSamples += samples.size();
      }
    }

    {
      auto dataDuration = totalSamples * 1000 / stream->codecpar->sample_rate * kj::MILLISECONDS;
      auto actualDuration = clock.now() - startTime;

      if (actualDuration + 5 * kj::SECONDS < dataDuration ||
          dataDuration + 5 * kj::SECONDS < actualDuration) {
        KJ_FAIL_ASSERT("camera stream time has drifetd from real time",
            dataDuration, actualDuration);
      }
    }
  }
}

// =======================================================================================

void writeStream(kj::ArrayPtr<RingBuffer> ringBuffers, kj::OutputStream& out,
                 kj::StringPtr filename) {
  AVFormatContext* formatCtx = KJ_AVCALL(avformat_alloc_context());
  KJ_DEFER(avformat_free_context(formatCtx));

  struct State {
    kj::OutputStream& out;
    kj::Vector<byte> outputBuffer;
    bool disconnected;
  };
  State state {out, kj::Vector<byte>(65536), false};

  formatCtx->pb = KJ_AVCALL(avio_alloc_context(
      reinterpret_cast<byte*>(av_malloc(65536)),
      65536, 1, &state,
      nullptr,
      [](void* opaque, uint8_t* buf, int size) -> int {
        auto& state = *reinterpret_cast<State*>(opaque);
        if (!state.disconnected) {
          try {
            state.outputBuffer.addAll(kj::arrayPtr(buf, size));
            //state.out.write(kj::arrayPtr(buf, size));
          } catch (...) {
            auto e = kj::getCaughtExceptionAsKj();
            if (e.getType() == kj::Exception::Type::DISCONNECTED) {
              KJ_DBG("disconnect");
              state.disconnected = true;
            } else {
              kj::throwFatalException(kj::mv(e));
            }
          }
        }
        return size;
      },
      nullptr));

  KJ_DEFER(avio_context_free(&formatCtx->pb));

  // Docs say buffer must be freed, but also the library may re-allocate it at any time, so it
  // may not even be the original pointer we passed to avio_alloc_context()? WTF?
  KJ_DEFER(av_free(formatCtx->pb->buffer));

  AVCodecID codecId = AV_CODEC_ID_OPUS;
  if (filename.endsWith(".aac")) {
    // TODO: AAC format doesn't seem to work. ffmpeg produces some sort of invalid output. I
    // can't figure out why. So for now we make iPhone use mp3 (which sucks).
    codecId = AV_CODEC_ID_AAC;
  } else if (filename.endsWith(".mp3")) {
    codecId = AV_CODEC_ID_MP3;
  }

  const AVCodec *codec = KJ_AVCALL(avcodec_find_encoder(codecId));

  AVChannelLayout layout = AV_CHANNEL_LAYOUT_MONO;

  AVStream* stream = avformat_new_stream(formatCtx, codec);
  stream->codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
  stream->codecpar->sample_rate = 48000;
  av_channel_layout_copy(&stream->codecpar->ch_layout, &layout);
  stream->codecpar->format = AV_SAMPLE_FMT_FLT;
  stream->codecpar->codec_id = codecId;
  if (codecId == AV_CODEC_ID_OPUS) {
    stream->codecpar->bit_rate = 64000;
    stream->codecpar->extradata = reinterpret_cast<uint8_t*>(av_malloc(19));
    stream->codecpar->extradata_size = 19;
    memcpy(stream->codecpar->extradata, OPUS_DEFAULT_EXTRADATA, 19);
    stream->codecpar->extradata[9] = layout.nb_channels;  // REALLY???
  } else if (codecId == AV_CODEC_ID_AAC) {
    stream->codecpar->bit_rate = 0;

    // This is the extradata my camera gives me idk.
    stream->codecpar->extradata = reinterpret_cast<uint8_t*>(av_malloc(2));
    stream->codecpar->extradata[0] = 17;
    stream->codecpar->extradata[1] = 136;
  } else if (codecId == AV_CODEC_ID_MP3) {
    stream->codecpar->bit_rate = 64000;
  }
  stream->time_base = {1, stream->codecpar->sample_rate};

  formatCtx->oformat = KJ_AVCALL(av_guess_format(nullptr, filename.cStr(), nullptr));

  // Does this matter?
  formatCtx->url = KJ_AVCALL(av_strdup(filename.cStr()));

  KJ_AVCALL(avformat_write_header(formatCtx, nullptr));

  AVCodecContext* codecCtx = KJ_AVCALL(avcodec_alloc_context3(codec));
  KJ_DEFER(avcodec_free_context(&codecCtx));

  // Copy params from stream.
  avcodec_parameters_to_context(codecCtx, stream->codecpar);

  // ffmpeg changes stream->time_base to 1/1000 during avformat_write_header. If we use that here,
  // nothing works. We have to set it back to 1/48000.
  codecCtx->time_base = {1, codecCtx->sample_rate};
  codecCtx->pkt_timebase = codecCtx->time_base;

  // You have to copy this bit over for some encoders. For whatever reasons, ffmpeg will not
  // figure it out for you?
  if (formatCtx->oformat->flags & AVFMT_GLOBALHEADER) {
    codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  }

  KJ_AVCALL(avcodec_open2(codecCtx, codec, nullptr));

  // Allocate a frame buffer.
  kj::Own<AVFrame> frame = KJ_AVALLOC(av_frame);
  frame->nb_samples = codecCtx->sample_rate / 50;
  KJ_AVCALL(av_samples_alloc(
      frame->data, frame->linesize, codecCtx->ch_layout.nb_channels,
      frame->nb_samples,
      codecCtx->sample_fmt, LINESIZE_ALIGNMENT));
  KJ_DEFER(av_freep(&frame->data[0]));

  frame->ch_layout = codecCtx->ch_layout;
  frame->sample_rate = codecCtx->sample_rate;
  frame->format = codecCtx->sample_fmt;
  frame->pts = 0;
  frame->time_base = {1, codecCtx->sample_rate};

  uint64_t inputOffsets[ringBuffers.size()];
  for (auto i: kj::zeroTo(ringBuffers.size())) {
    inputOffsets[i] = 0;
  }

  uint64_t totalSamples = 0;
  uint samplesSinceFlush = 0;
  for (uint counter = 0; !state.disconnected; counter++) {
    if (codecId == AV_CODEC_ID_AAC && counter >= 100) {
      // TODO: I'm only returning 1s of audio for AAC for debugging reasons... once it actually
      // works, remove the above.
      KJ_AVCALL(avcodec_send_frame(codecCtx, nullptr));
    } else {
      auto samples = kj::arrayPtr(reinterpret_cast<float*>(frame->data[0]), frame->nb_samples);
      for (auto i: kj::zeroTo(ringBuffers.size())) {
        // Don't get more than 1s behind.
        size_t maxLag = inputOffsets[i] == 0 ? 0 : frame->sample_rate * sizeof(float);
        inputOffsets[i] = ringBuffers[i].read(inputOffsets[i], maxLag, samples.asBytes(), i > 0);
      }

      KJ_AVCALL(avcodec_send_frame(codecCtx, frame));

      totalSamples += frame->nb_samples;
      samplesSinceFlush += frame->nb_samples;
      frame->pts = totalSamples * frame->time_base.den / frame->sample_rate;
    }

    // Note to self: To trigger EOF, do:
    //   KJ_AVCALL(avcodec_send_frame(codecCtx, nullptr));

    for (;;) {
      // Ask codec to deliver a packet.
      AVPacket* packet = av_packet_alloc();
      KJ_DEFER(av_packet_unref(packet));
      int result = avcodec_receive_packet(codecCtx, packet);
      if (result == AVERROR(EAGAIN)) {
        // No more packets available.
        break;
      } else if (result == AVERROR_EOF) {
        KJ_AVCALL(av_write_trailer(formatCtx));
        return;
      } else if (result < 0) {
        KJ_FAIL_ASSERT("avcodec_receive_frame()", result, avError(result));
      }

      KJ_AVCALL(av_write_frame(formatCtx, packet));
    }

    if (samplesSinceFlush > frame->sample_rate / 5) {
      state.out.write(state.outputBuffer.asPtr());
      state.outputBuffer.resize(0);
      samplesSinceFlush = 0;
    }
  }
}

// =======================================================================================

class HttpServiceImpl: public kj::HttpService {
public:
  HttpServiceImpl(kj::HttpHeaderTable& headerTable): headerTable(headerTable) {}

  kj::Promise<void> request(
      kj::HttpMethod method, kj::StringPtr url, const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody, Response& response) override {
    KJ_DBG(url);
    if (url != "/"_kj) {
      co_await response.sendError(404, "Not Found", kj::HttpHeaders(headerTable));
      co_return;
    }
    if (method != kj::HttpMethod::GET && method != kj::HttpMethod::HEAD) {
      co_await response.sendError(501, "Method Not Implemented", kj::HttpHeaders(headerTable));
      co_return;
    }

    kj::ArrayPtr<const byte> bytes = CLIENT_HTML.asBytes();
    kj::StringPtr contentType = "text/html;charset=UTF-8"_kj;

    kj::HttpHeaders responseHeaders(headerTable);
    responseHeaders.set(kj::HttpHeaderId::CONTENT_TYPE, contentType);
    auto stream = response.send(200, "OK", responseHeaders, bytes.size());
    co_await stream->write(bytes.begin(), bytes.size());
  }

private:
  kj::HttpHeaderTable& headerTable;
};

// =======================================================================================

class MonitorMain: private kj::TaskSet::ErrorHandler {
public:
  MonitorMain(kj::ProcessContext& context): context(context) {}

  kj::MainFunc getMain() {
    return kj::MainBuilder(context, "kvmonitor", "Kenton's Baby Monitor")
        .expectArg("<listen-addr>", KJ_BIND_METHOD(*this, setListenAddr))
        .expectOneOrMoreArgs("<camera-url>", KJ_BIND_METHOD(*this, addCamera))
        .callAfterParsing(KJ_BIND_METHOD(*this, run))
        .build();
  }

  kj::MainBuilder::Validity setListenAddr(kj::StringPtr arg) {
    listenAddr = arg;
    return true;
  }

  kj::MainBuilder::Validity addCamera(kj::StringPtr arg) {
    cameras.add(arg);
    return true;
  }

  kj::MainBuilder::Validity run() noexcept {
    signal(SIGPIPE, SIG_IGN);

    auto ringBuffers = ({
      auto builder = kj::heapArrayBuilder<RingBuffer>(cameras.size());
      for (auto i KJ_UNUSED: kj::indices(cameras)) {
        builder.add(1 << 20);
      }
      builder.finish();
    });

    auto cameraThreads = ({
      auto builder = kj::heapArrayBuilder<kj::Thread>(cameras.size());
      for (auto i: kj::indices(cameras)) {
        builder.add([url = cameras[i], &ringBuffer = ringBuffers[i]]() noexcept {
          for (;;) {
            try {
              readStream(url, ringBuffer);
            } catch (...) {
              KJ_LOG(ERROR, "camera stream failed, retrying in 5 seconds",
                  url, kj::getCaughtExceptionAsKj());
              sleep(5);
            }
          }
        });
      }
      builder.finish();
    });

    auto io = kj::setupAsyncIo();

    kj::HttpHeaderTable headerTable;
    HttpServiceImpl httpService(headerTable);

    kj::HttpServer server(io.provider->getTimer(), headerTable, httpService);

    auto listener = io.provider->getNetwork().parseAddress(listenAddr).wait(io.waitScope)
        ->listen();

    kj::TaskSet tasks(*this);

    auto handleConnection = [&](kj::Own<kj::AsyncIoStream> conn) -> kj::Promise<void> {
      auto suspendCallback = [&](kj::HttpServer::SuspendableRequest& req)
          -> kj::Maybe<kj::Own<kj::HttpService>> {
        auto method = req.method.tryGet<kj::HttpMethod>().orDefault(kj::HttpMethod::COPY);
        if ((req.url == "/stream.ogg" || req.url == "/stream.webm" ||
             req.url == "/stream.aac" || req.url == "/stream.mp3") &&
            method == kj::HttpMethod::GET) {
          auto filename = kj::str(req.url.slice(1));

          // Requesting the stream. Hand off to a thread.
          auto suspended = req.suspend();

          int fd_;
          KJ_SYSCALL(fd_ = dup(KJ_ASSERT_NONNULL(conn->getFd())));
          kj::AutoCloseFd sock(fd_);

          kj::Thread([sock = kj::mv(sock), ringBuffers = ringBuffers.asPtr(),
                      filename = kj::mv(filename)]() mutable noexcept {
            try {
              // Turn off non-blocking I/O.
              {
                int opt = 0;
                KJ_SYSCALL(ioctl(sock, FIONBIO, &opt));
              }

              kj::FdOutputStream out(kj::mv(sock));
              auto headers = kj::str(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: audio/ogg\r\n"
                  "\r\n");
              out.write(headers.asBytes());
              writeStream(ringBuffers, out, filename);
            } catch (...) {
              KJ_LOG(ERROR, kj::getCaughtExceptionAsKj());
            }
          }).detach();

          return kj::none;
        } else {
          return kj::attachRef(httpService);
        }
      };

      co_await server.listenHttpCleanDrain(*conn, kj::mv(suspendCallback));
    };

    KJ_DBG("awaiting connections...");

    for (;;) {
      auto conn = listener->accept().wait(io.waitScope);
      tasks.add(handleConnection(kj::mv(conn)));
    }
  }

private:
  kj::ProcessContext& context;
  kj::StringPtr listenAddr;
  kj::Vector<kj::StringPtr> cameras;

  void taskFailed(kj::Exception&& exception) override {
    KJ_LOG(ERROR, exception);
  }
};

}  // namespace kvmonitor

KJ_MAIN(kvmonitor::MonitorMain);
