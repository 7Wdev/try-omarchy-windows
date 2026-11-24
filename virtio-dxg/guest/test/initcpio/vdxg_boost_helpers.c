/*
 * vdxg_boost_helpers.c: tiny zero-dependency runtime helpers built into tmpfs by
 * desktop-boost when gcc is present in the guest:
 *   -DBUILD_GAPP_HOLD   -> libvdxg-gapp-hold.so  (keeps GApplication services resident)
 *   -DBUILD_WL_GATE     -> libvdxg-wl-gate.so    (pauses pre-warmed app on 1st toplevel commit)
 *   -DBUILD_FASTLAUNCH  -> vdxg-fastlaunch       (< 1 ms C IPC client for Chromium, VS Code,
 *                                                 terminals, and gated apps)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>

#if defined(BUILD_WL_GATE) || defined(BUILD_FASTLAUNCH)
static int write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}
#endif

#ifdef BUILD_FASTLAUNCH
static int connect_unix_uid(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid())
        return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(sa.sun_path)) {
        close(fd);
        return -1;
    }
    strcpy(sa.sun_path, path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

#ifdef BUILD_GAPP_HOLD

static pthread_mutex_t g_hold_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_held_apps[16];
static int g_held_count = 0;

__attribute__((constructor))
static void gapp_hold_init(void)
{
    unsetenv("LD_PRELOAD");
}

static void hold_gapp_once(void *app)
{
    typedef void (*fn_set_t)(void *, unsigned int);
    typedef void (*fn_hold_t)(void *);
    static fn_set_t real_set = NULL;
    static fn_hold_t real_hold = NULL;

    if (!real_set)
        real_set = (fn_set_t)dlsym(RTLD_NEXT, "g_application_set_inactivity_timeout");
    if (!real_hold) {
        real_hold = (fn_hold_t)dlsym(RTLD_NEXT, "g_application_hold");
        if (!real_hold)
            real_hold = (fn_hold_t)dlsym(RTLD_DEFAULT, "g_application_hold");
    }
    if (real_set && app)
        real_set(app, 0);
    if (app && real_hold) {
        int already = 0;
        pthread_mutex_lock(&g_hold_lock);
        for (int i = 0; i < g_held_count; i++) {
            if (g_held_apps[i] == app) {
                already = 1;
                break;
            }
        }
        if (!already && g_held_count < 16)
            g_held_apps[g_held_count++] = app;
        pthread_mutex_unlock(&g_hold_lock);
        if (!already)
            real_hold(app);
    }
}

void g_application_set_inactivity_timeout(void *app, unsigned int timeout)
{
    (void)timeout;
    hold_gapp_once(app);
}

int g_application_run(void *app, int argc, char **argv)
{
    typedef int (*fn_run_t)(void *, int, char **);
    static fn_run_t real_run = NULL;
    if (!real_run)
        real_run = (fn_run_t)dlsym(RTLD_NEXT, "g_application_run");
    hold_gapp_once(app);
    return real_run ? real_run(app, argc, argv) : 1;
}

#elif defined(BUILD_WL_GATE)

struct wl_proxy;
struct wl_display;
struct wl_array;

struct wl_message {
    const char *name;
    const char *signature;
    const void *types;
};

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const struct wl_message *methods;
    int event_count;
    const struct wl_message *events;
};

union wl_argument {
    int32_t i;
    uint32_t u;
    int32_t f;
    const char *s;
    struct wl_proxy *o;
    uint32_t n;
    struct wl_array *a;
    int32_t h;
};

typedef struct wl_proxy *(*fn_marshal_array_flags_t)(
    struct wl_proxy *, uint32_t, const struct wl_interface *,
    uint32_t, uint32_t, union wl_argument *);
typedef void (*fn_marshal_array_t)(struct wl_proxy *, uint32_t, union wl_argument *);
typedef struct wl_proxy *(*fn_marshal_array_ctor_t)(
    struct wl_proxy *, uint32_t, union wl_argument *, const struct wl_interface *);
typedef struct wl_proxy *(*fn_marshal_array_ctor_ver_t)(
    struct wl_proxy *, uint32_t, union wl_argument *, const struct wl_interface *, uint32_t);
typedef const struct wl_interface *(*fn_get_iface_t)(struct wl_proxy *);
typedef const char *(*fn_get_class_t)(struct wl_proxy *);
typedef uint32_t (*fn_get_id_t)(struct wl_proxy *);
typedef struct wl_display *(*fn_get_display_t)(struct wl_proxy *);
typedef int (*fn_display_flush_t)(struct wl_display *);

static fn_marshal_array_flags_t real_marshal_array_flags = NULL;
static fn_marshal_array_t real_marshal_array = NULL;
static fn_marshal_array_ctor_t real_marshal_array_ctor = NULL;
static fn_marshal_array_ctor_ver_t real_marshal_array_ctor_ver = NULL;
static fn_get_iface_t real_get_iface = NULL;
static fn_get_class_t real_get_class = NULL;
static fn_get_id_t real_get_id = NULL;
static fn_get_display_t real_get_display = NULL;
static fn_display_flush_t real_display_flush = NULL;

static pthread_once_t g_wl_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static char g_gate_sock[256];
static int g_gate_armed = 0;
static int g_gate_waiting = 0;
static int g_gate_released = 0;
static int g_client_fd = -1;

struct surface_track {
    struct wl_display *dpy;
    uint32_t wl_surface_id;
    uint32_t xdg_surface_id;
    int is_toplevel;
    int has_buffer;
};
static struct surface_track g_surfaces[32];
static int g_surface_count = 0;

typedef void *(*fn_dlsym_t)(void *, const char *);
static fn_dlsym_t g_real_dlsym = NULL;
static void * volatile g_wl_dlopen_handle = NULL;

static fn_dlsym_t get_real_dlsym(void)
{
    fn_dlsym_t fn = __atomic_load_n(&g_real_dlsym, __ATOMIC_ACQUIRE);
    if (fn)
        return fn;
    static const char * const vers[] = { "GLIBC_2.34", "GLIBC_2.2.5" };
    for (size_t i = 0; i < sizeof(vers) / sizeof(vers[0]); i++) {
        void *p = dlvsym(RTLD_NEXT, "dlsym", vers[i]);
        if (p) {
            memcpy(&fn, &p, sizeof(fn));
            __atomic_store_n(&g_real_dlsym, fn, __ATOMIC_RELEASE);
            return fn;
        }
    }
    return NULL;
}

static void *lookup_wl_sym(void *fallback, const char *name)
{
    fn_dlsym_t rdlsym = get_real_dlsym();
    if (!rdlsym)
        return NULL;
    void *p = rdlsym(RTLD_NEXT, name);
    if (!p && fallback)
        p = rdlsym(fallback, name);
    return p;
}

static void resolve_wl_syms_once(void)
{
    void *fb = __atomic_load_n(&g_wl_dlopen_handle, __ATOMIC_ACQUIRE);
    if (!fb)
        fb = dlopen("libwayland-client.so.0", RTLD_NOLOAD | RTLD_LAZY);
    if (!fb)
        fb = dlopen("libwayland-client.so", RTLD_NOLOAD | RTLD_LAZY);
    real_marshal_array_flags = (fn_marshal_array_flags_t)lookup_wl_sym(fb, "wl_proxy_marshal_array_flags");
    real_marshal_array = (fn_marshal_array_t)lookup_wl_sym(fb, "wl_proxy_marshal_array");
    real_marshal_array_ctor = (fn_marshal_array_ctor_t)lookup_wl_sym(fb, "wl_proxy_marshal_array_constructor");
    real_marshal_array_ctor_ver = (fn_marshal_array_ctor_ver_t)lookup_wl_sym(fb, "wl_proxy_marshal_array_constructor_versioned");
    real_get_iface = (fn_get_iface_t)lookup_wl_sym(fb, "wl_proxy_get_interface");
    real_get_class = (fn_get_class_t)lookup_wl_sym(fb, "wl_proxy_get_class");
    real_get_id = (fn_get_id_t)lookup_wl_sym(fb, "wl_proxy_get_id");
    real_get_display = (fn_get_display_t)lookup_wl_sym(fb, "wl_proxy_get_display");
    real_display_flush = (fn_display_flush_t)lookup_wl_sym(fb, "wl_display_flush");
}

static inline void resolve_wl_syms(void)
{
    pthread_once(&g_wl_once, resolve_wl_syms_once);
}

static int is_wrapper_process(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0)
        return 0;
    exe[n] = '\0';
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    return !strcmp(base, "env") || !strcmp(base, "nice") || !strcmp(base, "ionice") ||
           !strcmp(base, "timeout") || !strcmp(base, "sh") || !strcmp(base, "bash");
}

static void *gate_init_watchdog(void *arg)
{
    (void)arg;
    sleep(20);
    if (!__atomic_load_n(&g_gate_waiting, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&g_gate_released, __ATOMIC_ACQUIRE))
        _exit(1);
    return NULL;
}

__attribute__((constructor))
static void wl_gate_init(void)
{
    if (is_wrapper_process())
        return;
    const char *s = getenv("VDXG_WL_GATE_SOCK");
    if (s && *s && strlen(s) < sizeof(g_gate_sock) - 16) {
        strcpy(g_gate_sock, s);
        g_gate_armed = 1;
        pthread_t wth;
        pthread_attr_t wattr;
        pthread_attr_init(&wattr);
        pthread_attr_setdetachstate(&wattr, PTHREAD_CREATE_DETACHED);
        (void)pthread_create(&wth, &wattr, gate_init_watchdog, NULL);
        pthread_attr_destroy(&wattr);
    }
    unsetenv("LD_PRELOAD");
    unsetenv("VDXG_WL_GATE_SOCK");
}

static void *exit_watch_thread(void *arg)
{
    int fd = (int)(intptr_t)arg;
    char ch = 0;
    for (;;) {
        ssize_t n = read(fd, &ch, 1);
        if (n > 0 && ch == 'Q')
            _exit(0);
        if (n == 0)
            break;
        if (n < 0 && errno != EINTR)
            break;
    }
    close(fd);
    return NULL;
}

static void wait_at_gate(struct wl_proxy *surface)
{
    char sock_path[256], lock_path[280], tmp_path[280];
    int lock_fd = -1, lfd = -1, afd = -1;
    struct sockaddr_un sa;

    pthread_mutex_lock(&g_gate_mtx);
    if (!g_gate_armed || g_gate_released || !g_gate_sock[0]) {
        pthread_mutex_unlock(&g_gate_mtx);
        return;
    }
    strcpy(sock_path, g_gate_sock);
    pthread_mutex_unlock(&g_gate_mtx);
    __atomic_store_n(&g_gate_waiting, 1, __ATOMIC_RELEASE);

    /* Serialize multiple warm standby processes sharing sock_path. */
    snprintf(lock_path, sizeof(lock_path), "%s.lock", sock_path);
    lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd >= 0) {
        while (flock(lock_fd, LOCK_EX) != 0) {
            if (errno != EINTR) {
                close(lock_fd);
                lock_fd = -1;
                break;
            }
        }
    }

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", sock_path, (int)getpid());
    unlink(tmp_path);
    unlink(sock_path);

    lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lfd < 0) {
        if (lock_fd >= 0)
            close(lock_fd);
        return;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(tmp_path) >= sizeof(sa.sun_path)) {
        close(lfd);
        if (lock_fd >= 0)
            close(lock_fd);
        return;
    }
    strcpy(sa.sun_path, tmp_path);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
        chmod(tmp_path, 0600) != 0 ||
        listen(lfd, 4) != 0 ||
        rename(tmp_path, sock_path) != 0) {
        unlink(tmp_path);
        close(lfd);
        if (lock_fd >= 0)
            close(lock_fd);
        return;
    }

    for (;;) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, -1);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        afd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (afd < 0)
            continue;
        struct ucred uc;
        socklen_t ulen = sizeof(uc);
        if (getsockopt(afd, SOL_SOCKET, SO_PEERCRED, &uc, &ulen) != 0 || uc.uid != getuid()) {
            close(afd);
            afd = -1;
            continue;
        }
        char cmd = 0;
        struct pollfd rfd = { .fd = afd, .events = POLLIN };
        if (poll(&rfd, 1, 500) <= 0 || read(afd, &cmd, 1) != 1 || cmd != 'G') {
            close(afd);
            afd = -1;
            continue;
        }
        break;
    }

    unlink(sock_path);
    close(lfd);
    if (lock_fd >= 0)
        close(lock_fd); /* Releases LOCK_EX so next warm worker publishes immediately. */

    pthread_mutex_lock(&g_gate_mtx);
    g_gate_released = 1;
    g_client_fd = afd;
    pthread_mutex_unlock(&g_gate_mtx);

    if (afd >= 0) {
        (void)write_all(afd, "OK\n", 3);
        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, exit_watch_thread, (void *)(intptr_t)afd) != 0)
            close(afd);
        pthread_attr_destroy(&attr);
    }
    (void)surface;
}

static uint32_t proxy_id(struct wl_proxy *p)
{
    return (p && real_get_id) ? real_get_id(p) : 0;
}

static struct wl_display *proxy_dpy(struct wl_proxy *p)
{
    return (p && real_get_display) ? real_get_display(p) : NULL;
}

/* Key by (wl_display, object ID): EGL/Vulkan WSI create wl_proxy_create_wrapper()
 * handles that have distinct pointers but share the underlying surface ID. */
static struct surface_track *find_surface_locked(struct wl_proxy *wl_surface, int create)
{
    struct wl_display *dpy = proxy_dpy(wl_surface);
    uint32_t sid = proxy_id(wl_surface);
    if (!sid)
        return NULL;
    for (int i = 0; i < g_surface_count; i++) {
        if (g_surfaces[i].dpy == dpy && g_surfaces[i].wl_surface_id == sid)
            return &g_surfaces[i];
    }
    if (create && g_surface_count < 32) {
        struct surface_track *st = &g_surfaces[g_surface_count++];
        memset(st, 0, sizeof(*st));
        st->dpy = dpy;
        st->wl_surface_id = sid;
        return st;
    }
    return NULL;
}

static void inspect_before_call(struct wl_proxy *proxy, uint32_t opcode,
                                union wl_argument *args, struct wl_proxy **out_flush_proxy)
{
    *out_flush_proxy = NULL;
    if (!proxy || !g_gate_armed || g_gate_released || !real_get_class)
        return;
    const char *cls = real_get_class(proxy);
    if (!cls)
        return;

    if (strcmp(cls, "wl_surface") == 0) {
        if (opcode == 1 && args) { /* attach(buffer, x, y) */
            pthread_mutex_lock(&g_gate_mtx);
            struct surface_track *st = find_surface_locked(proxy, 1);
            if (st)
                st->has_buffer = (args[0].o != NULL);
            pthread_mutex_unlock(&g_gate_mtx);
        } else if (opcode == 6) { /* commit */
            int should_gate = 0;
            pthread_mutex_lock(&g_gate_mtx);
            struct surface_track *st = find_surface_locked(proxy, 0);
            if (!g_gate_released && st && st->is_toplevel && st->has_buffer)
                should_gate = 1;
            pthread_mutex_unlock(&g_gate_mtx);
            if (should_gate) {
                wait_at_gate(proxy);
                *out_flush_proxy = proxy;
            }
        } else if (opcode == 0) { /* destroy */
            struct wl_display *dpy = proxy_dpy(proxy);
            uint32_t sid = proxy_id(proxy);
            pthread_mutex_lock(&g_gate_mtx);
            for (int i = 0; i < g_surface_count; i++) {
                if (g_surfaces[i].dpy == dpy && g_surfaces[i].wl_surface_id == sid) {
                    g_surfaces[i] = g_surfaces[--g_surface_count];
                    break;
                }
            }
            pthread_mutex_unlock(&g_gate_mtx);
        }
    } else if (strcmp(cls, "xdg_surface") == 0) {
        struct wl_display *dpy = proxy_dpy(proxy);
        uint32_t xid = proxy_id(proxy);
        if (opcode == 1 && xid) { /* get_toplevel */
            pthread_mutex_lock(&g_gate_mtx);
            for (int i = 0; i < g_surface_count; i++) {
                if (g_surfaces[i].dpy == dpy && g_surfaces[i].xdg_surface_id == xid) {
                    g_surfaces[i].is_toplevel = 1;
                    break;
                }
            }
            pthread_mutex_unlock(&g_gate_mtx);
        } else if (opcode == 0 && xid) { /* destroy */
            pthread_mutex_lock(&g_gate_mtx);
            for (int i = 0; i < g_surface_count; i++) {
                if (g_surfaces[i].dpy == dpy && g_surfaces[i].xdg_surface_id == xid)
                    g_surfaces[i].xdg_surface_id = 0;
            }
            pthread_mutex_unlock(&g_gate_mtx);
        }
    }
}

static void inspect_after_call(struct wl_proxy *proxy, uint32_t opcode,
                               union wl_argument *args, struct wl_proxy *ret)
{
    if (!ret || !proxy || !g_gate_armed || g_gate_released || !real_get_class)
        return;
    const char *cls = real_get_class(proxy);
    if (cls && strcmp(cls, "xdg_wm_base") == 0 && opcode == 2 && args && args[1].o) {
        pthread_mutex_lock(&g_gate_mtx);
        struct surface_track *st = find_surface_locked(args[1].o, 1);
        if (st)
            st->xdg_surface_id = proxy_id(ret);
        pthread_mutex_unlock(&g_gate_mtx);
    }
}

static void parse_va_args(struct wl_proxy *proxy, uint32_t opcode,
                          union wl_argument *args, int max_args, va_list ap)
{
    if (!proxy || !real_get_iface)
        return;
    const struct wl_interface *iface = real_get_iface(proxy);
    if (!iface || (int)opcode >= iface->method_count)
        return;
    const char *sig = iface->methods[opcode].signature;
    if (!sig)
        return;
    int i = 0;
    while (*sig && i < max_args) {
        char c = *sig++;
        if (c == '?' || (c >= '0' && c <= '9'))
            continue;
        switch (c) {
        case 'i': args[i++].i = va_arg(ap, int32_t); break;
        case 'u': args[i++].u = va_arg(ap, uint32_t); break;
        case 'f': args[i++].f = va_arg(ap, int32_t); break;
        case 's': args[i++].s = va_arg(ap, const char *); break;
        case 'o': args[i++].o = va_arg(ap, struct wl_proxy *); break;
        case 'n': args[i++].o = va_arg(ap, struct wl_proxy *); break;
        case 'a': args[i++].a = va_arg(ap, struct wl_array *); break;
        case 'h': args[i++].h = va_arg(ap, int32_t); break;
        default: break;
        }
    }
}

struct wl_proxy *wl_proxy_marshal_array_flags(struct wl_proxy *proxy, uint32_t opcode,
                                              const struct wl_interface *interface,
                                              uint32_t version, uint32_t flags,
                                              union wl_argument *args)
{
    struct wl_proxy *flush_proxy = NULL;
    resolve_wl_syms();
    inspect_before_call(proxy, opcode, args, &flush_proxy);
    struct wl_proxy *ret = real_marshal_array_flags(proxy, opcode, interface, version, flags, args);
    if (flush_proxy && real_get_display && real_display_flush) {
        struct wl_display *d = real_get_display(flush_proxy);
        if (d)
            real_display_flush(d);
    }
    inspect_after_call(proxy, opcode, args, ret);
    return ret;
}

struct wl_proxy *wl_proxy_marshal_flags(struct wl_proxy *proxy, uint32_t opcode,
                                        const struct wl_interface *interface,
                                        uint32_t version, uint32_t flags, ...)
{
    union wl_argument args[20];
    va_list ap;
    resolve_wl_syms();
    memset(args, 0, sizeof(args));
    va_start(ap, flags);
    parse_va_args(proxy, opcode, args, 20, ap);
    va_end(ap);
    return wl_proxy_marshal_array_flags(proxy, opcode, interface, version, flags, args);
}

void wl_proxy_marshal_array(struct wl_proxy *proxy, uint32_t opcode, union wl_argument *args)
{
    struct wl_proxy *flush_proxy = NULL;
    resolve_wl_syms();
    inspect_before_call(proxy, opcode, args, &flush_proxy);
    real_marshal_array(proxy, opcode, args);
    if (flush_proxy && real_get_display && real_display_flush) {
        struct wl_display *d = real_get_display(flush_proxy);
        if (d)
            real_display_flush(d);
    }
}

void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...)
{
    union wl_argument args[20];
    va_list ap;
    resolve_wl_syms();
    memset(args, 0, sizeof(args));
    va_start(ap, opcode);
    parse_va_args(proxy, opcode, args, 20, ap);
    va_end(ap);
    wl_proxy_marshal_array(proxy, opcode, args);
}

struct wl_proxy *wl_proxy_marshal_array_constructor(struct wl_proxy *proxy, uint32_t opcode,
                                                    union wl_argument *args,
                                                    const struct wl_interface *interface)
{
    resolve_wl_syms();
    struct wl_proxy *ret = real_marshal_array_ctor(proxy, opcode, args, interface);
    inspect_after_call(proxy, opcode, args, ret);
    return ret;
}

struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *proxy, uint32_t opcode,
                                              const struct wl_interface *interface, ...)
{
    union wl_argument args[20];
    va_list ap;
    resolve_wl_syms();
    memset(args, 0, sizeof(args));
    va_start(ap, interface);
    parse_va_args(proxy, opcode, args, 20, ap);
    va_end(ap);
    return wl_proxy_marshal_array_constructor(proxy, opcode, args, interface);
}

struct wl_proxy *wl_proxy_marshal_array_constructor_versioned(struct wl_proxy *proxy, uint32_t opcode,
                                                              union wl_argument *args,
                                                              const struct wl_interface *interface,
                                                              uint32_t version)
{
    resolve_wl_syms();
    struct wl_proxy *ret = real_marshal_array_ctor_ver(proxy, opcode, args, interface, version);
    inspect_after_call(proxy, opcode, args, ret);
    return ret;
}

struct wl_proxy *wl_proxy_marshal_constructor_versioned(struct wl_proxy *proxy, uint32_t opcode,
                                                        const struct wl_interface *interface,
                                                        uint32_t version, ...)
{
    union wl_argument args[20];
    va_list ap;
    resolve_wl_syms();
    memset(args, 0, sizeof(args));
    va_start(ap, version);
    parse_va_args(proxy, opcode, args, 20, ap);
    va_end(ap);
    return wl_proxy_marshal_array_constructor_versioned(proxy, opcode, args, interface, version);
}

void *dlsym(void *handle, const char *name)
{
    fn_dlsym_t rdlsym = get_real_dlsym();
    if (!rdlsym)
        return NULL;
    void *real = rdlsym(handle, name);
    if (!real || !g_gate_armed || g_gate_released)
        return real;
    if (strncmp(name, "wl_proxy_marshal", 16) != 0)
        return real;
    if (handle && handle != RTLD_DEFAULT && handle != RTLD_NEXT)
        __atomic_store_n(&g_wl_dlopen_handle, handle, __ATOMIC_RELEASE);
    if (strcmp(name, "wl_proxy_marshal_array_flags") == 0)
        return (void *)wl_proxy_marshal_array_flags;
    if (strcmp(name, "wl_proxy_marshal_flags") == 0)
        return (void *)wl_proxy_marshal_flags;
    if (strcmp(name, "wl_proxy_marshal_array") == 0)
        return (void *)wl_proxy_marshal_array;
    if (strcmp(name, "wl_proxy_marshal") == 0)
        return (void *)wl_proxy_marshal;
    if (strcmp(name, "wl_proxy_marshal_array_constructor") == 0)
        return (void *)wl_proxy_marshal_array_constructor;
    if (strcmp(name, "wl_proxy_marshal_constructor") == 0)
        return (void *)wl_proxy_marshal_constructor;
    if (strcmp(name, "wl_proxy_marshal_array_constructor_versioned") == 0)
        return (void *)wl_proxy_marshal_array_constructor_versioned;
    if (strcmp(name, "wl_proxy_marshal_constructor_versioned") == 0)
        return (void *)wl_proxy_marshal_constructor_versioned;
    return real;
}

#elif defined(BUILD_FASTLAUNCH)

static const char *runtime_dir(char *buf, size_t sz)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) {
        snprintf(buf, sz, "%s", rt);
        return buf;
    }
    snprintf(buf, sz, "/run/user/%u", (unsigned)getuid());
    return buf;
}

static char ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static int is_valid_hex_addr(const char *s)
{
    size_t n = 0;
    if (!s || !*s)
        return 0;
    for (; s[n]; n++) {
        char c = s[n];
        int is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!is_hex || n >= 32)
            return 0;
    }
    return 1;
}

static int normalize_pwa_url(const char *raw, char *out, size_t out_sz)
{
    if (!raw || !out || out_sz < 16)
        return -1;
    while (*raw == ' ' || *raw == '\t')
        raw++;
    const char *scheme = NULL;
    size_t skip = 0;
    if (!strncasecmp(raw, "https://", 8)) {
        scheme = "https://";
        skip = 8;
    } else if (!strncasecmp(raw, "http://", 7)) {
        scheme = "http://";
        skip = 7;
    } else {
        return -1;
    }
    size_t pos = (size_t)snprintf(out, out_sz, "%s", scheme);
    const char *p = raw + skip;
    size_t auth_start = pos;
    while (*p && *p != '/' && *p != '?' && *p != '#') {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            return -1;
        if (pos + 2 >= out_sz)
            return -1;
        out[pos++] = ascii_lower(*p++);
    }
    if (pos == auth_start)
        return -1;
    if (*p != '/') {
        if (pos + 2 >= out_sz)
            return -1;
        out[pos++] = '/';
    }
    while (*p && *p != '#') {
        if (pos + 1 >= out_sz)
            return -1;
        out[pos++] = *p++;
    }
    while (pos > 0 && (out[pos - 1] == ' ' || out[pos - 1] == '\t'))
        pos--;
    out[pos] = '\0';
    return 0;
}

static int hypr_send_req(const char *ctl_sock, const char *cmd, int require_ok)
{
    int fd = connect_unix_uid(ctl_sock);
    if (fd < 0)
        return -1;
    if (write_all(fd, cmd, strlen(cmd)) != 0) {
        close(fd);
        return -1;
    }
    char resp[128];
    ssize_t n = 0;
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 1500) > 0)
        n = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    resp[n] = '\0';
    if (require_ok && strncmp(resp, "ok", 2) != 0)
        return -1;
    return 0;
}

static int reveal_prewarmed_pwa(const char *raw_url)
{
    const char *his = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!his || !*his || strchr(his, '/'))
        return 11;
    char norm[2048];
    if (normalize_pwa_url(raw_url, norm, sizeof(norm)) != 0)
        return 11;

    char rt[256], lock_path[512], tsv_path[512], tmp_path[544], ctl_sock[512];
    runtime_dir(rt, sizeof(rt));
    snprintf(lock_path, sizeof(lock_path), "%s/vdxg-boost/pwa-ready.lock", rt);
    snprintf(tsv_path, sizeof(tsv_path), "%s/vdxg-boost/pwa-ready.tsv", rt);
    if (access(tsv_path, R_OK) != 0)
        return 11;

    int lfd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lfd < 0)
        return 11;
    while (flock(lfd, LOCK_EX) != 0) {
        if (errno != EINTR) {
            close(lfd);
            return 11;
        }
    }

    char matched_addr[64] = "";
    FILE *rf = fopen(tsv_path, "r");
    if (rf) {
        snprintf(tmp_path, sizeof(tmp_path), "%s.tmp.%d", tsv_path, (int)getpid());
        FILE *wf = fopen(tmp_path, "w");
        char line[4096];
        int write_ok = (wf != NULL);
        while (fgets(line, sizeof(line), rf)) {
            size_t len = strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
                line[--len] = '\0';
            if (!len)
                continue;
            char *tab = strchr(line, '\t');
            if (!tab)
                continue;
            *tab = '\0';
            const char *u = line;
            const char *a = tab + 1;
            if (!matched_addr[0] && !strcmp(u, norm) && is_valid_hex_addr(a)) {
                snprintf(matched_addr, sizeof(matched_addr), "%s", a);
                continue;
            }
            if (wf && fprintf(wf, "%s\t%s\n", u, a) < 0)
                write_ok = 0;
        }
        fclose(rf);
        if (wf) {
            if (fflush(wf) != 0)
                write_ok = 0;
            if (fclose(wf) != 0)
                write_ok = 0;
            if (matched_addr[0] && write_ok) {
                if (rename(tmp_path, tsv_path) != 0)
                    unlink(tmp_path);
            } else {
                unlink(tmp_path);
            }
        }
    }
    flock(lfd, LOCK_UN);
    close(lfd);

    if (!matched_addr[0])
        return 11;

    snprintf(ctl_sock, sizeof(ctl_sock), "%s/hypr/%s/.socket.sock", rt, his);
    char move_cmd[256], focus_cmd[256];
    snprintf(move_cmd, sizeof(move_cmd),
             "eval hl.dispatch(hl.dsp.window.move({ workspace = \"e+0\", follow = true, window = \"address:0x%s\" }))",
             matched_addr);
    if (hypr_send_req(ctl_sock, move_cmd, 1) != 0)
        return 11;
    snprintf(focus_cmd, sizeof(focus_cmd),
             "eval hl.dispatch(hl.dsp.window.focus({ window = \"address:0x%s\" }))",
             matched_addr);
    (void)hypr_send_req(ctl_sock, focus_cmd, 0);
    return 0;
}

static int fast_chromium(int argc, char **argv)
{
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char spath[512];
    char cwd[PATH_MAX];
    size_t pos = 0;
    uint8_t msg[8192];

    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (!strncmp(s, "--user-data-dir", 15) || !strncmp(s, "--headless", 10) ||
            !strncmp(s, "--type=", 7) || !strcmp(s, "--version") ||
            !strcmp(s, "--product-version") || !strcmp(s, "--help") || !strcmp(s, "-h") ||
            !strcmp(s, "--diagnostics") || !strncmp(s, "--remote-debugging-", 19) ||
            !strncmp(s, "--dump-dom", 10) || !strncmp(s, "--print-to-pdf", 14))
            return 11;
    }
    if (argc == 2 && !strncmp(argv[1], "--app=", 6) &&
        !getenv("CHROME_USER_DATA_DIR") && !getenv("CHROME_CONFIG_HOME") &&
        reveal_prewarmed_pwa(argv[1] + 6) == 0)
        return 0;
    if (cfg && *cfg)
        snprintf(spath, sizeof(spath), "%s/chromium/SingletonSocket", cfg);
    else if (home && *home)
        snprintf(spath, sizeof(spath), "%s/.config/chromium/SingletonSocket", home);
    else
        return 11;

    int fd = connect_unix_uid(spath);
    if (fd < 0)
        return 11;
    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "/");

    pos = (size_t)snprintf((char *)msg, sizeof(msg), "START%c%s%cchromium", 0, cwd, 0);
    for (int a = 1; a < argc; a++) {
        size_t l = strlen(argv[a]);
        if (pos + 1 + l >= sizeof(msg)) {
            close(fd);
            return 11;
        }
        msg[pos++] = '\0';
        memcpy(msg + pos, argv[a], l);
        pos += l;
    }
    if (write_all(fd, msg, pos) != 0) {
        close(fd);
        return 11;
    }
    shutdown(fd, SHUT_WR);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    char ack[16];
    ssize_t n = 0;
    if (poll(&pfd, 1, 2000) > 0)
        n = read(fd, ack, sizeof(ack));
    close(fd);
    return (n >= 3 && memcmp(ack, "ACK", 3) == 0) ? 0 : 11;
}

static size_t put_uleb(uint8_t *dst, uint32_t v)
{
    size_t n = 0;
    while (v > 0x7fu) {
        dst[n++] = (uint8_t)((v & 0x7fu) | 0x80u);
        v >>= 7;
    }
    dst[n++] = (uint8_t)v;
    return n;
}

static size_t put_vs_str(uint8_t *dst, const char *s)
{
    uint32_t l = (uint32_t)strlen(s);
    size_t k = 0;
    dst[k++] = 0x01;
    k += put_uleb(dst + k, l);
    memcpy(dst + k, s, l);
    return k + l;
}

static size_t put_vs_json(uint8_t *dst, const char *s, size_t l)
{
    size_t k = 0;
    dst[k++] = 0x05;
    k += put_uleb(dst + k, (uint32_t)l);
    memcpy(dst + k, s, l);
    return k + l;
}

static size_t put_vs_u32(uint8_t *dst, uint32_t v)
{
    size_t k = 0;
    dst[k++] = 0x06;
    return k + put_uleb(dst + k, v);
}

struct jbuf {
    char *data;
    size_t len, cap;
};

static int jb_append(struct jbuf *j, const char *s, size_t n)
{
    if (j->len + n + 1 > j->cap) {
        size_t ncap = j->cap ? j->cap * 2 : 1024;
        while (ncap < j->len + n + 1)
            ncap *= 2;
        char *p = realloc(j->data, ncap);
        if (!p)
            return -1;
        j->data = p;
        j->cap = ncap;
    }
    memcpy(j->data + j->len, s, n);
    j->len += n;
    j->data[j->len] = '\0';
    return 0;
}

static int jb_str(struct jbuf *j, const char *s)
{
    if (jb_append(j, "\"", 1) != 0)
        return -1;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = { '\\', (char)*p };
            if (jb_append(j, esc, 2) != 0)
                return -1;
        } else if (*p < 0x20) {
            char esc[7];
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            if (jb_append(j, esc, 6) != 0)
                return -1;
        } else {
            if (jb_append(j, (const char *)p, 1) != 0)
                return -1;
        }
    }
    return jb_append(j, "\"", 1);
}

#define jb_lit(j, s) jb_append((j), (s), sizeof(s) - 1)

static void put_be32(uint8_t *dst, uint32_t v)
{
    dst[0] = (uint8_t)(v >> 24);
    dst[1] = (uint8_t)(v >> 16);
    dst[2] = (uint8_t)(v >> 8);
    dst[3] = (uint8_t)v;
}

static int fast_code(int argc, char **argv)
{
    int new_win = 0, reuse_win = 0, npaths = 0;
    const char *paths[64];
    char resolved[64][PATH_MAX];
    char cwd[PATH_MAX];

    if (!getcwd(cwd, sizeof(cwd)))
        strcpy(cwd, "/");

    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (!strcmp(s, "--new-window") || !strcmp(s, "-n")) {
            new_win = 1;
        } else if (!strcmp(s, "--reuse-window") || !strcmp(s, "-r")) {
            reuse_win = 1;
        } else if (!strncmp(s, "--ozone-platform=", 17) ||
                   !strncmp(s, "--ozone-platform-hint=", 22)) {
            continue;
        } else if (s[0] == '-' || npaths >= 64) {
            return 11;
        } else {
            if (realpath(s, resolved[npaths])) {
                paths[npaths] = resolved[npaths];
            } else {
                snprintf(resolved[npaths], PATH_MAX, "%s/%s", cwd, s);
                paths[npaths] = resolved[npaths];
            }
            npaths++;
        }
    }
    if (npaths == 0 && !reuse_win)
        new_win = 1;

    char rt[256];
    runtime_dir(rt, sizeof(rt));
    DIR *d = opendir(rt);
    if (!d)
        return 11;
    char best_sock[512] = "";
    time_t best_mtime = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        size_t l = strlen(ent->d_name);
        if (l > 17 && !strncmp(ent->d_name, "vscode-", 7) &&
            !strcmp(ent->d_name + l - 10, "-main.sock")) {
            char full[512];
            struct stat st;
            snprintf(full, sizeof(full), "%s/%s", rt, ent->d_name);
            if (stat(full, &st) == 0 && S_ISSOCK(st.st_mode) &&
                st.st_uid == getuid() && st.st_mtime >= best_mtime) {
                best_mtime = st.st_mtime;
                strcpy(best_sock, full);
            }
        }
    }
    closedir(d);
    if (!best_sock[0])
        return 11;

    int fd = connect_unix_uid(best_sock);
    if (fd < 0)
        return 11;

    static const uint8_t handshake[] = {
        0x01, 0,0,0,0, 0,0,0,0, 0,0,0,6,  0x01, 0x04, 'm','a','i','n',
        0x01, 0,0,0,0, 0,0,0,0, 0,0,0,6,  0x04, 0x01, 0x06, 0xc8, 0x01, 0x00
    };
    if (write_all(fd, handshake, sizeof(handshake)) != 0) {
        close(fd);
        return 11;
    }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint8_t rbuf[512];
    if (poll(&pfd, 1, 1000) <= 0 || read(fd, rbuf, sizeof(rbuf)) < 13) {
        close(fd);
        return 11;
    }

    struct jbuf jargs = {0}, jenv = {0};
    jb_append(&jargs, "{\"_\":[", 6);
    for (int i = 0; i < npaths; i++) {
        if (i)
            jb_append(&jargs, ",", 1);
        jb_str(&jargs, paths[i]);
    }
    char flags_tail[128];
    snprintf(flags_tail, sizeof(flags_tail), "],\"new-window\":%s,\"reuse-window\":%s}",
             new_win ? "true" : "false", reuse_win ? "true" : "false");
    jb_append(&jargs, flags_tail, strlen(flags_tail));

    static const char *env_keys[] = {
        "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "DISPLAY", "HYPRLAND_INSTANCE_SIGNATURE"
    };
    jb_append(&jenv, "{\"VSCODE_CLI\":\"1\",\"VSCODE_CWD\":", 31);
    jb_str(&jenv, cwd);
    for (size_t k = 0; k < sizeof(env_keys) / sizeof(env_keys[0]); k++) {
        const char *v = getenv(env_keys[k]);
        if (v && *v) {
            jb_append(&jenv, ",\"", 2);
            jb_append(&jenv, env_keys[k], strlen(env_keys[k]));
            jb_append(&jenv, "\":", 2);
            jb_str(&jenv, v);
        }
    }
    jb_append(&jenv, "}", 1);

    size_t pay_cap = jargs.len + jenv.len + 256;
    uint8_t *pkt = malloc(13 + pay_cap);
    if (!pkt) {
        free(jargs.data);
        free(jenv.data);
        close(fd);
        return 11;
    }
    size_t p = 13;
    pkt[p++] = 0x04; pkt[p++] = 4;
    p += put_vs_u32(pkt + p, 100);
    p += put_vs_u32(pkt + p, 0);
    p += put_vs_str(pkt + p, "launch");
    p += put_vs_str(pkt + p, "start");
    pkt[p++] = 0x04; pkt[p++] = 2;
    p += put_vs_json(pkt + p, jargs.data, jargs.len);
    p += put_vs_json(pkt + p, jenv.data, jenv.len);
    pkt[0] = 0x01;
    put_be32(pkt + 1, 2);
    put_be32(pkt + 5, 1);
    put_be32(pkt + 9, (uint32_t)(p - 13));

    int ok = (write_all(fd, pkt, p) == 0 && poll(&pfd, 1, 2500) > 0);
    if (ok) {
        ssize_t n = read(fd, rbuf, sizeof(rbuf));
        ok = (n >= 19 && rbuf[0] == 0x01);
    }
    free(pkt);
    free(jargs.data);
    free(jenv.data);
    close(fd);
    return ok ? 0 : 11;
}

static int run_and_wait_status(const char *prog, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return 11;
    if (pid == 0) {
        execv(prog, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return 11;
    }
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 11;
}

static int fast_foot(int argc, char **argv)
{
    char rt[256], spath[512];
    const char *wd = getenv("WAYLAND_DISPLAY");
    if (!wd || !*wd)
        wd = "wayland-0";
    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (!strcmp(s, "-s") || !strcmp(s, "--server") ||
            !strcmp(s, "-v") || !strcmp(s, "--version") ||
            !strcmp(s, "-h") || !strcmp(s, "--help") || !strcmp(s, "-C"))
            return 11;
    }
    runtime_dir(rt, sizeof(rt));
    snprintf(spath, sizeof(spath), "%s/foot-%s.sock", rt, wd);
    struct stat st;
    if (stat(spath, &st) != 0 || !S_ISSOCK(st.st_mode) || st.st_uid != getuid())
        return 11;
    char **nargv = calloc((size_t)argc + 1, sizeof(char *));
    if (!nargv)
        return 11;
    nargv[0] = (char *)"/usr/bin/footclient";
    for (int i = 1; i < argc; i++)
        nargv[i] = argv[i];
    execv("/usr/bin/footclient", nargv);
    free(nargv);
    return 11;
}

static int fast_alacritty(int argc, char **argv)
{
    char rt[256], spath[512];
    const char *workdir = NULL;

    for (int a = 1; a < argc; a++) {
        const char *s = argv[a];
        if (!strncmp(s, "--working-directory=", 20)) {
            workdir = s + 20;
        } else if (!strcmp(s, "--working-directory") && a + 1 < argc) {
            workdir = argv[++a];
        } else {
            return 11;
        }
    }
    runtime_dir(rt, sizeof(rt));
    snprintf(spath, sizeof(spath), "%s/vdxg-boost/alacritty.sock", rt);
    int fd = connect_unix_uid(spath);
    if (fd < 0)
        return 11;

    struct jbuf jb = {0};
    if (jb_lit(&jb, "{\"CreateWindow\":{\"terminal_options\":{\"working_directory\":") != 0 ||
        (workdir ? jb_str(&jb, workdir) : jb_lit(&jb, "null")) != 0 ||
        jb_lit(&jb, ",\"hold\":false,\"command\":[]},\"window_identity\":{\"title\":null,\"class\":null},\"activation_token\":null,\"option\":[]}}") != 0) {
        free(jb.data);
        close(fd);
        return 11;
    }
    int wr = write_all(fd, jb.data, jb.len);
    free(jb.data);
    if (wr != 0) {
        close(fd);
        return 11;
    }
    shutdown(fd, SHUT_WR);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 500) > 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf));
        (void)n;
    }
    close(fd);
    return 0;
}

static int fast_ghostty(int argc, char **argv)
{
    char rt[256], ready[512];
    if (argc != 1)
        return 11;
    (void)argv;
    runtime_dir(rt, sizeof(rt));
    snprintf(ready, sizeof(ready), "%s/vdxg-boost/ghostty.ready", rt);
    if (access(ready, F_OK) != 0)
        return 11;
    char *nargv[] = {
        (char *)"/usr/bin/ghostty", (char *)"+new-window", NULL
    };
    return run_and_wait_status("/usr/bin/ghostty", nargv);
}

static int fast_nautilus(int argc, char **argv)
{
    char rt[256], ready[512];
    if (argc > 2 || (argc == 2 && strcmp(argv[1], "--new-window") != 0))
        return 11;
    runtime_dir(rt, sizeof(rt));
    snprintf(ready, sizeof(ready), "%s/vdxg-boost/files.ready", rt);
    if (access(ready, F_OK) != 0)
        return 11;
    char *nargv[] = {
        (char *)"/usr/bin/nautilus", (char *)"--new-window", NULL
    };
    return run_and_wait_status("/usr/bin/nautilus", nargv);
}

static int mimeapps_chromium_default(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    int in_sec = 0;
    int res = -1;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';
        if (line[0] == '[') {
            in_sec = !strcmp(line, "[Default Applications]");
            continue;
        }
        if (!in_sec)
            continue;
        const char *val = NULL;
        if (!strncmp(line, "x-scheme-handler/https=", 23))
            val = line + 23;
        else if (!strncmp(line, "x-scheme-handler/http=", 22))
            val = line + 22;
        if (!val)
            continue;
        char copy[1024];
        snprintf(copy, sizeof(copy), "%s", val);
        char *saveptr = NULL;
        for (char *tok = strtok_r(copy, ";", &saveptr); tok; tok = strtok_r(NULL, ";", &saveptr)) {
            if (!*tok || !strcmp(tok, "omarchy-browser.desktop"))
                continue;
            res = !strcmp(tok, "chromium.desktop") ? 1 : 0;
            fclose(f);
            return res;
        }
    }
    fclose(f);
    return res;
}

static int is_chromium_default_browser(void)
{
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char cdir[512];
    if (cfg && *cfg)
        snprintf(cdir, sizeof(cdir), "%s", cfg);
    else if (home && *home)
        snprintf(cdir, sizeof(cdir), "%s/.config", home);
    else
        return 1;
    const char *files[] = { "hyprland-mimeapps.list", "mimeapps.list" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", cdir, files[i]);
        int r = mimeapps_chromium_default(p);
        if (r >= 0)
            return r;
    }
    return 1;
}

static int fast_webapp(int argc, char **argv)
{
    if (argc < 2 || argc >= 60 || getenv("CHROME_USER_DATA_DIR") ||
        getenv("CHROME_CONFIG_HOME") || !is_chromium_default_browser())
        return 11;
    char app_arg[4096];
    if (strlen(argv[1]) + 7 >= sizeof(app_arg))
        return 11;
    snprintf(app_arg, sizeof(app_arg), "--app=%s", argv[1]);
    char *nargv[64];
    int k = 0;
    nargv[k++] = (char *)"chromium";
    nargv[k++] = app_arg;
    for (int a = 2; a < argc; a++)
        nargv[k++] = argv[a];
    nargv[k] = NULL;
    return fast_chromium(k, nargv);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int fast_browser_handoff(int argc, char **argv)
{
    if (argc < 2 || getenv("CHROME_USER_DATA_DIR") || getenv("CHROME_CONFIG_HOME"))
        return 11;
    if (strcmp(base_name(argv[1]), "chromium") != 0)
        return 11;
    return fast_chromium(argc - 1, argv + 1);
}

static int fast_launch_browser(int argc, char **argv)
{
    if (argc >= 60 || getenv("CHROME_USER_DATA_DIR") ||
        getenv("CHROME_CONFIG_HOME") || !is_chromium_default_browser())
        return 11;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--private"))
            return 11;
    }
    if (argc == 1) {
        char *nargv[] = { (char *)"chromium", (char *)"--new-window", NULL };
        return fast_chromium(2, nargv);
    }
    return fast_chromium(argc, argv);
}

static int read_default_terminal(char *out, size_t sz)
{
    const char *cfg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    char cdir[512];
    if (cfg && *cfg)
        snprintf(cdir, sizeof(cdir), "%s", cfg);
    else if (home && *home)
        snprintf(cdir, sizeof(cdir), "%s/.config", home);
    else
        return -1;
    const char *files[] = { "hypr-xdg-terminals.list", "xdg-terminals.list" };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char p[600];
        snprintf(p, sizeof(p), "%s/%s", cdir, files[i]);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *s = line;
            while (*s == ' ' || *s == '\t')
                s++;
            size_t l = strlen(s);
            while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t'))
                s[--l] = '\0';
            if (!*s || *s == '#')
                continue;
            snprintf(out, sz, "%s", s);
            fclose(f);
            return 0;
        }
        fclose(f);
    }
    return -1;
}

static int fast_terminal_exec(int argc, char **argv, int from_omarchy_helper)
{
    const char *dir = NULL;
    if (from_omarchy_helper) {
        if (argc != 1)
            return 11;
    } else {
        for (int a = 1; a < argc; a++) {
            if (!strncmp(argv[a], "--dir=", 6))
                dir = argv[a] + 6;
            else
                return 11;
        }
    }
    char term[256] = "";
    if (read_default_terminal(term, sizeof(term)) != 0)
        return 11;
    if (!strcasecmp(term, "Alacritty.desktop") || !strcasecmp(term, "alacritty.desktop")) {
        if (dir) {
            char arg[PATH_MAX + 32];
            snprintf(arg, sizeof(arg), "--working-directory=%s", dir);
            char *nargv[] = { (char *)"alacritty", arg, NULL };
            return fast_alacritty(2, nargv);
        }
        char *nargv[] = { (char *)"alacritty", NULL };
        return fast_alacritty(1, nargv);
    }
    if (!strcmp(term, "foot.desktop") || !strcmp(term, "footclient.desktop")) {
        if (dir) {
            char *nargv[] = { (char *)"foot", (char *)"-D", (char *)dir, NULL };
            return fast_foot(3, nargv);
        }
        char *nargv[] = { (char *)"foot", NULL };
        return fast_foot(1, nargv);
    }
    if (!strcmp(term, "com.mitchellh.ghostty.desktop") || !strcmp(term, "ghostty.desktop")) {
        if (!dir) {
            char *nargv[] = { (char *)"ghostty", NULL };
            return fast_ghostty(1, nargv);
        }
    }
    return 11;
}

static volatile sig_atomic_t g_got_sig = 0;
static void on_term_sig(int sig) { g_got_sig = sig; }

static int fast_wl_gate(const char *sock_name, int detached)
{
    char rt[256], spath[512], lpath[530], okbuf[8];
    runtime_dir(rt, sizeof(rt));
    snprintf(spath, sizeof(spath), "%s/vdxg-boost/%s", rt, sock_name);
    snprintf(lpath, sizeof(lpath), "%s.lock", spath);
    int fd = connect_unix_uid(spath);
    if (fd < 0 && access(lpath, F_OK) == 0) {
        for (int tries = 0; tries < 36 && fd < 0; tries++) {
            usleep(5000);
            fd = connect_unix_uid(spath);
        }
    }
    if (fd < 0)
        return 11;
    if (write_all(fd, "G", 1) != 0) {
        close(fd);
        return 11;
    }
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    if (poll(&pfd, 1, 1500) <= 0 || read(fd, okbuf, 3) < 2 || memcmp(okbuf, "OK", 2) != 0) {
        close(fd);
        return 11;
    }
    if (detached) {
        close(fd);
        return 0;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term_sig;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    for (;;) {
        if (g_got_sig) {
            (void)write_all(fd, "Q", 1);
            break;
        }
        int r = poll(&pfd, 1, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pfd.revents & (POLLHUP | POLLERR))
            break;
        if (pfd.revents & POLLIN) {
            char tmp[16];
            if (read(fd, tmp, sizeof(tmp)) <= 0)
                break;
        }
    }
    close(fd);
    return 0;
}

static int dispatch_known_app(int argc, char **argv, int detached)
{
    if (argc < 1 || !argv[0])
        return 11;
    const char *app = base_name(argv[0]);

    if (!strcmp(app, "chromium"))
        return fast_chromium(argc, argv);
    if (!strcmp(app, "code"))
        return fast_code(argc, argv);
    if (!strcmp(app, "foot"))
        return fast_foot(argc, argv);
    if (!strcmp(app, "alacritty"))
        return fast_alacritty(argc, argv);
    if (!strcmp(app, "ghostty"))
        return fast_ghostty(argc, argv);
    if (!strcmp(app, "nautilus"))
        return detached ? fast_nautilus(argc, argv) : 11;
    if (!strcmp(app, "omacalc") && argc == 1)
        return fast_wl_gate("gate-omacalc.sock", detached);
    if (!strcmp(app, "gnome-calculator") && argc == 1)
        return fast_wl_gate("gate-gnome-calculator.sock", detached);
    if (!strcmp(app, "vkcube") && argc == 1)
        return fast_wl_gate("gate-vkcube.sock", detached);
    if (!strcmp(app, "blender")) {
        if (argc == 1)
            return fast_wl_gate("gate-blender.sock", detached);
        if (argc == 2 && !strcmp(argv[1], "--factory-startup"))
            return fast_wl_gate("gate-blender-factory.sock", detached);
    }
    if (!strcmp(app, "omarchy-launch-webapp"))
        return fast_webapp(argc, argv);
    if (!strcmp(app, "omarchy-cmd-browser-handoff"))
        return fast_browser_handoff(argc, argv);
    if (!strcmp(app, "omarchy-launch-browser"))
        return fast_launch_browser(argc, argv);
    if (!strcmp(app, "omarchy-launch-nautilus") && argc == 1)
        return fast_nautilus(argc, argv);
    if (!strcmp(app, "omarchy-launch-terminal"))
        return fast_terminal_exec(argc, argv, 1);
    if (!strcmp(app, "xdg-terminal-exec"))
        return fast_terminal_exec(argc, argv, 0);
    return 11;
}

static int find_desktop_file(const char *id, char *out, size_t out_sz)
{
    char clean[256];
    size_t len = strlen(id);
    if (!len || len >= sizeof(clean) - 16 || strchr(id, '/'))
        return -1;
    if (len > 8 && !strcmp(id + len - 8, ".desktop"))
        snprintf(clean, sizeof(clean), "%s", id);
    else
        snprintf(clean, sizeof(clean), "%s.desktop", id);

    const char *xdh = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char user_dir[512] = "";
    if (xdh && *xdh)
        snprintf(user_dir, sizeof(user_dir), "%s/applications", xdh);
    else if (home && *home)
        snprintf(user_dir, sizeof(user_dir), "%s/.local/share/applications", home);

    const char *dirs[4] = {
        user_dir[0] ? user_dir : NULL,
        "/run/virtio-dxg/applications",
        "/usr/local/share/applications",
        "/usr/share/applications",
    };
    for (int i = 0; i < 4; i++) {
        if (!dirs[i])
            continue;
        snprintf(out, out_sz, "%s/%s", dirs[i], clean);
        if (access(out, R_OK) == 0)
            return 0;
    }
    return -1;
}

static int parse_exec_tokens(const char *raw, char *buf, size_t bsz, char **toks, int max_toks)
{
    int nt = 0;
    size_t bo = 0;
    const char *p = raw;
    while (*p) {
        while (*p == ' ' || *p == '\t')
            p++;
        if (!*p)
            break;
        if (nt >= max_toks - 1)
            return -1;
        char *start = buf + bo;
        char q = 0;
        while (*p && (q || (*p != ' ' && *p != '\t'))) {
            if (*p == '\'' || *p == '"') {
                if (!q) {
                    q = *p++;
                    continue;
                } else if (*p == q) {
                    q = 0;
                    p++;
                    continue;
                }
            }
            if (!q && strchr("|&;<>()$`*?~", *p))
                return -1;
            if (*p == '\\' && p[1] && q != '\'')
                p++;
            if (bo + 2 >= bsz)
                return -1;
            buf[bo++] = *p++;
        }
        if (q)
            return -1;
        buf[bo++] = '\0';
        if (start[0] == '%' && start[1] && !start[2])
            continue;
        if (nt == 0 && strchr(start, '='))
            return -1;
        toks[nt++] = start;
    }
    toks[nt] = NULL;
    return nt;
}

static void launch_via_systemd_run(int argc, char **argv)
{
    if (argc < 1 || argc > 60)
        return;
    char *sargv[72];
    int k = 0;
    sargv[k++] = (char *)"/usr/bin/systemd-run";
    sargv[k++] = (char *)"--user";
    sargv[k++] = (char *)"--no-block";
    sargv[k++] = (char *)"--quiet";
    sargv[k++] = (char *)"--collect";
    sargv[k++] = (char *)"--slice=app-graphical.slice";
    sargv[k++] = (char *)"--";
    for (int i = 0; i < argc; i++)
        sargv[k++] = argv[i];
    sargv[k] = NULL;
    execv("/usr/bin/systemd-run", sargv);
}

static int fast_gtk_launch(int argc, char **argv, int from_uwsm)
{
    if (argc != 2 || !argv[1][0] || argv[1][0] == '-')
        return 11;
    char dpath[600];
    if (find_desktop_file(argv[1], dpath, sizeof(dpath)) != 0)
        return 11;
    FILE *f = fopen(dpath, "r");
    if (!f)
        return 11;
    char line[4096], exec_raw[4096] = "";
    int in_entry = 0, is_term = 0, is_dbus = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';
        if (line[0] == '[') {
            if (in_entry)
                break;
            in_entry = !strcmp(line, "[Desktop Entry]");
            continue;
        }
        if (!in_entry)
            continue;
        if (!strcmp(line, "Terminal=true"))
            is_term = 1;
        else if (!strcmp(line, "DBusActivatable=true"))
            is_dbus = 1;
        else if (!strncmp(line, "Exec=", 5))
            snprintf(exec_raw, sizeof(exec_raw), "%s", line + 5);
    }
    fclose(f);
    if (is_term || !exec_raw[0])
        return 11;
    char tokbuf[4096];
    char *toks[64];
    int ntoks = parse_exec_tokens(exec_raw, tokbuf, sizeof(tokbuf), toks, 64);
    if (ntoks <= 0)
        return 11;
    if (dispatch_known_app(ntoks, toks, 1) == 0)
        return 0;
    if (from_uwsm && !is_dbus) {
        launch_via_systemd_run(ntoks, toks);
        return 11;
    }
    return 11;
}

static int fast_uwsm_app(int argc, char **argv)
{
    if (argc < 3 || strcmp(argv[1], "--") != 0 || !argv[2][0] || argv[2][0] == '-')
        return 11;
    if (!strcmp(base_name(argv[2]), "gtk-launch"))
        return fast_gtk_launch(argc - 2, argv + 2, 1);
    if (dispatch_known_app(argc - 2, argv + 2, 1) == 0)
        return 0;
    launch_via_systemd_run(argc - 2, argv + 2);
    return 11;
}

int main(int argc, char **argv)
{
    const char *boost = getenv("VIRTIO_DXG_BOOST");
    if (boost && !strcmp(boost, "0"))
        return 11;
    if (argc < 2)
        return 11;
    const char *app = argv[1];
    char **app_argv = argv + 1;
    int app_argc = argc - 1;

    if (!strcmp(app, "uwsm-app"))
        return fast_uwsm_app(app_argc, app_argv);
    if (!strcmp(app, "gtk-launch"))
        return fast_gtk_launch(app_argc, app_argv, 0);
    return dispatch_known_app(app_argc, app_argv, 0);
}

#endif
