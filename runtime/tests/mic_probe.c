// SPDX-License-Identifier: GPL-3.0-only
// Record from the guest's default input through the stock AAudio/AudioRecord/AudioFlinger path
// and check real-time delivery. A 16 kHz request exercises AudioFlinger's resampler.
#include <aaudio/AAudio.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static int64_t now_ms(void) {
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

int main(int argc,char** argv) {
    int seconds=argc>1?atoi(argv[1]):3,rate=16000;
    AAudioStreamBuilder* builder;AAudioStream* stream;
    if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)return 2;
    AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder,rate);
    AAudioStreamBuilder_setChannelCount(builder,1);
    AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);
    aaudio_result_t result=AAudioStreamBuilder_openStream(builder,&stream);
    AAudioStreamBuilder_delete(builder);
    if(result!=AAUDIO_OK) {printf("MIC_PROBE open=%s\nMIC_PROBE_FAIL\n",AAudio_convertResultToText(result));return 1;}
    printf("MIC_PROBE stream rate=%d channels=%d device=%d\n",AAudioStream_getSampleRate(stream),
           AAudioStream_getChannelCount(stream),AAudioStream_getDeviceId(stream));
    if((result=AAudioStream_requestStart(stream))!=AAUDIO_OK) {
        printf("MIC_PROBE start=%s\nMIC_PROBE_FAIL\n",AAudio_convertResultToText(result));return 1;
    }
    int16_t pcm[1600];// 100 ms
    int64_t frames=0,nonzero=0,peak=0;double energy=0;
    int64_t start=now_ms(),first=-1;
    while(frames<(int64_t)seconds*rate) {
        result=AAudioStream_read(stream,pcm,1600,1000000000LL);
        if(result<0) {printf("MIC_PROBE read=%s\n",AAudio_convertResultToText(result));break;}
        for(int i=0;i<result;i++) {
            int v=abs(pcm[i]);if(v)nonzero++;if(v>peak)peak=v;energy+=(double)pcm[i]*pcm[i];
        }
        if(first<0 && result>0) first=now_ms();
        frames+=result;
    }
    int64_t end=now_ms(),elapsed=end-start,startup=first<0?elapsed:first-start,steady=end-(first<0?end:first);
    AAudioStream_requestStop(stream);AAudioStream_close(stream);
    printf("MIC_PROBE frames=%lld elapsed_ms=%lld startup_ms=%lld steady_ms=%lld nonzero=%lld peak=%lld rms=%.1f\n",(long long)frames,(long long)elapsed,(long long)startup,(long long)steady,
           (long long)nonzero,(long long)peak,frames?sqrt(energy/frames):0.0);
    // After the first block, delivery must follow real time: neither instant nor stalled. Opening the host
    // microphone may take a moment. Content depends on the host microphone.
    int64_t expected=(int64_t)seconds*1000-100;
    int realtime=frames>=(int64_t)seconds*rate && startup<=3000 && steady>=expected-500 && steady<=expected+1000;
    printf(realtime?"MIC_PROBE_PASS\n":"MIC_PROBE_FAIL\n");
    return realtime?0:1;
}
