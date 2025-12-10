/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "GRALLOC-GBM"

#include <gbm.h>

#include <dlfcn.h>
#include <log/log.h>
#include <stdlib.h>

static void* get_gbm_mesa_handle() {
    static void* handle = nullptr;

    if (handle != nullptr) [[likely]] {
        return handle;
    }

    handle = dlopen("libgbm_mesa.so", RTLD_NOW);
    if (!handle) {
        ALOGE("Failed to load libgbm_mesa.so: %s", dlerror());
        abort();
        return nullptr;
    }

    return handle;
}

static void* gbm_mesa_handle = get_gbm_mesa_handle();

template <typename T>
T get_gbm_mesa_symbol(const char* symbol) {
    if (!gbm_mesa_handle) {
        ALOGE("GBM MESA handle is null");
        abort();
        return nullptr;
    }

    T func = reinterpret_cast<T>(dlsym(gbm_mesa_handle, symbol));
    if (!func) {
        ALOGE("Failed to load symbol %s: %s", symbol, dlerror());
        abort();
        return nullptr;
    }

    return func;
}

#define LOAD_GBM_MESA_SYMBOL(sym) get_gbm_mesa_symbol<typeof(sym)*>(#sym);

int gbm_device_get_fd(struct gbm_device* gbm) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_device_get_fd);
    return func(gbm);
}

void gbm_device_destroy(struct gbm_device* gbm) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_device_destroy);
    return func(gbm);
}

struct gbm_device* gbm_create_device(int fd) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_create_device);
    return func(fd);
}

struct gbm_bo* gbm_bo_create(struct gbm_device* gbm, uint32_t width, uint32_t height,
                             uint32_t format, uint32_t flags) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_create);
    return func(gbm, width, height, format, flags);
}

struct gbm_bo* gbm_bo_import(struct gbm_device* gbm, uint32_t type, void* buffer, uint32_t flags) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_import);
    return func(gbm, type, buffer, flags);
}

void* gbm_bo_map(struct gbm_bo* bo, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                 uint32_t flags, uint32_t* stride, void** map_data) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_map);
    return func(bo, x, y, width, height, flags, stride, map_data);
}

void gbm_bo_unmap(struct gbm_bo* bo, void* map_data) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_unmap);
    return func(bo, map_data);
}

uint32_t gbm_bo_get_width(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_width);
    return func(bo);
}

uint32_t gbm_bo_get_height(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_height);
    return func(bo);
}

uint32_t gbm_bo_get_stride(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_stride);
    return func(bo);
}

int gbm_bo_get_fd(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_fd);
    return func(bo);
}

uint64_t gbm_bo_get_modifier(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_modifier);
    return func(bo);
}

void gbm_bo_set_user_data(struct gbm_bo* bo, void* data,
                          void (*destroy_user_data)(struct gbm_bo*, void*)) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_set_user_data);
    return func(bo, data, destroy_user_data);
}

void* gbm_bo_get_user_data(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_get_user_data);
    return func(bo);
}

void gbm_bo_destroy(struct gbm_bo* bo) {
    static auto func = LOAD_GBM_MESA_SYMBOL(gbm_bo_destroy);
    return func(bo);
}
