/*
 * Copyright (C) 2025  Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 *
 * Authors:
 *      Levi Marvin (LIU, YUANCHEN) <levimarvin@icloud.com>
 */

#include "gbm_mesa_loader.h"

#define LOG_TAG "libgralloc_gm"

#include <dlfcn.h>
#include <log/log.h>

struct gbm_priv_ops gbm_priv_ops = {
	false,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

bool setup_gbm_priv_ops(void)
{
	if (gbm_priv_ops.is_initialized) {
		return true;
	}

	void *handle = dlopen("libgbm_mesa.so", RTLD_NOW);
	if (!handle) {
		ALOGE("Failed to open libgbm_mesa.so: %s", dlerror());
		return false;
	}

#define LOAD_GBM_SYMBOL(sym) gbm_priv_ops.sym = (sym##_func)dlsym(handle, #sym);
	LOAD_GBM_SYMBOL(gbm_device_destroy);
	LOAD_GBM_SYMBOL(gbm_create_device);
	LOAD_GBM_SYMBOL(gbm_device_get_fd);
	LOAD_GBM_SYMBOL(gbm_device_get_backend_name);
	LOAD_GBM_SYMBOL(gbm_bo_create);
	LOAD_GBM_SYMBOL(gbm_bo_import);
	LOAD_GBM_SYMBOL(gbm_bo_map);
	LOAD_GBM_SYMBOL(gbm_bo_unmap);
	LOAD_GBM_SYMBOL(gbm_bo_get_width);
	LOAD_GBM_SYMBOL(gbm_bo_get_height);
	LOAD_GBM_SYMBOL(gbm_bo_get_stride);
	LOAD_GBM_SYMBOL(gbm_bo_get_fd);
	LOAD_GBM_SYMBOL(gbm_bo_get_modifier);
	LOAD_GBM_SYMBOL(gbm_bo_set_user_data);
	LOAD_GBM_SYMBOL(gbm_bo_get_user_data);
	LOAD_GBM_SYMBOL(gbm_bo_destroy);
#undef LOAD_GBM_SYMBOL

	gbm_priv_ops.is_initialized = true;

	return true;
}
