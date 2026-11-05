/*
 * SPDX-License-Identifier: MIT
 *
 * Complete libdrm KMS compatibility header (xf86drmMode.h) for Mesa 26.3.0
 */
#ifndef _XF86DRMMODE_H_
#define _XF86DRMMODE_H_

#include <stdint.h>
#include <stddef.h>
#include "drm.h"
#include "drm_mode.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _drmModeRes {
   int count_fbs;
   uint32_t *fbs;
   int count_crtcs;
   uint32_t *crtcs;
   int count_connectors;
   uint32_t *connectors;
   int count_encoders;
   uint32_t *encoders;
   uint32_t min_width, max_width;
   uint32_t min_height, max_height;
} drmModeRes, *drmModeResPtr;

typedef struct _drmModeModeInfo {
   uint32_t clock;
   uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
   uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
   uint32_t vrefresh;
   uint32_t flags;
   uint32_t type;
   char name[DRM_DISPLAY_MODE_LEN];
} drmModeModeInfo, *drmModeModeInfoPtr;

typedef struct _drmModeCrtc {
   uint32_t crtc_id;
   uint32_t buffer_id;
   uint32_t x, y;
   uint32_t width, height;
   int mode_valid;
   drmModeModeInfo mode;
   int gamma_size;
} drmModeCrtc, *drmModeCrtcPtr;

typedef struct _drmModeEncoder {
   uint32_t encoder_id;
   uint32_t encoder_type;
   uint32_t crtc_id;
   uint32_t possible_crtcs;
   uint32_t possible_clones;
} drmModeEncoder, *drmModeEncoderPtr;

typedef enum {
   DRM_MODE_CONNECTED         = 1,
   DRM_MODE_DISCONNECTED      = 2,
   DRM_MODE_UNKNOWNCONNECTION = 3
} drmModeConnection;

typedef enum {
   DRM_MODE_SUBPIXEL_UNKNOWN        = 1,
   DRM_MODE_SUBPIXEL_HORIZONTAL_RGB = 2,
   DRM_MODE_SUBPIXEL_HORIZONTAL_BGR = 3,
   DRM_MODE_SUBPIXEL_VERTICAL_RGB   = 4,
   DRM_MODE_SUBPIXEL_VERTICAL_BGR   = 5,
   DRM_MODE_SUBPIXEL_NONE           = 6
} drmModeSubPixel;

typedef struct _drmModeConnector {
   uint32_t connector_id;
   uint32_t encoder_id;
   uint32_t connector_type;
   uint32_t connector_type_id;
   drmModeConnection connection;
   uint32_t mmWidth, mmHeight;
   drmModeSubPixel subpixel;
   int count_modes;
   drmModeModeInfoPtr modes;
   int count_props;
   uint32_t *props;
   uint64_t *prop_values;
   int count_encoders;
   uint32_t *encoders;
} drmModeConnector, *drmModeConnectorPtr;

typedef struct _drmModePropertyBlob {
   uint32_t id;
   uint32_t length;
   void *data;
} drmModePropertyBlobRes, *drmModePropertyBlobPtr;

typedef struct _drmModeProperty {
   uint32_t prop_id;
   uint32_t flags;
   char name[DRM_PROP_NAME_LEN];
   int count_values;
   uint64_t *values;
   int count_enums;
   struct drm_mode_property_enum *enums;
   int count_blobs;
   uint32_t *blob_ids;
} drmModePropertyRes, *drmModePropertyPtr;

typedef struct _drmModeObjectProperties {
   uint32_t count_props;
   uint32_t *props;
   uint64_t *prop_values;
} drmModeObjectProperties, *drmModeObjectPropertiesPtr;

typedef struct _drmModePlane {
   uint32_t count_formats;
   uint32_t *formats;
   uint32_t plane_id;
   uint32_t crtc_id;
   uint32_t fb_id;
   uint32_t crtc_x, crtc_y;
   uint32_t x, y;
   uint32_t possible_crtcs;
   uint32_t gamma_size;
} drmModePlane, *drmModePlanePtr;

typedef struct _drmModePlaneRes {
   uint32_t count_planes;
   uint32_t *planes;
} drmModePlaneRes, *drmModePlaneResPtr;

typedef struct _drmModeAtomicReq drmModeAtomicReq, *drmModeAtomicReqPtr;

drmModeResPtr drmModeGetResources(int fd);
void drmModeFreeResources(drmModeResPtr ptr);
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId);
void drmModeFreeCrtc(drmModeCrtcPtr ptr);
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id);
void drmModeFreeEncoder(drmModeEncoderPtr ptr);
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId);
drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t connector_id);
void drmModeFreeConnector(drmModeConnectorPtr ptr);
drmModePlaneResPtr drmModeGetPlaneResources(int fd);
void drmModeFreePlaneResources(drmModePlaneResPtr ptr);
drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id);
void drmModeFreePlane(drmModePlanePtr ptr);
drmModePropertyPtr drmModeGetProperty(int fd, uint32_t propertyId);
void drmModeFreeProperty(drmModePropertyPtr ptr);
drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id);
void drmModeFreePropertyBlob(drmModePropertyBlobPtr ptr);
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id, uint32_t object_type);
void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr);
int drmModeConnectorSetProperty(int fd, uint32_t connector_id, uint32_t property_id, uint64_t value);
int drmModeCreatePropertyBlob(int fd, const void *data, size_t size, uint32_t *id);
int drmModeDestroyPropertyBlob(int fd, uint32_t id);
int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
                               const uint32_t bo_handles[4], const uint32_t pitches[4],
                               const uint32_t offsets[4], const uint64_t modifier[4],
                               uint32_t *buf_id, uint32_t flags);
int drmModeRmFB(int fd, uint32_t bufferId);
drmModeAtomicReqPtr drmModeAtomicAlloc(void);
void drmModeAtomicFree(drmModeAtomicReqPtr req);
int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value);
int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data);
int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects, int flags, uint32_t *lessee_id);

#ifdef __cplusplus
}
#endif

#endif /* _XF86DRMMODE_H_ */
