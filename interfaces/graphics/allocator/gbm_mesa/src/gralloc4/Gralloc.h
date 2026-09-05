#ifndef GRALLOC_GM_GRALLOC_HAL_MODULE_H_
#define GRALLOC_GM_GRALLOC_HAL_MODULE_H_

#ifdef __cplusplus
extern "C" {
#endif

enum {
	/* perform(const struct gralloc_module_t *mod,
	 *	   int op,
	 *	   int *fd);
	 */
	GRALLOC_MODULE_PERFORM_GET_DRM_FD                = 0x40000002,
};

#ifdef __cplusplus
}
#endif

#endif // GRALLOC_GM_GRALLOC_HAL_MODULE_H_