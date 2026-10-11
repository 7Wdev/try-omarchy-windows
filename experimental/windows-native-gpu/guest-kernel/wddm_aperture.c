// SPDX-License-Identifier: GPL-2.0-only
// Kernel ownership of the private QEMU native-memory apertures. Only a
// CAP_SYS_RAWIO broker can authorize a slice. Recipients receive a restricted
// mmap FD, not access to the whole BAR. No graphics/ioctl translation here.
#include <linux/anon_inodes.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "wddm_aperture.h"

#define CPU_STRIDE (1024ULL * 1024)
#define FENCE_MAGIC 0x31484e46
#define CPU_MAGIC 0x31484c41

struct aperture_hub {
	struct pci_dev *pci;
	resource_size_t start, bytes;
	u32 slots;
};
struct aperture_owner { unsigned int unused; };
struct aperture_lease {
	struct list_head link;
	struct aperture_owner *owner;
	struct file *file; /* One controller reference until revoke/remove/close. */
	u64 cookie, offset, bytes, host_generation;
	resource_size_t physical;
	u32 kind;
	bool writable, valid;
};
static DEFINE_MUTEX(aperture_lock);
static LIST_HEAD(aperture_leases);
static struct aperture_hub fence_hub, cpu_hub;
static u64 next_cookie, active_cpu_bytes;
static u32 active_leases;

static vm_fault_t lease_fault(struct vm_fault *fault)
{
	/* remap populates valid PTEs; a revoked/expanded mapping must never
	 * restore access to a recycled native allocation. */
	return VM_FAULT_SIGBUS;
}
static const struct vm_operations_struct lease_vm_ops = { .fault = lease_fault };

static int lease_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct aperture_lease *lease = file->private_data;
	int result = 0;
	mutex_lock(&aperture_lock);
	if (!lease->valid)
		result = -ENODEV;
	else if (!(vma->vm_flags & VM_SHARED) || vma->vm_pgoff ||
		 vma->vm_end - vma->vm_start != lease->bytes)
		result = -EINVAL;
	else if ((vma->vm_flags & VM_EXEC) ||
		 (!lease->writable && (vma->vm_flags & VM_WRITE)))
		result = -EACCES;
	else {
		vm_flags_clear(vma, VM_MAYEXEC | (lease->writable ? 0 : VM_MAYWRITE));
		vm_flags_set(vma, VM_DONTCOPY | VM_DONTDUMP | VM_DONTEXPAND);
		vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
		vma->vm_ops = &lease_vm_ops;
		result = vm_iomap_memory(vma, lease->physical, lease->bytes);
	}
	mutex_unlock(&aperture_lock);
	return result;
}
static int lease_release(struct inode *inode, struct file *file)
{
	struct aperture_lease *lease = file->private_data;
	/* The controller reference keeps valid leases alive. All VMAs also
	 * hold this file; reaching release means all recipients are finished. */
	WARN_ON(lease->valid);
	kfree(lease);
	return 0;
}
static const struct file_operations lease_fops = {
	.owner = THIS_MODULE, .mmap = lease_mmap, .release = lease_release,
};

/* Caller holds aperture_lock. Zap every recipient before freeing its range:
 * another creator must not reuse an offset while stale PTEs still exist.
 * unmap_mapping_range takes the inode mapping lock, not an mm's mmap lock;
 * the fault handler never takes aperture_lock. Drop the file outside it. */
static struct file *detach_lease(struct aperture_lease *lease)
{
	lease->valid = false;
	unmap_mapping_range(lease->file->f_mapping, 0, 0, 1);
	list_del(&lease->link);
	--active_leases;
	if (lease->kind == WDDM_APERTURE_ALLOCATION)
		active_cpu_bytes -= lease->bytes;
	return lease->file;
}
static void revoke_file(struct file *file)
{
	fput(file);
}
static void revoke_owner(struct aperture_owner *owner)
{
	struct aperture_lease *lease;
	struct file *file;
	for (;;) {
		file = NULL;
		mutex_lock(&aperture_lock);
		list_for_each_entry(lease, &aperture_leases, link) {
			if (!owner || lease->owner == owner) {
				file = detach_lease(lease);
				break;
			}
		}
		mutex_unlock(&aperture_lock);
		if (!file)
			return;
		revoke_file(file);
	}
}
static bool valid_create(const struct wddm_aperture_create *arg,
			 const struct aperture_hub *hub)
{
	if (arg->version != WDDM_APERTURE_VERSION || arg->cookie || arg->fd ||
	    arg->reserved[0] || arg->reserved[1] || !arg->bytes ||
	    !IS_ALIGNED(arg->offset, PAGE_SIZE) || !IS_ALIGNED(arg->bytes, PAGE_SIZE) ||
	    arg->offset >= hub->bytes || arg->bytes > hub->bytes - arg->offset)
		return false;
	if (arg->kind == WDDM_APERTURE_FENCE)
		return !arg->flags && !arg->host_generation && arg->bytes == PAGE_SIZE;
	return arg->kind == WDDM_APERTURE_ALLOCATION &&
		arg->flags == WDDM_APERTURE_WRITE && arg->host_generation &&
		arg->host_generation != U64_MAX && IS_ALIGNED(arg->offset, CPU_STRIDE) &&
		arg->bytes <= WDDM_APERTURE_CPU_LIMIT;
}
static long create_lease(struct file *control, void __user *pointer)
{
	struct wddm_aperture_create arg;
	struct aperture_lease *lease, *other;
	struct aperture_hub *hub;
	struct file *file;
	int fd, result = 0;
	if (!capable(CAP_SYS_RAWIO))
		return -EPERM;
	if (copy_from_user(&arg, pointer, sizeof(arg)))
		return -EFAULT;
	lease = kzalloc(sizeof(*lease), GFP_KERNEL);
	if (!lease)
		return -ENOMEM;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { kfree(lease); return fd; }
	/* A unique inode is essential: revoking one lease must not zap every
	 * anon-inode FD in the guest. Its mapping also tracks transferred FDs. */
	file = anon_inode_create_getfile("[wddm-aperture]", &lease_fops, lease,
					 O_RDWR, NULL);
	if (IS_ERR(file)) {
		put_unused_fd(fd); kfree(lease); return PTR_ERR(file);
	}
	mutex_lock(&aperture_lock);
	hub = arg.kind == WDDM_APERTURE_FENCE ? &fence_hub : &cpu_hub;
	if (!fence_hub.pci || !cpu_hub.pci)
		result = -ENODEV;
	else if (!valid_create(&arg, hub))
		result = -EINVAL;
	else if (active_leases >= WDDM_APERTURE_MAX_LEASES || next_cookie == U64_MAX ||
		 (arg.kind == WDDM_APERTURE_ALLOCATION &&
		  arg.bytes > WDDM_APERTURE_CPU_LIMIT - active_cpu_bytes))
		result = -ENOSPC;
	else if (arg.kind == WDDM_APERTURE_ALLOCATION) {
		list_for_each_entry(other, &aperture_leases, link) {
			if (other->kind == arg.kind && arg.offset < other->offset + other->bytes &&
			    other->offset < arg.offset + arg.bytes) {
				result = -EBUSY;
				break;
			}
		}
	}
	if (!result) {
		lease->owner = control->private_data;
		lease->cookie = ++next_cookie;
		lease->offset = arg.offset; lease->bytes = arg.bytes;
		lease->host_generation = arg.host_generation;
		lease->physical = hub->start + arg.offset;
		lease->kind = arg.kind;
		lease->writable = arg.flags == WDDM_APERTURE_WRITE;
		lease->file = file; lease->valid = true;
		list_add_tail(&lease->link, &aperture_leases);
		++active_leases;
		if (lease->kind == WDDM_APERTURE_ALLOCATION)
			active_cpu_bytes += lease->bytes;
		arg.cookie = lease->cookie; arg.fd = fd;
		if (copy_to_user(pointer, &arg, sizeof(arg))) {
			detach_lease(lease);
			result = -EFAULT;
		} else {
			/* Controller owns file's initial reference; fd_install owns
			 * the second. Neither controller close nor removal races a
			 * zero-reference file still on aperture_leases. */
			get_file(file);
		}
	}
	mutex_unlock(&aperture_lock);
	if (result) { put_unused_fd(fd); fput(file); return result; }
	fd_install(fd, file);
	return 0;
}
static long control_ioctl(struct file *control, unsigned int request, unsigned long address)
{
	void __user *pointer = (void __user *)address;
	struct wddm_aperture_revoke revoke;
	struct wddm_aperture_info info;
	struct aperture_lease *lease;
	struct file *file = NULL;
	u32 counter;
	int result = 0;
	if (request == WDDM_APERTURE_CREATE)
		return create_lease(control, pointer);
	if (request == WDDM_APERTURE_REVOKE) {
		if (copy_from_user(&revoke, pointer, sizeof(revoke))) return -EFAULT;
		if (revoke.version != WDDM_APERTURE_VERSION || revoke.reserved || !revoke.cookie)
			return -EINVAL;
		mutex_lock(&aperture_lock);
		list_for_each_entry(lease, &aperture_leases, link) {
			if (lease->cookie == revoke.cookie && lease->owner == control->private_data) {
				file = detach_lease(lease); break;
			}
		}
		mutex_unlock(&aperture_lock);
		if (!file) return -ENOENT;
		revoke_file(file);
		return 0;
	}
	if (request != WDDM_APERTURE_INFO) return -ENOTTY;
	if (copy_from_user(&info, pointer, sizeof(info))) return -EFAULT;
	if (info.version != WDDM_APERTURE_VERSION) return -EINVAL;
	memset(&info, 0, sizeof(info));
	mutex_lock(&aperture_lock);
	if (!fence_hub.pci || !cpu_hub.pci)
		result = -ENODEV;
	else {
		info.version = WDDM_APERTURE_VERSION;
		info.fence_slots = fence_hub.slots; info.allocation_slots = cpu_hub.slots;
		info.fence_bytes = fence_hub.bytes; info.allocation_bytes = cpu_hub.bytes;
		info.cpu_slot_bytes = CPU_STRIDE; info.active_leases = active_leases;
		info.active_cpu_bytes = active_cpu_bytes;
		pci_read_config_dword(fence_hub.pci, 0x48, &counter); info.fence_mmio_reads = counter;
		pci_read_config_dword(cpu_hub.pci, 0x4c, &counter); info.allocation_mmio_reads = counter;
		pci_read_config_dword(cpu_hub.pci, 0x50, &counter); info.allocation_mmio_writes = counter;
	}
	mutex_unlock(&aperture_lock);
	if (!result && copy_to_user(pointer, &info, sizeof(info))) result = -EFAULT;
	return result;
}
static int control_open(struct inode *inode, struct file *file)
{
	file->private_data = kzalloc(sizeof(struct aperture_owner), GFP_KERNEL);
	return file->private_data ? 0 : -ENOMEM;
}
static int control_release(struct inode *inode, struct file *file)
{
	revoke_owner(file->private_data);
	kfree(file->private_data);
	return 0;
}
static const struct file_operations control_fops = {
	.owner = THIS_MODULE, .open = control_open, .release = control_release,
	.unlocked_ioctl = control_ioctl, .compat_ioctl = compat_ptr_ioctl,
};
static struct miscdevice aperture_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "wddm-aperture", .fops = &control_fops,
	.mode = 0600,
};

static int aperture_probe(struct pci_dev *pci, const struct pci_device_id *id)
{
	bool fence = pci->device == 0x11fd;
	struct aperture_hub *hub = fence ? &fence_hub : &cpu_hub;
	u32 magic, slots, stride = 0;
	u64 bytes;
	int result;
	if (PAGE_SIZE != 4096) return -ENODEV;
	result = pci_enable_device_mem(pci);
	if (result) return result;
	pci_read_config_dword(pci, 0x40, &magic);
	pci_read_config_dword(pci, 0x44, &slots);
	if (!fence) pci_read_config_dword(pci, 0x48, &stride);
	bytes = fence ? (u64)slots * 4096 : (u64)slots * CPU_STRIDE;
	if ((fence && (magic != FENCE_MAGIC || (slots != 64 && slots != 128))) ||
	    (!fence && (magic != CPU_MAGIC || stride != CPU_STRIDE || slots < 16 ||
		       slots > 128 || !is_power_of_2(slots))) ||
	    !(pci_resource_flags(pci, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pci, 0) != bytes || !IS_ALIGNED(pci_resource_start(pci, 0), PAGE_SIZE)) {
		result = -ENODEV; goto disable;
	}
	result = pci_request_region(pci, 0, "wddm-aperture");
	if (result) goto disable;
	mutex_lock(&aperture_lock);
	if (hub->pci) result = -EBUSY;
	else {
		hub->pci = pci; hub->slots = slots;
		hub->start = pci_resource_start(pci, 0); hub->bytes = bytes;
		pci_set_drvdata(pci, hub);
	}
	mutex_unlock(&aperture_lock);
	if (result) { pci_release_region(pci, 0); goto disable; }
	dev_info(&pci->dev, "WDDM CPU aperture kind=%s slots=%u bytes=%llu\n",
		 fence ? "fence" : "allocation", slots, bytes);
	return 0;
disable:
	pci_disable_device(pci);
	return result;
}
static void aperture_remove(struct pci_dev *pci)
{
	struct aperture_hub *hub = pci_get_drvdata(pci);
	mutex_lock(&aperture_lock);
	hub->pci = NULL;
	mutex_unlock(&aperture_lock);
	/* No create can pass the ready check after either hub goes offline. */
	revoke_owner(NULL);
	pci_release_region(pci, 0);
	pci_disable_device(pci);
}
static const struct pci_device_id aperture_ids[] = {
	{ PCI_DEVICE(0x1234, 0x11fd) }, { PCI_DEVICE(0x1234, 0x11fc) }, { }
};
MODULE_DEVICE_TABLE(pci, aperture_ids);
static struct pci_driver aperture_driver = {
	.name = "wddm-aperture", .id_table = aperture_ids,
	.probe = aperture_probe, .remove = aperture_remove,
};
static int __init aperture_init(void)
{
	int result = pci_register_driver(&aperture_driver);
	if (result) return result;
	result = misc_register(&aperture_device);
	if (result) pci_unregister_driver(&aperture_driver);
	return result;
}
static void __exit aperture_exit(void)
{
	misc_deregister(&aperture_device);
	pci_unregister_driver(&aperture_driver);
}
module_init(aperture_init);
module_exit(aperture_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Private QEMU WDDM CPU-aperture lease driver; no DRM or graphics ABI");
