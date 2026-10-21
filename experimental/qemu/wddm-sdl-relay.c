/* SPDX-License-Identifier: MIT */
/* Host-controlled GPU relay into QEMU's existing SDL/OpenGL console. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/error-report.h"
#include "hw/core/qdev.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "ui/console.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include <epoxy/gl.h>
#include <glib/gstdio.h>
#include "wddm-sdl-command.h"

#define TYPE_WDDM_SDL_RELAY "wddm-sdl-relay"
OBJECT_DECLARE_SIMPLE_TYPE(WddmSdlRelay, WDDM_SDL_RELAY)

struct WddmSdlRelay {
    DeviceState parent_obj;
    QemuConsole *con;
    QEMUGLContext import_context;
    char *session, *expected_luid, *audit_directory, *source_format;
    char vendor[128], renderer[256], version[128], luid[17];
    char *audit_error;
    uint32_t width, height, audit_width, audit_height;
    uint64_t generation, last_frame, pending_frame, audit_frames, converted_frames;
    uint64_t source_fnv, window_fnv;
    GLuint memory, source_texture, scanout_texture, producer, consumer, channel_program;
    bool attached, ever_attached, poisoned, pending_audit, audit_called, root_verified, bgra;
    Error *migration_blocker;
};

static bool relay_gl_ok(Error **errp, const char *operation)
{
    GLenum code = glGetError();
    if (code != GL_NO_ERROR) {
        error_setg(errp, "SDL GPU relay %s failed (GL error 0x%x)", operation, code);
        return false;
    }
    return true;
}

static bool relay_adapter(WddmSdlRelay *s, Error **errp)
{
    static const char hex[] = "0123456789abcdef";
    GLubyte bytes[8];
    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    g_strlcpy(s->vendor, vendor ? vendor : "", sizeof s->vendor);
    g_strlcpy(s->renderer, renderer ? renderer : "", sizeof s->renderer);
    g_strlcpy(s->version, version ? version : "", sizeof s->version);
    if (!vendor || !strstr(vendor, "NVIDIA") ||
        !epoxy_has_gl_extension("GL_EXT_memory_object") ||
        !epoxy_has_gl_extension("GL_EXT_memory_object_win32") ||
        !epoxy_has_gl_extension("GL_EXT_semaphore") ||
        !epoxy_has_gl_extension("GL_EXT_semaphore_win32")) {
        error_setg(errp, "SDL GPU relay requires NVIDIA Win32 memory/semaphore extensions; got %s / %s",
                   s->vendor, s->renderer);
        return false;
    }
    glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, bytes);
    if (!relay_gl_ok(errp, "adapter LUID")) {
        return false;
    }
    for (unsigned i = 0; i < sizeof bytes; ++i) {
        s->luid[i * 2] = hex[bytes[i] >> 4];
        s->luid[i * 2 + 1] = hex[bytes[i] & 15];
    }
    s->luid[16] = 0;
    if (strcmp(s->luid, s->expected_luid)) {
        error_setg(errp, "SDL GPU relay adapter LUID differs from the owning D3D12 producer");
        return false;
    }
    return true;
}

static void relay_delete_objects(WddmSdlRelay *s)
{
    if (s->channel_program) {
        glDeleteProgram(s->channel_program);
        s->channel_program = 0;
    }
    if (s->source_texture) {
        glDeleteTextures(1, &s->source_texture);
    }
    if (s->scanout_texture) {
        glDeleteTextures(1, &s->scanout_texture);
    }
    if (s->memory) {
        glDeleteMemoryObjectsEXT(1, &s->memory);
    }
    if (s->producer) {
        glDeleteSemaphoresEXT(1, &s->producer);
    }
    if (s->consumer) {
        glDeleteSemaphoresEXT(1, &s->consumer);
    }
    s->source_texture = s->scanout_texture = s->memory = 0;
    s->producer = s->consumer = 0;
}

static bool relay_channel_program(WddmSdlRelay *s, Error **errp)
{
    const GLchar *text =
        "#version 430 core\n"
        "layout(local_size_x=16,local_size_y=16) in;\n"
        "layout(rgba8,binding=0) readonly uniform image2D sourceImage;\n"
        "layout(rgba8,binding=1) writeonly uniform image2D targetImage;\n"
        "void main() {\n"
        "  ivec2 p=ivec2(gl_GlobalInvocationID.xy);\n"
        "  if(any(greaterThanEqual(p,imageSize(targetImage)))) return;\n"
        "  imageStore(targetImage,p,imageLoad(sourceImage,p).bgra);\n"
        "}\n";
    GLuint shader;
    GLint status = GL_FALSE;
    char log[1024] = {0};
    if (!s->bgra) {
        return true;
    }
    shader = glCreateShader(GL_COMPUTE_SHADER);
    if (!shader) {
        error_setg(errp, "Cannot create the fixed GPU channel-conversion shader");
        return false;
    }
    glShaderSource(shader, 1, &text, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        glGetShaderInfoLog(shader, sizeof log, NULL, log);
        glDeleteShader(shader);
        error_setg(errp, "GPU channel-conversion compilation failed: %s", log);
        return false;
    }
    s->channel_program = glCreateProgram();
    if (!s->channel_program) {
        glDeleteShader(shader);
        error_setg(errp, "Cannot create the GPU channel-conversion program");
        return false;
    }
    glAttachShader(s->channel_program, shader);
    glLinkProgram(s->channel_program);
    glDeleteShader(shader);
    status = GL_FALSE;
    glGetProgramiv(s->channel_program, GL_LINK_STATUS, &status);
    if (!status) {
        glGetProgramInfoLog(s->channel_program, sizeof log, NULL, log);
        error_setg(errp, "GPU channel-conversion link failed: %s", log);
        return false;
    }
    return relay_gl_ok(errp, "GPU channel-conversion program");
}

static bool relay_attach(WddmSdlRelay *s, Error **errp)
{
    QEMUGLParams params = { .major_ver = 4, .minor_ver = 5 };
    GLint dedicated = GL_TRUE;
    const unsigned char black[4] = {0, 0, 0, 255};
    GLsync initialized = NULL;
    g_autofree char *prefix = NULL;
    g_autofree char *name = NULL;
    g_autofree gunichar2 *memory_name = NULL;
    g_autofree gunichar2 *producer_name = NULL;
    g_autofree gunichar2 *consumer_name = NULL;

    if (!console_has_gl(s->con) || s->ever_attached || s->import_context) {
        error_setg(errp, "SDL GPU relay needs an initialized GL frontend and a fresh device/session");
        return false;
    }
    s->import_context = dpy_gl_ctx_create(s->con, &params);
    if (!s->import_context ||
        dpy_gl_ctx_make_current(s->con, s->import_context) ||
        !relay_adapter(s, errp)) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create/activate the SDL shared import context");
        }
        goto fail;
    }
    prefix = g_strdup_printf("Local\\TryOmarchy-SDL-%s-%016" PRIx64, s->session, s->generation);
    name = g_strconcat(prefix, "-texture", NULL);
    memory_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    g_clear_pointer(&name, g_free);
    name = g_strconcat(prefix, "-producer", NULL);
    producer_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    g_clear_pointer(&name, g_free);
    name = g_strconcat(prefix, "-consumer", NULL);
    consumer_name = g_utf8_to_utf16(name, -1, NULL, NULL, NULL);
    if (!memory_name || !producer_name || !consumer_name) {
        error_setg(errp, "Cannot encode the bounded SDL GPU object names");
        goto fail;
    }
    glCreateMemoryObjectsEXT(1, &s->memory);
    glMemoryObjectParameterivEXT(s->memory, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    glImportMemoryWin32NameEXT(s->memory, 0, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, memory_name);
    glGenTextures(1, &s->source_texture);
    glBindTexture(GL_TEXTURE_2D, s->source_texture);
    glTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, s->width, s->height, s->memory, 0);
    glGenSemaphoresEXT(1, &s->producer);
    glGenSemaphoresEXT(1, &s->consumer);
    glImportSemaphoreWin32NameEXT(s->producer, GL_HANDLE_TYPE_D3D12_FENCE_EXT, producer_name);
    glImportSemaphoreWin32NameEXT(s->consumer, GL_HANDLE_TYPE_D3D12_FENCE_EXT, consumer_name);
    if (!relay_gl_ok(errp, "named GPU imports")) {
        goto fail;
    }
    glGenTextures(1, &s->scanout_texture);
    glBindTexture(GL_TEXTURE_2D, s->scanout_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, s->width, s->height);
    if (!relay_channel_program(s, errp)) {
        goto fail;
    }
    glClearTexImage(s->scanout_texture, 0, GL_RGBA, GL_UNSIGNED_BYTE, black);
    initialized = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!initialized || !relay_gl_ok(errp, "persistent GPU scanout allocation")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot fence the initial SDL GPU clear");
        }
        goto fail;
    }
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    glWaitSync(initialized, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(initialized);
    initialized = NULL;
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    if (!relay_adapter(s, errp) || !relay_gl_ok(errp, "SDL root texture binding")) {
        dpy_gl_scanout_disable(s->con);
        goto fail;
    }
    s->attached = s->ever_attached = true;
    s->root_verified = true;
    return true;
fail:
    if (s->import_context) {
        if (!dpy_gl_ctx_make_current(s->con, s->import_context)) {
            if (initialized) {
                glDeleteSync(initialized);
            }
            relay_delete_objects(s);
        }
        dpy_gl_ctx_destroy(s->con, s->import_context);
        s->import_context = NULL;
    }
    return false;
}

static uint64_t relay_fnv(const unsigned char *bytes, size_t count)
{
    uint64_t value = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < count; ++i) {
        value = (value ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return value;
}

static bool relay_save(WddmSdlRelay *s, const char *kind, const void *bytes, size_t count)
{
    g_autofree char *base = g_strdup_printf("relay-%s-%016" PRIu64 ".rgba", kind, s->pending_frame);
    g_autofree char *path = g_build_filename(s->audit_directory, base, NULL);
    FILE *file = g_fopen(path, "wbx");
    bool ok;
    if (!file) {
        return false;
    }
    ok = fwrite(bytes, 1, count, file) == count;
    return fclose(file) == 0 && ok;
}

/* Called by SDL after its actual blit and before SwapWindow, in the root GL context. */
static void relay_scanout_audit(void *opaque, uint32_t width, uint32_t height)
{
    WddmSdlRelay *s = opaque;
    GLint read_fb, read_buffer, texture, pack_buffer;
    GLint packing[4];
    const GLenum parameters[] = {GL_PACK_ALIGNMENT, GL_PACK_ROW_LENGTH, GL_PACK_SKIP_PIXELS, GL_PACK_SKIP_ROWS};
    size_t source_bytes = (size_t)s->width * s->height * 4;
    size_t window_bytes = (size_t)width * height * 4;
    g_autofree unsigned char *source = NULL;
    g_autofree unsigned char *window = NULL;
    bool saved_source, saved_window;
    if (!s->pending_audit || s->audit_called) {
        return;
    }
    s->audit_called = true;
    if (!width || !height || width > 3840 || height > 2160) {
        s->audit_error = g_strdup("SDL audit drawable is outside the bounded 4K extent");
        return;
    }
    source = g_malloc0(source_bytes);
    window = g_malloc0(window_bytes);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fb);
    glGetIntegerv(GL_READ_BUFFER, &read_buffer);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
    for (unsigned i = 0; i < G_N_ELEMENTS(parameters); ++i) {
        glGetIntegerv(parameters[i], &packing[i]);
        glPixelStorei(parameters[i], i ? 0 : 1);
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, s->scanout_texture);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, source);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, window);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fb);
    glReadBuffer(read_buffer);
    glBindTexture(GL_TEXTURE_2D, texture);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pack_buffer);
    for (unsigned i = 0; i < G_N_ELEMENTS(parameters); ++i) {
        glPixelStorei(parameters[i], packing[i]);
    }
    /* Retain even rejected pixels; a failed capture must never be overwritten. */
    saved_source = relay_save(s, "source", source, source_bytes);
    saved_window = relay_save(s, "window", window, window_bytes);
    if (glGetError() != GL_NO_ERROR || !saved_source || !saved_window) {
        s->audit_error = g_strdup("SDL complete GPU readback/write failed");
        return;
    }
    s->source_fnv = relay_fnv(source, source_bytes);
    s->window_fnv = relay_fnv(window, window_bytes);
    s->audit_width = width;
    s->audit_height = height;
    ++s->audit_frames;
}

static bool relay_present(WddmSdlRelay *s, uint64_t frame, bool audit, Error **errp)
{
    GLsync root_retired, copy_complete;
    GLuint64 target = frame;
    GLenum layout = GL_LAYOUT_GENERAL_EXT;
    if (!s->attached || s->poisoned || frame != s->last_frame + 1 ||
        (audit && (!s->audit_directory || !g_path_is_absolute(s->audit_directory)))) {
        error_setg(errp, "SDL GPU relay frame is stale, unowned, poisoned or lacks a bounded audit directory");
        return false;
    }
    /*
     * Make the root current and fence ALL its previous work, including spontaneous
     * SDL redraws. Main-loop serialization prevents another redraw during this
     * update. The copy context waits on the GPU before modifying the local image.
     */
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    root_retired = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!root_retired || !relay_gl_ok(errp, "root retirement fence")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create the SDL root retirement fence");
        }
        if (root_retired) {
            glDeleteSync(root_retired);
        }
        s->poisoned = true;
        return false;
    }
    if (dpy_gl_ctx_make_current(s->con, s->import_context)) {
        glDeleteSync(root_retired);
        error_setg(errp, "Cannot activate the SDL import context");
        s->poisoned = true;
        return false;
    }
    glWaitSync(root_retired, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(root_retired);
    glSemaphoreParameterui64vEXT(s->producer, GL_D3D12_FENCE_VALUE_EXT, &target);
    glWaitSemaphoreEXT(s->producer, 0, NULL, 1, &s->source_texture, &layout);
    if (s->bgra) {
        glUseProgram(s->channel_program);
        glBindImageTexture(0, s->source_texture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(1, s->scanout_texture, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glDispatchCompute((s->width + 15) / 16, (s->height + 15) / 16, 1);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT |
                        GL_TEXTURE_UPDATE_BARRIER_BIT | GL_FRAMEBUFFER_BARRIER_BIT);
        glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA8);
        glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
        glUseProgram(0);
    } else {
        glCopyImageSubData(s->source_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                           s->scanout_texture, GL_TEXTURE_2D, 0, 0, 0, 0,
                           s->width, s->height, 1);
    }
    glSemaphoreParameterui64vEXT(s->consumer, GL_D3D12_FENCE_VALUE_EXT, &target);
    glSignalSemaphoreEXT(s->consumer, 0, NULL, 1, &s->source_texture, &layout);
    copy_complete = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!copy_complete || !relay_gl_ok(errp, "GPU source copy and consumer signal")) {
        if (!errp || !*errp) {
            error_setg(errp, "Cannot create the SDL GPU copy completion fence");
        }
        if (copy_complete) {
            glDeleteSync(copy_complete);
        }
        s->poisoned = true;
        return false;
    }
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    glWaitSync(copy_complete, 0, GL_TIMEOUT_IGNORED);
    glDeleteSync(copy_complete);
    glMemoryBarrier(GL_FRAMEBUFFER_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    s->pending_audit = audit;
    s->audit_called = false;
    s->pending_frame = frame;
    dpy_gl_update(s->con, 0, 0, s->width, s->height);
    s->pending_audit = false;
    if (!relay_gl_ok(errp, "SDL scanout") ||
        (audit && (!s->audit_called || s->audit_error))) {
        if (!errp || !*errp) {
            error_setg(errp, "%s", s->audit_error ? s->audit_error : "SDL did not invoke the pre-swap audit");
        }
        s->poisoned = true;
        return false;
    }
    s->last_frame = frame;
    if (s->bgra) {
        ++s->converted_frames;
    }
    return true;
}

static bool relay_detach(WddmSdlRelay *s, Error **errp)
{
    GLsync retired;
    GLenum waited;
    if (!s->attached) {
        error_setg(errp, "SDL GPU relay is not attached");
        return false;
    }
    dpy_gl_scanout_texture(s->con, s->scanout_texture, false, s->width, s->height,
                          0, 0, s->width, s->height, NULL);
    retired = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    if (!retired) {
        error_setg(errp, "Cannot create the SDL teardown retirement fence");
        return false;
    }
    waited = glClientWaitSync(retired, GL_SYNC_FLUSH_COMMANDS_BIT, UINT64_C(5000000000));
    glDeleteSync(retired);
    if ((waited != GL_ALREADY_SIGNALED && waited != GL_CONDITION_SATISFIED) ||
        !relay_gl_ok(errp, "bounded teardown retirement")) {
        if (!errp || !*errp) {
            error_setg(errp, "SDL GPU relay teardown did not retire within five seconds");
        }
        s->poisoned = true;
        return false;
    }
    dpy_gl_scanout_disable(s->con);
    relay_delete_objects(s);
    if (!relay_gl_ok(errp, "shared GPU reference release")) {
        s->poisoned = true;
        return false;
    }
    dpy_gl_ctx_destroy(s->con, s->import_context);
    s->import_context = NULL;
    s->attached = false;
    return true;
}

static void relay_command(Object *object, const char *text, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    WddmSdlCommand command;
    if (!DEVICE(s)->realized || !wddm_sdl_parse(text, &command) ||
        command.generation != s->generation) {
        error_setg(errp, "Invalid, stale or unrealized SDL GPU relay command");
        return;
    }
    switch (command.action) {
    case WDDM_SDL_ATTACH: relay_attach(s, errp); break;
    case WDDM_SDL_PRESENT: relay_present(s, command.frame, false, errp); break;
    case WDDM_SDL_AUDIT: relay_present(s, command.frame, true, errp); break;
    case WDDM_SDL_DETACH: relay_detach(s, errp); break;
    }
}

static char *relay_state(Object *object, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    QDict *value = qdict_new();
    GString *json;
    char source[17], window[17];
    snprintf(source, sizeof source, "%016" PRIx64, s->source_fnv);
    snprintf(window, sizeof window, "%016" PRIx64, s->window_fnv);
    qdict_put_bool(value, "attached", s->attached);
    qdict_put_bool(value, "poisoned", s->poisoned);
    qdict_put_bool(value, "everAttached", s->ever_attached);
    qdict_put_bool(value, "rootAdapterVerified", s->root_verified);
    qdict_put_bool(value, "importContextLive", s->import_context != NULL);
    qdict_put_bool(value, "localScanoutLive", s->scanout_texture != 0);
    qdict_put_bool(value, "channelProgramLive", s->channel_program != 0);
    qdict_put_str(value, "sourceFormat", s->bgra ? "bgra8" : "rgba8");
    qdict_put_int(value, "convertedFrames", s->converted_frames);
    qdict_put_int(value, "liveImportedObjects", !!s->memory + !!s->source_texture + !!s->producer + !!s->consumer);
    qdict_put_int(value, "generation", s->generation);
    qdict_put_int(value, "lastQueuedFrame", s->last_frame);
    qdict_put_int(value, "auditFrames", s->audit_frames);
    qdict_put_int(value, "sourceWidth", s->width);
    qdict_put_int(value, "sourceHeight", s->height);
    qdict_put_int(value, "windowWidth", s->audit_width);
    qdict_put_int(value, "windowHeight", s->audit_height);
    qdict_put_str(value, "vendor", s->vendor);
    qdict_put_str(value, "renderer", s->renderer);
    qdict_put_str(value, "version", s->version);
    qdict_put_str(value, "adapterLuid", s->luid);
    qdict_put_str(value, "sourceFnv1a", source);
    qdict_put_str(value, "windowFnv1a", window);
    qdict_put_bool(value, "independentFences", true);
    qdict_put_bool(value, "cpuTextureUpload", false);
    qdict_put_bool(value, "physicalInputVerified", false);
    qdict_put_bool(value, "physicalScanoutVerified", false);
    qdict_put_bool(value, "guestGpuScanoutVerified", false);
    qdict_put_bool(value, "nearNativePerformanceVerified", false);
    json = qobject_to_json(QOBJECT(value));
    qobject_unref(value);
    return g_string_free(json, false);
}

static int relay_flags(void *opaque) { return GRAPHIC_FLAGS_GL; }
static const GraphicHwOps relay_hw_ops = {
    .get_flags = relay_flags,
    .gl_scanout_audit = relay_scanout_audit,
};

static void relay_realize(DeviceState *device, Error **errp)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(device);
    if (!wddm_sdl_hex(s->session, 16) || !wddm_sdl_hex(s->expected_luid, 8) ||
        !s->generation || s->generation > WDDM_SDL_MAX_COUNTER ||
        !s->width || !s->height || s->width > 3840 || s->height > 2160) {
        error_setg(errp, "SDL GPU relay needs a fresh 128-bit session, adapter LUID, generation and bounded 4K extent");
        return;
    }
    if (s->source_format && strcmp(s->source_format, "rgba8") && strcmp(s->source_format, "bgra8")) {
        error_setg(errp, "SDL GPU relay needs rgba8 or bgra8 source-format");
        return;
    }
    s->bgra = s->source_format && !strcmp(s->source_format, "bgra8");
    error_setg(&s->migration_blocker, "SDL GPU relay's native resource ownership is not migratable");
    if (migrate_add_blocker(&s->migration_blocker, errp)) {
        return;
    }
    s->con = graphic_console_init(device, 0, &relay_hw_ops, s);
    qemu_console_resize(s->con, s->width, s->height);
}

static void relay_unrealize(DeviceState *device)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(device);
    Error *error = NULL;
    if (s->attached && !relay_detach(s, &error)) {
        error_report_err(error);
    }
    if (s->con) {
        graphic_console_close(s->con);
    }
}

static void relay_finalize(Object *object)
{
    WddmSdlRelay *s = WDDM_SDL_RELAY(object);
    if (s->migration_blocker) {
        migrate_del_blocker(&s->migration_blocker);
    }
    g_free(s->audit_error);
}
static const Property relay_properties[] = {
    DEFINE_PROP_STRING("session", WddmSdlRelay, session),
    DEFINE_PROP_STRING("adapter-luid", WddmSdlRelay, expected_luid),
    DEFINE_PROP_STRING("audit-directory", WddmSdlRelay, audit_directory),
    DEFINE_PROP_STRING("source-format", WddmSdlRelay, source_format),
    DEFINE_PROP_UINT32("width", WddmSdlRelay, width, 3840),
    DEFINE_PROP_UINT32("height", WddmSdlRelay, height, 2160),
    DEFINE_PROP_UINT64("generation", WddmSdlRelay, generation, 1),
};
static void relay_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *device = DEVICE_CLASS(klass);
    device->realize = relay_realize;
    device->unrealize = relay_unrealize;
    device->hotpluggable = false;
    device->user_creatable = true;
    device->desc = "Host-owned named D3D12 GPU texture relay to the existing SDL console";
    device_class_set_props(device, relay_properties);
    object_class_property_add_str(klass, "relay-command", NULL, relay_command);
    object_class_property_add_str(klass, "relay-state", relay_state, NULL);
}
static const TypeInfo relay_info = {
    .name = TYPE_WDDM_SDL_RELAY, .parent = TYPE_DEVICE,
    .instance_size = sizeof(WddmSdlRelay), .instance_finalize = relay_finalize,
    .class_init = relay_class_init,
};
static void relay_register(void) { type_register_static(&relay_info); }
type_init(relay_register);
