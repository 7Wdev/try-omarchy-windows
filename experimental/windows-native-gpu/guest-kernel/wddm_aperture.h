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
#endif
