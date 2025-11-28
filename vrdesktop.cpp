// vrdesktop.cpp
// Wayland wlr-screencopy + SDL2 + OpenGL + OpenVR.
// - Default output: DP-3 (override: -o NAME)
// - Continuous capture (~120 fps) with cursor
// - Shows captured desktop on a quad in the SDL window (unless --no-window)
// - Renders a 3D plane in VR (stereo) floating in front of the user.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cerrno>

#include <string>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-protocol.h"
#include "xdg-output-unstable-v1-protocol.h"

// --------------------//Tray Icon Support
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <gtkmm.h>
#include <gio/gio.h>
#include <libayatana-appindicator/app-indicator.h>
#include <libdbusmenu-gtk/dbusmenu-gtk.h>

// --------------------

#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>

#include <openvr.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include "config.h"

struct SharedFrame {
    std::mutex m;
    std::vector<uint8_t> pixels;
    int width  = 0;
    int height = 0;
    int stride = 0;
    std::atomic<uint64_t> version{0};   // increments each time a new frame is available
};

static SharedFrame g_sharedFrame;
static std::atomic<bool> g_captureRunning{false};

// ---------------------------------------------------------------------------
// Simple shm helper
// ---------------------------------------------------------------------------

static std::atomic<bool> g_trayToggleRecenter{false};
static std::atomic<bool> g_trayToggleZoomIn{false};
static std::atomic<bool> g_trayToggleZoomOut{false};
static std::atomic<bool> g_trayToggleCurved{false};
static std::atomic<bool> g_trayTogglePreview{false};
static std::atomic<bool> g_trayToggleSave{false};
static std::atomic<bool> g_trayQuitRequest{false};

static int create_shm_file(off_t size)
{
    char template_name[] = "/dev/shm/vrdesktop-XXXXXX";
    int fd = mkstemp(template_name);
    if (fd < 0) {
        std::fprintf(stderr, "mkstemp failed: %s\n", std::strerror(errno));
        return -1;
    }

    // we don't need the name anymore
    unlink(template_name);

    if (ftruncate(fd, size) < 0) {
        std::fprintf(stderr, "ftruncate failed: %s\n", std::strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

// ---------------------------------------------------------------------------
// Output tracking
// ---------------------------------------------------------------------------

#define MAX_OUTPUTS 16

struct output_info {
    wl_output *wl_output_obj = nullptr;
    zxdg_output_v1 *xdg_output = nullptr;
    char *name = nullptr;      // e.g. "DP-3"
};

struct screencopy_state {
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    zwlr_screencopy_manager_v1 *screencopy_manager = nullptr;
    zxdg_output_manager_v1 *xdg_output_manager = nullptr;

    output_info outputs[MAX_OUTPUTS];
    int num_outputs = 0;

    wl_output *chosen_output = nullptr;

    // frame data
    int done = 0;
    int failed = 0;

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0; // wl_shm_format

    int shm_fd = -1;
    size_t shm_size = 0;
    void *shm_data = nullptr;
    wl_shm_pool *pool = nullptr;
    wl_buffer *buffer = nullptr;
};

// ---------------------------------------------------------------------------
// xdg-output listeners
// ---------------------------------------------------------------------------

static void xdg_output_logical_position(void *data,
                                        zxdg_output_v1 *xdg_output,
                                        int32_t x, int32_t y)
{
    (void)data; (void)xdg_output; (void)x; (void)y;
}

static void xdg_output_logical_size(void *data,
                                    zxdg_output_v1 *xdg_output,
                                    int32_t width, int32_t height)
{
    (void)data; (void)xdg_output; (void)width; (void)height;
}

static void xdg_output_done(void *data,
                            zxdg_output_v1 *xdg_output)
{
    (void)data; (void)xdg_output;
}

static void xdg_output_name(void *data,
                            zxdg_output_v1 *xdg_output,
                            const char *name)
{
    screencopy_state *st = static_cast<screencopy_state *>(data);

    for (int i = 0; i < st->num_outputs; ++i) {
        if (st->outputs[i].xdg_output == xdg_output) {
            free(st->outputs[i].name);
            st->outputs[i].name = strdup(name);
            std::fprintf(stderr, "Output %d name: %s\n", i, st->outputs[i].name);
            break;
        }
    }
}

static void xdg_output_description(void *data,
                                   zxdg_output_v1 *xdg_output,
                                   const char *description)
{
    (void)data; (void)xdg_output; (void)description;
}

static const zxdg_output_v1_listener xdg_output_listener = {
    xdg_output_logical_position,
    xdg_output_logical_size,
    xdg_output_done,
    xdg_output_name,
    xdg_output_description,
};

// ---------------------------------------------------------------------------
// Wayland registry
// ---------------------------------------------------------------------------

static void registry_global(void *data,
                            wl_registry *registry,
                            uint32_t name,
                            const char *interface,
                            uint32_t version)
{
    (void)version;
    screencopy_state *st = static_cast<screencopy_state *>(data);

    if (std::strcmp(interface, wl_shm_interface.name) == 0) {
        st->shm = static_cast<wl_shm *>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        st->screencopy_manager = static_cast<zwlr_screencopy_manager_v1 *>(
            wl_registry_bind(registry, name, &zwlr_screencopy_manager_v1_interface, 3));
    } else if (std::strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        st->xdg_output_manager = static_cast<zxdg_output_manager_v1 *>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, 3));
    } else if (std::strcmp(interface, wl_output_interface.name) == 0) {
        if (st->num_outputs < MAX_OUTPUTS) {
            output_info &out = st->outputs[st->num_outputs++];
            std::memset(&out, 0, sizeof(out));
            out.wl_output_obj = static_cast<wl_output *>(
                wl_registry_bind(registry, name, &wl_output_interface, 2));
        }
    }
}

static void registry_global_remove(void *data,
                                   wl_registry *registry,
                                   uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

static void setup_xdg_outputs(screencopy_state *st)
{
    if (!st->xdg_output_manager)
        return;

    for (int i = 0; i < st->num_outputs; ++i) {
        if (!st->outputs[i].wl_output_obj || st->outputs[i].xdg_output)
            continue;

        st->outputs[i].xdg_output =
            zxdg_output_manager_v1_get_xdg_output(
                st->xdg_output_manager,
                st->outputs[i].wl_output_obj);

        zxdg_output_v1_add_listener(
            st->outputs[i].xdg_output,
            &xdg_output_listener,
            st);
    }
}

static void choose_output(screencopy_state *st, const char *requested_name)
{
    st->chosen_output = nullptr;

    if (requested_name && st->xdg_output_manager) {
        for (int i = 0; i < st->num_outputs; ++i) {
            if (st->outputs[i].name &&
                std::strcmp(st->outputs[i].name, requested_name) == 0) {
                st->chosen_output = st->outputs[i].wl_output_obj;
                std::fprintf(stderr, "Selected output \"%s\" (index %d)\n",
                             st->outputs[i].name, i);
                return;
            }
        }
        std::fprintf(stderr,
                     "Requested output \"%s\" not found, falling back to first output.\n",
                     requested_name);
    }

    if (st->num_outputs > 0) {
        st->chosen_output = st->outputs[0].wl_output_obj;
        if (st->outputs[0].name)
            std::fprintf(stderr, "Using output \"%s\" (index 0)\n", st->outputs[0].name);
        else
            std::fprintf(stderr, "Using first output (index 0, no name)\n");
    } else {
        std::fprintf(stderr, "No wl_output objects found!\n");
    }
}

// ---------------------------------------------------------------------------
// Screencopy frame listener
// ---------------------------------------------------------------------------

static void frame_buffer(void *data,
                         zwlr_screencopy_frame_v1 *frame,
                         uint32_t format,
                         uint32_t width,
                         uint32_t height,
                         uint32_t stride)
{
    screencopy_state *st = static_cast<screencopy_state *>(data);
    (void)frame;

    st->format = format;
    st->width  = width;
    st->height = height;
    st->stride = stride;

    if (!st->buffer) {
        st->shm_size = (size_t)stride * (size_t)height;
        st->shm_fd = create_shm_file(st->shm_size);
        if (st->shm_fd < 0) {
            st->failed = 1;
            return;
        }

        st->shm_data = mmap(nullptr, st->shm_size,
                            PROT_READ | PROT_WRITE,
                            MAP_SHARED, st->shm_fd, 0);
        if (st->shm_data == MAP_FAILED) {
            std::fprintf(stderr, "mmap failed: %s\n", std::strerror(errno));
            close(st->shm_fd);
            st->shm_fd = -1;
            st->failed = 1;
            return;
        }

        st->pool = wl_shm_create_pool(st->shm, st->shm_fd, (int)st->shm_size);
        st->buffer = wl_shm_pool_create_buffer(
            st->pool,
            0,
            (int)width,
            (int)height,
            (int)stride,
            format
        );
    }

    // Ask compositor to copy into this buffer
    zwlr_screencopy_frame_v1_copy(frame, st->buffer);
}

static void frame_flags(void *data,
                        zwlr_screencopy_frame_v1 *frame,
                        uint32_t flags)
{
    (void)data; (void)frame; (void)flags;
}

static void frame_ready(void *data,
                        zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi,
                        uint32_t tv_sec_lo,
                        uint32_t tv_nsec)
{
    screencopy_state *st = static_cast<screencopy_state *>(data);
    (void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;

    st->done = 1;
    zwlr_screencopy_frame_v1_destroy(frame);
}

static void frame_failed(void *data,
                         zwlr_screencopy_frame_v1 *frame)
{
    screencopy_state *st = static_cast<screencopy_state *>(data);
    (void)frame;
    st->failed = 1;
    st->done = 1;
}

static void frame_damage(void *data,
                         zwlr_screencopy_frame_v1 *frame,
                         uint32_t x,
                         uint32_t y,
                         uint32_t width,
                         uint32_t height)
{
    (void)data; (void)frame; (void)x; (void)y; (void)width; (void)height;
}

static void frame_linux_dmabuf(void *data,
                               zwlr_screencopy_frame_v1 *frame,
                               uint32_t format,
                               uint32_t width,
                               uint32_t height)
{
    (void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_buffer_done(void *data,
                              zwlr_screencopy_frame_v1 *frame)
{
    (void)data; (void)frame;
    // For shm path we don't need to do anything here.
}

static const zwlr_screencopy_frame_v1_listener frame_listener = {
    frame_buffer,
    frame_flags,
    frame_ready,
    frame_failed,
    frame_damage,
    frame_linux_dmabuf,
    frame_buffer_done,
};

// ---------------------------------------------------------------------------
// Capture one frame
// ---------------------------------------------------------------------------

static int screencopy_capture(screencopy_state *st)
{
    if (!st->chosen_output) {
        std::fprintf(stderr, "No chosen_output set!\n");
        return -1;
    }

    st->done   = 0;
    st->failed = 0;

    // overlay_cursor = 1 -> include cursor in capture
    zwlr_screencopy_frame_v1 *frame =
        zwlr_screencopy_manager_v1_capture_output(
            st->screencopy_manager,
            1,
            st->chosen_output
        );

    zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, st);

    // Pump events until the frame is done
    while (!st->done && wl_display_dispatch(st->display) != -1) {
        // nothing else
    }

    if (st->failed) {
        std::fprintf(stderr, "screencopy: capture failed\n");
        return -1;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Upload frame into GL texture (create once, then subimage)
// ---------------------------------------------------------------------------

static void upload_frame_to_texture(screencopy_state *st,
                                    GLuint &tex,
                                    bool &tex_initialized)
{
    if (!st->shm_data || st->width == 0 || st->height == 0)
        return;

    if (!tex_initialized) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 4);
    GLfloat maxAniso = 0.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAniso);

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA,
            (GLsizei)st->width,
            (GLsizei)st->height,
            0,
            GL_RGBA,
            GL_UNSIGNED_INT_8_8_8_8_REV,
            st->shm_data
        );

        glBindTexture(GL_TEXTURE_2D, 0);
        tex_initialized = true;
    } else {
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(
            GL_TEXTURE_2D,
            0,
            0, 0,
            (GLsizei)st->width,
            (GLsizei)st->height,
            GL_RGBA,
            GL_UNSIGNED_INT_8_8_8_8_REV,
            st->shm_data
        );
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

// ---------------------------------------------------------------------------
// OpenVR state (minimal, with per-eye textures)
// ---------------------------------------------------------------------------

struct VRState {
    vr::IVRSystem *system = nullptr;
    uint32_t rtWidth = 0;
    uint32_t rtHeight = 0;
    GLuint eyeFbo[2]{0, 0};
    GLuint eyeTex[2]{0, 0};
};

static bool init_openvr(VRState &vrState)
{
    vr::EVRInitError eError = vr::VRInitError_None;
    vrState.system = vr::VR_Init(&eError, vr::VRApplication_Scene);
    if (eError != vr::VRInitError_None) {
        std::fprintf(stderr, "Unable to init OpenVR: %s\n",
                     vr::VR_GetVRInitErrorAsEnglishDescription(eError));
        vrState.system = nullptr;
        return false;
    }

    if (!vr::VRCompositor()) {
        std::fprintf(stderr, "OpenVR Compositor initialization failed.\n");
        vr::VR_Shutdown();
        vrState.system = nullptr;
        return false;
    }

    vrState.system->GetRecommendedRenderTargetSize(&vrState.rtWidth, &vrState.rtHeight);
    std::fprintf(stderr, "OpenVR recommended size: %ux%u\n",
                 vrState.rtWidth, vrState.rtHeight);
    // For PSVR2 you should see something like ~2000x2040 here.

    glGenFramebuffers(2, vrState.eyeFbo);
    glGenTextures(2, vrState.eyeTex);

    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, vrState.eyeTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA8,
            (GLsizei)vrState.rtWidth,
            (GLsizei)vrState.rtHeight,
            0,
            GL_RGBA,
            GL_UNSIGNED_BYTE,
            nullptr
        );

        glBindFramebuffer(GL_FRAMEBUFFER, vrState.eyeFbo[i]);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D,
            vrState.eyeTex[i],
            0
        );

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "FBO %d incomplete (status=0x%x)\n", i, status);
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

static void shutdown_openvr(VRState &vrState)
{
    if (vrState.eyeTex[0] || vrState.eyeTex[1]) {
        glDeleteTextures(2, vrState.eyeTex);
        vrState.eyeTex[0] = vrState.eyeTex[1] = 0;
    }
    if (vrState.eyeFbo[0] || vrState.eyeFbo[1]) {
        glDeleteFramebuffers(2, vrState.eyeFbo);
        vrState.eyeFbo[0] = vrState.eyeFbo[1] = 0;
    }
    if (vrState.system) {
        vr::VR_Shutdown();
        vrState.system = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Helpers: convert OpenVR matrix to OpenGL column-major
// ---------------------------------------------------------------------------

// --------- Simple 4x4 matrix helpers (row-major) ---------

static void mat4_identity(float m[16])
{
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_mul_row(const float a[16], const float b[16], float out[16])
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            out[r*4 + c] =
                a[r*4 + 0] * b[0*4 + c] +
                a[r*4 + 1] * b[1*4 + c] +
                a[r*4 + 2] * b[2*4 + c] +
                a[r*4 + 3] * b[3*4 + c];
        }
    }
}

static void mat4_row_to_col(const float inRow[16], float outCol[16])
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            outCol[c*4 + r] = inRow[r*4 + c];
}

// Convert OpenVR HmdMatrix34_t to 4x4 row-major
static void mat4_from_HmdMatrix34_row(const vr::HmdMatrix34_t &m34, float m44[16])
{
    mat4_identity(m44);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            m44[r*4 + c] = m34.m[r][c];
        }
    }
}

// Invert a rigid-body 4x4 (R|t; 0|1) into row-major
static void mat4_invert_rigid_row(const float in[16], float out[16])
{
    // Extract rotation (upper 3x3) and translation (last column)
    float R[3][3];
    float t[3];
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            R[r][c] = in[r*4 + c];
        }
        t[r] = in[r*4 + 3];
    }

    // R^T
    float Rt[3][3];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            Rt[r][c] = R[c][r];

    // t' = -R^T * t
    float tInv[3];
    for (int r = 0; r < 3; ++r) {
        tInv[r] = -(Rt[r][0] * t[0] +
                    Rt[r][1] * t[1] +
                    Rt[r][2] * t[2]);
    }

    mat4_identity(out);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r*4 + c] = Rt[r][c];
        }
        out[r*4 + 3] = tInv[r];
    }
}

// --------- Global state for plane and head pose ---------
static bool running = true;
static bool g_useCurvedSurface = false;
static bool hideWindow = true;               // --no-window
static float g_planePoseRow[16];            // absoluteFromPlane (row-major)
static bool  g_planePoseInitialized = false;
static bool  g_curvePoseInitialized = false;


static float g_lastAbsoluteFromHeadRow[16]; // absoluteFromHead
static bool  g_haveHeadPose = false;

// recenter: place plane planeDistance meters in front of current head
static void recenter_plane(float planeDistance)
{
    if (!g_haveHeadPose)
        return;

    // headToPlane: translate along -Z in head space
    float headToPlane[16];
    mat4_identity(headToPlane);
    headToPlane[2*4 + 3] = -planeDistance;  // row 2, col 3 (z translation)

    // absoluteFromPlane = absoluteFromHead * headToPlane
    mat4_mul_row(g_lastAbsoluteFromHeadRow, headToPlane, g_planePoseRow);
    g_planePoseInitialized = true;
    g_curvePoseInitialized = true;

    std::fprintf(stderr, "Recenter plane at distance %.2f m\n", planeDistance);
}

// recenter: place plane planeDistance meters in front of current head
static void recenter_curve(float curveDistance)
{
    if (!g_haveHeadPose)
        return;

    // headTocurve: translate along -Z in head space
    float headToCurve[16];
    mat4_identity(headToCurve);
    headToCurve[2*4 + 3] = -curveDistance;  // row 2, col 3 (z translation)

    // absoluteFromCurve = absoluteFromHead * headToCurve
    mat4_mul_row(g_lastAbsoluteFromHeadRow, headToCurve, g_planePoseRow);
    g_curvePoseInitialized = true;

    std::fprintf(stderr, "Recenter curve at distance %.2f m\n", curveDistance);
}

static void load_gl_projection_from_vr(const vr::HmdMatrix44_t &m)
{
    float mat[16];
    // OpenVR is row-major; OpenGL wants column-major
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            mat[c * 4 + r] = m.m[r][c];
        }
    }
    glMatrixMode(GL_PROJECTION);
    glLoadMatrixf(mat);
}

// Render plane in 3D (XY plane at z=0; caller sets camera/modelview)
static void render_desktop_plane_3d(GLuint desktopTex,
                                    float planeWidth,
                                    float planeHeight)
{
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, desktopTex);

    const float hw = planeWidth  * 0.5f;
    const float hh = planeHeight * 0.5f;

    glBegin(GL_QUADS);
        // Y flipped so desktop isn't upside-down
        glTexCoord2f(0.f, 1.f); glVertex3f(-hw, -hh, 0.f);
        glTexCoord2f(1.f, 1.f); glVertex3f( hw, -hh, 0.f);
        glTexCoord2f(1.f, 0.f); glVertex3f( hw,  hh, 0.f);
        glTexCoord2f(0.f, 0.f); glVertex3f(-hw,  hh, 0.f);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

// Render desktop on a cylindrical segment centered on Z axis
static void render_desktop_curved_3d(GLuint desktopTex,
                                     float planeWidth,
                                     float planeHeight)
{
    if (!desktopTex)
        return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, desktopTex);

    // How much of a cylinder arc to use (in degrees)
    const float arcDegrees = 90.0f;     // 90° wrap around you
    const int   segments   = 64;        // geometry resolution

    // Convert to radians
    const float arcRadians = arcDegrees * (float)M_PI / 180.0f;

    // planeWidth is the chord length; approximate radius so chord spans arc
    // chord = 2 * R * sin(theta/2)
    const float halfArc = arcRadians * 0.5f;
    const float radius  = planeWidth / (2.0f * sinf(halfArc));

    const float halfHeight = planeHeight * 0.5f;

    // Angle range from -halfArc..+halfArc (screen centered on -Z)
    const float thetaStart = -halfArc;
    const float thetaEnd   =  halfArc;

    glBegin(GL_TRIANGLE_STRIP);

    for (int i = 0; i <= segments; ++i) {
        float t = (float)i / (float)segments;
        float theta = thetaStart + t * (thetaEnd - thetaStart);

        // Cylinder parametric: around Y axis, facing -Z
        float x = radius * sinf(theta);
        float z = -radius * cosf(theta);

        // Texture coordinate across width
        float u = t;   // 0..1 across arc

        // Top vertex (y = +halfHeight)
        glTexCoord2f(u, 0.0f);
        glVertex3f(x, +halfHeight, z);

        // Bottom vertex (y = -halfHeight)
        glTexCoord2f(u, 1.0f);
        glVertex3f(x, -halfHeight, z);
    }

    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

static void render_desktop_curved(GLuint desktopTex,
                                     float planeWidth,
                                     float planeHeight)
{
    if (!desktopTex)
        return;

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, desktopTex);

    // How much of a cylinder arc to use (in degrees)
    const float arcDegrees = 90.0f;     // 90° wrap around you
    const int   segments   = 64;        // geometry resolution

    // Convert to radians
    const float arcRadians = arcDegrees * (float)M_PI / 180.0f;

    // planeWidth is the chord length; approximate radius so chord spans arc
    // chord = 2 * R * sin(theta/2)
    const float halfArc = arcRadians * 0.5f;
    const float radius  = planeWidth / (2.0f * sinf(halfArc));

    const float halfHeight = planeHeight * 0.5f;

    // Angle range from -halfArc..+halfArc (screen centered on -Z)
    const float thetaStart = -halfArc;
    const float thetaEnd   =  halfArc;

    glBegin(GL_TRIANGLE_STRIP);

    for (int i = 0; i <= segments; ++i) {
        float t = (float)i / (float)segments;
        float theta = thetaStart + t * (thetaEnd - thetaStart);

        // Cylinder parametric: around Y axis, facing -Z
        float x = radius * sinf(theta);
        float z = -radius * cosf(theta);

        // Texture coordinate across width
        float u = t;   // 0..1 across arc

        // Top vertex (y = +halfHeight)
        glTexCoord2f(u, 0.0f);
        glVertex3f(x, +halfHeight, z);

        // Bottom vertex (y = -halfHeight)
        glTexCoord2f(u, 1.0f);
        glVertex3f(x, -halfHeight, z);
    }

    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}


// Render to SDL window (simple 2D quad)
static void render_desktop_quad_2d(GLuint desktopTex)
{
    glClearColor(0.f, 0.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, desktopTex);

    glBegin(GL_QUADS);
        glTexCoord2f(0.f, 1.f); glVertex2f(-0.8f, -0.8f);
        glTexCoord2f(1.f, 1.f); glVertex2f( 0.8f, -0.8f);
        glTexCoord2f(1.f, 0.f); glVertex2f( 0.8f,  0.8f);
        glTexCoord2f(0.f, 0.f); glVertex2f(-0.8f,  0.8f);
    glEnd();

    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
}

// -------------------------------------------------------------------------
// terminal input
// -------------------------------------------------------------------------

static termios g_orig_termios;
static bool g_raw_enabled = false;

void enable_raw_mode()
{
    if (g_raw_enabled)
        return;

    if (!isatty(STDIN_FILENO))
        return; // running under something non-tty, bail

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) == -1)
        return;

    termios raw = g_orig_termios;

    // disable canonical mode, echo, signals
    raw.c_lflag &= ~(ICANON | ECHO | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_oflag &= ~(OPOST);

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == -1)
        return;

    // make stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags != -1)
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    g_raw_enabled = true;
}

void disable_raw_mode()
{
    if (!g_raw_enabled)
        return;

    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    g_raw_enabled = false;
}

int read_cli_key()
{
    if (!g_raw_enabled)
        return -1;

    unsigned char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n == 1) {
        return c;
    }
    return -1; // no key
}

//
//
//

static void capture_thread_func(screencopy_state *st)
{
    // Initialise shared buffer the first time
    {
        std::lock_guard<std::mutex> lock(g_sharedFrame.m);
        g_sharedFrame.width  = st->width;
        g_sharedFrame.height = st->height;
        g_sharedFrame.stride = st->stride;
        g_sharedFrame.pixels.resize(st->stride * st->height);
    }

    while (g_captureRunning.load()) {
        if (screencopy_capture(st) == 0) {
            // Copy shm buffer into our shared CPU buffer
            std::lock_guard<std::mutex> lock(g_sharedFrame.m);
            std::memcpy(g_sharedFrame.pixels.data(),
                        st->shm_data,
                        g_sharedFrame.stride * g_sharedFrame.height);
            g_sharedFrame.version.fetch_add(1, std::memory_order_relaxed);
        }

        // Optional: small sleep to avoid hammering compositor if you want
        // std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// ----------------------------------------------------------------------
// documentation
// ----------------------------------------------------------------------

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "------------------------------------------------------------\n"
        " VR Desktop Viewer (Wayland + OpenGL + OpenVR)\n"
        "------------------------------------------------------------\n"
        "Usage:\n"
        "  %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  -h, --help\n"
        "       Show this help text.\n"
        "\n"
        "  -o <output>, --output <output>\n"
        "       Select a Wayland output name (default: DP-3).\n"
        "       Example: --output HDMI-A-1\n"
        "\n"
        "  -n, --no-window\n"
        "       Hide the SDL window (VR-only mode).\n"
        "\n"
        "  -d <meters>\n"
        "       Set initial distance from the viewer to the VR desktop plane.\n"
        "       Default: 2.0 meters.\n"
        "       Example: --distance 3.0\n"
        "\n"
        "  -c\n"
        "       Enable curved desktop surface (cylindrical)\n"
        "       instead of a flat plane.\n"
        "\n"
        "Keyboard Controls:\n"
        "  Numpad +     Zoom in (move plane closer)  \n"
        "  Numpad -     Zoom out (move plane farther)\n"
        "  Numpad 5     Recenter desktop plane to the middle of your view\n"
        "  ESC          Quit\n"
        "\n"
        "Description:\n"
        "  - Captures a Wayland desktop at full resolution (wlr-screencopy)\n"
        "  - Uploads it as a GL texture (1:1 pixels)\n"
        "  - Renders the desktop as a floating surface in VR\n"
        "  - Uses OpenVR for full head-tracked viewing\n"
        "  - Supports flat or curved viewing surfaces\n"
        "  - Allows zooming and recentring in real time\n"
        "\n"
        "Examples:\n"
        "  %s                          # default: DP-3, flat screen\n"
        "  %s --curved                 # curved cinema-style surface\n"
        "  %s --output HDMI-A-1        # select a specific display\n"
        "  %s --distance 3.0           # place screen 3m away\n"
        "  %s -n --curved              # VR only, curved\n"
        "\n",
        prog, prog, prog, prog, prog, prog
    );
}

std::string getConfigPath()
{
    const char* home = std::getenv("HOME");
    if (!home) {
        // fallback (very rare)
        home = ".";
    }
    std::string path = std::string(home) + "/.config/vrdesktop";
    // Create the folder if it does not exist
    mkdir(path.c_str(), 0755);
    // Append the config file
    path += "/vrdesktop.cfg";
    return path;
}


// ---------------------------------------------------------------------------
// appindicator functions
// ---------------------------------------------------------------------------

static void on_menu_toggle_recenter(GtkMenuItem*, gpointer) {
    g_trayToggleRecenter.store(true);
}

static void on_menu_toggle_zoom_in(GtkMenuItem*, gpointer) {
    g_trayToggleZoomIn.store(true);
}

static void on_menu_toggle_zoom_out(GtkMenuItem*, gpointer) {
    g_trayToggleZoomOut.store(true);
}

static void on_menu_toggle_curved_flat(GtkMenuItem*, gpointer) {
    g_trayToggleCurved.store(true);
}

static void on_menu_toggle_preview(GtkMenuItem*, gpointer) {
    g_trayTogglePreview.store(true);
}

static void on_menu_toggle_save(GtkMenuItem*, gpointer) {
    g_trayToggleSave.store(true);
}

static void on_menu_quit(GtkMenuItem*, gpointer) {
    g_trayQuitRequest.store(true);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    Config cfg;

    std::string configFile = getConfigPath();

    std::ifstream test(configFile);
    if (!test.good()) {
        std::ofstream out(configFile);
        out << "DP-3\n";
        out << "curved\n";
        out << "-0.3\n";
        out << "enabled\n";
        out.close();
    }
    test.close();

    // Create a new AppIndicator
    gtk_init(&argc, &argv);
    AppIndicator *indicator = app_indicator_new(
        "vrdesktop",
        "video-display",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS
    );
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item_recenter = gtk_menu_item_new_with_label("Recenter View");
    GtkWidget *item_zoom_in = gtk_menu_item_new_with_label("Zoom In");
    GtkWidget *item_zoom_out = gtk_menu_item_new_with_label("Zoom Out");
    GtkWidget *item_preview = gtk_menu_item_new_with_label("Show Preview");
    GtkWidget *item_curved = gtk_menu_item_new_with_label("Toggle Curved/Flat");
    GtkWidget *item_save_config = gtk_menu_item_new_with_label("Save Configuration");
    GtkWidget *item_quit = gtk_menu_item_new_with_label("Quit");

    g_signal_connect(item_recenter, "activate", G_CALLBACK(on_menu_toggle_recenter), nullptr);
    g_signal_connect(item_zoom_in, "activate", G_CALLBACK(on_menu_toggle_zoom_in), nullptr);
    g_signal_connect(item_zoom_out, "activate", G_CALLBACK(on_menu_toggle_zoom_out), nullptr);
    g_signal_connect(item_curved, "activate", G_CALLBACK(on_menu_toggle_curved_flat), nullptr);
    g_signal_connect(item_preview, "activate", G_CALLBACK(on_menu_toggle_preview), nullptr);
    g_signal_connect(item_save_config, "activate", G_CALLBACK(on_menu_toggle_save), nullptr);
    g_signal_connect(item_quit, "activate", G_CALLBACK(on_menu_quit), nullptr);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_recenter);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_zoom_in);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_zoom_out);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_preview);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_curved);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_save_config);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item_quit);
    gtk_widget_show_all(menu);
    app_indicator_set_menu(indicator, GTK_MENU(menu));

    const char *requested_output = cfg.displayOutput.c_str();  // default Wayland output
    float planeDistance = 0.7;
    float curveDistance = 0.7;
    fprintf(stderr, "show window value: %d", cfg.hide_window);
    fprintf(stderr, "curved window value: %d", cfg.curved);

    // ---------------- Parse command line ----------------
    if (argc == 1) {
        print_usage(argv[0]);
    }

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            requested_output = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "--no-window") == 0) {
            hideWindow = true;
        } else if ((strcmp(argv[i], "-d") == 0 ) && i + 1 < argc){
        planeDistance = strtof(argv[++i],nullptr);
        curveDistance = strtof(argv[++i],nullptr);
    } else if (strcmp(argv[i], "-c") == 0) {
            g_useCurvedSurface = true;
            fprintf(stderr, "Using curved desktop surface.\n");
    } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (!loadConfig(configFile, cfg)) {
        fprintf(stderr, "unable to load config");
        return 1;
    } else {
    fprintf(stderr, "config file loaded");
        requested_output = cfg.displayOutput.c_str();
        planeDistance = cfg.distance;
        curveDistance = cfg.distance;
        g_useCurvedSurface = cfg.curved;
        hideWindow = cfg.hide_window;
    }

    fprintf(stderr, "Requested output: %s\n", requested_output);
    if (hideWindow)
        enable_raw_mode();
        fprintf(stderr, "SDL window hidden (--no-window)\n");

    // ---------------- Wayland init ----------------
    screencopy_state st{};
    st.shm_fd = -1;

    st.display = wl_display_connect(nullptr);
    if (!st.display) {
        fprintf(stderr, "Failed to connect to Wayland\n");
        return 1;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.shm || !st.screencopy_manager || st.num_outputs == 0) {
        fprintf(stderr, "Wayland globals missing\n");
        return 1;
    }

    setup_xdg_outputs(&st);
    wl_display_roundtrip(st.display);

    choose_output(&st, requested_output);
    if (!st.chosen_output)
        return 1;

    // ---------------- SDL + OpenGL init ----------------
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    Uint32 winFlags = SDL_WINDOW_OPENGL;
    if (!hideWindow) winFlags |= SDL_WINDOW_RESIZABLE;
    else winFlags |= SDL_WINDOW_HIDDEN;

    SDL_Window *window = SDL_CreateWindow(
        "VR Desktop",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 720, winFlags
    );

    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GLContext glctx = SDL_GL_CreateContext(window);
    if (!glctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetSwapInterval(0);

    // Window size is irrelevant for VR resolution
    int winW = 1280, winH = 720;

    // ---------------- OpenVR init ----------------
    VRState vrState{};
    bool vr_ok = init_openvr(vrState);

    if (!vr_ok)
        fprintf(stderr, "OpenVR unavailable — VR disabled.\n");

    // ---------------- Capture first frame ----------------
    GLuint desktopTex = 0;
    bool desktopTexInitialized = false;

    if (screencopy_capture(&st) == 0)
        upload_frame_to_texture(&st, desktopTex, desktopTexInitialized);
    else
        fprintf(stderr, "Initial capture failed\n");

    g_captureRunning.store(true);
    std::thread captureThread(capture_thread_func, &st);

    // ---------------- Plane size + interaction ----------------
    float planeWidth = 1.5f;
    float planeHeight = 0.9f;

    if (st.width && st.height)
        planeHeight = planeWidth * ((float)st.height / (float)st.width);

    const float planeDistanceMin = 0.5f;
    const float planeDistanceMax = 5.0f;
    const float curveDistanceMin = -1.0f;
    const float curveDistanceMax = 5.0f;

    // ---------------- Main loop ----------------
    while (running) {
        while (gtk_events_pending()){
            gtk_main_iteration_do(FALSE);
        }
        if (!hideWindow) {
            // ------------ Input handling ------------
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                if (e.type == SDL_QUIT) running = false;
               if (e.type == SDL_KEYDOWN) {
                    SDL_Keycode key = e.key.keysym.sym;
                    if (key == SDLK_ESCAPE || SDLK_q)
                        running = false;
                    else if (key == SDLK_KP_PLUS) {
                        // Zoom in
                   if (!g_useCurvedSurface){
                        planeDistance -= 0.1f;
                        if (planeDistance < planeDistanceMin){
                            planeDistance = planeDistanceMin;
                                recenter_plane(planeDistance);
                    }
                }else{
                        curveDistance -= 0.1f;
                        if (curveDistance < curveDistanceMin){
                            curveDistance = curveDistanceMin;
                        recenter_curve(curveDistance);
                }
                }
                    }
                    else if (key == SDLK_KP_MINUS) {
                        // Zoom out
                if (!g_useCurvedSurface){
                        planeDistance += 0.1f;
                        if (planeDistance > planeDistanceMax){
                            planeDistance = planeDistanceMax;
                            recenter_plane(planeDistance);
                    }
                }else{
                        curveDistance += 0.1f;
                        if (curveDistance > curveDistanceMax){
                            curveDistance = curveDistanceMax;
                        recenter_curve(curveDistance);
                }
                }
                    }

                    else if (key == SDLK_KP_5) {
                        // Recenter plane at current distance
                if (!g_useCurvedSurface){
                            recenter_plane(planeDistance);
                }else{
                            recenter_curve(curveDistance);
                }
            } else if (key == SDLK_c) {
                    g_useCurvedSurface = !g_useCurvedSurface;
                    fprintf(stderr, "Surface mode: %s\n",
                        g_useCurvedSurface ? "curved" : "flat");
                }
            }
    }
    } else {
        // CLI mode: read keys from terminal
        if (hideWindow && g_raw_enabled) {
            char ch;
            ssize_t n;
            // read all available characters
            while ((n = read(STDIN_FILENO, &ch, 1)) > 0) {
                if (ch == 'q' || ch == 'Q') {
                    running = false;
                } else if (ch == '+') {
            if (!g_useCurvedSurface){
                        planeDistance -= 0.1f;
                        if (planeDistance < planeDistanceMin)
                            planeDistance = planeDistanceMin;
                        recenter_plane(planeDistance);
                        fprintf(stderr, "Zoom in (CLI): distance=%.2f\n", planeDistance);
            } else {
                        curveDistance -= 0.1f;
                        if (curveDistance < curveDistanceMin)
                            curveDistance = curveDistanceMin;
                        recenter_curve(curveDistance);
                        fprintf(stderr, "Zoom in (CLI): distance=%.2f\n", planeDistance);
            }
            } else if (ch == '-') {
            if (!g_useCurvedSurface){
                        planeDistance += 0.1f;
                        if (planeDistance > planeDistanceMax)
                            planeDistance = planeDistanceMax;
                        recenter_plane(planeDistance);
                        fprintf(stderr, "Zoom out (CLI): distance=%.2f\n", planeDistance);
            } else {
                        curveDistance += 0.1f;
                        if (curveDistance < curveDistanceMin)
                            curveDistance = curveDistanceMin;
                        recenter_curve(curveDistance);
                        fprintf(stderr, "Zoom out (CLI): distance=%.2f\n", planeDistance);
            }
                } else if (ch == '5') {
                    recenter_plane(planeDistance);
                    fprintf(stderr, "Recenter (CLI)\n");
                } else if (ch == 27) { // ESC
                    running = false;
        } else if (ch == 'c') {
                g_useCurvedSurface = !g_useCurvedSurface;
                fprintf(stderr, "Surface mode: %s\n",
                    g_useCurvedSurface ? "curved" : "flat");
               }
            }
    }
        // ------------ Tray actions ------------
        if (g_trayToggleRecenter.exchange(false)) {
            recenter_plane(planeDistance);
            fprintf(stderr, "Recentering View\n");
        }
        if (g_trayToggleZoomIn.exchange(false)) {
                        if (!g_useCurvedSurface){
                            planeDistance -= 0.1f;
                            if (planeDistance < planeDistanceMin)
                                planeDistance = planeDistanceMin;
                            recenter_plane(planeDistance);
                            fprintf(stderr, "Zoom in (CLI): distance=%.2f\n", planeDistance);
                        } else {
                            curveDistance -= 0.1f;
                            if (curveDistance < curveDistanceMin)
                                curveDistance = curveDistanceMin;
                            recenter_curve(curveDistance);
                            fprintf(stderr, "Zoom in (CLI): distance=%.2f\n", planeDistance);
                        }

        }
        if (g_trayToggleZoomOut.exchange(false)) {
            if (!g_useCurvedSurface){
                        planeDistance += 0.1f;
                        if (planeDistance > planeDistanceMax)
                            planeDistance = planeDistanceMax;
                        recenter_plane(planeDistance);
                        fprintf(stderr, "Zoom out (CLI): distance=%.2f\n", planeDistance);
            } else {
                        curveDistance += 0.1f;
                        if (curveDistance < curveDistanceMin)
                            curveDistance = curveDistanceMin;
                        recenter_curve(curveDistance);
                        fprintf(stderr, "Zoom out (CLI): distance=%.2f\n", planeDistance);
            }
        }
        if (g_trayToggleCurved.exchange(false)) {
            g_useCurvedSurface = !g_useCurvedSurface;
            fprintf(stderr, "Surface mode (tray): %s\n", g_useCurvedSurface ? "curved" : "flat");
        }
        if (g_trayTogglePreview.exchange(false)) {
            hideWindow = !hideWindow;
        fprintf(stderr, "Window Hidden: %d\n", hideWindow);
            if (hideWindow) {
                SDL_HideWindow(window);
            } else {
                SDL_ShowWindow(window);
            }
            fprintf(stderr, "Surface mode (tray): %s\n", hideWindow ? "Hidden" : "Shown");
        }
        if (g_trayToggleSave.exchange(false)) {
            if (!saveConfig(configFile, cfg)) {
                fprintf(stderr, "Unable to save config\n");
                return 1;
            } else {
                fprintf(stderr, "Configuration Saved\n");
	    }
        }
        if (g_trayQuitRequest.exchange(false)) {
            fprintf(stderr, "Quit requested from tray\n");
            running = false;
        }
    }
    // ------------ 120fps screencopy capture ------------
    static uint64_t lastUploadedVersion = 0;

    uint64_t v = g_sharedFrame.version.load(std::memory_order_relaxed);
    if (v != lastUploadedVersion && v != 0) {
        std::lock_guard<std::mutex> lock(g_sharedFrame.m);

        if (!desktopTexInitialized) {
        glGenTextures(1, &desktopTex);
            glBindTexture(GL_TEXTURE_2D, desktopTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                         g_sharedFrame.width,
                         g_sharedFrame.height,
                         0, GL_RGBA, GL_UNSIGNED_BYTE,
                         g_sharedFrame.pixels.data());
            desktopTexInitialized = true;
        } else {
            glBindTexture(GL_TEXTURE_2D, desktopTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                            0, 0,
                            g_sharedFrame.width,
                            g_sharedFrame.height,
                            GL_RGBA, GL_UNSIGNED_BYTE,
                            g_sharedFrame.pixels.data());
        }

        lastUploadedVersion = v;
    }


        // ------------ VR rendering ------------
    if (vr_ok && desktopTexInitialized) {
        vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
        vr::VRCompositor()->WaitGetPoses(
        poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0);

        // ---- Get latest HMD pose once per frame ----
        const vr::TrackedDevicePose_t &hmdPose =
        poses[vr::k_unTrackedDeviceIndex_Hmd];

        if (hmdPose.bPoseIsValid) {
            mat4_from_HmdMatrix34_row(
            hmdPose.mDeviceToAbsoluteTracking,
            g_lastAbsoluteFromHeadRow
            );
            g_haveHeadPose = true;

            // First-time setup of plane/curve pose
            if (!g_planePoseInitialized) {
                recenter_plane(planeDistance);
            }
            if (!g_curvePoseInitialized) {
                recenter_curve(curveDistance);
            }
        }
        // Safety: if we *still* don't have a pose, skip VR for this frame
        if (!g_haveHeadPose) {
            // Optionally clear the eye FBOs to black so they don't contain garbage
            for (int eye = 0; eye < 2; ++eye) {
                glBindFramebuffer(GL_FRAMEBUFFER, vrState.eyeFbo[eye]);
                glViewport(0, 0, vrState.rtWidth, vrState.rtHeight);
                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        } else {
            // ---- Render each eye using the same HMD pose ----
            for (int eye = 0; eye < 2; ++eye) {
                vr::Hmd_Eye vrEye = (eye == 0) ? vr::Eye_Left : vr::Eye_Right;

                // Get eye->head transform from OpenVR
                vr::HmdMatrix34_t eyeToHead =
                    vrState.system->GetEyeToHeadTransform(vrEye);

                float headFromEye[16];
                mat4_from_HmdMatrix34_row(eyeToHead, headFromEye);

                // absolute<-eye = absolute<-head * head<-eye
                float absoluteFromEye[16];
                mat4_mul_row(g_lastAbsoluteFromHeadRow, headFromEye, absoluteFromEye);

                // eye<-absolute = inverse(absolute<-eye)
                float eyeFromAbsoluteRow[16];
                mat4_invert_rigid_row(absoluteFromEye, eyeFromAbsoluteRow);

                // eye<-plane = eye<-absolute * absolute<-plane
                float eyeFromPlaneRow[16];
                mat4_mul_row(eyeFromAbsoluteRow, g_planePoseRow, eyeFromPlaneRow);

                // Convert to column-major for OpenGL
                float mvCol[16];
                mat4_row_to_col(eyeFromPlaneRow, mvCol);

                // Projection from OpenVR
                vr::HmdMatrix44_t proj =
                    vrState.system->GetProjectionMatrix(vrEye, 0.1f, 100.0f);

                float projCol[16];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        projCol[c*4 + r] = proj.m[r][c];

                // --- Draw into this eye's FBO ---
                glBindFramebuffer(GL_FRAMEBUFFER, vrState.eyeFbo[eye]);
                glViewport(0, 0, vrState.rtWidth, vrState.rtHeight);

                glMatrixMode(GL_PROJECTION);
                glLoadMatrixf(projCol);

                glMatrixMode(GL_MODELVIEW);
                glLoadMatrixf(mvCol);

                glClearColor(0.f, 0.f, 0.f, 1.f);
                glClear(GL_COLOR_BUFFER_BIT);

                if (g_useCurvedSurface) {
                    render_desktop_curved_3d(desktopTex, planeWidth, planeHeight);
                } else {
                    render_desktop_plane_3d(desktopTex, planeWidth, planeHeight);
                }
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }

        // ---- Submit both eyes to the compositor ----
        vr::Texture_t leftEyeTex  = {
            (void*)(uintptr_t)vrState.eyeTex[0],
            vr::TextureType_OpenGL,
            vr::ColorSpace_Auto
        };
        vr::Texture_t rightEyeTex = {
            (void*)(uintptr_t)vrState.eyeTex[1],
            vr::TextureType_OpenGL,
            vr::ColorSpace_Auto
        };
        vr::VRCompositor()->Submit(vr::Eye_Left,  &leftEyeTex);
        vr::VRCompositor()->Submit(vr::Eye_Right, &rightEyeTex);
    }
        // ------------ Optional SDL window preview ------------
        if (!hideWindow && desktopTexInitialized) {
            SDL_GetWindowSize(window, &winW, &winH);
            glViewport(0, 0, winW, winH);
            glDisable(GL_SCISSOR_TEST);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (g_useCurvedSurface) {
                render_desktop_curved(desktopTex, planeWidth, planeHeight);
            } else {
                render_desktop_quad_2d(desktopTex);
            }
            SDL_GL_SwapWindow(window);
        }
    }

    // ---------------- Cleanup ----------------
    g_captureRunning.store(false);
    if (captureThread.joinable()) {
        captureThread.join();
    }
    if (desktopTex)
        glDeleteTextures(1, &desktopTex);
        shutdown_openvr(vrState);

    if (st.buffer) wl_buffer_destroy(st.buffer);
    if (st.pool) wl_shm_pool_destroy(st.pool);
    if (st.shm_data && st.shm_data != MAP_FAILED)
        munmap(st.shm_data, st.shm_size);
    if (st.shm_fd >= 0)
        close(st.shm_fd);

    for (int i = 0; i < st.num_outputs; i++) {
        if (st.outputs[i].xdg_output)
            zxdg_output_v1_destroy(st.outputs[i].xdg_output);
        if (st.outputs[i].wl_output_obj)
            wl_output_destroy(st.outputs[i].wl_output_obj);
        free(st.outputs[i].name);
    }

    if (st.xdg_output_manager)
        zxdg_output_manager_v1_destroy(st.xdg_output_manager);
    if (st.screencopy_manager)
        zwlr_screencopy_manager_v1_destroy(st.screencopy_manager);
    if (st.shm)
        wl_shm_destroy(st.shm);
    if (st.registry)
        wl_registry_destroy(st.registry);
    if (st.display)
        wl_display_disconnect(st.display);

    SDL_GL_DeleteContext(glctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    disable_raw_mode();
    return 0;
}
