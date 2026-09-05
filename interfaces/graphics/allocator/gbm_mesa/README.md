# gralloc_gbm_mesa
This project provides the implements of Android Hardware Graphics Interfaces.

## General
The code is tested on the platform with RK3588 (Mali G610 GPU) and using the libgbm_mesa from Mesa 25.1.4.

We still need to do more things, but it seems that the basic function is fine.

### Components
Provided:

- Allocator AIDL V2 Service
- Mapper Stable-C (IMapper V5) HAL Module
- Gralloc 4 HAL Module
- Library for using gralloc with GBM (Provided by Mesa)

## Usage
Add packages to `PRODUCT_PACKAGES`, and build the code with AOSP.

