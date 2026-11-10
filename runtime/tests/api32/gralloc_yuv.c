// SPDX-License-Identifier: GPL-3.0-only
// Exercise the actual gralloc module, including a handle imported by a child.
#include <hardware/gralloc.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef POCKET_NATIVE_MEMFD_FIXTURE
#include <sys/mman.h>
// The Linux-only fixture has no Android property area/ashmem device. Use the
// same kernel memfd backing selected by sys.use_memfd on the Android guest.
int ashmem_create_region(const char* name, size_t size) {
    int fd = memfd_create(name, MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd >= 0 && ftruncate(fd, size)) { close(fd); return -1; }
    return fd;
}
#endif

static int verify(const gralloc_module_t* module, buffer_handle_t handle,
                  int left, int top, int width, int height) {
    struct android_ycbcr p = {0};
    int rc = module->lock_ycbcr(module, handle, GRALLOC_USAGE_SW_READ_OFTEN,
                               left, top, width, height, &p);
    if (rc || !p.y || !p.cb || !p.cr || p.ystride != 160 ||
            p.cstride != 80 || p.chroma_step != 1) return 20;
    for (int y = 0; y < 120; ++y) for (int x = 0; x < 160; ++x)
        if (((uint8_t*)p.y)[y * p.ystride + x] != (uint8_t)(x + y)) return 21;
    for (int y = 0; y < 60; ++y) for (int x = 0; x < 80; ++x)
        if (((uint8_t*)p.cb)[y * p.cstride + x] != 71 ||
                ((uint8_t*)p.cr)[y * p.cstride + x] != 191) return 22;
    return module->unlock(module, handle) ? 23 : 0;
}
int main(void) {
    void* library = dlopen("/vendor/lib64/hw/gralloc.pocket.so", RTLD_NOW | RTLD_LOCAL);
    if (!library) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    const gralloc_module_t* module = dlsym(library, HAL_MODULE_INFO_SYM_AS_STR);
    if (!module || !module->lock_ycbcr) return 2;
    alloc_device_t* dev = NULL;
    if (module->common.methods->open(&module->common, GRALLOC_HARDWARE_GPU0,
                                    (hw_device_t**)&dev)) return 3;
    const int formats[] = {HAL_PIXEL_FORMAT_YCbCr_420_888, HAL_PIXEL_FORMAT_YV12};
    for (unsigned f = 0; f < sizeof(formats)/sizeof(formats[0]); ++f) {
        buffer_handle_t handle = NULL; int stride = 0;
        int allocated = dev->alloc(dev, 160, 120, formats[f],
                       GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN,
                       &handle, &stride);
        if (allocated || stride != 160) {
            fprintf(stderr, "GRALLOC_YUV_ALLOC_FAIL format=%d status=%d stride=%d errno=%d\n",
                    formats[f], allocated, stride, errno);
            return 4;
        }
        struct android_ycbcr p = {0};
        if (module->lock_ycbcr(module, handle, GRALLOC_USAGE_SW_WRITE_OFTEN,
                              0, 0, 160, 120, &p) || !p.y || !p.cb || !p.cr) return 5;
        for (int y = 0; y < 120; ++y) for (int x = 0; x < 160; ++x)
            ((uint8_t*)p.y)[y*p.ystride+x] = (uint8_t)(x+y);
        memset(p.cb, 71, p.cstride*60); memset(p.cr, 191, p.cstride*60);
        if (module->unlock(module, handle)) return 6;
        pid_t child = fork();
        if (child < 0) return 7;
        if (!child) {
            size_t bytes = sizeof(native_handle_t) + (handle->numFds+handle->numInts)*sizeof(int);
            native_handle_t* copy = malloc(bytes); if (!copy) _exit(8);
            memcpy(copy, handle, bytes); copy->data[0] = dup(handle->data[0]);
            if (copy->data[0] < 0 || module->registerBuffer(module, copy)) _exit(9);
            int result = verify(module, copy, 0, 0, 0, 0);
            if (!result) result = verify(module, copy, 1, 3, 79, 61);
            if (!result) result = verify(module, copy, 0, 0, 160, 120);
            struct android_ycbcr invalid = {0};
            if (!result && module->lock_ycbcr(module, copy, GRALLOC_USAGE_SW_READ_OFTEN,
                                              159, 0, 2, 120, &invalid) != -EINVAL) result = 24;
            if (!result && module->lock_ycbcr(module, copy, GRALLOC_USAGE_SW_READ_OFTEN,
                                              0, 0, 0, 120, &invalid) != -EINVAL) result = 25;
            if (module->unregisterBuffer(module, copy)) result = 26;
            close(copy->data[0]); free(copy); _exit(result);
        }
        int status = 0;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) {
            fprintf(stderr, "GRALLOC_YUV_CHILD_FAIL format=%d status=%d\n", formats[f], status);
            return 10;
        }
        if (dev->free(dev, handle)) return 11;
    }
    if (dev->common.close((hw_device_t*)dev)) return 12;
    puts("API32_GRALLOC_YUV_PASS formats=2 imported_child=1 empty_crop=1 odd_crop=1 full_crop=1 bounds_rejection=1 pixels_verified=1");
    return 0;
}
