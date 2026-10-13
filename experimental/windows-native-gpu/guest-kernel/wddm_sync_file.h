/* SPDX-License-Identifier: GPL-2.0-only */
/* Included after aperture leases/lock. The native fence page is never read
 * after revocation: that decision and the read share aperture_lock. Polling is
 * a bounded integration baseline, not an interrupt/performance implementation. */
#include <linux/dma-fence.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/sync_file.h>
#include <linux/workqueue.h>

#define WDDM_SYNC_MAX 256
#define WDDM_SYNC_TIMEOUT_MS 5000
static bool enable_native_sync_files;
module_param(enable_native_sync_files, bool, 0400);
MODULE_PARM_DESC(enable_native_sync_files, "Explicit experimental native GPU sync_file export");
static struct workqueue_struct *native_sync_wq;
static u32 native_sync_held, native_sync_pending;
static u64 native_sync_created, native_sync_signaled, native_sync_errors, native_sync_released;
struct native_sync {
	struct dma_fence fence;
	spinlock_t lock;
	struct delayed_work poll;
	struct work_struct free;
	struct file *source;
	void __iomem *word;
	u64 target;
	unsigned long deadline;
	bool published;
};
static const char *native_sync_driver(struct dma_fence *fence) { return "wddm-aperture"; }
static const char *native_sync_timeline(struct dma_fence *fence) { return "native-monitored-fence"; }
static bool native_sync_enable(struct dma_fence *fence) { return true; }
static void native_sync_free(struct work_struct *work)
{
	struct native_sync *sync = container_of(work, struct native_sync, free);
	iounmap(sync->word);
	fput(sync->source);
	mutex_lock(&aperture_lock);
	--native_sync_held;
	if (sync->published) ++native_sync_released;
	mutex_unlock(&aperture_lock);
	kfree(sync);
	/* Module exit destroys/flushed this queue before its code is removed. */
	module_put(THIS_MODULE);
}
static void native_sync_release(struct dma_fence *fence)
{
	struct native_sync *sync = container_of(fence, struct native_sync, fence);
	/* Last put may run in atomic context. iounmap/fput use our workqueue. */
	queue_work(native_sync_wq, &sync->free);
}
static const struct dma_fence_ops native_sync_ops = {
	.get_driver_name = native_sync_driver, .get_timeline_name = native_sync_timeline,
	.enable_signaling = native_sync_enable, .release = native_sync_release,
};
static void native_sync_poll(struct work_struct *work)
{
	struct native_sync *sync = container_of(to_delayed_work(work), struct native_sync, poll);
	struct aperture_lease *lease = sync->source->private_data;
	int status = 0;
	u64 value = 0;
	mutex_lock(&aperture_lock);
	if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) status = -ENODEV;
	else {
		value = readq(sync->word);
		if (value == U64_MAX) status = -EIO;
		else if (value >= sync->target) status = 1;
		else if (time_after_eq(jiffies, sync->deadline)) status = -ETIMEDOUT;
	}
	if (status) {
		--native_sync_pending;
		if (status > 0) ++native_sync_signaled; else ++native_sync_errors;
	}
	mutex_unlock(&aperture_lock);
	if (!status) {
		queue_delayed_work(native_sync_wq, &sync->poll, max_t(unsigned long, 1, msecs_to_jiffies(1)));
		return;
	}
	if (status < 0) dma_fence_set_error(&sync->fence, status);
	dma_fence_signal(&sync->fence);
	dma_fence_put(&sync->fence); /* Retained polling reference. */
}
static long native_sync_create(struct file *control, void __user *pointer)
{
	struct wddm_sync_file_create arg;
	struct aperture_lease *lease;
	struct native_sync *sync;
	struct sync_file *file;
	int fd, result = -ENOENT;
	if (!capable(CAP_SYS_RAWIO)) return -EPERM;
	if (!enable_native_sync_files) return -EOPNOTSUPP;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION || arg.flags || !arg.cookie ||
	    !arg.value || arg.value == U64_MAX || arg.fd || arg.reserved || (arg.offset & 7)) return -EINVAL;
	sync = kzalloc(sizeof(*sync), GFP_KERNEL);
	if (!sync) return -ENOMEM;
	mutex_lock(&aperture_lock);
	list_for_each_entry(lease, &aperture_leases, link) {
		if (lease->cookie != arg.cookie || lease->owner != control->private_data) continue;
		if (!lease->valid || !fence_hub.pci || !cpu_hub.pci) { result = -ENODEV; break; }
		if (lease->kind != WDDM_APERTURE_FENCE || lease->writable ||
		    arg.offset >= lease->bytes || sizeof(u64) > lease->bytes - arg.offset) { result = -EINVAL; break; }
		if (native_sync_held >= WDDM_SYNC_MAX) { result = -ENOSPC; break; }
		sync->word = ioremap(lease->physical + arg.offset, sizeof(u64));
		if (!sync->word) { result = -ENOMEM; break; }
		sync->source = get_file(lease->file);
		++native_sync_held; result = 0; break;
	}
	mutex_unlock(&aperture_lock);
	if (result) { kfree(sync); return result; }
	__module_get(THIS_MODULE);
	spin_lock_init(&sync->lock);
	sync->target = arg.value;
	INIT_WORK(&sync->free, native_sync_free);
	INIT_DELAYED_WORK(&sync->poll, native_sync_poll);
	/* Independent one-shot context; the native 64-bit target is kept above.
	 * No ordering comparison truncates that value into dma_fence's seqno. */
	dma_fence_init(&sync->fence, &native_sync_ops, &sync->lock, dma_fence_context_alloc(1), 1);
	file = sync_file_create(&sync->fence);
	if (!file) { dma_fence_put(&sync->fence); return -ENOMEM; }
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) { fput(file->file); dma_fence_put(&sync->fence); return fd; }
	arg.fd = fd;
	if (copy_to_user(pointer, &arg, sizeof(arg))) {
		put_unused_fd(fd); fput(file->file); dma_fence_put(&sync->fence); return -EFAULT;
	}
	mutex_lock(&aperture_lock);
	sync->published = true;
	++native_sync_created; ++native_sync_pending;
	mutex_unlock(&aperture_lock);
	sync->deadline = jiffies + msecs_to_jiffies(WDDM_SYNC_TIMEOUT_MS);
	/* Initial reference belongs to polling until signaled/error; the sync_file
 * holds a separate reference. Last-FD close cannot free a pending worker. */
	queue_delayed_work(native_sync_wq, &sync->poll, 0);
	fd_install(fd, file->file);
	return 0;
}
static long native_sync_stats(void __user *pointer)
{
	struct wddm_sync_file_stats arg;
	if (copy_from_user(&arg, pointer, sizeof(arg))) return -EFAULT;
	if (arg.version != WDDM_APERTURE_VERSION) return -EINVAL;
	mutex_lock(&aperture_lock);
	arg = (struct wddm_sync_file_stats){ WDDM_APERTURE_VERSION, native_sync_held, native_sync_pending, 0,
		native_sync_created, native_sync_signaled, native_sync_errors, native_sync_released };
	mutex_unlock(&aperture_lock);
	return copy_to_user(pointer, &arg, sizeof(arg)) ? -EFAULT : 0;
}
