#ifndef GRALLOC_GBM_MESA_MAPPER_H_
#define GRALLOC_GBM_MESA_MAPPER_H_

#include <aidl/android/hardware/graphics/common/BlendMode.h>
#include <aidl/android/hardware/graphics/common/Dataspace.h>
#include <aidl/android/hardware/graphics/common/Cta861_3.h>
#include <aidl/android/hardware/graphics/common/Smpte2086.h>

typedef struct gralloc_metadata {
	int prime_fd; // same with the prime_fd of gralloc_handle_t
	aidl::android::hardware::graphics::common::BlendMode blend_mode;
	aidl::android::hardware::graphics::common::Dataspace dataspace;
	aidl::android::hardware::graphics::common::Cta861_3 cta861_3; // optional
	aidl::android::hardware::graphics::common::Smpte2086 smpte2086; // optional
} gralloc_metadata_t;

#endif // GRALLOC_GBM_MESA_MAPPER_H_