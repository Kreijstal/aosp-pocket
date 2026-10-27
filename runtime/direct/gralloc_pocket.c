// SPDX-License-Identifier: GPL-3.0-only
// Guest gralloc (gralloc.pocket.so) in front of the reference image's gralloc.redroid.so, which
// allocates ashmem buffers for RGB formats only. Camera formats (flexible YUV 4:2:0, YV12, NV21
// and BLOB for JPEG) become redroid RGBA buffers that start with a self-describing header page,
// so any process that imports the handle can lock them with the right plane layout. Every other
// buffer, and every registration, is redroid's unchanged.
#include <dlfcn.h>
#include <errno.h>
#include <hardware/gralloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system/graphics.h>

#define HEADER_BYTES 4096
#define HEADER_MAGIC 0x504b43414d425546ull // "PKCAMBUF"
#define ALIGN(v,a) (((v)+(a)-1)/(a)*(a))

struct header {
    uint64_t magic;
    uint32_t format, width, height, ystride, cstride, size;
};

static gralloc_module_t* base;

static int load_base(void) {
    if (base) return 0;
    void* library = dlopen("/vendor/lib64/hw/gralloc.redroid.so", RTLD_NOW | RTLD_LOCAL);
    if (library) base = (gralloc_module_t*)dlsym(library, HAL_MODULE_INFO_SYM_AS_STR);
    if (!base) fprintf(stderr, "POCKET_GRALLOC_BASE_UNAVAILABLE %s\n", dlerror());
    return base ? 0 : -ENODEV;
}

// Describes a camera-format buffer; returns 0 for formats redroid handles itself.
static int describe(int format, int width, int height, struct header* h) {
    memset(h, 0, sizeof(*h));
    h->magic = HEADER_MAGIC; h->format = format; h->width = width; h->height = height;
    switch (format) {
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
    case HAL_PIXEL_FORMAT_YV12:
        if ((width & 1) || (height & 1)) return -EINVAL;
        h->ystride = ALIGN(width, 16); h->cstride = ALIGN(h->ystride / 2, 16);
        h->size = h->ystride * height + h->cstride * height;
        return 1;
    case HAL_PIXEL_FORMAT_YCrCb_420_SP: // NV21: VU interleaved, stride equal to width
        if ((width & 1) || (height & 1)) return -EINVAL;
        h->ystride = width; h->cstride = width;
        h->size = width * height * 3 / 2;
        return 1;
    case HAL_PIXEL_FORMAT_BLOB:
        if (height != 1 || width <= 0) return -EINVAL;
        h->size = width;
        return 1;
    }
    return 0;
}

// Locks through redroid; *header is set for camera-format buffers, whose payload follows it.
static int base_lock(buffer_handle_t handle, int usage, void** address, const struct header** found) {
    void* mapped = NULL;
    // Redroid maps the whole buffer and ignores the region; the header page is in the first row.
    int result = base->lock(base, handle, usage | GRALLOC_USAGE_SW_READ_OFTEN, 0, 0, 1, 1, &mapped);
    if (result) return result;
    const struct header* h = mapped;
    *found = h && h->magic == HEADER_MAGIC ? h : NULL;
    *address = mapped;
    return 0;
}

static int pocket_register(gralloc_module_t const* m, buffer_handle_t handle) {
    (void)m; return base->registerBuffer(base, handle);
}
static int pocket_unregister(gralloc_module_t const* m, buffer_handle_t handle) {
    (void)m; return base->unregisterBuffer(base, handle);
}
static int pocket_lock(gralloc_module_t const* m, buffer_handle_t handle, int usage,
                       int l, int t, int w, int h, void** vaddr) {
    (void)m;
    void* mapped; const struct header* found;
    int result = base_lock(handle, usage, &mapped, &found);
    if (result) return result;
    if (!found) {
        // Not ours: redroid's own lock, with the caller's region.
        base->unlock(base, handle);
        return base->lock(base, handle, usage, l, t, w, h, vaddr);
    }
    *vaddr = (uint8_t*)mapped + HEADER_BYTES;
    return 0;
}
static int pocket_lock_ycbcr(gralloc_module_t const* m, buffer_handle_t handle, int usage,
                             int l, int t, int w, int h, struct android_ycbcr* ycbcr) {
    (void)m; (void)l; (void)t; (void)w; (void)h;
    void* mapped; const struct header* found;
    int result = base_lock(handle, usage, &mapped, &found);
    if (result) return result;
    uint8_t* y = (uint8_t*)mapped + HEADER_BYTES;
    if (!found || found->format == HAL_PIXEL_FORMAT_BLOB) {
        base->unlock(base, handle);
        return -EINVAL;
    }
    memset(ycbcr, 0, sizeof(*ycbcr));
    ycbcr->y = y; ycbcr->ystride = found->ystride; ycbcr->cstride = found->cstride;
    uint8_t* chroma = y + found->ystride * found->height;
    if (found->format == HAL_PIXEL_FORMAT_YCrCb_420_SP) {
        ycbcr->cr = chroma; ycbcr->cb = chroma + 1; ycbcr->chroma_step = 2;
    } else if (found->format == HAL_PIXEL_FORMAT_YV12) {
        ycbcr->cr = chroma; ycbcr->cb = chroma + found->cstride * found->height / 2; ycbcr->chroma_step = 1;
    } else {
        ycbcr->cb = chroma; ycbcr->cr = chroma + found->cstride * found->height / 2; ycbcr->chroma_step = 1;
    }
    return 0;
}
static int pocket_unlock(gralloc_module_t const* m, buffer_handle_t handle) {
    (void)m; return base->unlock(base, handle);
}

struct pocket_alloc {
    alloc_device_t device;
    alloc_device_t* base;
};

static int pocket_alloc(alloc_device_t* dev, int w, int h, int format, int usage,
                        buffer_handle_t* handle, int* stride) {
    struct pocket_alloc* self = (struct pocket_alloc*)dev;
    struct header header;
    int camera = describe(format, w, h, &header);
    if (camera < 0) return camera;
    if (!camera) return self->base->alloc(self->base, w, h, format, usage, handle, stride);
    // An RGBA buffer 1024 pixels (4096 bytes) wide holds the header page plus the payload.
    int rows = (HEADER_BYTES + header.size + 4095) / 4096, ignored;
    int sw = GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN;
    int result = self->base->alloc(self->base, 1024, rows, HAL_PIXEL_FORMAT_RGBA_8888,
                                   (usage & ~GRALLOC_USAGE_SW_READ_MASK & ~GRALLOC_USAGE_SW_WRITE_MASK) | sw,
                                   handle, &ignored);
    if (result) return result;
    void* mapped = NULL;
    result = base->lock(base, *handle, GRALLOC_USAGE_SW_WRITE_OFTEN, 0, 0, 1024, rows, &mapped);
    if (result) { self->base->free(self->base, *handle); return result; }
    memcpy(mapped, &header, sizeof(header));
    base->unlock(base, *handle);
    *stride = format == HAL_PIXEL_FORMAT_BLOB ? w : (int)header.ystride;
    return 0;
}
static int pocket_free(alloc_device_t* dev, buffer_handle_t handle) {
    struct pocket_alloc* self = (struct pocket_alloc*)dev;
    return self->base->free(self->base, handle);
}
static void pocket_dump(alloc_device_t* dev, char* buffer, int length) {
    struct pocket_alloc* self = (struct pocket_alloc*)dev;
    if (self->base->dump) self->base->dump(self->base, buffer, length);
}
static int pocket_close(hw_device_t* dev) {
    struct pocket_alloc* self = (struct pocket_alloc*)dev;
    int result = self->base->common.close(&self->base->common);
    free(self);
    return result;
}

static int pocket_open(const hw_module_t* module, const char* name, hw_device_t** device) {
    int result = load_base();
    if (result) return result;
    hw_device_t* opened = NULL;
    result = base->common.methods->open(&base->common, name, &opened);
    if (result || strcmp(name, GRALLOC_HARDWARE_GPU0)) { *device = opened; return result; }
    struct pocket_alloc* self = calloc(1, sizeof(*self));
    if (!self) { opened->close(opened); return -ENOMEM; }
    self->base = (alloc_device_t*)opened;
    self->device.common = self->base->common;
    self->device.common.module = (hw_module_t*)module;
    self->device.common.close = pocket_close;
    self->device.alloc = pocket_alloc;
    self->device.free = pocket_free;
    self->device.dump = pocket_dump;
    *device = &self->device.common;
    return 0;
}

static struct hw_module_methods_t methods = {.open = pocket_open};

__attribute__((constructor)) static void pocket_load(void) { load_base(); }

__attribute__((visibility("default"))) gralloc_module_t HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        // 0.2: lock_ycbcr, without the asynchronous entry points (callers then wait on fences).
        .module_api_version = GRALLOC_MODULE_API_VERSION_0_2,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = GRALLOC_HARDWARE_MODULE_ID,
        .name = "Pocket gralloc (camera formats over redroid)",
        .author = "aosp-pocket",
        .methods = &methods,
    },
    .registerBuffer = pocket_register,
    .unregisterBuffer = pocket_unregister,
    .lock = pocket_lock,
    .unlock = pocket_unlock,
    .lock_ycbcr = pocket_lock_ycbcr,
};
