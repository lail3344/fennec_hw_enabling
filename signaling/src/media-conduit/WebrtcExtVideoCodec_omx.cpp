/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CSFLog.h"
#include "nspr.h"

#include <iostream>

#include <mozilla/Scoped.h>
#include "VideoConduit.h"
#include "AudioConduit.h"

#include "../../../trunk/webrtc/video_engine/include/vie_external_codec.h"

#include "runnable_utils.h"
#include "WebrtcExtVideoCodec.h"

#include "mozilla/Monitor.h"


//#include "MediaDefs.h"
#include <stagefright/MetaData.h>
#include <stagefright/OMXCodec.h>
#include <stagefright/OMXClient.h>

using namespace android;

#define EXT_VIDEO_PLAYLOAD_NAME "VP8"
#define EXT_VIDEO_FRAME_WIDTH 640
#define EXT_VIDEO_FRAME_HEIGHT 480
#define EXT_VIDEO_MAX_FRAMERATE 30

// for debugging
#define DUMP_FRAMES 0
#include <cstdio>
//#define LOG_TAG "WebrtcExtVideoCodec"
#include <utils/Log.h>

// for experiment
//#include <cutils/properties.h>

namespace mozilla {

static const char* logTag ="WebrtcExtVideoCodec";

// MediaSource
class WebrtcMediaSource: public MediaSource, public MediaBufferObserver {
public:
  WebrtcMediaSource(sp<MetaData> format):
      format_(format),
      monitor_("WebrtcMediaSource"),
      codecPaused_(false), dummyBuffer_(new MediaBuffer(1)) {}
  virtual ~WebrtcMediaSource() {
    dummyBuffer_->setObserver(0);
    dummyBuffer_->release();
  }
  virtual sp<MetaData> getFormat() { return format_; }
  virtual status_t start(MetaData *params = nullptr) { /* TODO */ return OK; }
  virtual status_t stop() {
    MonitorAutoLock lock(monitor_);
    while (!buffers_.empty()) {
      // FIXME: Leak! Must figure out why deleting will cause SIGSEGV
      // MediaBuffer* front = buffers_.front();
      //RUN_ON_THREAD(thread_,
      //  WrapRunnable(front, &MediaBuffer::release),
      //  NS_DISPATCH_NORMAL);

      buffers_.pop();
    }
    return OK;
  }

  virtual status_t read(MediaBuffer **buffer,
      const ReadOptions *options = nullptr) {
    if (buffer) {
      CSFLogDebug(logTag,  "%s: read buffer ", __FUNCTION__);
      *buffer = getBuffer();
      if (!*buffer) {
        if (!codecPaused_) {
          codec_->pause();
          codecPaused_ = true;
        }
        *buffer = dummyBuffer_;
        dummyBuffer_->setObserver(this);
        dummyBuffer_->add_ref();
        //return ERROR_END_OF_STREAM;
      }
    }
    return OK;
  }

  virtual void signalBufferReturned(MediaBuffer* buffer) {
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    if (buffer == dummyBuffer_) {
      return;
    }
//    MediaBuffer* front = getBuffer();
    MOZ_ASSERT(buffer == front);
    MonitorAutoLock lock(monitor_);
    buffers_.pop();

    buffer->setObserver(0);
    // FIXME: Leak! Must figure out why deleting will cause SIGSEGV
    //RUN_ON_THREAD(thread_,
    //  WrapRunnable(front, &MediaBuffer::release),
    //  NS_DISPATCH_NORMAL);

    delete [] buffer->data();
  }

  virtual void pushBuffer(void* data, size_t size, uint32_t timestamp) {
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    MediaBuffer* buffer = new MediaBuffer(data, size);

    buffer->setObserver(this);
    buffer->add_ref();
    buffer->meta_data()->setInt64(kKeyTime, timestamp);

    if (codecPaused_) {
      codec_->start();
      codecPaused_ = false;
    }

    MonitorAutoLock lock(monitor_);
    buffers_.push(buffer);
    lock.Notify();
  }

  void setCodec(MediaSource* codec) {
    codec_ = codec;
  }

private:
//  nsCOMPtr<nsIThread> thread_;
  sp<MetaData> format_;

  std::queue<MediaBuffer*> buffers_;
  Monitor monitor_;
  virtual MediaBuffer* getBuffer() {
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    MonitorAutoLock lock(monitor_);
//    MediaBuffer* front = nullptr;
    if (buffers_.empty()) {
      // FIXME: remove experimental property after proper value is found
//      char timeout[32];
//      property_get("webrtc.read_timeout", timeout, "3000");
//      lock.Wait(PR_MillisecondsToInterval(atoi(timeout)));
      lock.Wait(PR_MillisecondsToInterval(3000));
    }
    return buffers_.empty()? nullptr:buffers_.front();
  }

  MediaSource* codec_;
  bool codecPaused_;
  MediaBuffer* dummyBuffer_;

  static const int FRAME_READ_TIMEOUT;
};

const int WebrtcMediaSource::FRAME_READ_TIMEOUT = PR_MillisecondsToInterval(3000); // TODO: find proper value

class WebrtcOMX {
public:
  WebrtcOMX(sp<MetaData> format) {
    source_ = new WebrtcMediaSource(format);
  }
  virtual ~WebrtcOMX() {}

  sp<MediaSource> omx_; // OMXCodec
  sp<WebrtcMediaSource> source_; // OMX input source
};

// TODO: eliminate global variable after implementing prpper lifecycle management code
static WebrtcOMX* omxEnc = nullptr;

// Encoder.
WebrtcExtVideoEncoder::WebrtcExtVideoEncoder()
  : timestamp_(0),
    callback_(nullptr),
    mutex_("WebrtcExtVideoEncoder"),
    encoder_(nullptr) {
  nsIThread* thread;
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  
  nsresult rv = NS_NewNamedThread("encoder-thread", &thread);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  thread_ = thread;

  memset(&encoded_image_, 0, sizeof(encoded_image_));
  LOGV("WebrtcExtVideoEncoder::WebrtcExtVideoEncoder %p", this);
}

static MetaData* VideoCodecSettings2Metadata(
    const webrtc::VideoCodec* codecSettings) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  MetaData* enc_meta = new MetaData;

  enc_meta->setInt32(kKeyBitRate, codecSettings->maxBitrate * 1000); // kbps->bps

  // FIXME: get correct parameters from codecSettings
  
  // get from framewrok/av/media/libstagefright/MediaDefs.cpp
  enc_meta->setCString(kKeyMIMEType, "video/x-vnd.on2.vp8");
   
  // get from vendor/intel/hardware/omx-components/videocodec/OMXVideoEncoderVP8.cpp
//  enc_meta->setCString(kKeyMIMEType, "video/x-webm"); 
  enc_meta->setInt32(kKeyVideoProfile, 0x01);  // get from framework/native/include/media/openmax/OMX_VideoExt.h
  enc_meta->setInt32(kKeyVideoLevel, 0x08);
  enc_meta->setInt32(kKeyIFramesInterval, 10);

  enc_meta->setInt32(kKeyFrameRate, codecSettings->maxFramerate);
  enc_meta->setInt32(kKeyWidth, codecSettings->width);
  enc_meta->setInt32(kKeyHeight, codecSettings->height);
  enc_meta->setInt32(kKeyStride, codecSettings->width);
  enc_meta->setInt32(kKeySliceHeight, codecSettings->height);
  enc_meta->setInt32(kKeyBitRate, codecSettings->minBitrate * 1000); // kbps->bps

  enc_meta->setInt32(kKeyColorFormat, OMX_COLOR_FormatYUV420SemiPlanar);

  return enc_meta;
}

/*
 * Setup OMXCodec for Encode
 */
int32_t SetupOMX(sp<MetaData> enc_meta,
        WebrtcOMX* encoder) {
    OMXClient client;
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    MOZ_ASSERT(, "Cannot connect to media service");
    if (client.connect() != OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    uint32_t encoder_flags = 0;
    encoder_flags |= OMXCodec::kOnlySubmitOneInputBufferAtOneTime;

    sp<MediaSource> omx = OMXCodec::Create(
      client.interface(), enc_meta,
      true /* createEncoder */, encoder->source_,
      nullptr, encoder_flags);
    if (omx == nullptr) {
      encoder->source_->stop();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    encoder->omx_ = omx;
    encoder->source_->setCodec(omx.get());
    omx->start(enc_meta.get());

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoEncoder::InitEncode(
    const webrtc::VideoCodec* codecSettings,
    int32_t numberOfCores,
    uint32_t maxPayloadSize) {
  max_payload_size_ = maxPayloadSize;
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoEncoder::InitEncode %p", this);

  if (omxEnc == nullptr) { // FIXME: implement proper lifecycle management
    // FIXME: use input parameters
    webrtc::VideoCodec codec_inst;
    memset(&codec_inst, 0, sizeof(webrtc::VideoCodec));
    strncpy(codec_inst.plName, EXT_VIDEO_PLAYLOAD_NAME, 31);
    codec_inst.plType = 124;
    codec_inst.width = EXT_VIDEO_FRAME_WIDTH;
    codec_inst.height = EXT_VIDEO_FRAME_HEIGHT;

    // FIXME: remove experimental property
//    char bitrate[32];
//    property_get("webrtc.bitrate", bitrate, "300");
//    codec_inst.maxBitrate = atoi(bitrate);
    codec_inst.maxBitrate = 2000;

    codec_inst.maxFramerate = codecSettings->maxFramerate;

    sp<MetaData> enc_meta = VideoCodecSettings2Metadata(&codec_inst);
    omxEnc  = new WebrtcOMX(enc_meta);
    SetupOMX(enc_meta, omxEnc);
  }
  encoder_ = omxEnc;

  MutexAutoLock lock(mutex_);

  // TODO: eliminate extra pixel copy & color conversion
  size_t size = EXT_VIDEO_FRAME_WIDTH * EXT_VIDEO_FRAME_HEIGHT * 3 / 2;
  if (encoded_image_._size < size) {
    if (encoded_image_._buffer) {
      delete [] encoded_image_._buffer;
    }
    encoded_image_._buffer = new uint8_t[size];
    encoded_image_._size = size;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoEncoder::Encode(
    const webrtc::I420VideoFrame& inputImage,
    const webrtc::CodecSpecificInfo* codecSpecificInfo,
    const std::vector<webrtc::VideoFrameType>* frame_types) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  if (encoder_ == nullptr) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  
  CSFLogDebug(logTag,  "%s 1 ", __FUNCTION__);
  uint32_t time = PR_IntervalNow();
  WebrtcOMX* encoder = (reinterpret_cast<WebrtcOMX*>(encoder_));

  // TODO: eliminate extra pixel copy & color conversion
  size_t sizeY = inputImage.allocated_size(webrtc::kYPlane);
  size_t sizeUV = inputImage.allocated_size(webrtc::kUPlane);
  const uint8_t* u = inputImage.buffer(webrtc::kUPlane);
  const uint8_t* v = inputImage.buffer(webrtc::kVPlane);
  size_t size = sizeY + 2 * sizeUV;
  uint8_t* dstY = new uint8_t[size];
  uint16_t* dstUV = reinterpret_cast<uint16_t*>(dstY + sizeY);
  memcpy(dstY, inputImage.buffer(webrtc::kYPlane), sizeY);
  for (int i = 0; i < sizeUV;
      i++, dstUV++, u++, v++) {
    *dstUV = (*u << 8) + *v;
  }
  WebrtcMediaSource* source = encoder->source_.get();
  source->pushBuffer(static_cast<void*>(dstY), size, timestamp_);

  timestamp_ += 90000 / 30; // FIXME: use timestamp from camera

  EncodedFrame frame;
  frame.width_ = inputImage.width();
  frame.height_ = inputImage.height();
  frame.timestamp_ = timestamp_;
  CSFLogDebug(logTag,  "%s::Encode(%d) %dx%d -> %dx%d ", __FUNCTION__, PR_IntervalToMilliseconds(PR_IntervalNow()-time), inputImage.width(), inputImage.height(),
    frame.width_, frame.height_);

  LOGV("WebrtcExtVideoEncoder::Encode(%d) %dx%d -> %dx%d",
    PR_IntervalToMilliseconds(PR_IntervalNow()-time), inputImage.width(), inputImage.height(),
    frame.width_, frame.height_);

  MutexAutoLock lock(mutex_);
  frames_.push(frame);

  RUN_ON_THREAD(thread_,
    WrapRunnable(this, &WebrtcExtVideoEncoder::EmitFrames),
    NS_DISPATCH_NORMAL);

  return WEBRTC_VIDEO_CODEC_OK;
}

void WebrtcExtVideoEncoder::EmitFrames() {
  MutexAutoLock lock(mutex_);
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  while(!frames_.empty()) {
    EncodedFrame *frame = &frames_.front();
    EmitFrame(frame);
    frames_.pop();
  }
}

void WebrtcExtVideoEncoder::EmitFrame(EncodedFrame *frame) {
  MOZ_ASSERT(frame);
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  WebrtcOMX* encoder = (reinterpret_cast<WebrtcOMX*>(encoder_));
  MediaBuffer* buffer = nullptr;
  sp<MediaSource> omx = encoder->omx_;
  
  CSFLogDebug(logTag,  "%s 1", __FUNCTION__);
  
  if (omx->read(&buffer) != OK || buffer == nullptr) {
    return;
  }
  
  CSFLogDebug(logTag,  "%s 2", __FUNCTION__);
  
  encoded_image_._encodedWidth = frame->width_;
  encoded_image_._encodedHeight = frame->height_;
  encoded_image_._timeStamp = frame->timestamp_;
  encoded_image_.capture_time_ms_ = frame->timestamp_;

  sp<MetaData> meta = buffer->meta_data();
  int32_t isKey = false;
  if (meta.get() &&
      ((meta->findInt32(kKeyIsSyncFrame, &isKey) && isKey) ||
       (meta->findInt32(kKeyIsCodecConfig, &isKey) && isKey))) {
    encoded_image_._frameType = webrtc::kKeyFrame;
  } else {
    encoded_image_._frameType = webrtc::kDeltaFrame;
  }

  CSFLogDebug(logTag,  "%s 3", __FUNCTION__);

  encoded_image_._length = buffer->range_length();
  uint8_t* data = static_cast<uint8_t*>(buffer->data()) + buffer->range_offset();
  memcpy(encoded_image_._buffer, data, encoded_image_._length);
  encoded_image_._completeFrame = true;

  CSFLogDebug(logTag,  "%s 4", __FUNCTION__);
#if DUMP_FRAMES
  char path[127];
  sprintf(path, "/sdcard/omx-%012d.h264", frame->timestamp_);
  FILE* out = fopen(path, "w+");
  fwrite(data, encoded_image_._length, 1, out);
  fclose(out);
#endif

  buffer->release();
  CSFLogDebug(logTag,  "%s , WebrtcExtVideoEncoder::EmitFrame(%d) encoded len:%d", __FUNCTION__, encoded_image_._frameType, encoded_image_._length);
  LOGV("WebrtcExtVideoEncoder::EmitFrame(%d) encoded len:%d", encoded_image_._frameType, encoded_image_._length);
  callback_->Encoded(encoded_image_, nullptr, nullptr);
}

int32_t WebrtcExtVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  callback_ = callback;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoEncoder::Release() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoEncoder::Release %p", this);
  if (encoder_) {
    // FIXME: Leak! Should implement proper lifecycle management
    // WebrtcOMX* omx = reinterpret_cast<WebrtcOMX*>(encoder_);
    // delete omx;
    //encoder_ = nullptr;
  }

  MutexAutoLock lock(mutex_);
  if (encoded_image_._buffer) {
    delete [] encoded_image_._buffer;
    encoded_image_._buffer = nullptr;
    encoded_image_._size = 0;
  }

  return WEBRTC_VIDEO_CODEC_OK;
}

WebrtcExtVideoEncoder::~WebrtcExtVideoEncoder() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoEncoder::~WebrtcExtVideoEncoder %p", this);
}

// TODO
int32_t WebrtcExtVideoEncoder::SetChannelParameters(uint32_t packetLoss,
                                                           int rtt) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  return WEBRTC_VIDEO_CODEC_OK;
}

// TODO
int32_t WebrtcExtVideoEncoder::SetRates(uint32_t newBitRate,
                                               uint32_t frameRate) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  return WEBRTC_VIDEO_CODEC_OK;
}

// TODO: eliminate global variable after implementing prpper lifecycle management code
static WebrtcOMX* omxDec = nullptr;

// Decoder.
WebrtcExtVideoDecoder::WebrtcExtVideoDecoder()
    : callback_(nullptr),
      mutex_("WebrtcExtVideoDecoder") {
  nsIThread* thread;
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  nsresult rv = NS_NewNamedThread("decoder-thread", &thread);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  thread_ = thread;

  LOGV("WebrtcExtVideoDecoder::WebrtcExtVideoDecoder %p", this);
}

int32_t SetupOMXDec(sp<MetaData> dec_meta,
        WebrtcOMX* decoder) {
    OMXClient client;
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    MOZ_ASSERT(, "Cannot connect to media service");
    if (client.connect() != OK) {
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    uint32_t decoder_flags = 0;

    // FIXME: remove experimental property
//    char type[32];
//    property_get("webrtc.omx", type, "0");
//    if (atoi(type) == 2) {
//      decoder_flags |= OMXCodec::kSoftwareCodecsOnly;
//    }

    decoder_flags |= OMXCodec::kOnlySubmitOneInputBufferAtOneTime;

    sp<MediaSource> omx = OMXCodec::Create(
      client.interface(), dec_meta,
      false /* createEncoder */, decoder->source_,
      nullptr, decoder_flags);
    if (omx == nullptr) {
      decoder->source_->stop();
      return WEBRTC_VIDEO_CODEC_ERROR;
    }

    decoder->omx_ = omx;
    omx->start(dec_meta.get());

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoDecoder::InitDecode(
    const webrtc::VideoCodec* codecSettings,
    int32_t numberOfCores) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoDecoder::InitDecode %p", this);
  // FIXME: implement proper lifecycle management
  if (omxDec == nullptr) {
    // FIXME: use input parameters
    webrtc::VideoCodec codec_inst;
    memset(&codec_inst, 0, sizeof(webrtc::VideoCodec));
    strncpy(codec_inst.plName, EXT_VIDEO_PLAYLOAD_NAME, 31);
    codec_inst.plType = 100;
    codec_inst.width = EXT_VIDEO_FRAME_WIDTH;
    codec_inst.height = EXT_VIDEO_FRAME_HEIGHT;

    codec_inst.maxFramerate = codecSettings->maxFramerate;
    codec_inst.maxBitrate = codecSettings->maxBitrate;

    sp<MetaData> dec_meta = VideoCodecSettings2Metadata(&codec_inst);
    omxDec  = new WebrtcOMX(dec_meta);
    SetupOMXDec(dec_meta, omxDec);
  }
  decoder_ = omxDec;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoDecoder::Decode(
    const webrtc::EncodedImage& inputImage,
    bool missingFrames,
    const webrtc::RTPFragmentationHeader* fragmentation,
    const webrtc::CodecSpecificInfo*
    codecSpecificInfo,
    int64_t renderTimeMs) {
  if (inputImage._length== 0 || !inputImage._buffer) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  WebrtcOMX* decoder = static_cast<WebrtcOMX*>(decoder_);
  WebrtcMediaSource* source = decoder->source_.get();

  // FIXME: eliminate extra memory copy
  uint8_t* input = new uint8_t[inputImage._length];
  memcpy(input, inputImage._buffer, inputImage._length);
  source->pushBuffer(input, inputImage._length, inputImage._timeStamp);

  EncodedFrame* frame = new EncodedFrame();
  frame->width_ = EXT_VIDEO_FRAME_WIDTH;
  frame->height_ = EXT_VIDEO_FRAME_HEIGHT;
  frame->timestamp_ = inputImage._timeStamp;

  RUN_ON_THREAD(thread_,
    WrapRunnable(this, &WebrtcExtVideoDecoder::DecodeFrame, frame),
    NS_DISPATCH_NORMAL);

  CSFLogDebug(logTag,  "%s , WebrtcExtVideoDecoder::Decode() end input len:%d", __FUNCTION__, inputImage._length);
  LOGV("WebrtcExtVideoDecoder::Decode() end input len:%d", inputImage._length);
  return WEBRTC_VIDEO_CODEC_OK;
}

void WebrtcExtVideoDecoder::DecodeFrame(EncodedFrame* frame) {
  WebrtcOMX* decoder = static_cast<WebrtcOMX*>(decoder_);
  
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  sp<MediaSource> omx = decoder->omx_;
  MediaBuffer* decoded = nullptr;
  uint32_t time = PR_IntervalNow();
  status_t status = omx->read(&decoded);
  if (status != OK) {
    CSFLogDebug(logTag,  "%s, WebrtcExtVideoDecoder::DecodeFrame() OMX err: %d ", __FUNCTION__, status);
    LOGE("WebrtcExtVideoDecoder::DecodeFrame() OMX err: %d", status);
    return;
  }
  time = PR_IntervalToMilliseconds(PR_IntervalNow() - time);
  
  CSFLogDebug(logTag,  "%s, WebrtcExtVideoDecoder::DecodeFrame() after OMX len:%d, took %dms", __FUNCTION__, decoded->range_length(), time);
  
  LOGE("WebrtcExtVideoDecoder::DecodeFrame() after OMX len:%d, took %dms",
    decoded->range_length(), time);

  // FIXME: eliminate extra pixel copy/color conversion
  // QCOM HW only support OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka
  size_t width = frame->width_;
  size_t height = frame->height_;
  size_t widthUV = width / 2;
  if (decoded_image_.CreateEmptyFrame(width, height,
                                      width, widthUV, widthUV)) {
    return;
  }
  size_t roundedStride = (width + 31) & ~31;
  size_t roundedSliceHeight = (height + 31) & ~31;
  uint8_t* y = static_cast<uint8_t*>(decoded->data()) + decoded->range_offset();
  uint8_t* uv = y + (roundedStride * roundedSliceHeight);
  uint8_t* dstY = decoded_image_.buffer(webrtc::kYPlane);
  uint8_t* dstU = decoded_image_.buffer(webrtc::kUPlane);
  uint8_t* dstV = decoded_image_.buffer(webrtc::kVPlane);
  size_t heightUV = height / 2;
  size_t padding = roundedStride - width;
  for (int i = 0; i < height; i++) {
    memcpy(dstY, y, width);
    y += roundedStride;
    dstY += width;
    if (i < heightUV) {
      for (int j = 0; j < widthUV; j++) {
        *dstV++ = *uv++;
        *dstU++ = *uv++;
      }
      uv += padding;
    }
  }

  decoded_image_.set_timestamp(frame->timestamp_);

#if DUMP_FRAMES
  static size_t decCount = 0;
  char path[127];
  sprintf(path, "/data/local/tmp/omx%05d.Y", decCount);
  FILE* out = fopen(path, "w+");
  fwrite(decoded_image_.buffer(webrtc::kYPlane), decoded_image_.allocated_size(webrtc::kYPlane), 1, out);
  fclose(out);
  sprintf(path, "/data/local/tmp/omx%05d.U", decCount);
  out = fopen(path, "w+");
  fwrite(decoded_image_.buffer(webrtc::kUPlane), decoded_image_.allocated_size(webrtc::kUPlane), 1, out);
  fclose(out);
  sprintf(path, "/data/local/tmp/omx%05d.V", decCount++);
  out = fopen(path, "w+");
  fwrite(decoded_image_.buffer(webrtc::kVPlane), decoded_image_.allocated_size(webrtc::kVPlane), 1, out);
  fclose(out);
#endif

  decoded->release();
  delete frame;

  RunCallback();
}

void WebrtcExtVideoDecoder::RunCallback() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  callback_->Decoded(decoded_image_);
}

int32_t WebrtcExtVideoDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  callback_ = callback;

  return WEBRTC_VIDEO_CODEC_OK;
}


int32_t WebrtcExtVideoDecoder::Release() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoEncoder::Release %p", this);
  return WEBRTC_VIDEO_CODEC_OK;
}

WebrtcExtVideoDecoder::~WebrtcExtVideoDecoder() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  LOGV("WebrtcExtVideoDecoder::~WebrtcExtVideoDecoder %p", this);
}

int32_t WebrtcExtVideoDecoder::Reset() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  return WEBRTC_VIDEO_CODEC_OK;
}

}
