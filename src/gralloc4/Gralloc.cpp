/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#include "Gralloc.h"

#define LOG_TAG "gralloc.gm"

#include <memory>

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include <hardware/gralloc.h>

#include "gralloc_gbm_mesa.h"
#include "gbm_mesa_loader.h"
#include "log.h"

struct gralloc_gbm_module_t {
    gralloc_module_t base;
    pthread_mutex_t mutex;
    bool initialized;
    std::shared_ptr<gbm_device> gbm_dev;
};

struct gralloc_gbm_alloc_device_t {
    alloc_device_t base;
    gralloc_gbm_module_t* module;
};

static int gralloc_mod_gbm_init(gralloc_gbm_module_t* mod) {
    log_i("GBM Mesa Gralloc HAL Module initializing...");
    pthread_mutex_lock(&mod->mutex);
    if (!mod->initialized) {
        int fd = gralloc_gbm_device_init();
        if (fd < 0) {
            pthread_mutex_unlock(&mod->mutex);
            return -EINVAL;
        }

        struct gbm_device* dev = nullptr;
        if (gralloc_gbm_device_create(fd, &dev)) {
            pthread_mutex_unlock(&mod->mutex);
            return -EINVAL;
        }
        mod->gbm_dev = std::shared_ptr<gbm_device>(dev, [](gbm_device* d) {
            if (d) gbm_priv_ops.gbm_device_destroy(d);
        });
        mod->initialized = true;
    }
    pthread_mutex_unlock(&mod->mutex);
    return 0;
}

static int gralloc_mod_gbm_perform(const struct gralloc_module_t* mod, int op, ...) {
    gralloc_gbm_module_t* gbm_mod = (gralloc_gbm_module_t*)mod;
    va_list args;
    int err = gralloc_mod_gbm_init(gbm_mod);
    if (err) return err;

    va_start(args, op);
    switch (op) {
    case GRALLOC_MODULE_PERFORM_GET_DRM_FD: {
        int* fd = va_arg(args, int*);
        *fd = gbm_priv_ops.gbm_device_get_fd(gbm_mod->gbm_dev.get());
        err = 0;
        break;
    }
    default:
        err = -EINVAL;
        break;
    }
    va_end(args);
    return err;
}

static int gralloc_mod_register_buffer(gralloc_module_t const* mod, buffer_handle_t handle) {
    log_i("registerBuffer: handle=%p", handle);
    gralloc_gbm_module_t* gbm_mod = (gralloc_gbm_module_t*)mod;
    int err = gralloc_mod_gbm_init(gbm_mod);
    if (err) return err;

    err = gralloc_gm_buffer_import(handle);
    if (err) {
        log_e("gralloc_gm_buffer_import failed with %d", err);
    }
    return err;
}

static int gralloc_mod_unregister_buffer(gralloc_module_t const* mod, buffer_handle_t handle) {
    return gralloc_gm_buffer_free(handle);
}

static int gralloc_mod_lock_async(gralloc_module_t const* mod, buffer_handle_t handle,
                                 int usage, int l, int t, int w, int h, void** vaddr, int fence_fd) {
    return gralloc_gbm_bo_lock_async(handle, usage, l, t, w, h, vaddr, fence_fd);
}

static int gralloc_mod_unlock_async(gralloc_module_t const* mod, buffer_handle_t handle, int* fence_fd) {
    return gralloc_gbm_bo_unlock_async(handle, fence_fd);
}

static int gralloc_mod_lock_async_ycbcr(gralloc_module_t const* mod, buffer_handle_t handle,
                                       int usage, int l, int t, int w, int h,
                                       struct android_ycbcr* ycbcr, int fence_fd) {
    return gralloc_gbm_bo_lock_async_ycbcr(handle, usage, l, t, w, h, ycbcr, fence_fd);
}

static int gralloc_mod_lock(gralloc_module_t const* mod, buffer_handle_t handle,
                           int usage, int x, int y, int w, int h, void** vaddr) {
    return gralloc_mod_lock_async(mod, handle, usage, x, y, w, h, vaddr, -1);
}

static int gralloc_mod_unlock(gralloc_module_t const* mod, buffer_handle_t handle) {
    return gralloc_gbm_bo_unlock(handle);
}

static int gralloc_mod_lock_ycbcr(gralloc_module_t const* mod, buffer_handle_t handle,
                                 int usage, int x, int y, int w, int h, struct android_ycbcr* ycbcr) {
    return gralloc_mod_lock_async_ycbcr(mod, handle, usage, x, y, w, h, ycbcr, -1);
}

static int gralloc_mod_alloc_close(struct hw_device_t* dev) {
    gralloc_gbm_alloc_device_t* alloc_dev = (gralloc_gbm_alloc_device_t*)dev;
    delete alloc_dev;
    return 0;
}

static int gralloc_mod_alloc_free(alloc_device_t* dev, buffer_handle_t handle) {
    return gralloc_gm_buffer_free(handle);
}

static int gralloc_mod_alloc_alloc(alloc_device_t* dev, int w, int h, int format, int usage,
                                  buffer_handle_t* handle, int* stride) {
    gralloc_gbm_alloc_device_t* alloc_dev = (gralloc_gbm_alloc_device_t*)dev;
    int err = gralloc_mod_gbm_init(alloc_dev->module);
    if (err) return err;

    gralloc_buffer_desc desc = {
        .width = static_cast<uint32_t>(w),
        .height = static_cast<uint32_t>(h),
        .android_format = static_cast<uint32_t>(format),
        .android_usage = static_cast<uint32_t>(usage),
        .gbm_format = gralloc_gm_android_format_to_gbm_format(format),
        .flags = gralloc_gm_get_gbm_flags_from_android_usage(usage, format)
    };

    native_handle_t* hnd = nullptr;
    err = gralloc_allocate(&desc, stride, &hnd);
    if (!err) {
        *handle = hnd;
    }
    return err;
}

static int gralloc_mod_alloc_open(gralloc_gbm_module_t* mod, hw_device_t** dev) {
    auto alloc_dev = new gralloc_gbm_alloc_device_t();
    if (!alloc_dev) return -ENOMEM;

    alloc_dev->base.common.tag = HARDWARE_DEVICE_TAG;
    alloc_dev->base.common.version = 0;
    alloc_dev->base.common.module = &mod->base.common;
    alloc_dev->base.common.close = gralloc_mod_alloc_close;
    alloc_dev->base.alloc = gralloc_mod_alloc_alloc;
    alloc_dev->base.free = gralloc_mod_alloc_free;
    alloc_dev->module = mod;

    *dev = &alloc_dev->base.common;
    return 0;
}

static int gralloc_mod_open(const struct hw_module_t* module, const char* name,
                           struct hw_device_t** device) {
    gralloc_gbm_module_t* mod = (gralloc_gbm_module_t*)module;
    
    if (strcmp(name, GRALLOC_HARDWARE_GPU0) == 0) {
        return gralloc_mod_alloc_open(mod, device);
    }
    return -EINVAL;
}

static struct hw_module_methods_t gralloc_gm_module_methods = {
    .open = gralloc_mod_open
};

struct gralloc_gbm_module_t HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = GRALLOC_MODULE_API_VERSION_0_3,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = GRALLOC_HARDWARE_MODULE_ID,
            .name = "GBM Mesa Gralloc",
            .author = "Levi Marvin",
            .methods = &gralloc_gm_module_methods,
        },
        .registerBuffer = gralloc_mod_register_buffer,
        .unregisterBuffer = gralloc_mod_unregister_buffer,
        .lock = gralloc_mod_lock,
        .unlock = gralloc_mod_unlock,
        .lock_ycbcr = gralloc_mod_lock_ycbcr,
        .perform = gralloc_mod_gbm_perform,
		.lockAsync = gralloc_mod_lock_async,
		.unlockAsync = gralloc_mod_unlock_async,
		.lockAsync_ycbcr = gralloc_mod_lock_async_ycbcr,
    },
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .initialized = false,
    .gbm_dev = nullptr,
};