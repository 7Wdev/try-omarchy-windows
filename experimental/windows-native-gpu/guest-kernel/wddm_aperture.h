/* SPDX-License-Identifier: MIT */
/* Private QEMU CPU-aperture ABI. This is not DRM, dma-buf or /dev/dxg. */
#ifndef WDDM_APERTURE_H
#define WDDM_APERTURE_H
#include <linux/ioctl.h>
#include <linux/types.h>

#define WDDM_APERTURE_VERSION 1
#define WDDM_APERTURE_FENCE 1
#define WDDM_APERTURE_ALLOCATION 2
#define WDDM_APERTURE_WRITE 1
#define WDDM_APERTURE_MAX_LEASES 256
#define WDDM_APERTURE_CPU_LIMIT (96ULL * 1024 * 1024)
#define WDDM_OBJECT_SYNC 1
#define WDDM_OBJECT_RESOURCE 2
#define WDDM_OBJECT_MAX 256

struct wddm_aperture_info {
	__u32 version, fence_slots, allocation_slots, active_leases;
	__u64 fence_bytes, allocation_bytes, cpu_slot_bytes, active_cpu_bytes;
	__u64 fence_mmio_reads, allocation_mmio_reads, allocation_mmio_writes;
};
struct wddm_aperture_create {
	__u32 version, kind;
	__u64 offset, bytes, host_generation, cookie;
	__s32 fd;
	__u32 flags;
	__u64 reserved[2];
};
struct wddm_aperture_revoke {
	__u32 version, reserved;
	__u64 cookie;
};

#define WDDM_APERTURE_INFO _IOWR('W', 0x40, struct wddm_aperture_info)
#define WDDM_APERTURE_CREATE _IOWR('W', 0x41, struct wddm_aperture_create)
#define WDDM_APERTURE_REVOKE _IOW('W', 0x42, struct wddm_aperture_revoke)

/* Opaque bridge-session tickets, never native HANDLEs, CPU mappings,
 * dma-bufs or sync_files. Only the trusted controller can resolve/reap. */
struct wddm_object_create {
	__u32 version, kind;
	__u64 cookie;
	__u32 ticket;
	__s32 fd;
	__u64 reserved[2];
};
struct wddm_object_resolve {
	__u32 version, kind;
	__s32 fd;
	__u32 ticket;
	__u64 cookie, reserved;
};
struct wddm_object_reap {
	__u32 version, kind, ticket, reserved;
	__u64 cookie;
};
struct wddm_object_stats {
	__u32 version, held, active, reserved;
	__u64 created, closed, revoked;
};
#define WDDM_OBJECT_CREATE _IOWR('W', 0x43, struct wddm_object_create)
#define WDDM_OBJECT_RESOLVE _IOWR('W', 0x44, struct wddm_object_resolve)
#define WDDM_OBJECT_REVOKE _IOW('W', 0x45, struct wddm_aperture_revoke)
#define WDDM_OBJECT_REAP _IOWR('W', 0x46, struct wddm_object_reap)
#define WDDM_OBJECT_STATS _IOWR('W', 0x47, struct wddm_object_stats)

/* Explicit experimental sync_file export from an authorized read-only native
 * monitored-fence lease. No DMA-BUF or EGL-native-fence import is implied. */
struct wddm_sync_file_create {
	__u32 version, flags;
	__u64 cookie, offset, value;
	__s32 fd;
	__u32 reserved;
};
struct wddm_sync_file_stats {
	__u32 version, held, pending, reserved;
	__u64 created, signaled, errors, released;
};
#define WDDM_SYNC_FILE_CREATE _IOWR('W', 0x48, struct wddm_sync_file_create)
#define WDDM_SYNC_FILE_STATS _IOWR('W', 0x49, struct wddm_sync_file_stats)
#endif
