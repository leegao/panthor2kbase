/*
 * SPDX-License-Identifier: MIT
 *
 * libdrm KMS / Display & X11/XCB Shim Stubs for Android NDK Builds
 */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include "xf86drm.h"
#include "xf86drmMode.h"

drmModeResPtr drmModeGetResources(int fd) { (void)fd; return NULL; }
void drmModeFreeResources(drmModeResPtr ptr) { free(ptr); }
drmModeCrtcPtr drmModeGetCrtc(int fd, uint32_t crtcId) { (void)fd; (void)crtcId; return NULL; }
void drmModeFreeCrtc(drmModeCrtcPtr ptr) { free(ptr); }
drmModeEncoderPtr drmModeGetEncoder(int fd, uint32_t encoder_id) { (void)fd; (void)encoder_id; return NULL; }
void drmModeFreeEncoder(drmModeEncoderPtr ptr) { free(ptr); }
drmModeConnectorPtr drmModeGetConnector(int fd, uint32_t connectorId) { (void)fd; (void)connectorId; return NULL; }
drmModeConnectorPtr drmModeGetConnectorCurrent(int fd, uint32_t connector_id) { (void)fd; (void)connector_id; return NULL; }
void drmModeFreeConnector(drmModeConnectorPtr ptr) { free(ptr); }
drmModePlaneResPtr drmModeGetPlaneResources(int fd) { (void)fd; return NULL; }
void drmModeFreePlaneResources(drmModePlaneResPtr ptr) { free(ptr); }
drmModePlanePtr drmModeGetPlane(int fd, uint32_t plane_id) { (void)fd; (void)plane_id; return NULL; }
void drmModeFreePlane(drmModePlanePtr ptr) { free(ptr); }
drmModePropertyPtr drmModeGetProperty(int fd, uint32_t propertyId) { (void)fd; (void)propertyId; return NULL; }
void drmModeFreeProperty(drmModePropertyPtr ptr) { free(ptr); }
drmModePropertyBlobPtr drmModeGetPropertyBlob(int fd, uint32_t blob_id) { (void)fd; (void)blob_id; return NULL; }
void drmModeFreePropertyBlob(drmModePropertyBlobPtr ptr) { free(ptr); }
drmModeObjectPropertiesPtr drmModeObjectGetProperties(int fd, uint32_t object_id, uint32_t object_type) {
   (void)fd; (void)object_id; (void)object_type; return NULL;
}
void drmModeFreeObjectProperties(drmModeObjectPropertiesPtr ptr) { free(ptr); }
int drmModeConnectorSetProperty(int fd, uint32_t connector_id, uint32_t property_id, uint64_t value) {
   (void)fd; (void)connector_id; (void)property_id; (void)value; return -ENOSYS;
}
int drmModeCreatePropertyBlob(int fd, const void *data, size_t size, uint32_t *id) {
   (void)fd; (void)data; (void)size; (void)id; return -ENOSYS;
}
int drmModeDestroyPropertyBlob(int fd, uint32_t id) { (void)fd; (void)id; return -ENOSYS; }
int drmModeAddFB2WithModifiers(int fd, uint32_t width, uint32_t height, uint32_t pixel_format,
                               const uint32_t bo_handles[4], const uint32_t pitches[4],
                               const uint32_t offsets[4], const uint64_t modifier[4],
                               uint32_t *buf_id, uint32_t flags) {
   (void)fd; (void)width; (void)height; (void)pixel_format;
   (void)bo_handles; (void)pitches; (void)offsets; (void)modifier;
   (void)buf_id; (void)flags;
   return -ENOSYS;
}
int drmModeRmFB(int fd, uint32_t bufferId) { (void)fd; (void)bufferId; return -ENOSYS; }
drmModeAtomicReqPtr drmModeAtomicAlloc(void) { return NULL; }
void drmModeAtomicFree(drmModeAtomicReqPtr req) { free(req); }
int drmModeAtomicAddProperty(drmModeAtomicReqPtr req, uint32_t object_id, uint32_t property_id, uint64_t value) {
   (void)req; (void)object_id; (void)property_id; (void)value; return -ENOSYS;
}
int drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data) {
   (void)fd; (void)req; (void)flags; (void)user_data; return -ENOSYS;
}
int drmModeCreateLease(int fd, const uint32_t *objects, int num_objects, int flags, uint32_t *lessee_id) {
   (void)fd; (void)objects; (void)num_objects; (void)flags; (void)lessee_id; return -ENOSYS;
}
int drmCrtcGetSequence(int fd, uint32_t crtcId, uint64_t *sequence, uint64_t *ns) {
   (void)fd; (void)crtcId; (void)sequence; (void)ns; return -ENOSYS;
}
int drmCrtcQueueSequence(int fd, uint32_t crtcId, uint32_t flags,
                         uint64_t sequence, uint64_t *sequence_queued, uint64_t user_data) {
   (void)fd; (void)crtcId; (void)flags; (void)sequence; (void)sequence_queued; (void)user_data; return -ENOSYS;
}
int drmGetMagic(int fd, drm_magic_t *magic) { (void)fd; if (magic) *magic = 0; return 0; }
int drmAuthMagic(int fd, drm_magic_t magic) { (void)fd; (void)magic; return 0; }
int drmSetMaster(int fd) { (void)fd; return 0; }
int drmDropMaster(int fd) { (void)fd; return 0; }
int drmIsMaster(int fd) { (void)fd; return 0; }
int drmHandleEvent(int fd, drmEventContextPtr evctx) { (void)fd; (void)evctx; return 0; }
int drmCommandWriteRead(int fd, unsigned long drmCommandIndex, void *data, unsigned long size) {
   unsigned long req = _IOC(_IOC_READ | _IOC_WRITE, 'd', DRM_COMMAND_BASE + drmCommandIndex, size);
   return drmIoctl(fd, req, data);
}
