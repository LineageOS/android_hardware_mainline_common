/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#ifndef _GBM_MESA_LOADER_H_
#define _GBM_MESA_LOADER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

struct gbm_device;
struct gbm_bo;

typedef void (*gbm_device_destroy_func)(struct gbm_device *gbm);
typedef struct gbm_device *(*gbm_create_device_func)(int fd);
typedef int (*gbm_device_get_fd_func)(struct gbm_device *gbm);
typedef const char *(*gbm_device_get_backend_name_func)(struct gbm_device *gbm);
typedef struct gbm_bo *(*gbm_bo_create_func)(struct gbm_device *gbm, uint32_t width,
					     uint32_t height, uint32_t format, uint32_t flags);
typedef struct gbm_bo *(*gbm_bo_import_func)(struct gbm_device *gbm, uint32_t type, void *buffer,
					     uint32_t flags);
typedef void *(*gbm_bo_map_func)(struct gbm_bo *bo, uint32_t x, uint32_t y, uint32_t width,
				 uint32_t height, uint32_t flags, uint32_t *stride,
				 void **map_data);
typedef void (*gbm_bo_unmap_func)(struct gbm_bo *bo, void *map_data);
typedef uint32_t (*gbm_bo_get_width_func)(struct gbm_bo *bo);
typedef uint32_t (*gbm_bo_get_height_func)(struct gbm_bo *bo);
typedef uint32_t (*gbm_bo_get_stride_func)(struct gbm_bo *bo);
typedef int (*gbm_bo_get_fd_func)(struct gbm_bo *bo);
typedef uint64_t (*gbm_bo_get_modifier_func)(struct gbm_bo *bo);
typedef void (*gbm_bo_set_user_data_func)(struct gbm_bo *bo, void *data,
					  void (*destroy_user_data)(struct gbm_bo *, void *));
typedef void *(*gbm_bo_get_user_data_func)(struct gbm_bo *bo);
typedef void (*gbm_bo_destroy_func)(struct gbm_bo *bo);

struct gbm_priv_ops {
	bool is_initialized;

	gbm_device_destroy_func gbm_device_destroy;
	gbm_create_device_func gbm_create_device;
	gbm_device_get_fd_func gbm_device_get_fd;
	gbm_device_get_backend_name_func gbm_device_get_backend_name;
	gbm_bo_create_func gbm_bo_create;
	gbm_bo_import_func gbm_bo_import;
	gbm_bo_map_func gbm_bo_map;
	gbm_bo_unmap_func gbm_bo_unmap;
	gbm_bo_get_width_func gbm_bo_get_width;
	gbm_bo_get_height_func gbm_bo_get_height;
	gbm_bo_get_stride_func gbm_bo_get_stride;
	gbm_bo_get_fd_func gbm_bo_get_fd;
	gbm_bo_get_modifier_func gbm_bo_get_modifier;
	gbm_bo_set_user_data_func gbm_bo_set_user_data;
	gbm_bo_get_user_data_func gbm_bo_get_user_data;
	gbm_bo_destroy_func gbm_bo_destroy;
};

extern struct gbm_priv_ops gbm_priv_ops;

bool setup_gbm_priv_ops(void);

#ifdef __cplusplus
}
#endif

#endif
