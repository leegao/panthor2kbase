#pragma once
#include <stdint.h>
typedef enum android_dataspace { HAL_DATASPACE_UNKNOWN = 0 } android_dataspace_t;
typedef enum android_pixel_format {
   HAL_PIXEL_FORMAT_RGBA_8888 = 1,
   HAL_PIXEL_FORMAT_RGBX_8888 = 2,
   HAL_PIXEL_FORMAT_RGB_888 = 3,
   HAL_PIXEL_FORMAT_RGB_565 = 4,
   HAL_PIXEL_FORMAT_BGRA_8888 = 5,
   HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED = 34,
   HAL_PIXEL_FORMAT_YCbCr_420_888 = 35,
   HAL_PIXEL_FORMAT_YV12 = 0x32315659,
} android_pixel_format_t;
struct android_ycbcr {
   void *y;
   void *cb;
   void *cr;
   size_t ystride;
   size_t cstride;
   size_t chroma_step;
   uint32_t reserved[8];
};
