// SPDX-License-Identifier: Apache-2.0
// ExternalCameraDeviceSession streaming functions over host I420 frames, from the Android 9 port
// (runtime/camera/external-camera-pocket.patch); spliced into 12L by port_session.py.
int ExternalCameraDeviceSession::v4l2StreamOffLocked() {
    if (!mV4l2Streaming) {
        return OK;
    }

    {
        std::lock_guard<std::mutex> lk(mV4l2BufferLock);
        if (mNumDequeuedV4l2Buffers != 0)  {
            ALOGE("%s: there are %zu inflight V4L buffers",
                __FUNCTION__, mNumDequeuedV4l2Buffers);
            return -1;
        }
    }
    mV4L2BufferCount = 0;

    // Pocket: closing the stream socket stops the host camera.
    mV4l2Fd.reset();
    mV4l2Streaming = false;
    return OK;
}

int ExternalCameraDeviceSession::setV4l2FpsLocked(double fps) {
    // Pocket: the host stream is opened at its rate; see configureV4l2StreamLocked.
    mV4l2StreamingFps = fps;
    return 0;
}

int ExternalCameraDeviceSession::configureV4l2StreamLocked(
        const SupportedV4L2Format& v4l2Fmt, double requestFps) {
    ATRACE_CALL();
    int ret = v4l2StreamOffLocked();
    if (ret != OK) {
        ALOGE("%s: stop v4l2 streaming failed: ret %d", __FUNCTION__, ret);
        return ret;
    }

    const double kDefaultFps = 30.0;
    double fps = 1000.0;
    if (requestFps != 0.0) {
        fps = requestFps;
    } else {
        double maxFps = -1.0;
        // Try to pick the slowest fps that is at least 30
        for (const auto& fr : v4l2Fmt.frameRates) {
            double f = fr.getDouble();
            if (maxFps < f) {
                maxFps = f;
            }
            if (f >= kDefaultFps && f < fps) {
                fps = f;
            }
        }
        if (fps == 1000.0) {
            fps = maxFps;
        }
    }

    // Pocket: the host streams tightly packed I420 of exactly this size.
    int fd = ::pocket::hostCameraOpen(mCameraId, v4l2Fmt.width, v4l2Fmt.height,
                                      static_cast<int>(fps + 0.5));
    if (fd < 0) {
        ALOGE("%s: host camera %s stream %dx%d failed: %d", __FUNCTION__, mCameraId.c_str(),
                v4l2Fmt.width, v4l2Fmt.height, fd);
        return fd;
    }
    mV4l2Fd.reset(fd);
    mMaxV4L2BufferSize = v4l2Fmt.width * v4l2Fmt.height * 3 / 2;
    setV4l2FpsLocked(fps);
    mV4L2BufferCount = (fps >= kDefaultFps) ? mCfg.numVideoBuffers : mCfg.numStillBuffers;

    ALOGI("%s: start host streaming %dx%d@%ffps",
                __FUNCTION__, v4l2Fmt.width, v4l2Fmt.height, fps);
    mV4l2StreamingFmt = v4l2Fmt;
    mV4l2Streaming = true;
    return OK;
}

sp<V4L2Frame> ExternalCameraDeviceSession::dequeueV4l2FrameLocked(/*out*/nsecs_t* shutterTs) {
    ATRACE_CALL();
    sp<V4L2Frame> ret = nullptr;

    if (shutterTs == nullptr) {
        ALOGE("%s: shutterTs must not be null!", __FUNCTION__);
        return ret;
    }

    {
        std::unique_lock<std::mutex> lk(mV4l2BufferLock);
        if (mNumDequeuedV4l2Buffers == mV4L2BufferCount) {
            int waitRet = waitForV4L2BufferReturnLocked(lk);
            if (waitRet != 0) {
                return ret;
            }
        }
    }

    // Pocket: one host frame per request; the count above still bounds frames in flight.
    std::vector<uint8_t> data(mMaxV4L2BufferSize);
    int64_t hostTimestamp;
    ATRACE_BEGIN("hostCameraRead");
    int readRet = ::pocket::hostCameraRead(mV4l2Fd.get(), data, &hostTimestamp);
    ATRACE_END();
    if (readRet != 0) {
        ALOGE("%s: host frame read failed: %d", __FUNCTION__, readRet);
        return ret;
    }
    // The host's sensor clock may be BOOTTIME; the guest declares an unknown (monotonic) source.
    *shutterTs = systemTime(SYSTEM_TIME_MONOTONIC);

    {
        std::lock_guard<std::mutex> lk(mV4l2BufferLock);
        mNumDequeuedV4l2Buffers++;
    }
    return new V4L2Frame(
            mV4l2StreamingFmt.width, mV4l2StreamingFmt.height, mV4l2StreamingFmt.fourcc,
            std::move(data));
}

void ExternalCameraDeviceSession::enqueueV4l2Frame(const sp<V4L2Frame>& frame) {
    ATRACE_CALL();
    frame->unmap();

    {
        std::lock_guard<std::mutex> lk(mV4l2BufferLock);
        mNumDequeuedV4l2Buffers--;
    }
    mV4L2BufferReturned.notify_one();
}
