/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#ifndef _GRALLOC_GBM_MESA_H_
#define _GRALLOC_GBM_MESA_H_

#include <drm/gralloc_handle.h>

#include <mesa/gbm.h>
#include <mesa/gbm_backend_abi.h>

#define GRALLOC_DEFAULT_DEVICE_PROP "vendor.gralloc.device"
#define GRALLOC_DEFAULT_DEVICE_PATH "/dev/dri/renderD128"

#define MAX(A, B) ((A) > (B) ? (A) : (B))
#define ALIGN(A, B) (((A) + (B)-1) & ~((B)-1))
#define IS_ALIGNED(A, B) (ALIGN((A), (B)) == (A))
#define DIV_ROUND_UP(n, d) (((n) + (d)-1) / (d))

typedef struct gralloc_buffer_desc {
    uint32_t width;
    uint32_t height;
    uint32_t android_format;       // Android PixelFormat
    uint32_t android_usage;        // Android usage
    uint32_t android_reserved_size;
    uint32_t gbm_format;       // GBM FourCC format
    uint32_t flags;        // gbm_bo_flags combinations
    uint32_t layer_count;  // Number of layout
} gralloc_buffer_desc_t;

typedef struct bo_data {
	void *map_data;
	int lock_count;
	int locked_for;
} bo_data_t;

/*
 * gralloc_gbm_device_init()
 * Initialize a GBM device (not create), we create the device by using
 * gralloc_gbm_device_create().
 * @return the fd of GBM device
 */
int gralloc_gbm_device_init();

uint32_t gralloc_gm_android_format_to_gbm_format(uint32_t android_format);
unsigned int gralloc_gm_get_gbm_flags_from_android_usage(int usage, int format);
int gralloc_gm_get_bpp_from_gbm_format(int gbm_format);
int gralloc_gm_get_bytes_per_pixel_from_gbm_format(int gbm_format);
int gralloc_gm_get_bytes_per_pixel_from_android_format(int android_format);
uint32_t gralloc_gm_android_caculate_pixel_stride(uint32_t android_format, uint32_t stride);
inline static int gralloc_get_max_texture_2d_size() {
    // Only VirGL has the max size (witdh and height) limit of texture. 
    return UINT32_MAX;
}
/*
 * Create or reuse a GBM device.
 * @return Error code.
 */
int gralloc_gbm_device_create(int fd, struct gbm_device **dev);
bool gralloc_is_format_supported();
bool gralloc_is_desc_support(const struct gralloc_buffer_desc* desc);
int32_t gralloc_allocate(const struct gralloc_buffer_desc *desc, int32_t *out_stride, native_handle_t **out_handle);
struct gbm_bo *gralloc_get_gbm_bo_from_handle(buffer_handle_t handle);
void gralloc_gbm_destroy_user_data(struct gbm_bo *bo, void *data);
static int gralloc_gbm_map(buffer_handle_t handle, int enable_write, void **addr);
static void gralloc_gbm_unmap(struct gbm_bo *bo);
int gralloc_gbm_bo_lock(buffer_handle_t handle, int usage, int /*x*/, int /*y*/, int /*w*/, int /*h*/, void **addr);
int gralloc_gbm_bo_unlock(buffer_handle_t handle);
int gralloc_gbm_bo_lock_ycbcr(buffer_handle_t handle, int usage, int x, int y, int w, int h, struct android_ycbcr *ycbcr);
int gralloc_gbm_bo_lock_async(buffer_handle_t handle, int usage, int x, int y, int w, int h, void **addr, int fence_fd);
int gralloc_gbm_bo_unlock_async(buffer_handle_t handle, int *fence_fd);
int gralloc_gbm_bo_lock_async_ycbcr(buffer_handle_t handle, int usage, int x, int y, int w, int h, struct android_ycbcr *ycbcr, int fence_fd);
int gralloc_gm_buffer_import(buffer_handle_t buffer_handle);
int gralloc_gm_buffer_free(buffer_handle_t handle);

#endif // _GRALLOC_GBM_MESA_H_