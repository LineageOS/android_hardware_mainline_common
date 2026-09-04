/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#include "gralloc_gbm_mesa.h"
#include "gbm_mesa_loader.h"

#define LOG_TAG "libgralloc_gm"

#include <assert.h>
#include <errno.h>
#include <error.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syscall.h>

// #include <mutex>
#include <unordered_map>

#include <cutils/log.h>
#include <cutils/properties.h>
#include <hardware/gralloc.h>
#include <sync/sync.h>

#include "log.h"

// We store the BO with a K,V map [buffer_handle_t, struct gbm_bo] named gbm_bo_handle_map.
static std::unordered_map<buffer_handle_t, struct gbm_bo *> gbm_bo_handle_map;

// static std::mutex _gbm_bo_handle_map_mutex;
// static std::mutex _gbm_dev_mutex;

static int _gbm_dev_fd = -1;
static struct gbm_device* _gbm_dev = nullptr;

int gralloc_gbm_device_init() {
    int fd = -1;
    char device_path[PROPERTY_VALUE_MAX];
    struct gbm_device *dev = nullptr;

    if (!setup_gbm_priv_ops()) {
        log_e("Failed to initialize GBM function pointers!");
        return -EINVAL;
    }

    __redirect_standard_outputs();

    property_get(GRALLOC_DEFAULT_DEVICE_PROP, device_path, GRALLOC_DEFAULT_DEVICE_PATH);

    fd = open(device_path, O_RDWR | O_CLOEXEC); //TODO: Shall we add O_CLOEXEC?
    if (fd < 0) {
        log_e("Failed to open device %s, err=%d", device_path, errno);
        return -EINVAL;
    }
    log_v("opened device %s, fd=%d", device_path, fd);

    if (gralloc_gbm_device_create(fd, &dev)) {
        log_e("Failed to initialize the gralloc_gm because cannot create GBM device!");
        return -EINVAL;
    }

    log_i("The GBM device has been initialized, fd=%d, dev_fd=%d", fd, _gbm_dev_fd);

    // we shouldn't close the fd.
    return _gbm_dev_fd;
}

// The GBM (DRI backend) supported formats can be found at gbm_dri_visuals_table[]
// in <mesa_dir>/src/gbm/backends/dri/gbm_dri.c
// The DRI supported formats can be found at dri2_format_table[]
// in <mesa_dir>/src/gallium/frontends/dri/dri_helpers.c
uint32_t gralloc_gm_android_format_to_gbm_format(uint32_t android_format)
{
    uint32_t fmt;

    /* The order of color format should be reversed 
       while converting Android format to GBM format */
    switch (android_format) {
    case HAL_PIXEL_FORMAT_RGBA_8888:
        fmt = GBM_FORMAT_ABGR8888; // not GBM_FORMAT_RGBA8888
        break;
    case HAL_PIXEL_FORMAT_RGBX_8888:
        fmt = GBM_FORMAT_XBGR8888; // not GBM_FORMAT_RGBX8888
        break;
    case HAL_PIXEL_FORMAT_RGB_888:
        fmt = GBM_FORMAT_BGR888; // not GBM_FORMAT_RGB888
        break;
    case HAL_PIXEL_FORMAT_RGB_565:
        fmt = GBM_FORMAT_BGR565; // not GBM_FORMAT_RGB565
        break;
    case HAL_PIXEL_FORMAT_BGRA_8888:
        fmt = GBM_FORMAT_ARGB8888; // not GBM_FORMAT_BGRA8888
        break;
    case HAL_PIXEL_FORMAT_RAW16:
        fmt = GBM_FORMAT_R16;
        break;
    case HAL_PIXEL_FORMAT_YV12:
        /* YV12 is planar, but must be a single buffer so ask for GR88 */
        fmt = GBM_FORMAT_GR88;
        break;
    case HAL_PIXEL_FORMAT_Y8:
        fmt = GBM_FORMAT_R8;
        break;
    case HAL_PIXEL_FORMAT_Y16:
        fmt = GBM_FORMAT_R16;
        break;
    case HAL_PIXEL_FORMAT_RGBA_FP16:
        fmt = GBM_FORMAT_ABGR16161616F;
        break;
    case HAL_PIXEL_FORMAT_RGBA_1010102:
        fmt = GBM_FORMAT_ABGR2101010; // not GBM_FORMAT_RGBA1010102
        break;
    case HAL_PIXEL_FORMAT_YCbCr_422_SP:
        fmt = GBM_FORMAT_YUV422;
        break;
    case HAL_PIXEL_FORMAT_YCbCr_420_888:
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        fmt = GBM_FORMAT_YUV420;
        break;
    case HAL_PIXEL_FORMAT_YCBCR_P010:
        fmt = __gbm_fourcc_code('P', '0', '1', '0');
        break;
    /*
     * Choose GBM_FORMAT_R8 because <system/graphics.h> requires the buffers
     * with a format HAL_PIXEL_FORMAT_BLOB have a height of 1, and width
     * equal to their size in bytes.
     */
    case HAL_PIXEL_FORMAT_BLOB:
        fmt = GBM_FORMAT_R8;
        break;
    case HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED:
        return __gbm_fourcc_code('9','9','9','8');
        break;
    default:
        fmt = 0;
        log_e("Unknown android format '%d', failed to convert!", android_format);
        break;
    }

    log_v("convert android format '%d' to '%d'", android_format, fmt);
    return fmt;
}

unsigned int gralloc_gm_get_gbm_flags_from_android_usage(int usage, int android_format)
{
    unsigned int flags = 0;
    uint32_t gbm_format = gralloc_gm_android_format_to_gbm_format(android_format);

    if (usage & (GRALLOC_USAGE_SW_READ_OFTEN | GRALLOC_USAGE_SW_WRITE_OFTEN))
        flags |= GBM_BO_USE_LINEAR;
    if (usage & GRALLOC_USAGE_CURSOR) {
        switch (gbm_format) {
            case GBM_FORMAT_ARGB8888:
                flags |= GBM_BO_USE_CURSOR;
                break;
            default:
                log_w("The GBM_BO_USE_CURSOR is required but using unsupported format (%d).", android_format);
                break;
        }
    }
    if (usage & (GRALLOC_USAGE_HW_RENDER | GRALLOC_USAGE_HW_TEXTURE))
        flags |= GBM_BO_USE_RENDERING;
    if (usage & GRALLOC_USAGE_HW_FB)
        flags |= GBM_BO_USE_SCANOUT;
    if (usage & GRALLOC_USAGE_HW_COMPOSER)
        flags |= GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING;
    if (usage & GRALLOC_USAGE_HW_RENDER)
        flags |= GBM_BO_USE_RENDERING;
    if (usage & GRALLOC_USAGE_PROTECTED)
        flags |= GBM_BO_USE_PROTECTED;

    if ((flags & GBM_BO_USE_SCANOUT) &&
        !(gbm_format == GBM_FORMAT_XRGB8888 || gbm_format == GBM_FORMAT_XBGR8888)) {
        log_w("We added GBM_BO_USE_SCANOUT but using unsupported format (%d).\n" \
              "This may cause an issue if the flags has GBM_BO_USE_WRITE.", android_format);
    }

    return flags;
}

int gralloc_gm_get_bpp_from_gbm_format(int gbm_format) {
    int bpp;

    switch (gbm_format) {
        case GBM_FORMAT_C8:
        case GBM_FORMAT_RGB332:
        case GBM_FORMAT_BGR233:
            bpp = 8; break;
        case GBM_FORMAT_YUV420: // Planar/semi-planar YUV420 formats
        case GBM_FORMAT_NV12:
        case GBM_FORMAT_NV21:
        case GBM_FORMAT_YVU420:
            bpp = 12; break;
        case GBM_FORMAT_XRGB4444:
        case GBM_FORMAT_XBGR4444:
        case GBM_FORMAT_RGBX4444:
        case GBM_FORMAT_BGRX4444:
        case GBM_FORMAT_ARGB4444:
        case GBM_FORMAT_ABGR4444:
        case GBM_FORMAT_RGBA4444:
        case GBM_FORMAT_BGRA4444:
        case GBM_FORMAT_XRGB1555:
        case GBM_FORMAT_XBGR1555:
        case GBM_FORMAT_RGBX5551:
        case GBM_FORMAT_BGRX5551:
        case GBM_FORMAT_ARGB1555:
        case GBM_FORMAT_ABGR1555:
        case GBM_FORMAT_RGBA5551:
        case GBM_FORMAT_BGRA5551:
        case GBM_FORMAT_RGB565:
        case GBM_FORMAT_BGR565:
            bpp = 16; break;
        case GBM_FORMAT_YUYV:
        case GBM_FORMAT_YVYU:
        case GBM_FORMAT_UYVY:
        case GBM_FORMAT_VYUY:
        case GBM_FORMAT_YUV422: // Packed YUV422, e.g., UYVY
            bpp = 16; break;
        case GBM_FORMAT_R16:
        case GBM_FORMAT_GR88:
            bpp = 16; break;
        case GBM_FORMAT_RGB888:
        case GBM_FORMAT_BGR888:
            bpp = 24; break;
        case GBM_FORMAT_YUV444:
            bpp = 24; break;
        case GBM_FORMAT_XRGB8888:
        case GBM_FORMAT_XBGR8888:
        case GBM_FORMAT_RGBX8888:
        case GBM_FORMAT_BGRX8888:
        case GBM_FORMAT_ARGB8888:
        case GBM_FORMAT_ABGR8888:
        case GBM_FORMAT_RGBA8888:
        case GBM_FORMAT_BGRA8888:
        case GBM_FORMAT_XRGB2101010:
        case GBM_FORMAT_XBGR2101010:
        case GBM_FORMAT_ARGB2101010:
        case GBM_FORMAT_ABGR2101010:
            bpp = 32; break;
        case GBM_FORMAT_RG1616:
            bpp = 32; break;
        case GBM_FORMAT_XBGR16161616:
        case GBM_FORMAT_ABGR16161616:
            bpp = 64; break;
        case GBM_FORMAT_XBGR16161616F:
        case GBM_FORMAT_ABGR16161616F:
            bpp = 64; break;
        default:
            bpp = 0; break;
    }
    
    if (bpp == 0) {
        log_e("Unsupported or compressed GBM pixel format (%d)! "
            "Return bpp=0, and this will cause the 'stride' to be zero.", gbm_format);
    }
    
    log_v("set bpp to %d for format %d", bpp, gbm_format);

    return bpp;
}

int gralloc_gm_get_bytes_per_pixel_from_gbm_format(int gbm_format) {
    int bpp = gralloc_gm_get_bpp_from_gbm_format(gbm_format);
    return (bpp > 0) ? (bpp + 7)/8 : 4; // default: 4 bytes
}

int gralloc_gm_get_bytes_per_pixel_from_android_format(int android_format) {
    int gbm_format = gralloc_gm_android_format_to_gbm_format(android_format);
    return gralloc_gm_get_bytes_per_pixel_from_gbm_format(gbm_format);
}

uint32_t gralloc_gm_android_caculate_pixel_stride(uint32_t android_format, uint32_t stride) {
    int bytes_per_pixel = gralloc_gm_get_bytes_per_pixel_from_android_format(android_format);
    // TODO: Check the source of stride
    return DIV_ROUND_UP(stride, bytes_per_pixel);
}

int gralloc_gbm_device_create(int fd, struct gbm_device **dev) {
    if (!dev) {
        log_e("Invalid pointer to receive GBM device!");
        return -EINVAL;
    }

    // std::lock_guard<std::mutex> lock(_gbm_dev_mutex);

    if ((_gbm_dev_fd > 0) && _gbm_dev) {
        *dev = _gbm_dev;
        log_v("reusing existed GBM device.");
        return 0;
    }

    if (fd < 0) {
        log_e("Invalid fd to create GBM device, fd=%d", fd);
        return -EINVAL;
    }

    _gbm_dev = gbm_priv_ops.gbm_create_device(fd);
    if (!_gbm_dev) {
        log_e("Failed to create GBM device, fd=%d", fd);
        _gbm_dev_fd = -1;
        return -EINVAL;
    }

    _gbm_dev_fd = gbm_priv_ops.gbm_device_get_fd(_gbm_dev);
    log_i("Created the GBM device with backend '%s'.", gbm_priv_ops.gbm_device_get_backend_name(_gbm_dev));

    *dev = _gbm_dev;
    return 0;
}

bool gralloc_is_format_supported() {
    // TODO: Finish the pixel format checking.

    return true;
}

bool gralloc_is_desc_support(const struct gralloc_buffer_desc* desc) {
    uint32_t max_texture_size = gralloc_get_max_texture_2d_size();
    if (!gralloc_is_format_supported())
        return false;

    return desc->width <= max_texture_size && desc->height <= max_texture_size;
}

int32_t gralloc_allocate(const struct gralloc_buffer_desc *desc, int32_t *out_stride, native_handle_t **out_handle) {
    int ret = 0;
    size_t num_planes;
    size_t num_fds;
    size_t num_ints;
    struct gbm_bo *bo = nullptr;
    struct gbm_device *dev;
    native_handle_t *_handle = nullptr;
    gralloc_handle_t *handle = nullptr;

    if (!gralloc_is_desc_support(desc)) {
        log_e("Unsupported gralloc_buffer_desc, abort.");
        return -EINVAL;
    }

    ret = gralloc_gbm_device_create(_gbm_dev_fd, &dev);
    if (!dev) {
        log_e("Invalid GBM device, abort.");
        return ret;
    }

    // TODO: Does Android using GBM format directly?
    _handle = gralloc_handle_create(desc->width, desc->height, desc->android_format, desc->android_usage);
    if (!_handle) {
        log_e("Failed to create native handle, abort.");
        return -EINVAL;
    }
    buffer_handle_t buffer_handle = _handle;
    if (!buffer_handle) {
        log_e("Failed to convert native_handle_t to buffer_handle_t, abort.");
        return -EINVAL;
    }
    handle = gralloc_handle(buffer_handle);
    if (!handle) {
        log_e("Failed to create gralloc_handle_t from buffer_handle_t, abort.");
        return -EINVAL;
    }

    int format = gralloc_gm_android_format_to_gbm_format(handle->format);
    int flags = gralloc_gm_get_gbm_flags_from_android_usage(handle->usage, handle->format);
    int width, height;

    width = handle->width;
    height = handle->height;
    if (flags & GBM_BO_USE_CURSOR) {
        width = ALIGN(MAX(handle->width, 64), 16);
        height = ALIGN(MAX(handle->height, 64), 16);
    }

    /*
     * For YV12, we request GR88, so halve the width since we're getting
     * 16bpp. Then increase the height by 1.5 for the U and V planes.
     */
    if (handle->format == HAL_PIXEL_FORMAT_YV12) {
        width = ALIGN(handle->width, 32) / 2;
        height += ALIGN(handle->height, 2) / 2;
    }

    log_v("trying to create BO, size=%dx%d, fmt(gbm)=%d, usage=%x",
          handle->width, handle->height, format, flags);
    bo = gbm_priv_ops.gbm_bo_create(dev, width, height, format,
               flags);
    if (!bo) {
        log_e("Failed to create BO, size=%dx%d, fmt=%d, usage=%x",
              handle->width, handle->height, handle->format, flags);
        native_handle_delete(_handle);
        return -errno;
    }

    handle->prime_fd = gbm_priv_ops.gbm_bo_get_fd(bo);
	handle->stride = gbm_priv_ops.gbm_bo_get_stride(bo);
#ifdef GBM_BO_IMPORT_FD_MODIFIER
    handle->modifier = gbm_priv_ops.gbm_bo_get_modifier(bo);
#endif

    {
        // std::lock_guard<std::mutex> lock(_gbm_bo_handle_map_mutex);
        gbm_bo_handle_map.emplace(buffer_handle, bo);
    }

    *out_stride = handle->stride;
    *out_handle = _handle;

    log_v("allocated buffer: prime_fd=%d, width=%d, height=%d, handle->stride=%d, format=%d",
        handle->prime_fd, handle->width, handle->height, handle->stride, format);

    // Don't call gbm_device_destroy(dev) in gralloc_allocate().
    return 0;
}

struct gbm_bo *gralloc_get_gbm_bo_from_handle(buffer_handle_t handle) {
    // std::lock_guard<std::mutex> lock(_gbm_bo_handle_map_mutex);
    auto it = gbm_bo_handle_map.find(handle);
    return (it != gbm_bo_handle_map.end()) ? it->second : nullptr;
}

void gralloc_gbm_destroy_user_data(struct gbm_bo *bo, void *data) {
    bo_data_t *bo_data = (bo_data_t *)data;
    delete bo_data;

    (void)bo;
}

static int gralloc_gbm_map(buffer_handle_t handle, int enable_write, void **addr) {
    int err = 0;
    int flags = GBM_BO_TRANSFER_READ;
    struct gbm_bo *bo = gralloc_get_gbm_bo_from_handle(handle);
    bo_data_t *bo_data = (bo_data_t *)gbm_priv_ops.gbm_bo_get_user_data(bo);
    uint32_t stride;

    if (bo_data->map_data)
        return -EINVAL;

    if (enable_write)
        flags |= GBM_BO_TRANSFER_WRITE;

    *addr = gbm_priv_ops.gbm_bo_map(bo, 0, 0, gbm_priv_ops.gbm_bo_get_width(bo),
                       gbm_priv_ops.gbm_bo_get_height(bo),
                       flags, &stride, &bo_data->map_data);
    log_v("mapped bo %p at %p", bo, *addr);
    if (*addr == NULL)
        return -ENOMEM;

    assert(stride == gbm_priv_ops.gbm_bo_get_stride(bo));

    return err;
}

static void gralloc_gbm_unmap(struct gbm_bo *bo) {
    bo_data_t *bo_data = (bo_data_t *)gbm_priv_ops.gbm_bo_get_user_data(bo);

    log_v("unmapped bo %p", bo);
    gbm_priv_ops.gbm_bo_unmap(bo, bo_data->map_data);
    bo_data->map_data = NULL;
}

int gralloc_gbm_bo_lock(buffer_handle_t handle,
                        int usage, int /*x*/, int /*y*/, int /*w*/, int /*h*/,
                        void **addr)
{
    struct gralloc_handle_t *gbm_handle = gralloc_handle(handle);
    struct gbm_bo *bo = gralloc_get_gbm_bo_from_handle(handle);
    bo_data_t *bo_data;

    if (!bo)
        return -EINVAL;

    if ((gbm_handle->usage & usage) != (uint32_t)usage) {
        /* make FB special for testing software renderer with */

        if (!(gbm_handle->usage & GRALLOC_USAGE_SW_READ_OFTEN) &&
                !(gbm_handle->usage & GRALLOC_USAGE_HW_FB) &&
                !(gbm_handle->usage & GRALLOC_USAGE_HW_TEXTURE)) {
            log_e("bo.usage:x%X/usage:x%X is not GRALLOC_USAGE_HW_FB or GRALLOC_USAGE_HW_TEXTURE",
                gbm_handle->usage, usage);
            return -EINVAL;
        }
    }

    bo_data = (bo_data_t *)gbm_priv_ops.gbm_bo_get_user_data(bo);
    if (!bo_data) {
        bo_data = new struct bo_data();
        gbm_priv_ops.gbm_bo_set_user_data(bo, bo_data, gralloc_gbm_destroy_user_data);
    }

    log_v("lock bo %p, cnt=%d, usage=%x, prime_fd=%d", bo, bo_data->lock_count, usage, gbm_handle->prime_fd);

    /* allow multiple locks with compatible usages */
    if (bo_data->lock_count && (bo_data->locked_for & usage) != usage)
        return -EINVAL;

    usage |= bo_data->locked_for;

    if (usage & (GRALLOC_USAGE_SW_WRITE_MASK |
             GRALLOC_USAGE_SW_READ_MASK)) {
        /* the driver is supposed to wait for the bo */
        int write = !!(usage & GRALLOC_USAGE_SW_WRITE_MASK);
        int err = gralloc_gbm_map(handle, write, addr);
        if (err)
            return err;
    }
    else {
        /* kernel handles the synchronization here */
    }

    bo_data->lock_count++;
    bo_data->locked_for |= usage;

    return 0;
}

int gralloc_gbm_bo_unlock(buffer_handle_t handle) {
    struct gbm_bo *bo = gralloc_get_gbm_bo_from_handle(handle);
    bo_data_t *bo_data;
    if (!bo)
        return -EINVAL;

    bo_data = (bo_data_t *)gbm_priv_ops.gbm_bo_get_user_data(bo);

    int mapped = bo_data->locked_for &
        (GRALLOC_USAGE_SW_WRITE_MASK | GRALLOC_USAGE_SW_READ_MASK);

    if (!bo_data->lock_count) {
        log_v("unlock on already unlocked BO");
        return 0;
    }

    if (mapped)
        gralloc_gbm_unmap(bo);

    bo_data->lock_count--;
    if (!bo_data->lock_count)
        bo_data->locked_for = 0;

    return 0;
}

int gralloc_gbm_bo_lock_ycbcr(buffer_handle_t handle,
                                int usage, int x, int y, int w, int h,
                                struct android_ycbcr *ycbcr) {
    struct gralloc_handle_t *hnd = gralloc_handle(handle);
    int ystride, cstride;
    void *addr = 0;
    int err;

    log_v("handle %p, hnd %p, usage 0x%x", handle, hnd, usage);

    err = gralloc_gbm_bo_lock(handle, usage, x, y, w, h, &addr);
    if (err)
        return err;

    memset(ycbcr->reserved, 0, sizeof(ycbcr->reserved));

    switch (hnd->format) {
    case HAL_PIXEL_FORMAT_YCrCb_420_SP:
        ystride = cstride = ALIGN(hnd->width, 16);
        ycbcr->y = addr;
        ycbcr->cr = (unsigned char *)addr + ystride * hnd->height;
        ycbcr->cb = (unsigned char *)addr + ystride * hnd->height + 1;
        ycbcr->ystride = ystride;
        ycbcr->cstride = cstride;
        ycbcr->chroma_step = 2;
        break;
    case HAL_PIXEL_FORMAT_YV12:
        ystride = hnd->width;
        cstride = ALIGN(ystride / 2, 16);
        ycbcr->y = addr;
        ycbcr->cr = (unsigned char *)addr + ystride * hnd->height;
        ycbcr->cb = (unsigned char *)addr + ystride * hnd->height + cstride * hnd->height / 2;
        ycbcr->ystride = ystride;
        ycbcr->cstride = cstride;
        ycbcr->chroma_step = 1;
        break;
    default:
        log_e("Can not lock buffer, invalid format: 0x%x", hnd->format);
        return -EINVAL;
    }

    return 0;
}

int gralloc_gbm_bo_lock_async(buffer_handle_t handle, int usage, int x, int y, int w, int h, void **addr, int fence_fd) {
    // Waiting for fence signal
    if (fence_fd >= 0) {
        int err = sync_wait(fence_fd, 3000);
        if (err < 0) return err;
        close(fence_fd);
    }
    return gralloc_gbm_bo_lock(handle, usage, x, y, w, h, addr);
}

int gralloc_gbm_bo_unlock_async(buffer_handle_t handle, int *fence_fd) {
    int ret = gralloc_gbm_bo_unlock(handle);
    if (ret != 0) {
        return ret;
    }
    
    if (fence_fd) {
        *fence_fd = -1;
    }
    return 0;
}

int gralloc_gbm_bo_lock_async_ycbcr(buffer_handle_t handle, int usage, int x, int y, int w, int h, struct android_ycbcr *ycbcr, int fence_fd) {
    // Waiting for fence signal
    if (fence_fd >= 0) {
        int err = sync_wait(fence_fd, 3000); // timeout: 3s
        if (err < 0) {
            log_e("sync_wait failed: %s", strerror(-err));
            return err;
        }
        close(fence_fd);
    }
    return gralloc_gbm_bo_lock_ycbcr(handle, usage, x, y, w, h, ycbcr);
}

int gralloc_gm_buffer_import(buffer_handle_t buffer_handle) {
    struct gbm_bo *bo;
    struct gbm_device *dev = nullptr;
    struct gralloc_handle_t *handle = gralloc_handle(buffer_handle);
#ifdef GBM_BO_IMPORT_FD_MODIFIER
    struct gbm_import_fd_modifier_data data;
#else
    struct gbm_import_fd_data data;
#endif

    if (!buffer_handle || !handle) {
        log_e("Invalid buffer_handle_t or gralloc_handle_t.");
        return -EINVAL;
    }

    {
        // std::lock_guard<std::mutex> lock(_gbm_bo_handle_map_mutex);
        if (gbm_bo_handle_map.count(buffer_handle)) {
            log_e("Duplicated buffer was requested to be imported.");
            return -EINVAL;
        }
    }

    gralloc_gbm_device_create(_gbm_dev_fd, &dev);
    if (!dev) {
        log_e("Invalid GBM device.");
        return -EINVAL;
    }

    if (handle->prime_fd < 0) {
        log_e("The input handle has an invalid prime_fd (%d)", handle->prime_fd);
        return -EINVAL;
    }

    int format = gralloc_gm_android_format_to_gbm_format(handle->format);
    if (format == 0) {
        log_e("Unsupported format: %d", handle->format);
        return -EINVAL;
    }

    memset(&data, 0, sizeof(data));
    data.width = handle->width;
    data.height = handle->height;
    data.format = format;
    
    if (handle->usage & GRALLOC_USAGE_CURSOR) {
        data.width = ALIGN(MAX(handle->width, 64), 16);
        data.height = ALIGN(MAX(handle->height, 64), 16);
    }

    /* Adjust the width and height for a GBM GR88 buffer */
    if (handle->format == HAL_PIXEL_FORMAT_YV12) {
        data.width = ALIGN(handle->width, 32) / 2;
        data.height = handle->height + ALIGN(handle->height, 2) / 2;
    }

#ifdef GBM_BO_IMPORT_FD_MODIFIER
    data.num_fds = 1;
    data.fds[0] = handle->prime_fd;
    data.strides[0] = handle->stride;
    data.modifier = handle->modifier;
    bo = gbm_priv_ops.gbm_bo_import(dev, GBM_BO_IMPORT_FD_MODIFIER, &data, 0);
#else
    data.fd = handle->prime_fd;
    data.stride = handle->stride;
    bo = gbm_priv_ops.gbm_bo_import(dev, GBM_BO_IMPORT_FD, &data, 0);
#endif

    if (!bo) {
        log_e("gbm_bo_import failed: %s (width=%d, height=%d, format=%d, stride=%d)",
              strerror(errno), data.width, data.height, format, 
              #ifdef GBM_BO_IMPORT_FD_MODIFIER
              data.strides[0]
              #else
              data.stride
              #endif
              );
        return -EINVAL;
    }

    gbm_bo_handle_map.emplace(buffer_handle, bo);

    log_v("imported buffer: bo %p, prime_fd=%d, width=%d, height=%d, handle->stride=%d, format=%d",
        bo, handle->prime_fd, handle->width, handle->height, handle->stride, format);

    return 0;

}

int gralloc_gm_buffer_free(buffer_handle_t handle) {
    struct gbm_bo *bo = gralloc_get_gbm_bo_from_handle(handle);
    auto hnd = gralloc_handle(handle);

    if (!hnd) {
        log_e("Failed to convert buffer_handle_t to gralloc_handle_t, err=%d", -errno);
        return -errno;
    }

    if (!bo) {
        log_e("Failed to get BO from handle, err=%d", -errno);
        return -errno;
    }

    // std::lock_guard<std::mutex> lock(_gbm_bo_handle_map_mutex);
    bo = gralloc_get_gbm_bo_from_handle(handle);
    if (!bo) {
        log_e("Failed to get BO from handle, err=%d", -errno);
        return -errno;
    }
    gbm_bo_handle_map.erase(handle);

    auto it_bo = gbm_bo_handle_map.find(handle);
    if (it_bo != gbm_bo_handle_map.end()) {
        gbm_bo_handle_map.erase(it_bo);
    }

    gbm_priv_ops.gbm_bo_destroy(bo);

    log_v("freed buffer: prime_fd=%d, width=%d, height=%d, hnd->stride=%d",
        hnd->prime_fd, hnd->width, hnd->height, hnd->stride);

    return 0;
}

__attribute__((destructor)) void _cleanup_all() {
    _gbm_dev_fd = -1;
    if (_gbm_dev) {
        gbm_priv_ops.gbm_device_destroy(_gbm_dev);
        _gbm_dev = nullptr;
    }
}