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

#include <mozilla/Scoped.h>

#include <gui/Surface.h>

#include "runnable_utils.h"

#include "VideoConduit.h"
#include "AudioConduit.h"
#include "WebrtcExtVideoCodec.h"

#include <ICrypto.h>
#include <stagefright/foundation/ABuffer.h>
#include <stagefright/foundation/AMessage.h>
#include <OMX_Component.h>
#include <IOMX.h>
#include <stagefright/MediaCodec.h>
#include <stagefright/MediaDefs.h>
#include <stagefright/MediaErrors.h>

using namespace android;

#define EXT_VIDEO_PLAYLOAD_NAME "VP8"
#define EXT_VIDEO_FRAME_WIDTH 640
#define EXT_VIDEO_FRAME_HEIGHT 480
#define EXT_VIDEO_MAX_FRAMERATE 30
#define DEQUEUE_BUFFER_TIMEOUT_US (100 * 1000ll) // 100ms
#define START_DEQUEUE_BUFFER_TIMEOUT_US (10 * DEQUEUE_BUFFER_TIMEOUT_US) // 1s

// for debugging
#define DUMP_FRAMES 0
#include <cstdio>
//#define LOG_TAG "WebrtcExtVideoCodec"
#include <utils/Log.h>

// for experiment
//#include <cutils/properties.h>

namespace mozilla {

// Link to Stagefright
class WebrtcOMX {
public:
  WebrtcOMX(const char* mime, bool encoder) {
    looper_ = new ALooper;
    looper_->start();
    omx_ = MediaCodec::CreateByType(looper_, mime, encoder);
  }

  virtual ~WebrtcOMX() {
    looper_.clear();
    omx_.clear();
  }

  status_t Configure(const sp<AMessage>& format,
    const sp<Surface>& nativeWindow,
    const sp<ICrypto>& crypto, uint32_t flags) {
//    return omx_->configure(format, nativeWindow, crypto, flags);
    return omx_->configure(format, nullptr, crypto, flags);
  }

  status_t Start() {
    status_t err = omx_->start();
    started_ = err == OK;

    omx_->getInputBuffers(&input_);
    omx_->getOutputBuffers(&output_);

    return err;
  }

  status_t Stop() {
    status_t err = omx_->stop();
    started_ = err == OK;
    return err;
  }

  friend class WebrtcExtVideoEncoder;
  friend class WebrtcExtVideoDecoder;

//  static bool threadPool;

  sp<ALooper> looper_;
  sp<MediaCodec> omx_; // OMXCodec
  bool started_;
  Vector<sp<ABuffer> > input_;
  Vector<sp<ABuffer> > output_;
  // To distinguish different OMX configuration:
  // 1. HW
  // 2. SW
  // 3. HW using graphic buffer)
  int type_;

/*
  sp<GonkNativeWindow> native_window_;
  sp<GonkNativeWindowClient> native_window_client_;
*/

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(WebrtcOMX)
};


// Graphic buffer management. Steal from content/media/omx/OmxDecoder.*
class VideoGraphicBuffer : public layers::GraphicBufferLocked {
public:
    VideoGraphicBuffer(const sp<MediaCodec>& aOmx, uint32_t bufferIndex,
                       webrtc::I420VideoFrame* aFrame,
                       layers::SurfaceDescriptor& aDescriptor)
      :layers::GraphicBufferLocked(aDescriptor),
      mOmx(aOmx), mBufferIndex(bufferIndex), mFrame(aFrame),
      mMonitor("VideoGraphicBuffer") {}

    virtual ~VideoGraphicBuffer() {
      EXT_LOGV("~VideoGraphicBuffer(%p) ref:%u omx:%p", this, mRefCnt.get(), mOmx.get());
      MonitorAutoLock lock(mMonitor);
      if (mOmx.get()) {
        mOmx->releaseOutputBuffer(mBufferIndex);
        mOmx = nullptr;
      }
      clearFrame();
    }

    void Unlock() {
      EXT_LOGV("VideoGraphicBuffer::Unlock(%p) ref:%u omx:%p", this, mRefCnt.get(), mOmx.get());
      MonitorAutoLock lock(mMonitor);
      if (mOmx.get()) {
        mOmx->releaseOutputBuffer(mBufferIndex);
        mOmx = nullptr;
      }
      clearFrame();
    }

private:
  sp<MediaCodec> mOmx;
  uint32_t mBufferIndex;
  webrtc::I420VideoFrame* mFrame;
  Monitor mMonitor;

  void clearFrame() {
    if (mFrame) {
      VideoGraphicBuffer* vgb = static_cast<VideoGraphicBuffer*>(mFrame->GetGonkBuffer());
      EXT_LOGV("VideoGraphicBuffer::clearFrame() vgb:%p ref:%u this:%p ref:%u",
        vgb, vgb? vgb->mRefCnt.get():0, this, mRefCnt.get());
//      if (vgb == this) {
//        mFrame->SetGonkBufferInUse(false);
//        mFrame->SetGonkBuffer(nullptr, nullptr);
//      }
      mFrame = nullptr;
    }
  }
};

static WebrtcOMX* omxEnc = nullptr;

// Encoder.
WebrtcExtVideoEncoder::WebrtcExtVideoEncoder()
  : timestamp_(0),
    callback_(nullptr),
    mutex_("WebrtcExtVideoEncoder"),
    encoder_(nullptr) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  memset(&encoded_image_, 0, sizeof(encoded_image_));
//  EXT_LOGV("WebrtcExtVideoEncoder::WebrtcExtVideoEncoder %p", this);
}

static AMessage* VideoCodecSettings2AMessage(
    const webrtc::VideoCodec* codecSettings) {
  AMessage* format = new AMessage;
  
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  format->setString("mime", "video/x-vnd.on2.vp8");
//  format->setInt32("store-metadata-in-buffers", 0);
//  format->setInt32("prepend-sps-pps-to-idr-frames", 0);
  format->setInt32("bitrate", codecSettings->minBitrate * 1000); // kbps->bps
  format->setInt32("width", codecSettings->width);
  format->setInt32("height", codecSettings->height);
//  format->setInt32("stride", codecSettings->width);
//  format->setInt32("slice-height", codecSettings->height);
  format->setFloat("frame-rate", (float)codecSettings->maxFramerate);
  // TODO: QCOM encoder only support this format. See
  // <B2G>/hardware/qcom/media/mm-video/vidc/venc/src/video_encoder_device.cpp:
  // venc_dev::venc_set_color_format()
  format->setInt32("color-format", OMX_COLOR_FormatYUV420SemiPlanar);
  // FIXME: get correct parameters from codecSettings?
//  format->setInt32("i-frame-interval", 1); // one I-frame per sec
//  format->setInt32("profile", OMX_VIDEO_AVCProfileBaseline);
//  format->setInt32("level", OMX_VIDEO_AVCLevel3);
  //format->setInt32("bitrate-mode", OMX_Video_ControlRateConstant);

  return format;
}

int32_t WebrtcExtVideoEncoder::InitEncode(
    const webrtc::VideoCodec* codecSettings,
    int32_t numberOfCores,
    uint32_t maxPayloadSize) {
  max_payload_size_ = maxPayloadSize;
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
//  EXT_LOGV("WebrtcExtVideoEncoder::InitEncode %p", this);

  if (omxEnc == nullptr) { // FIXME: implement proper lifecycle management
    // FIXME: use input parameters
    webrtc::VideoCodec codec_inst;
    memset(&codec_inst, 0, sizeof(webrtc::VideoCodec));
    strncpy(codec_inst.plName, EXT_VIDEO_PLAYLOAD_NAME, 31);
    codec_inst.plType = 120;
    codec_inst.width = EXT_VIDEO_FRAME_WIDTH;
    codec_inst.height = EXT_VIDEO_FRAME_HEIGHT;

    codec_inst.maxBitrate = codecSettings->maxBitrate;
    codec_inst.minBitrate = codecSettings->minBitrate;

    codec_inst.maxFramerate = codecSettings->maxFramerate;

    sp<AMessage> conf = VideoCodecSettings2AMessage(&codec_inst);
    omxEnc = new WebrtcOMX("video/x-vnd.on2.vp8", true /* encoder */);
    omxEnc->Configure(conf, nullptr, nullptr,
      MediaCodec::CONFIGURE_FLAG_ENCODE);
  }
  omxEnc->started_ = false;
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

  uint32_t time = PR_IntervalNow();
  WebrtcOMX* encoder = (reinterpret_cast<WebrtcOMX*>(encoder_));
  if (!encoder->started_) {
    encoder->Start();
    CSFLogDebug(logTag,  "%s encoder->start()", __FUNCTION__);
    EXT_LOGV("WebrtcExtVideoEncoder::Encode() start OMX took %u ms",
      PR_IntervalToMilliseconds(PR_IntervalNow()-time));
    time = PR_IntervalNow();
  }
  // TODO: eliminate extra pixel copy & color conversion
  size_t sizeY = inputImage.allocated_size(webrtc::kYPlane);
  size_t sizeUV = inputImage.allocated_size(webrtc::kUPlane);
  const uint8_t* u = inputImage.buffer(webrtc::kUPlane);
  const uint8_t* v = inputImage.buffer(webrtc::kVPlane);
  size_t size = sizeY + 2 * sizeUV;

  sp<MediaCodec> omx = encoder->omx_;
  size_t index;
  status_t err = omx->dequeueInputBuffer(&index, DEQUEUE_BUFFER_TIMEOUT_US);
  if (err != OK) {
    CSFLogError(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX input buffer error:%d", __FUNCTION__, err);
    EXT_LOGE("WebrtcExtVideoEncoder::Encode() dequeue OMX input buffer error:%d", err);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX input buffer took %u ms", __FUNCTION__, PR_IntervalToMilliseconds(PR_IntervalNow()-time));

  EXT_LOGV("WebrtcExtVideoEncoder::Encode() dequeue OMX input buffer took %u ms",
    PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  time = PR_IntervalNow();

  const sp<ABuffer>& omxIn = encoder->input_.itemAt(index);
  MOZ_ASSERT(omxIn->capacity() >= size);
  omxIn->setRange(0, size);
  uint8_t* dstY = omxIn->data();
  uint16_t* dstUV = reinterpret_cast<uint16_t*>(dstY + sizeY);
  memcpy(dstY, inputImage.buffer(webrtc::kYPlane), sizeY);
  for (int i = 0; i < sizeUV;
      i++, dstUV++, u++, v++) {
    *dstUV = (*v << 8) + *u;
  }

  err = omx->queueInputBuffer(index, 0, size,
    inputImage.render_time_ms() * 1000000, // ms to us
    0);
  if (err != OK) {
    CSFLogError(logTag,  "%s WebrtcExtVideoEncoder::Encode() queue OMX input buffer error:%d", __FUNCTION__, , err);
    EXT_LOGE("WebrtcExtVideoEncoder::Encode() queue OMX input buffer error:%d", err);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() queue OMX input buffer took %u ms", __FUNCTION__, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoEncoder::Encode() queue OMX input buffer took %u ms",
    PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  time = PR_IntervalNow();

  timestamp_ = inputImage.timestamp();

  EncodedFrame frame;
  frame.width_ = inputImage.width();
  frame.height_ = inputImage.height();
  frame.timestamp_ = timestamp_;

  CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() %dx%d -> %ux%u took %u ms", __FUNCTION__, inputImage.width(), inputImage.height(), frame.width_, frame.height_,
    PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoEncoder::Encode() %dx%d -> %ux%u took %u ms",
    inputImage.width(), inputImage.height(), frame.width_, frame.height_,
    PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  time = PR_IntervalNow();

  encoded_image_._encodedWidth = frame.width_;
  encoded_image_._encodedHeight = frame.height_;
  encoded_image_._timeStamp = frame.timestamp_;
  encoded_image_.capture_time_ms_ = frame.timestamp_;

  size_t outOffset;
  size_t outSize;
  int64_t outTime;
  uint32_t outFlags;
  err = omx->dequeueOutputBuffer(&index, &outOffset, &outSize, &outTime,
    &outFlags, DEQUEUE_BUFFER_TIMEOUT_US);
  if (err == INFO_FORMAT_CHANGED) {
    CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX output format change", __FUNCTION__);
    EXT_LOGV("WebrtcExtVideoEncoder::Encode() dequeue OMX output format change");
    return WEBRTC_VIDEO_CODEC_OK;
  } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
    err = omx->getOutputBuffers(&encoder->output_);
    CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer change", __FUNCTION__);
    EXT_LOGV("WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer change");
    MOZ_ASSERT(err == OK);
    return WEBRTC_VIDEO_CODEC_OK;
  } else if (err != OK) {
    CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer error:%d", __FUNCTION__, err);
    EXT_LOGE("WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer error:%d", err);
    return WEBRTC_VIDEO_CODEC_ERROR;
  }
  CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer err:%d len:%u time:%lld flags:0x%08x took %u ms", __FUNCTION__, err, outSize, outTime, outFlags, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoEncoder::Encode() dequeue OMX output buffer err:%d len:%u time:%lld flags:0x%08x took %u ms",
   err, outSize, outTime, outFlags, PR_IntervalToMilliseconds(PR_IntervalNow()-time));

  sp<ABuffer> omxOut = encoder->output_.itemAt(index);
  encoded_image_._length = omxOut->size();
  uint8_t* data = omxOut->data();
  memcpy(encoded_image_._buffer, data, encoded_image_._length);
  omx->releaseOutputBuffer(index);

  encoded_image_._completeFrame = true;
  encoded_image_._frameType =
    outFlags & (MediaCodec::BUFFER_FLAG_SYNCFRAME | MediaCodec::BUFFER_FLAG_CODECCONFIG)?
    webrtc::kKeyFrame:webrtc::kDeltaFrame;

#if DUMP_FRAMES
  char path[127];
  sprintf(path, "/data/local/tmp/omx-%012d.h264", frame.timestamp_);
  FILE* out = fopen(path, "w+");
  fwrite(encoded_image_._buffer, encoded_image_._length, 1, out);
  fclose(out);
#endif

  CSFLogDebug(logTag,  "%s WebrtcExtVideoEncoder::Encode() frame type:%d size:%u", __FUNCTION__, encoded_image_._frameType, encoded_image_._length);
  EXT_LOGV("WebrtcExtVideoEncoder::Encode() frame type:%d size:%u", encoded_image_._frameType, encoded_image_._length);
  callback_->Encoded(encoded_image_, nullptr, nullptr);

  return WEBRTC_VIDEO_CODEC_OK;
}

void WebrtcExtVideoEncoder::EmitFrames() {
  MutexAutoLock lock(mutex_);

  while(!frames_.empty()) {
    EncodedFrame *frame = &frames_.front();
    EmitFrame(frame);
    frames_.pop();
  }
}

void WebrtcExtVideoEncoder::EmitFrame(EncodedFrame *frame) {
  MOZ_ASSERT(frame);
}

int32_t WebrtcExtVideoEncoder::RegisterEncodeCompleteCallback(
    webrtc::EncodedImageCallback* callback) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  callback_ = callback;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoEncoder::Release() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  EXT_LOGV("WebrtcExtVideoEncoder::Release %p", this);
  if (encoder_) {
    // FIXME: Leak! Should implement proper lifecycle management
    // WebrtcOMX* omx = static_cast<WebrtcOMX*>(encoder_);
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
  EXT_LOGV("WebrtcExtVideoEncoder::~WebrtcExtVideoEncoder %p", this);
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
  EXT_LOGV("WebrtcExtVideoEncoder::SetRates(%u, %u)", newBitRate, frameRate);
  WebrtcOMX* encoder = static_cast<WebrtcOMX*>(encoder_);
  if (!encoder || !encoder->omx_.get()) {
    return WEBRTC_VIDEO_CODEC_UNINITIALIZED;
  }

  sp<MediaCodec> omx = encoder->omx_;
  sp<AMessage> msg = new AMessage();
  msg->setInt32("videoBitrate", newBitRate * 1000 /* kbps -> bps */);
  msg->setInt32("frame-rate", frameRate);
  omx->setParameters(msg);

  return WEBRTC_VIDEO_CODEC_OK;
}

// TODO: eliminate global variable after implementing prpper lifecycle management code
static WebrtcOMX* omxDec = nullptr;

// Decoder.
WebrtcExtVideoDecoder::WebrtcExtVideoDecoder()
    : callback_(nullptr),
      mutex_("WebrtcExtVideoDecoder") {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  in_use_[0] = in_use_[1] = false;
  EXT_LOGV("WebrtcExtVideoDecoder::WebrtcExtVideoDecoder %p", this);
}

int32_t WebrtcExtVideoDecoder::InitDecode(
    const webrtc::VideoCodec* codecSettings,
    int32_t numberOfCores) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  EXT_LOGV("WebrtcExtVideoDecoder::InitDecode %p", this);
  // FIXME: implement proper lifecycle management
  if (omxDec == nullptr) {
    // FIXME: use input parameters
    webrtc::VideoCodec codec_inst;
    memset(&codec_inst, 0, sizeof(webrtc::VideoCodec));
    strncpy(codec_inst.plName, EXT_VIDEO_PLAYLOAD_NAME, 31);
    codec_inst.plType = 124;
    codec_inst.width = EXT_VIDEO_FRAME_WIDTH;
    codec_inst.height = EXT_VIDEO_FRAME_HEIGHT;

    codec_inst.maxFramerate = codecSettings->maxFramerate;
    codec_inst.maxBitrate = codecSettings->maxBitrate;
    codec_inst.minBitrate = codecSettings->minBitrate;

    sp<AMessage> conf = VideoCodecSettings2AMessage(&codec_inst);
    omxDec  = new WebrtcOMX("video/x-vnd.on2.vp8", false /* encoder */);

//    char type[32];
//    property_get("webrtc.omx", type, "3");
    omxDec->type_ = 2;//atoi(type);
    sp<Surface> nativeWindow = nullptr;

//    if (omxDec->type_ == 3) {
//      omxDec->native_window_ = new GonkNativeWindow();
//#if defined(MOZ_WIDGET_GONK) && ANDROID_VERSION >= 18
//      omxDec->native_window_client_ =
//        new GonkNativeWindowClient(omxDec->native_window_->getBufferQueue());
//#else
//      omxDec->native_window_client_ =
//        new GonkNativeWindowClient(omxDec->native_window_);
//#endif
//      nativeWindow = new Surface(omxDec->native_window_client_->getIGraphicBufferProducer());
//    }
//    omxDec->Configure(conf, nativeWindow, nullptr, 0);
    omxDec->Configure(conf, nullptr, nullptr, 0);
  }
  omxDec->started_ = false;
  decoder_ = omxDec;

  return WEBRTC_VIDEO_CODEC_OK;
}

/*
void generateVideoFrame_hw(GonkNativeWindow* nativeWindow, EncodedFrame* frame,
  const sp<MediaCodec>& omx, uint32_t bufferIndex, const sp<ABuffer>& decoded,
  webrtc::I420VideoFrame* videoFrame, bool* used) {
  //MOZ_ASSERT(!videoFrame->IsGonkBufferInUse());
  
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  sp<RefBase> obj;
  bool hasGraphicBuffer = decoded->meta()->findObject("graphic-buffer", &obj);
  MOZ_ASSERT(hasGraphicBuffer);
  sp<GraphicBuffer> gb = static_cast<GraphicBuffer*>(obj.get());
  MOZ_ASSERT(gb.get());

  EXT_LOGV("WebrtcExtVideoDecoder::Decode() generateVideoFrame_hw() data:%p gb:%p",
    decoded->data(), hasGraphicBuffer? gb.get():nullptr);

  layers::SurfaceDescriptor *descriptor = nativeWindow->getSurfaceDescriptorFromBuffer(gb.get());

  // Change the descriptor's size to video's size. There are cases that
  // GraphicBuffer's size and actual video size is different.
  // See Bug 850566.
  layers::SurfaceDescriptorGralloc newDescriptor = descriptor->get_SurfaceDescriptorGralloc();
  newDescriptor.size() = nsIntSize(frame->width_, frame->height_);

  mozilla::layers::SurfaceDescriptor descWrapper(newDescriptor);
  VideoGraphicBuffer* vgb = new VideoGraphicBuffer(omx, bufferIndex,
    videoFrame, descWrapper);

  size_t width = frame->width_;
  size_t height = frame->height_;
  size_t widthUV = width / 2;
  if (videoFrame->IsZeroSize()) {
    if (videoFrame->CreateEmptyFrame(width, height,
                                        width, widthUV, widthUV)) {
      return;
    }
  }

  EXT_LOGV("generateVideoFrame_hw() gfx:%p vgb:%p", gb.get(), vgb);
  VideoGraphicBuffer* old = static_cast<VideoGraphicBuffer*>(videoFrame->GetGonkBuffer());
  videoFrame->SetGonkBuffer(vgb, used);

  videoFrame->set_timestamp(frame->timestamp_);
}
*/

void generateVideoFrame_sw(EncodedFrame* frame, const sp<ABuffer>& decoded,
  webrtc::I420VideoFrame* videoFrame) {
  
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  
  // FIXME: eliminate extra pixel copy/color conversion
  // QCOM HW only support OMX_QCOM_COLOR_FormatYVU420PackedSemiPlanar32m4ka
  size_t width = frame->width_;
  size_t height = frame->height_;
  size_t widthUV = width / 2;
  if (videoFrame->CreateEmptyFrame(width, height,
                                      width, widthUV, widthUV)) {
    return;
  }
  size_t roundedStride = (width + 31) & ~31;
  size_t roundedSliceHeight = (height + 31) & ~31;
  uint8_t* y = decoded->data();
  uint8_t* uv = y + (roundedStride * roundedSliceHeight);
  uint8_t* dstY = videoFrame->buffer(webrtc::kYPlane);
  uint8_t* dstU = videoFrame->buffer(webrtc::kUPlane);
  uint8_t* dstV = videoFrame->buffer(webrtc::kVPlane);
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

  videoFrame->set_timestamp(frame->timestamp_);

#if DUMP_FRAMES
  static size_t decCount = 0;
  char path[127];
  sprintf(path, "/data/local/tmp/omx%05d.Y", decCount);
  FILE* out = fopen(path, "w+");
  fwrite(videoFrame->buffer(webrtc::kYPlane), videoFrame->allocated_size(webrtc::kYPlane), 1, out);
  fclose(out);
  sprintf(path, "/data/local/tmp/omx%05d.U", decCount);
  out = fopen(path, "w+");
  fwrite(videoFrame->buffer(webrtc::kUPlane), videoFrame->allocated_size(webrtc::kUPlane), 1, out);
  fclose(out);
  sprintf(path, "/data/local/tmp/omx%05d.V", decCount++);
  out = fopen(path, "w+");
  fwrite(videoFrame->buffer(webrtc::kVPlane), videoFrame->allocated_size(webrtc::kVPlane), 1, out);
  fclose(out);
#endif
}

status_t feedOMXInput(WebrtcOMX* decoder, const sp<MediaCodec>& omx,
  const webrtc::EncodedImage& inputImage, int64_t* timeUs) {
  static int64_t firstTime = -1ll;
  size_t index;
  uint32_t time = PR_IntervalNow();
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  status_t err = omx->dequeueInputBuffer(&index, 
    firstTime < 0? START_DEQUEUE_BUFFER_TIMEOUT_US:DEQUEUE_BUFFER_TIMEOUT_US);
  if (err != OK) {
    CSFLogError(logTag,  "%s WebrtcExtVideoDecoder::Decode() dequeue input buffer error:%d", __FUNCTION__, err);
    EXT_LOGE("WebrtcExtVideoDecoder::Decode() dequeue input buffer error:%d", err);
    return err;
  }
  CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() dequeue input buffer took %u ms", __FUNCTION__, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoDecoder::Decode() dequeue input buffer took %u ms",
    PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  time = PR_IntervalNow();

  uint32_t flags = 0;
  if (inputImage._frameType == webrtc::kKeyFrame) {
    flags |= (firstTime < 0)? MediaCodec::BUFFER_FLAG_CODECCONFIG:MediaCodec::BUFFER_FLAG_SYNCFRAME;
  }
  size_t size = inputImage._length;
  const sp<ABuffer>& omxIn = decoder->input_.itemAt(index);
  MOZ_ASSERT(omxIn->capacity() >= size);
  omxIn->setRange(0, size);
  memcpy(omxIn->data(), inputImage._buffer, size);
  if (firstTime < 0) {
    firstTime = inputImage._timeStamp;
  }
  *timeUs = (inputImage._timeStamp - firstTime) * 10; // FIXME
  err = omx->queueInputBuffer(index, 0, size, *timeUs, flags);

  CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() queue input buffer len:%u flags:%u time:%lld took %u ms", __FUNCTION__, size, flags, *timeUs, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoDecoder::Decode() queue input buffer len:%u flags:%u time:%lld took %u ms",
    size, flags, *timeUs, PR_IntervalToMilliseconds(PR_IntervalNow()-time));

  return err;
}

status_t getOMXOutput(WebrtcOMX* decoder, const sp<MediaCodec>& omx,
  const webrtc::EncodedImage& inputImage, const int64_t timeUs,
  webrtc::I420VideoFrame* video_frames_, bool* in_use_,
  webrtc::DecodedImageCallback* callback_) {
  sp<ABuffer> omxOut;
  
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);

  EncodedFrame* frame = new EncodedFrame();
  // FIXME: inputImage.encoded_(width|height) was not set correctly
  frame->width_ = EXT_VIDEO_FRAME_WIDTH;
  frame->height_ = EXT_VIDEO_FRAME_HEIGHT;
  frame->timestamp_ = inputImage._timeStamp;
  frame->decode_timestamp_ = PR_IntervalNow();

  uint32_t time = PR_IntervalNow();

  size_t index;
  size_t outOffset;
  size_t outSize;
  int64_t outTime;
  uint32_t outFlags;
  status_t err = omx->dequeueOutputBuffer(&index, &outOffset, &outSize, &outTime, &outFlags);
  if (err == INFO_FORMAT_CHANGED) {
    // TODO: handle format change
    CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    goto end;
  } else if (err == INFO_OUTPUT_BUFFERS_CHANGED) {
    CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() dequeue OMX output buffer change:%d", __FUNCTION__, err);
    EXT_LOGV("WebrtcExtVideoDecoder::Decode() dequeue OMX output buffer change:%d", err);
    err = omx->getOutputBuffers(&decoder->output_);
    MOZ_ASSERT(err == OK);
    err = INFO_OUTPUT_BUFFERS_CHANGED;
    goto end;
  } else if (err != OK) {
    CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() dequeue OMX output buffer error:%d", __FUNCTION__, err);
    EXT_LOGE("WebrtcExtVideoDecoder::Decode() dequeue OMX output buffer error:%d", err);
    goto end;
  }
  omxOut = decoder->output_.itemAt(index);

  CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() dequeue output buffer#%u(%p) err:%d len:%u time:%lld flags:0x%08x took %u ms", __FUNCTION__, index, omxOut.get(), err, outSize, outTime, outFlags, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  EXT_LOGV("WebrtcExtVideoDecoder::Decode() dequeue output buffer#%u(%p) err:%d len:%u time:%lld flags:0x%08x took %u ms",
    index, omxOut.get(), err, outSize, outTime, outFlags, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  time = PR_IntervalNow();

  if (timeUs < outTime) {
    // invalid buffer
    omx->releaseOutputBuffer(index);
  } else if (decoder->type_ == 3) {
  CSFLogDebug(logTag,  "%s decoder->type_ == 3", __FUNCTION__);
//    int i = in_use_[0]? 1:0;
//    webrtc::I420VideoFrame* videoFrame = &video_frames_[i];
//    generateVideoFrame_hw(decoder->native_window_.get(), frame,
//      omx, index, omxOut, videoFrame, &in_use_[i]);
//    callback_->Decoded(*videoFrame);
  } else {
    CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() generate video frame", __FUNCTION__);
    EXT_LOGV("WebrtcExtVideoDecoder::Decode() generate video frame");
    generateVideoFrame_sw(frame, omxOut, &video_frames_[0]);
    callback_->Decoded(video_frames_[0]);

    omx->releaseOutputBuffer(index);
  }

end:
  delete frame;

  return err;
}

int32_t WebrtcExtVideoDecoder::Decode(
    const webrtc::EncodedImage& inputImage,
    bool missingFrames,
    const webrtc::RTPFragmentationHeader* fragmentation,
    const webrtc::CodecSpecificInfo* codecSpecificInfo,
    int64_t renderTimeMs) {
    
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  EXT_LOGV("WebrtcExtVideoDecoder::Decode()");

  if (inputImage._length== 0 || !inputImage._buffer) {
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  uint32_t time = PR_IntervalNow();
  WebrtcOMX* decoder = static_cast<WebrtcOMX*>(decoder_);
  if (!decoder->started_) {
    decoder->Start();
    CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() start decoder took %u ms", __FUNCTION__, PR_IntervalToMilliseconds(PR_IntervalNow()-time));
    EXT_LOGV("WebrtcExtVideoDecoder::Decode() start decoder took %u ms",
      PR_IntervalToMilliseconds(PR_IntervalNow()-time));
  }

  sp<MediaCodec> omx = decoder->omx_;
  bool feedFrame = true;

  while (feedFrame) {
    int64_t timeUs;
    status_t err = feedOMXInput(decoder, omx, inputImage, &timeUs);
    feedFrame = (err == -EAGAIN);
    do {
      err = getOMXOutput(decoder, omx, inputImage, timeUs, video_frames_, in_use_, callback_);
    } while (err == INFO_OUTPUT_BUFFERS_CHANGED);
  }

  EXT_LOGV("WebrtcExtVideoDecoder::Decode() end");
  CSFLogDebug(logTag,  "%s WebrtcExtVideoDecoder::Decode() end", __FUNCTION__);
  return WEBRTC_VIDEO_CODEC_OK;
}

void WebrtcExtVideoDecoder::DecodeFrame(EncodedFrame* frame) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
}

int32_t WebrtcExtVideoDecoder::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
    
  callback_ = callback;

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t WebrtcExtVideoDecoder::Release() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  EXT_LOGV("WebrtcExtVideoEncoder::Release %p", this);
  return WEBRTC_VIDEO_CODEC_OK;
}

WebrtcExtVideoDecoder::~WebrtcExtVideoDecoder() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  EXT_LOGV("WebrtcExtVideoDecoder::~WebrtcExtVideoDecoder %p", this);
}

int32_t WebrtcExtVideoDecoder::Reset() {
  CSFLogDebug(logTag,  "%s ", __FUNCTION__);
  return WEBRTC_VIDEO_CODEC_OK;
}

}
