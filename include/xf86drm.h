/*
 * SPDX-License-Identifier: MIT
 *
 * Complete libdrm compatibility header (xf86drm.h) for Mesa 26.3.0 PanVK
 * Supports both Patched Kmod and Unpatched Runtime-Hook builds on Linux & Android NDK.
 */
#ifndef _XF86DRM_H_
#define _XF86DRM_H_

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef DRM_MAX_MINOR
#define DRM_MAX_MINOR 64
#endif

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3

#define DRM_BUS_PCI       0
#define DRM_BUS_USB       1
#define DRM_BUS_PLATFORM  2
#define DRM_BUS_HOST1X    3
#define DRM_BUS_FAUX      4

#define DRM_PLATFORM_DEVICE_NAME_LEN 512
#define DRM_HOST1X_DEVICE_NAME_LEN   512
#define DRM_FAUX_DEVICE_NAME_LEN     512

#ifndef DRM_CAP_DUMB_BUFFER
#define DRM_CAP_DUMB_BUFFER          0x1
#define DRM_CAP_VBLANK_HIGH_CRTC     0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH 0x3
#define DRM_CAP_DUMB_PREFER_SHADOW   0x4
#define DRM_CAP_PRIME                0x5
#define DRM_PRIME_CAP_IMPORT         0x1
#define DRM_PRIME_CAP_EXPORT         0x2
#define DRM_CAP_TIMESTAMP_MONOTONIC  0x6
#define DRM_CAP_ASYNC_PAGE_FLIP      0x7
#define DRM_CAP_CURSOR_WIDTH         0x8
#define DRM_CAP_CURSOR_HEIGHT        0x9
#define DRM_CAP_ADDFB2_MODIFIERS     0x10
#define DRM_CAP_PAGE_FLIP_TARGET     0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT 0x12
#define DRM_CAP_SYNCOBJ              0x13
#define DRM_CAP_SYNCOBJ_TIMELINE     0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15
#endif

#ifndef DRM_CLIENT_CAP_STEREO_3D
#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_ASPECT_RATIO         4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5
#endif

typedef struct _drmVersion {
   int version_major;
   int version_minor;
   int version_patchlevel;
   int name_len;
   char *name;
   int date_len;
   char *date;
   int desc_len;
   char *desc;
} drmVersion, *drmVersionPtr;

typedef struct _drmPciBusInfo {
   uint16_t domain;
   uint8_t bus;
   uint8_t dev;
   uint8_t func;
} drmPciBusInfo, *drmPciBusInfoPtr;

typedef struct _drmPciDeviceInfo {
   uint16_t vendor_id;
   uint16_t device_id;
   uint16_t subvendor_id;
   uint16_t subdevice_id;
   uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;

typedef struct _drmUsbBusInfo {
   uint8_t bus;
   uint8_t dev;
} drmUsbBusInfo, *drmUsbBusInfoPtr;

typedef struct _drmUsbDeviceInfo {
   uint16_t vendor;
   uint16_t product;
} drmUsbDeviceInfo, *drmUsbDeviceInfoPtr;

typedef struct _drmPlatformBusInfo {
   char fullname[DRM_PLATFORM_DEVICE_NAME_LEN];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;

typedef struct _drmPlatformDeviceInfo {
   char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;

typedef struct _drmHost1xBusInfo {
   char fullname[DRM_HOST1X_DEVICE_NAME_LEN];
} drmHost1xBusInfo, *drmHost1xBusInfoPtr;

typedef struct _drmHost1xDeviceInfo {
   char **compatible;
} drmHost1xDeviceInfo, *drmHost1xDeviceInfoPtr;

typedef struct _drmFauxBusInfo {
   char name[DRM_FAUX_DEVICE_NAME_LEN];
} drmFauxBusInfo, *drmFauxBusInfoPtr;

typedef struct _drmDevice {
   char **nodes;
   int available_nodes;
   int bustype;
   union {
      drmPciBusInfoPtr pci;
      drmUsbBusInfoPtr usb;
      drmPlatformBusInfoPtr platform;
      drmHost1xBusInfoPtr host1x;
      drmFauxBusInfoPtr faux;
   } businfo;
   union {
      drmPciDeviceInfoPtr pci;
      drmUsbDeviceInfoPtr usb;
      drmPlatformDeviceInfoPtr platform;
      drmHost1xDeviceInfoPtr host1x;
   } deviceinfo;
} drmDevice, *drmDevicePtr;

#define DRM_EVENT_CONTEXT_VERSION 4

typedef struct _drmEventContext {
   int version;
   void (*vblank_handler)(int fd, unsigned int sequence, unsigned int tv_sec,
                          unsigned int tv_usec, void *user_data);
   void (*page_flip_handler)(int fd, unsigned int sequence, unsigned int tv_sec,
                             unsigned int tv_usec, void *user_data);
   void (*page_flip_handler2)(int fd, unsigned int sequence, unsigned int tv_sec,
                              unsigned int tv_usec, unsigned int crtc_id, void *user_data);
   void (*sequence_handler)(int fd, uint64_t sequence, uint64_t ns, uint64_t user_data);
} drmEventContext, *drmEventContextPtr;

/* Core libdrm APIs */
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr v);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmSetClientCap(int fd, uint64_t capability, uint64_t value);
int drmCloseBufferHandle(int fd, uint32_t handle);
int drmCommandWrite(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size);
int drmGetMagic(int fd, drm_magic_t *magic);
int drmAuthMagic(int fd, drm_magic_t magic);
int drmSetMaster(int fd);
int drmDropMaster(int fd);
int drmIsMaster(int fd);
int drmHandleEvent(int fd, drmEventContextPtr evctx);

/* Device enumeration APIs */
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
int drmGetDevices(drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
int drmGetDevice(int fd, drmDevicePtr *device);
int drmGetDeviceFromDevId(dev_t find_rdev, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
int drmGetNodeTypeFromFd(int fd);
char *drmGetRenderDeviceNameFromFd(int fd);
char *drmGetDeviceNameFromFd2(int fd);
char *drmGetPrimaryDeviceNameFromFd(int fd);

/* PRIME / dma-buf APIs */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);

/* Syncobj APIs */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles,
                   int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles,
                             uint64_t *points, uint32_t handle_count);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points,
                           unsigned num_handles, int64_t timeout_nsec,
                           unsigned flags, uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points,
                     uint32_t handle_count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point,
                       uint32_t src_handle, uint64_t src_point, uint32_t flags);

/* KMS sequence APIs */
int drmCrtcGetSequence(int fd, uint32_t crtcId, uint64_t *sequence, uint64_t *ns);
int drmCrtcQueueSequence(int fd, uint32_t crtcId, uint32_t flags,
                         uint64_t sequence, uint64_t *sequence_queued, uint64_t user_data);

#ifdef __cplusplus
}
#endif

#endif /* _XF86DRM_H_ */
