# SPDX-License-Identifier: GPL-3.0-only
"""Pocket edits to the 12L external camera HAL session (device@3.4): host I420 frames instead of V4L2 buffers.

The streaming functions are the ones the Android 9 port uses (runtime/camera/external-camera-pocket.patch);
12L keeps the same session structure around them.
"""
import sys
from pathlib import Path

from port_util import edit, region

root = Path(sys.argv[1]) / 'camera/device/3.4/default'
path = root / 'ExternalCameraDeviceSession.cpp'
text = path.read_text()
text = edit(text, path.name, [
('#include "ExternalCameraDeviceSession.h"\n', '#include "ExternalCameraDeviceSession.h"\n#include "host_camera.h"\n'),
('''    struct v4l2_capability capability;
    int ret = ioctl(mV4l2Fd.get(), VIDIOC_QUERYCAP, &capability);
    std::string make, model;
    if (ret < 0) {
        ALOGW("%s v4l2 QUERYCAP failed", __FUNCTION__);
        mExifMake = "Generic UVC webcam";
        mExifModel = "Generic UVC webcam";
    } else {
        // capability.card is UTF-8 encoded
        char card[32];
        int j = 0;
        for (int i = 0; i < 32; i++) {
            if (capability.card[i] < 128) {
                card[j++] = capability.card[i];
            }
            if (capability.card[i] == '\\0') {
                break;
            }
        }
        if (j == 0 || card[j - 1] != '\\0') {
            mExifMake = "Generic UVC webcam";
            mExifModel = "Generic UVC webcam";
        } else {
            mExifMake = card;
            mExifModel = card;
        }
    }
''', '''    // Pocket: frames come from the host camera; the stream opens when one is configured.
    mExifMake = "Pocket";
    mExifModel = "Host camera " + mCameraId;
'''),
('''bool ExternalCameraDeviceSession::initialize() {
    if (mV4l2Fd.get() < 0) {
        ALOGE("%s: invalid v4l2 device fd %d!", __FUNCTION__, mV4l2Fd.get());
        return true;
    }
''', '''bool ExternalCameraDeviceSession::initialize() {
'''),
('''    if (req->frameIn->mFourcc != V4L2_PIX_FMT_MJPEG && req->frameIn->mFourcc != V4L2_PIX_FMT_Z16) {''',
 '''    if (req->frameIn->mFourcc != V4L2_PIX_FMT_MJPEG && req->frameIn->mFourcc != V4L2_PIX_FMT_Z16 &&
            req->frameIn->mFourcc != V4L2_PIX_FMT_YUV420) {'''),
('''    // TODO: in some special case maybe we can decode jpg directly to gralloc output?
    if (req->frameIn->mFourcc == V4L2_PIX_FMT_MJPEG) {''', '''    if (req->frameIn->mFourcc == V4L2_PIX_FMT_YUV420) {
        // Pocket: host frames are already tightly packed I420 of the stream size.
        const uint32_t w = mYu12Frame->mWidth, h = mYu12Frame->mHeight;
        int copied = -1;
        if (inDataSize == static_cast<size_t>(w) * h * 3 / 2) {
            ATRACE_BEGIN("I420Copy");
            copied = libyuv::I420Copy(
                    inData, w, inData + w * h, w / 2, inData + w * h + (w / 2) * (h / 2), w / 2,
                    static_cast<uint8_t*>(mYu12FrameLayout.y), mYu12FrameLayout.yStride,
                    static_cast<uint8_t*>(mYu12FrameLayout.cb), mYu12FrameLayout.cStride,
                    static_cast<uint8_t*>(mYu12FrameLayout.cr), mYu12FrameLayout.cStride,
                    w, h);
            ATRACE_END();
        }
        if (copied != 0) {
            ALOGE("%s: host I420 frame of %zu bytes does not match %ux%u", __FUNCTION__, inDataSize, w, h);
            lk.unlock();
            Status st = parent->processCaptureRequestError(req);
            if (st != Status::OK) {
                return onDeviceError("%s: failed to process capture request error!", __FUNCTION__);
            }
            signalRequestDone();
            return true;
        }
    }

    // TODO: in some special case maybe we can decode jpg directly to gralloc output?
    if (req->frameIn->mFourcc == V4L2_PIX_FMT_MJPEG) {'''),
])
donor = (Path(__file__).parent / 'host_streaming.inc').read_text()
a, c = region(text, 'int ExternalCameraDeviceSession::v4l2StreamOffLocked() {', 'void ExternalCameraDeviceSession::enqueueV4l2Frame(')
text = text[:a] + donor[donor.index('int ExternalCameraDeviceSession::'):] + text[c:]
path.write_text(text)

# A frame received from the host, owned in memory instead of mmap'ed from a V4L2 buffer.
path = root / 'ExternalCameraUtils.cpp'
path.write_text(edit(path.read_text(), path.name, [
('''        mBufferIndex(bufIdx), mFd(fd), mDataSize(dataSize), mOffset(offset) {}
''', '''        mBufferIndex(bufIdx), mFd(fd), mDataSize(dataSize), mOffset(offset) {}

V4L2Frame::V4L2Frame(uint32_t w, uint32_t h, uint32_t fourcc, std::vector<uint8_t>&& data) :
        Frame(w, h, fourcc),
        mBufferIndex(-1), mFd(-1), mDataSize(data.size()), mOffset(0),
        mHeapData(std::move(data)) {}
'''),
('''    std::lock_guard<std::mutex> lk(mLock);
    if (!mMapped) {
        void* addr = mmap(''', '''    std::lock_guard<std::mutex> lk(mLock);
    if (mFd < 0) {
        *data = mHeapData.data();
        *dataSize = mHeapData.size();
        return 0;
    }
    if (!mMapped) {
        void* addr = mmap('''),
]))
path = root / 'include/ext_device_v3_4_impl/ExternalCameraUtils.h'
path.write_text(edit(path.read_text(), path.name, [
('''              uint32_t dataSize, uint64_t offset);
    ~V4L2Frame() override;''', '''              uint32_t dataSize, uint64_t offset);
    // Pocket: a frame received from the host, owned in memory instead of mmap'ed from V4L2.
    V4L2Frame(uint32_t w, uint32_t h, uint32_t fourcc, std::vector<uint8_t>&& data);
    ~V4L2Frame() override;'''),
('''    uint8_t* mData = nullptr;
    bool  mMapped = false;
};''', '''    uint8_t* mData = nullptr;
    bool  mMapped = false;
    std::vector<uint8_t> mHeapData;
};'''),
]))
