// SPDX-License-Identifier: GPL-3.0-only
// Drive a guest camera through the stock NDK Camera2 -> cameraserver -> camera HAL path:
// list cameras, stream YUV 640x480 for a few seconds, then take one JPEG still.
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImageReader.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int frames,jpegs;
static long long jpegBytes;
static int jpegValid;
static double lumaMean,lumaDeviation;
static int64_t firstStamp,lastStamp;

static int64_t nowMs(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000LL+t.tv_nsec/1000000;}

static void onYuv(void* context,AImageReader* reader) {
    (void)context;AImage* image=NULL;
    if(AImageReader_acquireLatestImage(reader,&image)!=AMEDIA_OK || !image)return;
    int64_t stamp=0;AImage_getTimestamp(image,&stamp);
    uint8_t* y=NULL;int length=0,stride=0,width=0,height=0;
    AImage_getPlaneData(image,0,&y,&length);AImage_getPlaneRowStride(image,0,&stride);
    AImage_getWidth(image,&width);AImage_getHeight(image,&height);
    double sum=0,squares=0;long count=0;
    for(int row=0;row<height;row+=8)for(int col=0;col<width;col+=8){double v=y[row*stride+col];sum+=v;squares+=v*v;count++;}
    pthread_mutex_lock(&lock);
    if(!frames)firstStamp=stamp;
    lastStamp=stamp;frames++;
    if(count){lumaMean=sum/count;lumaDeviation=sqrt(squares/count-lumaMean*lumaMean);}
    pthread_mutex_unlock(&lock);
    AImage_delete(image);
}
static void onJpeg(void* context,AImageReader* reader) {
    (void)context;AImage* image=NULL;
    if(AImageReader_acquireNextImage(reader,&image)!=AMEDIA_OK || !image)return;
    uint8_t* data=NULL;int length=0;AImage_getPlaneData(image,0,&data,&length);
    // The BLOB buffer is padded; the JPEG ends at its EOI marker.
    int end=-1;for(int i=0;i+1<length;i++)if(data[i]==0xFF && data[i+1]==0xD9)end=i+2;
    pthread_mutex_lock(&lock);
    jpegs++;jpegBytes=end;jpegValid=length>4 && data[0]==0xFF && data[1]==0xD8 && end>0;
    pthread_mutex_unlock(&lock);
    AImage_delete(image);
}
static void onDisconnected(void* c,ACameraDevice* d){(void)c;(void)d;printf("CAMERA_PROBE device_disconnected\n");}
static void onError(void* c,ACameraDevice* d,int e){(void)c;(void)d;printf("CAMERA_PROBE device_error=%d\n",e);}
static void onSessionNoop(void* c,ACameraCaptureSession* s){(void)c;(void)s;}

int main(int argc,char** argv) {
    int seconds=argc>1?atoi(argv[1]):3;
    ACameraManager* manager=ACameraManager_create();
    ACameraIdList* ids=NULL;
    if(ACameraManager_getCameraIdList(manager,&ids)!=ACAMERA_OK || !ids || ids->numCameras<1) {
        printf("CAMERA_PROBE cameras=0\nCAMERA_PROBE_FAIL\n");return 1;
    }
    printf("CAMERA_PROBE cameras=%d\n",ids->numCameras);
    const char* chosen=ids->cameraIds[0];
    int32_t jpegWidth=0,jpegHeight=0;
    for(int i=0;i<ids->numCameras;i++) {
        ACameraMetadata* chars=NULL;ACameraManager_getCameraCharacteristics(manager,ids->cameraIds[i],&chars);
        ACameraMetadata_const_entry e;int facing=-1,orientation=-1,level=-1;
        if(ACameraMetadata_getConstEntry(chars,ACAMERA_LENS_FACING,&e)==ACAMERA_OK)facing=e.data.u8[0];
        if(ACameraMetadata_getConstEntry(chars,ACAMERA_SENSOR_ORIENTATION,&e)==ACAMERA_OK)orientation=e.data.i32[0];
        if(ACameraMetadata_getConstEntry(chars,ACAMERA_INFO_SUPPORTED_HARDWARE_LEVEL,&e)==ACAMERA_OK)level=e.data.u8[0];
        int sizes=0;
        if(ACameraMetadata_getConstEntry(chars,ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS,&e)==ACAMERA_OK)
            for(uint32_t j=0;j+3<e.count;j+=4) {
                if(e.data.i32[j+3]!=ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT)continue;
                if(e.data.i32[j]==AIMAGE_FORMAT_YUV_420_888)sizes++;
                if(i==0 && e.data.i32[j]==AIMAGE_FORMAT_JPEG && e.data.i32[j+1]*e.data.i32[j+2]>jpegWidth*jpegHeight)
                    {jpegWidth=e.data.i32[j+1];jpegHeight=e.data.i32[j+2];}
            }
        printf("CAMERA_PROBE camera id=%s facing=%d orientation=%d level=%d yuv_sizes=%d\n",ids->cameraIds[i],facing,orientation,level,sizes);
        ACameraMetadata_free(chars);
    }
    ACameraDevice_StateCallbacks deviceCallbacks={NULL,onDisconnected,onError};
    ACameraDevice* device=NULL;
    camera_status_t status=ACameraManager_openCamera(manager,chosen,&deviceCallbacks,&device);
    if(status!=ACAMERA_OK){printf("CAMERA_PROBE open=%d\nCAMERA_PROBE_FAIL\n",status);return 1;}
    AImageReader *yuv=NULL,*jpeg=NULL;
    AImageReader_new(640,480,AIMAGE_FORMAT_YUV_420_888,4,&yuv);
    AImageReader_new(jpegWidth,jpegHeight,AIMAGE_FORMAT_JPEG,2,&jpeg);
    AImageReader_ImageListener yuvListener={NULL,onYuv},jpegListener={NULL,onJpeg};
    AImageReader_setImageListener(yuv,&yuvListener);AImageReader_setImageListener(jpeg,&jpegListener);
    ANativeWindow *yuvWindow,*jpegWindow;AImageReader_getWindow(yuv,&yuvWindow);AImageReader_getWindow(jpeg,&jpegWindow);
    ACaptureSessionOutputContainer* outputs;ACaptureSessionOutputContainer_create(&outputs);
    ACaptureSessionOutput *yuvOutput,*jpegOutput;
    ACaptureSessionOutput_create(yuvWindow,&yuvOutput);ACaptureSessionOutput_create(jpegWindow,&jpegOutput);
    ACaptureSessionOutputContainer_add(outputs,yuvOutput);ACaptureSessionOutputContainer_add(outputs,jpegOutput);
    ACameraCaptureSession_stateCallbacks sessionCallbacks={NULL,onSessionNoop,onSessionNoop,onSessionNoop};
    ACameraCaptureSession* session=NULL;
    status=ACameraDevice_createCaptureSession(device,outputs,&sessionCallbacks,&session);
    if(status!=ACAMERA_OK){printf("CAMERA_PROBE session=%d\nCAMERA_PROBE_FAIL\n",status);return 1;}
    ACaptureRequest *preview,*still;ACameraOutputTarget *yuvTarget,*jpegTarget;
    ACameraOutputTarget_create(yuvWindow,&yuvTarget);ACameraOutputTarget_create(jpegWindow,&jpegTarget);
    ACameraDevice_createCaptureRequest(device,TEMPLATE_PREVIEW,&preview);ACaptureRequest_addTarget(preview,yuvTarget);
    ACameraDevice_createCaptureRequest(device,TEMPLATE_STILL_CAPTURE,&still);ACaptureRequest_addTarget(still,jpegTarget);
    int64_t start=nowMs();
    status=ACameraCaptureSession_setRepeatingRequest(session,NULL,1,&preview,NULL);
    if(status!=ACAMERA_OK){printf("CAMERA_PROBE repeating=%d\nCAMERA_PROBE_FAIL\n",status);return 1;}
    sleep(seconds);
    ACameraCaptureSession_stopRepeating(session);
    int64_t elapsed=nowMs()-start;
    status=ACameraCaptureSession_capture(session,NULL,1,&still,NULL);
    for(int i=0;i<100;i++){pthread_mutex_lock(&lock);int done=jpegs;pthread_mutex_unlock(&lock);if(done)break;usleep(100000);}
    pthread_mutex_lock(&lock);
    double fps=frames>1?(frames-1)*1e9/(double)(lastStamp-firstStamp):0;
    printf("CAMERA_PROBE preview frames=%d elapsed_ms=%lld fps=%.1f luma_mean=%.1f luma_deviation=%.1f\n",
           frames,(long long)elapsed,fps,lumaMean,lumaDeviation);
    printf("CAMERA_PROBE still capture=%d size=%dx%d jpeg_bytes=%lld jpeg_valid=%d\n",status,jpegWidth,jpegHeight,jpegBytes,jpegValid);
    // Preview must keep flowing; image content depends on the host camera.
    int ok=frames>=seconds*5 && jpegValid;
    pthread_mutex_unlock(&lock);
    ACameraCaptureSession_close(session);ACameraDevice_close(device);
    printf(ok?"CAMERA_PROBE_PASS\n":"CAMERA_PROBE_FAIL\n");
    return ok?0:1;
}
