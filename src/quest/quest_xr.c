/*
 * OpenXR render thread for the Wing Commander Quest port.  See quest_xr.h.
 */
#include "quest_xr.h"
#include "quest_keyboard_layout.h"
#include "quest_scene.h"

#include <android/log.h>
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "SDL.h"
#include "SDL_system.h"

#define LOG_TAG "WC2VR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* Screen layout (metres, LOCAL space relative to the recentre anchor). */
#define SCREEN_DISTANCE 2.4f
#define SCREEN_WIDTH_M 2.6f
#define SCREEN_HEIGHT_M (SCREEN_WIDTH_M * 3.0f / 4.0f)
#define SCREEN_DROP_M 0.05f
/* 320x200 shown at 4:3 with integer scaling (5x horizontally, 6x vertically). */
#define SCREEN_PIXELS_W 1600
#define SCREEN_PIXELS_H 1200
#define BEAM_WIDTH_M 0.004f
#define BEAM_MAX_M 6.0f
/* On-screen keyboard: tilted panel between the player and the screen. */
#define KEYBOARD_WIDTH_M 1.0f
#define KEYBOARD_HEIGHT_M (KEYBOARD_WIDTH_M * QUEST_KEYBOARD_HEIGHT / QUEST_KEYBOARD_WIDTH)
#define KEYBOARD_DISTANCE 0.9f
#define KEYBOARD_DROP_M 0.5f
#define KEYBOARD_TILT_RAD 0.7f
#define KEY_QUEUE_SIZE 16

typedef struct Vec3 { float x, y, z; } Vec3;

static Vec3 v3(float x, float y, float z) { Vec3 r = { x, y, z }; return r; }
static Vec3 v3add(Vec3 a, Vec3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec3 v3sub(Vec3 a, Vec3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec3 v3scale(Vec3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
static float v3dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 v3cross(Vec3 a, Vec3 b)
{
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
              a.x * b.y - a.y * b.x);
}
static Vec3 v3norm(Vec3 a)
{
    float l = sqrtf(v3dot(a, a));
    return l > 1e-6f ? v3scale(a, 1.0f / l) : v3(0, 0, -1);
}
static Vec3 fromXr(XrVector3f v) { return v3(v.x, v.y, v.z); }
static XrVector3f toXr(Vec3 v) { XrVector3f r = { v.x, v.y, v.z }; return r; }

static Vec3 quatRotate(XrQuaternionf q, Vec3 v)
{
    Vec3 u = v3(q.x, q.y, q.z);
    Vec3 t = v3scale(v3cross(u, v), 2.0f);
    return v3add(v3add(v, v3scale(t, q.w)), v3cross(u, t));
}

static XrQuaternionf quatYaw(float yaw)
{
    XrQuaternionf q = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
    return q;
}

/* Rotation matrix with columns x, y, z -> quaternion. */
static XrQuaternionf quatFromBasis(Vec3 x, Vec3 y, Vec3 z)
{
    XrQuaternionf q;
    float trace = x.x + y.y + z.z;

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f;
        q.w = 0.25f * s;
        q.x = (y.z - z.y) / s;
        q.y = (z.x - x.z) / s;
        q.z = (x.y - y.x) / s;
    } else if (x.x > y.y && x.x > z.z) {
        float s = sqrtf(1.0f + x.x - y.y - z.z) * 2.0f;
        q.w = (y.z - z.y) / s;
        q.x = 0.25f * s;
        q.y = (y.x + x.y) / s;
        q.z = (z.x + x.z) / s;
    } else if (y.y > z.z) {
        float s = sqrtf(1.0f + y.y - x.x - z.z) * 2.0f;
        q.w = (z.x - x.z) / s;
        q.x = (y.x + x.y) / s;
        q.y = 0.25f * s;
        q.z = (z.y + y.z) / s;
    } else {
        float s = sqrtf(1.0f + z.z - x.x - y.y) * 2.0f;
        q.w = (x.y - y.x) / s;
        q.x = (z.x + x.z) / s;
        q.y = (z.y + y.z) / s;
        q.z = 0.25f * s;
    }
    return q;
}

/* ------------------------------------------------------------------ */
/* Shared state between the game thread and the XR thread              */
/* ------------------------------------------------------------------ */

static pthread_t g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_tickCond = PTHREAD_COND_INITIALIZER;
static volatile int g_threadRunning;
static volatile int g_quit;
static unsigned int g_tickCount;

static unsigned char g_frameRgba[QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT * 4];
static unsigned int g_frameSerial;
static unsigned int g_errorColour;
static int g_errorColourSet;

static QuestXrControllers g_controllers;
static QuestXrPointer g_pointer;
static volatile int g_piloting;

static float g_hapticAmplitude[2];
static float g_hapticSeconds[2];

static unsigned char *g_kbImage;      /* premultiplied RGBA, set once */
static unsigned char *g_kbScratch;
static volatile int g_kbVisible;
static volatile int g_kbShift;
static int g_kbUploadedShift = -1;
static int g_kbQueue[KEY_QUEUE_SIZE];
static int g_kbQueueHead, g_kbQueueTail;

/* Stereo scene (game thread -> XR thread, under g_lock). */
typedef struct AtlasUpload {
    struct AtlasUpload *next;
    int layer, x, y, width, height;
    unsigned char *pixels;
} AtlasUpload;

static int g_stereoMode;
static unsigned int g_sceneSerial;
static QuestSceneHeader g_sceneHeader;
static QuestSceneQuad g_sceneQuads[QUEST_SCENE_MAX_QUADS];
static unsigned char g_sceneHud[QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT * 4];
static unsigned char g_sceneDash[QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT * 4];
static unsigned char g_scenePalette[256 * 4];
static AtlasUpload *g_uploadHead, *g_uploadTail;

/* ------------------------------------------------------------------ */
/* XR thread state                                                      */
/* ------------------------------------------------------------------ */

typedef struct Swapchain {
    XrSwapchain handle;
    int width, height;
    uint32_t count;
    XrSwapchainImageOpenGLESKHR images[8];
    GLuint framebuffers[8];
} Swapchain;

static JavaVM *g_vm;
static jobject g_activity;

static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLConfig g_eglConfig;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;

static XrInstance g_instance = XR_NULL_HANDLE;
static XrSystemId g_system = XR_NULL_SYSTEM_ID;
static XrSession g_session = XR_NULL_HANDLE;
static XrSpace g_localSpace = XR_NULL_HANDLE;
static XrSpace g_viewSpace = XR_NULL_HANDLE;
static XrSessionState g_sessionState = XR_SESSION_STATE_UNKNOWN;
static int g_sessionRunning;
static int g_hasRefreshRateExt;

static XrActionSet g_actionSet;
static XrAction g_stickXAction, g_stickYAction;
static XrAction g_aimAction, g_triggerAction, g_gripAction, g_stickAction,
    g_stickClickAction, g_aAction, g_bAction, g_xAction, g_yAction,
    g_menuAction, g_hapticAction;
static XrPath g_handPath[2];
static XrSpace g_aimSpace[2];

static Swapchain g_screenChain;
static Swapchain g_beamChain;
static Swapchain g_kbChain;
static int g_kbHover = -1;
static int g_kbUploadedHover = -2;
static int g_kbPrevTrigger;
static int g_srgb;
static GLuint g_frameTexture;
static GLuint g_frameReadFbo;
static unsigned int g_uploadedSerial = ~0U;

/* XR-thread copies and GL objects for stereo frames. */
static int g_xrStereo;
static unsigned int g_xrSceneSerial = ~0U;
static QuestSceneHeader g_xrHeader;
static QuestSceneQuad g_xrQuads[QUEST_SCENE_MAX_QUADS];
static unsigned char g_xrHud[QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT * 4];
static unsigned char g_xrDash[QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT * 4];
static unsigned char g_xrPalette[256 * 4];
static Swapchain g_eyeChain[2];
static Swapchain g_hudChain, g_dashChain;
static GLuint g_atlasTexture, g_paletteTexture;
static GLuint g_hudTexture, g_dashTexture, g_hudReadFbo, g_dashReadFbo;
static GLuint g_spriteProgram, g_starProgram;
static GLint g_spriteViewProj, g_spriteAtlas, g_spritePalette;
static GLint g_starViewProj, g_starRot;
static GLuint g_spriteVao, g_spriteVbo, g_starVao, g_starVbo;
static int g_starCount;
static XrView g_views[2];

/* 3D cockpit shell: the dashboard art projected from the eye point onto a
 * curved surface (console toward the lap, canopy frame arching away). */
#define CP_STEP 4
#define CP_WRAP_X 260
#define CP_WRAP_Y 140
static GLuint g_cockpitProgram, g_cockpitVao, g_cockpitVbo, g_cockpitIbo;
static GLint g_cockpitViewProj, g_cockpitTex;
static int g_cockpitIndexCount;
static int g_cockpitBuilt;
static QuestSceneHeader g_cockpitBuiltFor;

/* Recentre anchor: head position + yaw. */
static int g_anchorValid;
static Vec3 g_anchorPos;
static float g_anchorYaw;
static int g_recentreRequested = 1;
static double g_recentreHoldStart;
static int g_aimHand = 1;

static double NowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

#define XR_CHECK(call)                                                   \
    do {                                                                 \
        XrResult xr_check_result = (call);                               \
        if (XR_FAILED(xr_check_result)) {                                \
            LOGE("%s failed: %d (%s:%d)", #call, (int)xr_check_result,   \
                 __FILE__, __LINE__);                                    \
            return 0;                                                    \
        }                                                                \
    } while (0)

/* ------------------------------------------------------------------ */
/* EGL                                                                  */
/* ------------------------------------------------------------------ */

static int InitEgl(void)
{
    static const EGLint configAttribs[] = {
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 0, EGL_STENCIL_SIZE, 0,
        EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT_KHR */,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE
    };
    static const EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE
    };
    static const EGLint surfaceAttribs[] = {
        EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE
    };
    EGLint count = 0;

    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY ||
        !eglInitialize(g_eglDisplay, 0, 0)) {
        LOGE("eglInitialize failed");
        return 0;
    }
    if (!eglChooseConfig(g_eglDisplay, configAttribs, &g_eglConfig, 1,
                         &count) || count < 1) {
        LOGE("eglChooseConfig failed");
        return 0;
    }
    g_eglContext = eglCreateContext(g_eglDisplay, g_eglConfig,
                                    EGL_NO_CONTEXT, contextAttribs);
    if (g_eglContext == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return 0;
    }
    g_eglSurface = eglCreatePbufferSurface(g_eglDisplay, g_eglConfig,
                                           surfaceAttribs);
    if (g_eglSurface == EGL_NO_SURFACE ||
        !eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface,
                        g_eglContext)) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return 0;
    }
    LOGI("GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    return 1;
}

static void ShutdownEgl(void)
{
    if (g_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (g_eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(g_eglDisplay, g_eglSurface);
        if (g_eglContext != EGL_NO_CONTEXT)
            eglDestroyContext(g_eglDisplay, g_eglContext);
        eglTerminate(g_eglDisplay);
    }
    g_eglDisplay = EGL_NO_DISPLAY;
    g_eglContext = EGL_NO_CONTEXT;
    g_eglSurface = EGL_NO_SURFACE;
}

/* ------------------------------------------------------------------ */
/* OpenXR setup                                                         */
/* ------------------------------------------------------------------ */

static int HasExtension(const XrExtensionProperties *props, uint32_t count,
                        const char *name)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        if (strcmp(props[i].extensionName, name) == 0)
            return 1;
    return 0;
}

static int InitInstance(void)
{
    PFN_xrInitializeLoaderKHR initializeLoader = 0;
    XrLoaderInitInfoAndroidKHR loaderInfo;
    XrInstanceCreateInfoAndroidKHR androidInfo;
    XrInstanceCreateInfo createInfo;
    XrExtensionProperties *props;
    uint32_t extCount = 0;
    const char *extensions[4];
    uint32_t enabled = 0;
    XrSystemGetInfo systemInfo;

    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                          (PFN_xrVoidFunction *)&initializeLoader);
    if (initializeLoader != 0) {
        memset(&loaderInfo, 0, sizeof(loaderInfo));
        loaderInfo.type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
        loaderInfo.applicationVM = g_vm;
        loaderInfo.applicationContext = g_activity;
        initializeLoader((const XrLoaderInitInfoBaseHeaderKHR *)&loaderInfo);
    }

    XR_CHECK(xrEnumerateInstanceExtensionProperties(0, 0, &extCount, 0));
    props = (XrExtensionProperties *)calloc(extCount ? extCount : 1,
                                            sizeof(*props));
    {
        uint32_t i;
        for (i = 0; i < extCount; i++)
            props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    xrEnumerateInstanceExtensionProperties(0, extCount, &extCount, props);
    if (!HasExtension(props, extCount, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME)) {
        LOGE("runtime lacks XR_KHR_opengl_es_enable");
        free(props);
        return 0;
    }
    extensions[enabled++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    if (HasExtension(props, extCount,
                     XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
        extensions[enabled++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    if (HasExtension(props, extCount,
                     XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
        extensions[enabled++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
        g_hasRefreshRateExt = 1;
    }
    free(props);

    memset(&androidInfo, 0, sizeof(androidInfo));
    androidInfo.type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM = g_vm;
    androidInfo.applicationActivity = g_activity;

    memset(&createInfo, 0, sizeof(createInfo));
    createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    createInfo.next = &androidInfo;
    strcpy(createInfo.applicationInfo.applicationName, "Wing Commander II VR");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy(createInfo.applicationInfo.engineName, "wc1-re");
    createInfo.applicationInfo.engineVersion = 1;
    createInfo.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    createInfo.enabledExtensionCount = enabled;
    createInfo.enabledExtensionNames = extensions;
    XR_CHECK(xrCreateInstance(&createInfo, &g_instance));

    memset(&systemInfo, 0, sizeof(systemInfo));
    systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(g_instance, &systemInfo, &g_system));
    return 1;
}

static XrPath MakePath(const char *text)
{
    XrPath path = XR_NULL_PATH;
    xrStringToPath(g_instance, text, &path);
    return path;
}

static int CreateAction(XrAction *action, const char *name,
                        XrActionType type, int perHand)
{
    XrActionCreateInfo info;

    memset(&info, 0, sizeof(info));
    info.type = XR_TYPE_ACTION_CREATE_INFO;
    info.actionType = type;
    strcpy(info.actionName, name);
    strcpy(info.localizedActionName, name);
    if (perHand) {
        info.countSubactionPaths = 2;
        info.subactionPaths = g_handPath;
    }
    XR_CHECK(xrCreateAction(g_actionSet, &info, action));
    return 1;
}

static int InitActions(void)
{
    XrActionSetCreateInfo setInfo;
    XrActionSuggestedBinding bindings[32];
    XrInteractionProfileSuggestedBinding suggested;
    XrSessionActionSetsAttachInfo attach;
    int n = 0;
    int hand;

    memset(&setInfo, 0, sizeof(setInfo));
    setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
    strcpy(setInfo.actionSetName, "wc2");
    strcpy(setInfo.localizedActionSetName, "Wing Commander");
    XR_CHECK(xrCreateActionSet(g_instance, &setInfo, &g_actionSet));

    g_handPath[0] = MakePath("/user/hand/left");
    g_handPath[1] = MakePath("/user/hand/right");

    if (!CreateAction(&g_aimAction, "aim", XR_ACTION_TYPE_POSE_INPUT, 1) ||
        !CreateAction(&g_triggerAction, "trigger",
                      XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
        !CreateAction(&g_gripAction, "grip", XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
        !CreateAction(&g_stickAction, "stick",
                      XR_ACTION_TYPE_VECTOR2F_INPUT, 1) ||
        !CreateAction(&g_stickXAction, "stick_x",
                      XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
        !CreateAction(&g_stickYAction, "stick_y",
                      XR_ACTION_TYPE_FLOAT_INPUT, 1) ||
        !CreateAction(&g_stickClickAction, "stick_click",
                      XR_ACTION_TYPE_BOOLEAN_INPUT, 1) ||
        !CreateAction(&g_aAction, "a", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !CreateAction(&g_bAction, "b", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !CreateAction(&g_xAction, "x", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !CreateAction(&g_yAction, "y", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !CreateAction(&g_menuAction, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, 0) ||
        !CreateAction(&g_hapticAction, "haptic",
                      XR_ACTION_TYPE_VIBRATION_OUTPUT, 1))
        return 0;

#define BIND(xr_action, xr_path)                              \
    do {                                                      \
        bindings[n].action = (xr_action);                     \
        bindings[n].binding = MakePath(xr_path);              \
        n++;                                                  \
    } while (0)
    BIND(g_aimAction, "/user/hand/left/input/aim/pose");
    BIND(g_aimAction, "/user/hand/right/input/aim/pose");
    BIND(g_triggerAction, "/user/hand/left/input/trigger/value");
    BIND(g_triggerAction, "/user/hand/right/input/trigger/value");
    BIND(g_gripAction, "/user/hand/left/input/squeeze/value");
    BIND(g_gripAction, "/user/hand/right/input/squeeze/value");
    BIND(g_stickAction, "/user/hand/left/input/thumbstick");
    BIND(g_stickAction, "/user/hand/right/input/thumbstick");
    BIND(g_stickXAction, "/user/hand/left/input/thumbstick/x");
    BIND(g_stickXAction, "/user/hand/right/input/thumbstick/x");
    BIND(g_stickYAction, "/user/hand/left/input/thumbstick/y");
    BIND(g_stickYAction, "/user/hand/right/input/thumbstick/y");
    BIND(g_stickClickAction, "/user/hand/left/input/thumbstick/click");
    BIND(g_stickClickAction, "/user/hand/right/input/thumbstick/click");
    BIND(g_aAction, "/user/hand/right/input/a/click");
    BIND(g_bAction, "/user/hand/right/input/b/click");
    BIND(g_xAction, "/user/hand/left/input/x/click");
    BIND(g_yAction, "/user/hand/left/input/y/click");
    BIND(g_menuAction, "/user/hand/left/input/menu/click");
    BIND(g_hapticAction, "/user/hand/left/output/haptic");
    BIND(g_hapticAction, "/user/hand/right/output/haptic");
#undef BIND

    memset(&suggested, 0, sizeof(suggested));
    suggested.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    suggested.interactionProfile =
        MakePath("/interaction_profiles/oculus/touch_controller");
    suggested.countSuggestedBindings = (uint32_t)n;
    suggested.suggestedBindings = bindings;
    XR_CHECK(xrSuggestInteractionProfileBindings(g_instance, &suggested));

    for (hand = 0; hand < 2; hand++) {
        XrActionSpaceCreateInfo spaceInfo;

        memset(&spaceInfo, 0, sizeof(spaceInfo));
        spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        spaceInfo.action = g_aimAction;
        spaceInfo.subactionPath = g_handPath[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace(g_session, &spaceInfo,
                                     &g_aimSpace[hand]));
    }

    memset(&attach, 0, sizeof(attach));
    attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets = &g_actionSet;
    XR_CHECK(xrAttachSessionActionSets(g_session, &attach));
    return 1;
}

static int CreateSwapchain(Swapchain *chain, int width, int height,
                           int64_t format)
{
    XrSwapchainCreateInfo info;
    uint32_t i;

    memset(chain, 0, sizeof(*chain));
    memset(&info, 0, sizeof(info));
    info.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                      XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                      XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(g_session, &info, &chain->handle));
    chain->width = width;
    chain->height = height;
    XR_CHECK(xrEnumerateSwapchainImages(chain->handle, 0, &chain->count, 0));
    if (chain->count > 8)
        chain->count = 8;
    for (i = 0; i < chain->count; i++)
        chain->images[i].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
    XR_CHECK(xrEnumerateSwapchainImages(
        chain->handle, chain->count, &chain->count,
        (XrSwapchainImageBaseHeader *)chain->images));
    glGenFramebuffers((GLsizei)chain->count, chain->framebuffers);
    for (i = 0; i < chain->count; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffers[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, chain->images[i].image, 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 1;
}

static int AcquireImage(Swapchain *chain, uint32_t *index)
{
    XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };

    XR_CHECK(xrAcquireSwapchainImage(chain->handle, &acquire, index));
    wait.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(chain->handle, &wait));
    return 1;
}

static void ReleaseImage(Swapchain *chain)
{
    XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    xrReleaseSwapchainImage(chain->handle, &release);
}


/* ------------------------------------------------------------------ */
/* Stereo renderer                                                      */
/* ------------------------------------------------------------------ */

static const char g_spriteVs[] =
    "#version 300 es\n"
    "uniform mat4 u_viewProj;\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_tex;\n"
    "out vec3 v_tex;\n"
    "void main() {\n"
    "    v_tex = a_tex;\n"
    "    gl_Position = u_viewProj * vec4(a_pos, 1.0);\n"
    "}\n";
static const char g_spriteFs[] =
    "#version 300 es\n"
    "precision highp float;\n"
    "precision highp int;\n"
    "precision highp sampler2DArray;\n"
    "uniform sampler2DArray u_atlas;\n"
    "uniform sampler2D u_palette;\n"
    "in vec3 v_tex;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "    ivec3 c = ivec3(int(floor(v_tex.x)), int(floor(v_tex.y)),\n"
    "                    int(v_tex.z + 0.5));\n"
    "    int index = int(texelFetch(u_atlas, c, 0).r * 255.0 + 0.5);\n"
    "    if (index == 255) discard;\n"
    "    o_colour = vec4(texelFetch(u_palette, ivec2(index, 0), 0).rgb, 1.0);\n"
    "}\n";
static const char g_starVs[] =
    "#version 300 es\n"
    "uniform mat4 u_viewProj;\n"
    "uniform mat3 u_rot;\n"
    "layout(location = 0) in vec4 a_star;\n"
    "out float v_bright;\n"
    "void main() {\n"
    "    vec3 d = normalize(u_rot * a_star.xyz);\n"
    "    v_bright = a_star.w;\n"
    "    gl_PointSize = a_star.w > 0.8 ? 3.0 : 2.0;\n"
    "    gl_Position = u_viewProj * vec4(d * 50000.0, 1.0);\n"
    "}\n";
static const char g_starFs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in float v_bright;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "    float b = v_bright * v_bright;\n"
    "    o_colour = vec4(b * 0.9, b * 0.92, b, 1.0);\n"
    "}\n";

static const char g_cockpitVs[] =
    "#version 300 es\n"
    "uniform mat4 u_viewProj;\n"
    "layout(location = 0) in vec3 a_pos;\n"
    "layout(location = 1) in vec3 a_tex;\n"
    "out vec2 v_uv;\n"
    "out float v_shade;\n"
    "void main() {\n"
    "    v_uv = a_tex.xy;\n"
    "    v_shade = a_tex.z;\n"
    "    gl_Position = u_viewProj * vec4(a_pos, 1.0);\n"
    "}\n";
static const char g_cockpitFs[] =
    "#version 300 es\n"
    "precision mediump float;\n"
    "uniform sampler2D u_tex;\n"
    "in vec2 v_uv;\n"
    "in float v_shade;\n"
    "out vec4 o_colour;\n"
    "void main() {\n"
    "    ivec2 c = clamp(ivec2(floor(v_uv)), ivec2(0), ivec2(319, 199));\n"
    "    vec4 t = texelFetch(u_tex, c, 0);\n"
    "    if (t.a < 0.5) discard;\n"
    "    o_colour = vec4(t.rgb * v_shade, 1.0);\n"
    "}\n";

static GLuint CompileProgram(const char *vs, const char *fs)
{
    GLuint shaders[2];
    const char *sources[2] = { vs, fs };
    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLuint program = glCreateProgram();
    GLint ok = 0;
    char log[1024];
    int i;

    for (i = 0; i < 2; i++) {
        shaders[i] = glCreateShader(types[i]);
        glShaderSource(shaders[i], 1, &sources[i], 0);
        glCompileShader(shaders[i]);
        glGetShaderiv(shaders[i], GL_COMPILE_STATUS, &ok);
        if (!ok) {
            glGetShaderInfoLog(shaders[i], sizeof(log), 0, log);
            LOGE("shader compile failed: %s", log);
            return 0;
        }
        glAttachShader(program, shaders[i]);
    }
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        glGetProgramInfoLog(program, sizeof(log), 0, log);
        LOGE("program link failed: %s", log);
        return 0;
    }
    glDeleteShader(shaders[0]);
    glDeleteShader(shaders[1]);
    return program;
}

static GLuint MakePanelTexture(GLuint *readFbo)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, g_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                   QUEST_FRAME_WIDTH, QUEST_FRAME_HEIGHT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, readFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, *readFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return texture;
}

static int InitStereo(int64_t format)
{
    XrViewConfigurationView viewConfigs[2];
    uint32_t count = 0;
    unsigned char *blank;
    float *stars;
    unsigned int seed = 0x5eed1234U;
    int i;

    memset(viewConfigs, 0, sizeof(viewConfigs));
    viewConfigs[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    viewConfigs[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    XR_CHECK(xrEnumerateViewConfigurationViews(
        g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2,
        &count, viewConfigs));
    for (i = 0; i < 2; i++) {
        int w = (int)viewConfigs[i].recommendedImageRectWidth;
        int h = (int)viewConfigs[i].recommendedImageRectHeight;
        if (!CreateSwapchain(&g_eyeChain[i], w, h, format))
            return 0;
        g_views[i].type = XR_TYPE_VIEW;
    }
    LOGI("eye swapchains %dx%d", g_eyeChain[0].width, g_eyeChain[0].height);

    g_spriteProgram = CompileProgram(g_spriteVs, g_spriteFs);
    g_starProgram = CompileProgram(g_starVs, g_starFs);
    g_cockpitProgram = CompileProgram(g_cockpitVs, g_cockpitFs);
    if (g_spriteProgram == 0 || g_starProgram == 0 || g_cockpitProgram == 0)
        return 0;
    g_cockpitViewProj = glGetUniformLocation(g_cockpitProgram, "u_viewProj");
    g_cockpitTex = glGetUniformLocation(g_cockpitProgram, "u_tex");
    g_spriteViewProj = glGetUniformLocation(g_spriteProgram, "u_viewProj");
    g_spriteAtlas = glGetUniformLocation(g_spriteProgram, "u_atlas");
    g_spritePalette = glGetUniformLocation(g_spriteProgram, "u_palette");
    g_starViewProj = glGetUniformLocation(g_starProgram, "u_viewProj");
    g_starRot = glGetUniformLocation(g_starProgram, "u_rot");

    /* Sprite atlas: indexed pixels, 255 = transparent. */
    glGenTextures(1, &g_atlasTexture);
    glBindTexture(GL_TEXTURE_2D_ARRAY, g_atlasTexture);
    glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R8, QUEST_ATLAS_SIZE,
                   QUEST_ATLAS_SIZE, QUEST_ATLAS_LAYERS);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    blank = (unsigned char *)malloc(QUEST_ATLAS_SIZE * QUEST_ATLAS_SIZE);
    if (blank == 0)
        return 0;
    memset(blank, 0xff, QUEST_ATLAS_SIZE * QUEST_ATLAS_SIZE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (i = 0; i < QUEST_ATLAS_LAYERS; i++)
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, QUEST_ATLAS_SIZE,
                        QUEST_ATLAS_SIZE, 1, GL_RED, GL_UNSIGNED_BYTE, blank);
    free(blank);

    /* Palette: sRGB so the sRGB eye images reproduce the game colours. */
    glGenTextures(1, &g_paletteTexture);
    glBindTexture(GL_TEXTURE_2D, g_paletteTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, g_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, 256,
                   1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    g_hudTexture = MakePanelTexture(&g_hudReadFbo);
    g_dashTexture = MakePanelTexture(&g_dashReadFbo);

    glGenVertexArrays(1, &g_spriteVao);
    glGenBuffers(1, &g_spriteVbo);
    glBindVertexArray(g_spriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_spriteVbo);
    glBufferData(GL_ARRAY_BUFFER, QUEST_SCENE_MAX_QUADS * 6 * 6 * sizeof(float),
                 0, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (const void *)(3 * sizeof(float)));

    /* Star sphere, fixed in the game world. */
    g_starCount = 2500;
    stars = (float *)malloc((size_t)g_starCount * 4 * sizeof(float));
    if (stars == 0)
        return 0;
    for (i = 0; i < g_starCount; i++) {
        float x, y, z, l, b;
        do {
            seed = seed * 1664525U + 1013904223U;
            x = (float)((seed >> 8) & 0xffff) / 32768.0f - 1.0f;
            seed = seed * 1664525U + 1013904223U;
            y = (float)((seed >> 8) & 0xffff) / 32768.0f - 1.0f;
            seed = seed * 1664525U + 1013904223U;
            z = (float)((seed >> 8) & 0xffff) / 32768.0f - 1.0f;
            l = x * x + y * y + z * z;
        } while (l > 1.0f || l < 0.01f);
        l = sqrtf(l);
        seed = seed * 1664525U + 1013904223U;
        b = (float)((seed >> 8) & 0xffff) / 65536.0f;
        stars[i * 4 + 0] = x / l;
        stars[i * 4 + 1] = y / l;
        stars[i * 4 + 2] = z / l;
        stars[i * 4 + 3] = 0.35f + 0.65f * b * b * b;
    }
    glGenVertexArrays(1, &g_starVao);
    glGenBuffers(1, &g_starVbo);
    glBindVertexArray(g_starVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_starVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)g_starCount * 4 * sizeof(float),
                 stars, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), 0);
    glBindVertexArray(0);
    free(stars);
    return 1;
}

/* Column-major 4x4 helpers. */
static void MatMul(float *out, const float *a, const float *b)
{
    float r[16];
    int c, row, k;

    for (c = 0; c < 4; c++)
        for (row = 0; row < 4; row++) {
            float s = 0.0f;
            for (k = 0; k < 4; k++)
                s += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = s;
        }
    memcpy(out, r, sizeof(r));
}

static void MatFromPose(float *m, Vec3 pos, XrQuaternionf q)
{
    float x = q.x, y = q.y, z = q.z, w = q.w;

    m[0] = 1 - 2 * (y * y + z * z);
    m[1] = 2 * (x * y + z * w);
    m[2] = 2 * (x * z - y * w);
    m[3] = 0;
    m[4] = 2 * (x * y - z * w);
    m[5] = 1 - 2 * (x * x + z * z);
    m[6] = 2 * (y * z + x * w);
    m[7] = 0;
    m[8] = 2 * (x * z + y * w);
    m[9] = 2 * (y * z - x * w);
    m[10] = 1 - 2 * (x * x + y * y);
    m[11] = 0;
    m[12] = pos.x;
    m[13] = pos.y;
    m[14] = pos.z;
    m[15] = 1;
}

static void MatInverseRigid(float *out, const float *m)
{
    int i, j;

    for (i = 0; i < 3; i++)
        for (j = 0; j < 3; j++)
            out[i * 4 + j] = m[j * 4 + i];
    out[3] = out[7] = out[11] = 0.0f;
    for (i = 0; i < 3; i++)
        out[12 + i] = -(out[i] * m[12] + out[4 + i] * m[13] +
                        out[8 + i] * m[14]);
    out[15] = 1.0f;
}

static void MatProjection(float *m, XrFovf fov, float nearZ, float farZ)
{
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float u = tanf(fov.angleUp), d = tanf(fov.angleDown);
    float w = r - l, h = u - d;

    memset(m, 0, 16 * sizeof(float));
    m[0] = 2.0f / w;
    m[5] = 2.0f / h;
    m[8] = (r + l) / w;
    m[9] = (u + d) / h;
    m[10] = -(farZ + nearZ) / (farZ - nearZ);
    m[11] = -1.0f;
    m[14] = -(2.0f * farZ * nearZ) / (farZ - nearZ);
}


static float Clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Forward distance (metres) of the cockpit surface behind frame pixel
 * (px, py): the console tilts from its top edge toward the pilot's lap, the
 * canopy frame arches away above it, and the sides curve in like a tub. */
static float CockpitDepth(const QuestSceneHeader *h, float px, float py)
{
    float d = h->dashDistance;
    float zTop = 0.85f * d, zBottom = 0.5f * d, zCanopy = 1.15f * d;
    int column = (int)Clampf(px, 0.0f, (float)(QUEST_FRAME_WIDTH - 1));
    float top = (float)h->consoleTop[column];
    float nx = (px - (float)QUEST_FRAME_WIDTH * 0.5f) /
               ((float)QUEST_FRAME_WIDTH * 0.5f);
    float z;

    if (py >= top) {
        float span = (float)QUEST_FRAME_HEIGHT - top;
        float s = (py - top) / (span < 8.0f ? 8.0f : span);
        z = zTop + (zBottom - zTop) * s;
    } else {
        float s = (top - py) / (top < 8.0f ? 8.0f : top);
        z = zTop + (zCanopy - zTop) * s;
    }
    return z * (1.0f - 0.2f * nx * nx);
}

/* Vertex for virtual frame pixel (px, py).  Inside the 320x200 frame the
 * vertex lies exactly on the pixel's view ray, so from the default head
 * position the shell looks identical to the 2D cockpit.  Outside, the edge
 * point is swung around the eye (wrap-around), darkening with angle. */
static void CockpitVertex(const QuestSceneHeader *h, float px, float py,
                          float *out)
{
    float cx = Clampf(px, 0.0f, (float)QUEST_FRAME_WIDTH);
    float cy = Clampf(py, 0.0f, (float)QUEST_FRAME_HEIGHT);
    float z = CockpitDepth(h, cx, cy);
    float u = (cx - h->centreX) / h->focal;
    float v = (cy - h->centreY) * h->stretch / h->focal;
    float x = u * z, y = -v * z, w = -z;
    float ax = (px - cx) / h->focal / (1.0f + u * u);
    float ay = (py - cy) * h->stretch / h->focal / (1.0f + v * v);
    float angle = sqrtf(ax * ax + ay * ay);
    float c, s, ny, nz, nx;

    /* Pitch: overflow below the frame swings down toward the lap. */
    c = cosf(-ay);
    s = sinf(-ay);
    ny = y * c - w * s;
    nz = y * s + w * c;
    y = ny;
    w = nz;
    /* Yaw: overflow right of the frame swings to the right. */
    c = cosf(-ax);
    s = sinf(-ax);
    nx = x * c + w * s;
    nz = -x * s + w * c;
    x = nx;
    w = nz;

    out[0] = x;
    out[1] = y;
    out[2] = w;
    out[3] = Clampf(px, 0.0f, (float)QUEST_FRAME_WIDTH - 0.5f);
    out[4] = Clampf(py, 0.0f, (float)QUEST_FRAME_HEIGHT - 0.5f);
    out[5] = angle > 0.0f ? Clampf(0.85f - 0.75f * angle, 0.3f, 0.85f) : 1.0f;
}

static int CockpitNeedsRebuild(const QuestSceneHeader *h)
{
    const QuestSceneHeader *b = &g_cockpitBuiltFor;

    return !g_cockpitBuilt || b->centreX != h->centreX ||
           b->centreY != h->centreY || b->focal != h->focal ||
           b->stretch != h->stretch || b->dashDistance != h->dashDistance ||
           b->cockpitWrap != h->cockpitWrap ||
           memcmp(b->consoleTop, h->consoleTop, sizeof(h->consoleTop)) != 0;
}

static void BuildCockpitMesh(const QuestSceneHeader *h)
{
    int wrapX = h->cockpitWrap ? CP_WRAP_X : 0;
    int wrapY = h->cockpitWrap ? CP_WRAP_Y : 0;
    int cols = (QUEST_FRAME_WIDTH + 2 * wrapX) / CP_STEP + 1;
    int rows = (QUEST_FRAME_HEIGHT + 2 * wrapY) / CP_STEP + 1;
    float *vertices;
    unsigned short *indices;
    int r, c, n = 0;

    if (cols * rows > 65535)
        return;
    vertices = (float *)malloc((size_t)cols * rows * 6 * sizeof(float));
    indices = (unsigned short *)malloc((size_t)(cols - 1) * (rows - 1) * 6 *
                                       sizeof(unsigned short));
    if (vertices == 0 || indices == 0) {
        free(vertices);
        free(indices);
        return;
    }
    for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
            CockpitVertex(h, (float)(c * CP_STEP - wrapX),
                          (float)(r * CP_STEP - wrapY),
                          vertices + (r * cols + c) * 6);
    for (r = 0; r + 1 < rows; r++)
        for (c = 0; c + 1 < cols; c++) {
            unsigned short a = (unsigned short)(r * cols + c);
            unsigned short b = (unsigned short)(a + 1);
            unsigned short d = (unsigned short)(a + cols);
            unsigned short e = (unsigned short)(d + 1);
            indices[n++] = a;
            indices[n++] = d;
            indices[n++] = b;
            indices[n++] = b;
            indices[n++] = d;
            indices[n++] = e;
        }
    if (g_cockpitVao == 0) {
        glGenVertexArrays(1, &g_cockpitVao);
        glGenBuffers(1, &g_cockpitVbo);
        glGenBuffers(1, &g_cockpitIbo);
    }
    glBindVertexArray(g_cockpitVao);
    glBindBuffer(GL_ARRAY_BUFFER, g_cockpitVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)cols * rows * 6 * sizeof(float),
                 vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), 0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_cockpitIbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(unsigned short),
                 indices, GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    free(vertices);
    free(indices);
    g_cockpitIndexCount = n;
    g_cockpitBuiltFor = *h;
    g_cockpitBuilt = 1;
    LOGI("3D cockpit mesh %dx%d (%d triangles), console top at column 160: %d",
         cols, rows, n / 3, (int)h->consoleTop[160]);
}

static void DrawCockpit(const float *viewProj)
{
    if (!g_xrHeader.hasDashboard || !g_xrHeader.cockpit3d ||
        g_cockpitProgram == 0)
        return;
    if (CockpitNeedsRebuild(&g_xrHeader))
        BuildCockpitMesh(&g_xrHeader);
    if (g_cockpitIndexCount == 0)
        return;
    glUseProgram(g_cockpitProgram);
    glUniformMatrix4fv(g_cockpitViewProj, 1, GL_FALSE, viewProj);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_dashTexture);
    glUniform1i(g_cockpitTex, 0);
    glBindVertexArray(g_cockpitVao);
    glDrawElements(GL_TRIANGLES, g_cockpitIndexCount, GL_UNSIGNED_SHORT, 0);
    glBindVertexArray(0);
}

/* Copies the newest stereo scene and performs pending GL uploads. */
static void SyncStereoScene(void)
{
    AtlasUpload *uploads;
    int newScene = 0;

    pthread_mutex_lock(&g_lock);
    g_xrStereo = g_stereoMode;
    uploads = g_uploadHead;
    g_uploadHead = g_uploadTail = 0;
    if (g_sceneSerial != g_xrSceneSerial) {
        g_xrSceneSerial = g_sceneSerial;
        g_xrHeader = g_sceneHeader;
        memcpy(g_xrQuads, g_sceneQuads,
               sizeof(QuestSceneQuad) * (size_t)g_sceneHeader.quadCount);
        memcpy(g_xrHud, g_sceneHud, sizeof(g_xrHud));
        memcpy(g_xrDash, g_sceneDash, sizeof(g_xrDash));
        memcpy(g_xrPalette, g_scenePalette, sizeof(g_xrPalette));
        newScene = 1;
    }
    pthread_mutex_unlock(&g_lock);

    if (uploads != 0) {
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_atlasTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        while (uploads != 0) {
            AtlasUpload *next = uploads->next;
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, uploads->x, uploads->y,
                            uploads->layer, uploads->width, uploads->height, 1,
                            GL_RED, GL_UNSIGNED_BYTE, uploads->pixels);
            free(uploads->pixels);
            free(uploads);
            uploads = next;
        }
    }
    if (newScene) {
        unsigned char rgba[256 * 4];
        int i;

        for (i = 0; i < 256; i++) {
            rgba[i * 4 + 0] = g_xrPalette[i * 4 + 2];
            rgba[i * 4 + 1] = g_xrPalette[i * 4 + 1];
            rgba[i * 4 + 2] = g_xrPalette[i * 4 + 0];
            rgba[i * 4 + 3] = 255;
        }
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glBindTexture(GL_TEXTURE_2D, g_paletteTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_RGBA,
                        GL_UNSIGNED_BYTE, rgba);
        glBindTexture(GL_TEXTURE_2D, g_hudTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, QUEST_FRAME_WIDTH,
                        QUEST_FRAME_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                        g_xrHud);
        glBindTexture(GL_TEXTURE_2D, g_dashTexture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, QUEST_FRAME_WIDTH,
                        QUEST_FRAME_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                        g_xrDash);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
}

static void DrawEye(int eye, const float *anchorMatrix)
{
    float eyeMatrix[16], view[16], proj[16], viewProj[16];
    float rot[9];
    Swapchain *chain = &g_eyeChain[eye];
    uint32_t index;
    int i, r, c;

    if (!AcquireImage(chain, &index))
        return;
    MatFromPose(eyeMatrix, fromXr(g_views[eye].pose.position),
                g_views[eye].pose.orientation);
    MatInverseRigid(view, eyeMatrix);
    MatMul(view, view, anchorMatrix);
    MatProjection(proj, g_views[eye].fov, 0.05f, 100000.0f);
    MatMul(viewProj, proj, view);

    glBindFramebuffer(GL_FRAMEBUFFER, chain->framebuffers[index]);
    glViewport(0, 0, chain->width, chain->height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* GLSL mat3 is column-major: column c = row c of worldToCockpit^T. */
    for (r = 0; r < 3; r++)
        for (c = 0; c < 3; c++)
            rot[c * 3 + r] = g_xrHeader.worldToCockpit[r][c];
    glUseProgram(g_starProgram);
    glUniformMatrix4fv(g_starViewProj, 1, GL_FALSE, viewProj);
    glUniformMatrix3fv(g_starRot, 1, GL_FALSE, rot);
    glBindVertexArray(g_starVao);
    glDrawArrays(GL_POINTS, 0, g_starCount);

    if (g_xrHeader.quadCount > 0) {
        glUseProgram(g_spriteProgram);
        glUniformMatrix4fv(g_spriteViewProj, 1, GL_FALSE, viewProj);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, g_atlasTexture);
        glUniform1i(g_spriteAtlas, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, g_paletteTexture);
        glUniform1i(g_spritePalette, 1);
        glBindVertexArray(g_spriteVao);
        glDrawArrays(GL_TRIANGLES, 0, g_xrHeader.quadCount * 6);
        glActiveTexture(GL_TEXTURE0);
    }
    /* Nearer than every ship (those sit beyond their collision radius), so
     * drawing last is enough; the shell is star-shaped around the eye. */
    DrawCockpit(viewProj);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    ReleaseImage(chain);
    (void)i;
}

static void UploadSpriteVertices(void)
{
    static float vertices[QUEST_SCENE_MAX_QUADS * 6 * 6];
    static const int order[6] = { 0, 1, 2, 2, 1, 3 };
    int q, v;

    for (q = 0; q < g_xrHeader.quadCount; q++) {
        const QuestSceneQuad *quad = &g_xrQuads[q];
        for (v = 0; v < 6; v++) {
            float *out = vertices + (q * 6 + v) * 6;
            int corner = order[v];
            out[0] = quad->p[corner][0];
            out[1] = quad->p[corner][1];
            out[2] = quad->p[corner][2];
            out[3] = quad->uv[corner][0];
            out[4] = quad->uv[corner][1];
            out[5] = quad->layer;
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, g_spriteVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    (GLsizeiptr)g_xrHeader.quadCount * 6 * 6 * sizeof(float),
                    vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void BlitPanel(Swapchain *chain, GLuint readFbo)
{
    uint32_t index;

    if (!AcquireImage(chain, &index))
        return;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, chain->framebuffers[index]);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
    glBlitFramebuffer(0, 0, QUEST_FRAME_WIDTH, QUEST_FRAME_HEIGHT, 0,
                      SCREEN_PIXELS_H, SCREEN_PIXELS_W, 0, GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    ReleaseImage(chain);
}

static int InitSession(void)
{
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = 0;
    XrGraphicsRequirementsOpenGLESKHR requirements;
    XrGraphicsBindingOpenGLESAndroidKHR binding;
    XrSessionCreateInfo sessionInfo;
    XrReferenceSpaceCreateInfo spaceInfo;
    int64_t formats[128];
    uint32_t formatCount = 0;
    int64_t chosen = GL_RGBA8;
    uint32_t i;
    uint32_t index;

    xrGetInstanceProcAddr(g_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction *)&getRequirements);
    if (getRequirements == 0) {
        LOGE("xrGetOpenGLESGraphicsRequirementsKHR missing");
        return 0;
    }
    memset(&requirements, 0, sizeof(requirements));
    requirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
    XR_CHECK(getRequirements(g_instance, g_system, &requirements));

    memset(&binding, 0, sizeof(binding));
    binding.type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = g_eglDisplay;
    binding.config = g_eglConfig;
    binding.context = g_eglContext;

    memset(&sessionInfo, 0, sizeof(sessionInfo));
    sessionInfo.type = XR_TYPE_SESSION_CREATE_INFO;
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    XR_CHECK(xrCreateSession(g_instance, &sessionInfo, &g_session));

    memset(&spaceInfo, 0, sizeof(spaceInfo));
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(g_session, &spaceInfo, &g_localSpace));
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(g_session, &spaceInfo, &g_viewSpace));

    if (!InitActions())
        return 0;

    XR_CHECK(xrEnumerateSwapchainFormats(g_session, 0, &formatCount, 0));
    if (formatCount > 128)
        formatCount = 128;
    XR_CHECK(xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount,
                                         formats));
    for (i = 0; i < formatCount; i++) {
        if (formats[i] == GL_SRGB8_ALPHA8) {
            chosen = GL_SRGB8_ALPHA8;
            g_srgb = 1;
            break;
        }
    }
    LOGI("swapchain format 0x%llx (srgb=%d)", (long long)chosen, g_srgb);
    if (!CreateSwapchain(&g_screenChain, SCREEN_PIXELS_W, SCREEN_PIXELS_H,
                         chosen) ||
        !CreateSwapchain(&g_beamChain, 16, 16, chosen) ||
        !CreateSwapchain(&g_kbChain, QUEST_KEYBOARD_WIDTH,
                         QUEST_KEYBOARD_HEIGHT, chosen) ||
        !CreateSwapchain(&g_hudChain, SCREEN_PIXELS_W, SCREEN_PIXELS_H,
                         chosen) ||
        !CreateSwapchain(&g_dashChain, SCREEN_PIXELS_W, SCREEN_PIXELS_H,
                         chosen))
        return 0;
    if (!InitStereo(chosen))
        return 0;

    /* The beam image never changes: fill it once. */
    if (AcquireImage(&g_beamChain, &index)) {
        glBindFramebuffer(GL_FRAMEBUFFER, g_beamChain.framebuffers[index]);
        glViewport(0, 0, 16, 16);
        /* Premultiplied light blue. */
        glClearColor(0.25f * 0.85f, 0.65f * 0.85f, 1.0f * 0.85f, 0.85f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        ReleaseImage(&g_beamChain);
    }

    /* Game frame texture: sRGB-encoded palette colours.  With an sRGB
     * swapchain, an sRGB source texture makes the blit an identity copy. */
    glGenTextures(1, &g_frameTexture);
    glBindTexture(GL_TEXTURE_2D, g_frameTexture);
    glTexStorage2D(GL_TEXTURE_2D, 1, g_srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                   QUEST_FRAME_WIDTH, QUEST_FRAME_HEIGHT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &g_frameReadFbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_frameReadFbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_frameTexture, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    return 1;
}

static void RequestRefreshRate(void)
{
    PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = 0;
    PFN_xrRequestDisplayRefreshRateFB requestRate = 0;
    float rates[16];
    uint32_t count = 0;
    float best = 0.0f;
    uint32_t i;

    if (!g_hasRefreshRateExt)
        return;
    xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB",
                          (PFN_xrVoidFunction *)&enumerateRates);
    xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB",
                          (PFN_xrVoidFunction *)&requestRate);
    if (enumerateRates == 0 || requestRate == 0)
        return;
    if (XR_FAILED(enumerateRates(g_session, 16, &count, rates)))
        return;
    for (i = 0; i < count; i++)
        if (rates[i] <= 72.5f && rates[i] > best)
            best = rates[i];
    if (best > 0.0f) {
        requestRate(g_session, best);
        LOGI("display refresh rate %.1f Hz", best);
    }
}

/* ------------------------------------------------------------------ */
/* Per-frame work                                                       */
/* ------------------------------------------------------------------ */

static void HandleEvents(void)
{
    XrEventDataBuffer event;

    for (;;) {
        memset(&event, 0, sizeof(event));
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
        if (xrPollEvent(g_instance, &event) != XR_SUCCESS)
            break;
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const XrEventDataSessionStateChanged *changed =
                (const XrEventDataSessionStateChanged *)&event;
            g_sessionState = changed->state;
            LOGI("session state %d", (int)g_sessionState);
            if (g_sessionState == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo begin;
                memset(&begin, 0, sizeof(begin));
                begin.type = XR_TYPE_SESSION_BEGIN_INFO;
                begin.primaryViewConfigurationType =
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                if (XR_SUCCEEDED(xrBeginSession(g_session, &begin))) {
                    g_sessionRunning = 1;
                    RequestRefreshRate();
                }
            } else if (g_sessionState == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_session);
                g_sessionRunning = 0;
            } else if (g_sessionState == XR_SESSION_STATE_EXITING ||
                       g_sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                g_sessionRunning = 0;
                g_quit = 1;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            g_recentreRequested = 1;
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            g_quit = 1;
            break;
        default:
            break;
        }
    }
}

static float GetFloat(XrAction action, int hand)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };

    info.action = action;
    info.subactionPath = hand >= 0 ? g_handPath[hand] : XR_NULL_PATH;
    if (XR_FAILED(xrGetActionStateFloat(g_session, &info, &state)) ||
        !state.isActive)
        return 0.0f;
    return state.currentState;
}

static int GetBool(XrAction action, int hand)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

    info.action = action;
    info.subactionPath = hand >= 0 ? g_handPath[hand] : XR_NULL_PATH;
    if (XR_FAILED(xrGetActionStateBoolean(g_session, &info, &state)) ||
        !state.isActive)
        return 0;
    return state.currentState ? 1 : 0;
}

static XrResult g_stickResult[2];
static int g_stickActive[2];

static XrVector2f GetVec2(XrAction action, int hand)
{
    XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
    XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };
    XrVector2f zero = { 0.0f, 0.0f };

    info.action = action;
    info.subactionPath = g_handPath[hand];
    g_stickResult[hand] = xrGetActionStateVector2f(g_session, &info, &state);
    g_stickActive[hand] = state.isActive ? 1 : 0;
    if (XR_FAILED(g_stickResult[hand]) || !state.isActive)
        return zero;
    return state.currentState;
}

/* Thumbstick: vector action, with the x/y float components as fallback. */
static XrVector2f GetStick(int hand, XrVector2f *vectorValue,
                           XrVector2f *componentValue)
{
    XrVector2f result;

    *vectorValue = GetVec2(g_stickAction, hand);
    componentValue->x = GetFloat(g_stickXAction, hand);
    componentValue->y = GetFloat(g_stickYAction, hand);
    result = *vectorValue;
    if (result.x == 0.0f && result.y == 0.0f)
        result = *componentValue;
    return result;
}

static void ApplyHaptics(void)
{
    int hand;

    for (hand = 0; hand < 2; hand++) {
        float amplitude;
        float seconds;

        pthread_mutex_lock(&g_lock);
        amplitude = g_hapticAmplitude[hand];
        seconds = g_hapticSeconds[hand];
        g_hapticAmplitude[hand] = 0.0f;
        pthread_mutex_unlock(&g_lock);
        if (amplitude > 0.0f) {
            XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
            XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };

            info.action = g_hapticAction;
            info.subactionPath = g_handPath[hand];
            vibration.amplitude = amplitude;
            vibration.duration = (XrDuration)(seconds * 1e9);
            vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(g_session, &info,
                                  (const XrHapticBaseHeader *)&vibration);
        }
    }
}

static int LocatePose(XrSpace space, XrTime time, XrPosef *pose)
{
    XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };

    if (XR_FAILED(xrLocateSpace(space, g_localSpace, time, &location)))
        return 0;
    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) == 0 ||
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0)
        return 0;
    *pose = location.pose;
    return 1;
}

/* Screen centre and axes in LOCAL space. */
static void ScreenFrame(Vec3 *centre, Vec3 *right, Vec3 *up, Vec3 *normal,
                        XrQuaternionf *orientation)
{
    XrQuaternionf q = quatYaw(g_anchorYaw);

    *orientation = q;
    *right = quatRotate(q, v3(1, 0, 0));
    *up = v3(0, 1, 0);
    *normal = quatRotate(q, v3(0, 0, 1));
    *centre = v3add(g_anchorPos,
                    v3add(quatRotate(q, v3(0, 0, -SCREEN_DISTANCE)),
                          v3(0, -SCREEN_DROP_M, 0)));
}

typedef struct Panel {
    Vec3 centre, right, up, normal;
    float width, height;
    XrQuaternionf q;
} Panel;

/* Cockpit panel: the 320x200 frame at `distance`, subtending exactly the
 * engine's projection (pixel centreX/Y straight ahead). */
static void CockpitPanel(Panel *panel, float distance)
{
    const QuestSceneHeader *h = &g_xrHeader;
    XrQuaternionf q = quatYaw(g_anchorYaw);
    float perPixel = distance / h->focal;
    Vec3 offset;

    offset = v3(((float)QUEST_FRAME_WIDTH * 0.5f - h->centreX) * perPixel,
                -((float)QUEST_FRAME_HEIGHT * 0.5f - h->centreY) * perPixel *
                    h->stretch,
                -distance);
    panel->q = q;
    panel->right = quatRotate(q, v3(1, 0, 0));
    panel->up = v3(0, 1, 0);
    panel->normal = quatRotate(q, v3(0, 0, 1));
    panel->centre = v3add(g_anchorPos, quatRotate(q, offset));
    panel->width = (float)QUEST_FRAME_WIDTH * perPixel;
    panel->height = (float)QUEST_FRAME_HEIGHT * perPixel * h->stretch;
}

static void PointerPanel(Panel *panel)
{
    if (g_xrStereo) {
        CockpitPanel(panel, g_xrHeader.dashDistance);
        return;
    }
    ScreenFrame(&panel->centre, &panel->right, &panel->up, &panel->normal,
                &panel->q);
    panel->width = SCREEN_WIDTH_M;
    panel->height = SCREEN_HEIGHT_M;
}

static void KeyboardFrame(Vec3 *centre, Vec3 *right, Vec3 *up, Vec3 *normal,
                          XrQuaternionf *orientation)
{
    XrQuaternionf yaw = quatYaw(g_anchorYaw);
    XrQuaternionf pitch = { -sinf(KEYBOARD_TILT_RAD * 0.5f), 0.0f, 0.0f,
                            cosf(KEYBOARD_TILT_RAD * 0.5f) };
    XrQuaternionf q;

    /* q = yaw * pitch */
    q.w = yaw.w * pitch.w - yaw.x * pitch.x - yaw.y * pitch.y - yaw.z * pitch.z;
    q.x = yaw.w * pitch.x + yaw.x * pitch.w + yaw.y * pitch.z - yaw.z * pitch.y;
    q.y = yaw.w * pitch.y - yaw.x * pitch.z + yaw.y * pitch.w + yaw.z * pitch.x;
    q.z = yaw.w * pitch.z + yaw.x * pitch.y - yaw.y * pitch.x + yaw.z * pitch.w;
    *orientation = q;
    *right = quatRotate(q, v3(1, 0, 0));
    *up = quatRotate(q, v3(0, 1, 0));
    *normal = quatRotate(q, v3(0, 0, 1));
    *centre = v3add(g_anchorPos,
                    v3add(quatRotate(yaw, v3(0, 0, -KEYBOARD_DISTANCE)),
                          v3(0, -KEYBOARD_DROP_M, 0)));
}

/* Ray vs. rectangle: returns 1 with u,v in 0..1 (v down) and distance t. */
static int RayQuad(Vec3 origin, Vec3 dir, Vec3 centre, Vec3 right, Vec3 up,
                   Vec3 normal, float width, float height, float *u,
                   float *v, float *t)
{
    float denom = v3dot(dir, normal);
    Vec3 rel;

    if (denom > -1e-4f)
        return 0;
    *t = v3dot(v3sub(centre, origin), normal) / denom;
    if (*t <= 0.0f)
        return 0;
    rel = v3sub(v3add(origin, v3scale(dir, *t)), centre);
    *u = v3dot(rel, right) / width + 0.5f;
    *v = 0.5f - v3dot(rel, up) / height;
    return *u >= 0.0f && *u < 1.0f && *v >= 0.0f && *v < 1.0f;
}

static void QueueKeyboardKey(int keycode)
{
    pthread_mutex_lock(&g_lock);
    if ((g_kbQueueTail + 1) % KEY_QUEUE_SIZE != g_kbQueueHead) {
        g_kbQueue[g_kbQueueTail] = keycode;
        g_kbQueueTail = (g_kbQueueTail + 1) % KEY_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&g_lock);
}

static void UpdateInput(XrTime time, XrPosef *headPose, int headValid,
                        XrCompositionLayerQuad *beamLayer, int *beamVisible)
{
    XrActiveActionSet active;
    XrActionsSyncInfo sync;
    QuestXrControllers c;
    QuestXrPointer p;
    XrPosef aim[2];
    int aimValid[2];
    int hand;
    int focused = g_sessionState == XR_SESSION_STATE_FOCUSED;
    int piloting = g_piloting;

    memset(&c, 0, sizeof(c));
    memset(&p, 0, sizeof(p));
    *beamVisible = 0;

    if (focused) {
        memset(&active, 0, sizeof(active));
        active.actionSet = g_actionSet;
        memset(&sync, 0, sizeof(sync));
        sync.type = XR_TYPE_ACTIONS_SYNC_INFO;
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        xrSyncActions(g_session, &sync);

        {
            XrVector2f lv, lc, rv, rc;
            XrVector2f left = GetStick(0, &lv, &lc);
            XrVector2f right = GetStick(1, &rv, &rc);
            c.leftStickX = left.x;
            c.leftStickY = left.y;
            c.rightStickX = right.x;
            c.rightStickY = right.y;
        }
        c.leftTrigger = GetFloat(g_triggerAction, 0);
        c.rightTrigger = GetFloat(g_triggerAction, 1);
        c.leftGrip = GetFloat(g_gripAction, 0);
        c.rightGrip = GetFloat(g_gripAction, 1);
        c.leftStickClick = GetBool(g_stickClickAction, 0);
        c.rightStickClick = GetBool(g_stickClickAction, 1);
        c.a = GetBool(g_aAction, -1);
        c.b = GetBool(g_bAction, -1);
        c.x = GetBool(g_xAction, -1);
        c.y = GetBool(g_yAction, -1);
        c.menu = GetBool(g_menuAction, -1);
        c.active = 1;
        ApplyHaptics();
    }

    /* Recentre: left stick click held for one second (a short tap is the
     * nav map, handled on the game side). */
    if (c.leftStickClick) {
        double now = NowSeconds();
        if (g_recentreHoldStart == 0.0)
            g_recentreHoldStart = now;
        else if (now - g_recentreHoldStart > 1.0 && g_recentreHoldStart > 0.0) {
            g_recentreRequested = 1;
            g_recentreHoldStart = -1.0; /* wait for release */
        }
    } else {
        g_recentreHoldStart = 0.0;
    }
    if (g_recentreRequested && headValid) {
        Vec3 forward = quatRotate(headPose->orientation, v3(0, 0, -1));
        g_anchorPos = fromXr(headPose->position);
        g_anchorYaw = atan2f(-forward.x, -forward.z);
        g_anchorValid = 1;
        g_recentreRequested = 0;
        LOGI("recentred: pos %.2f %.2f %.2f yaw %.2f", g_anchorPos.x,
             g_anchorPos.y, g_anchorPos.z, g_anchorYaw);
    }

    for (hand = 0; hand < 2; hand++)
        aimValid[hand] = focused && LocatePose(g_aimSpace[hand], time,
                                               &aim[hand]);

    /* The hand whose trigger was pressed last aims; the right hand is the
     * default. */
    if (c.leftTrigger > 0.6f && c.rightTrigger < 0.2f)
        g_aimHand = 0;
    else if (c.rightTrigger > 0.6f)
        g_aimHand = 1;
    if (!aimValid[g_aimHand] && aimValid[1 - g_aimHand])
        g_aimHand = 1 - g_aimHand;

    if (!piloting && g_anchorValid && aimValid[g_aimHand]) {
        Vec3 centre, right, up, normal;
        XrQuaternionf screenQ;
        Vec3 origin = fromXr(aim[g_aimHand].position);
        Vec3 dir = quatRotate(aim[g_aimHand].orientation, v3(0, 0, -1));
        float length = BEAM_MAX_M;
        float u, v, t;
        int onKeyboard = 0;
        int trigger = g_aimHand == 1 ? c.rightTrigger > 0.55f
                                     : c.leftTrigger > 0.55f;

        g_kbHover = -1;
        if (g_kbVisible && g_kbImage != 0) {
            KeyboardFrame(&centre, &right, &up, &normal, &screenQ);
            if (RayQuad(origin, dir, centre, right, up, normal,
                        KEYBOARD_WIDTH_M, KEYBOARD_HEIGHT_M, &u, &v, &t)) {
                int px = (int)(u * QUEST_KEYBOARD_WIDTH);
                int py = (int)(v * QUEST_KEYBOARD_HEIGHT);
                int k;

                onKeyboard = 1;
                length = t;
                for (k = 0; k < QUEST_KEYBOARD_KEY_COUNT; k++) {
                    const QuestKeyboardKey *key = &g_questKeyboardKeys[k];
                    if (px >= key->x0 && px < key->x1 && py >= key->y0 &&
                        py < key->y1) {
                        g_kbHover = k;
                        break;
                    }
                }
                if (trigger && !g_kbPrevTrigger && g_kbHover >= 0) {
                    QueueKeyboardKey(g_questKeyboardKeys[g_kbHover].keycode);
                    g_hapticAmplitude[g_aimHand] = 0.4f;
                    g_hapticSeconds[g_aimHand] = 0.03f;
                }
            }
        }
        g_kbPrevTrigger = trigger;
        {
            Panel panel;
            PointerPanel(&panel);
            centre = panel.centre;
            right = panel.right;
            up = panel.up;
            normal = panel.normal;
            screenQ = panel.q;
            if (!onKeyboard &&
                RayQuad(origin, dir, centre, right, up, normal, panel.width,
                        panel.height, &u, &v, &t))
                onKeyboard = 2;
        }
        if (onKeyboard == 2) {
            p.onScreen = 1;
            p.x = (int)(u * QUEST_FRAME_WIDTH);
            p.y = (int)(v * QUEST_FRAME_HEIGHT);
            length = t;
        }
        if (g_aimHand == 1) {
            p.primary = c.rightTrigger > 0.55f;
            p.secondary = c.rightGrip > 0.55f;
        } else {
            p.primary = c.leftTrigger > 0.55f;
            p.secondary = c.leftGrip > 0.55f;
        }

        /* Beam quad: Y along the ray, facing the head. */
        {
            Vec3 mid = v3add(origin, v3scale(dir, length * 0.5f));
            Vec3 toHead = headValid ? v3sub(fromXr(headPose->position), mid)
                                    : v3scale(dir, -1.0f);
            Vec3 z = v3sub(toHead, v3scale(dir, v3dot(toHead, dir)));
            Vec3 x;

            z = v3norm(z);
            x = v3norm(v3cross(dir, z));
            z = v3cross(x, dir);
            memset(beamLayer, 0, sizeof(*beamLayer));
            beamLayer->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
            beamLayer->layerFlags =
                XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            beamLayer->space = g_localSpace;
            beamLayer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            beamLayer->subImage.swapchain = g_beamChain.handle;
            beamLayer->subImage.imageRect.extent.width = 16;
            beamLayer->subImage.imageRect.extent.height = 16;
            beamLayer->pose.position = toXr(mid);
            beamLayer->pose.orientation = quatFromBasis(x, dir, z);
            beamLayer->size.width = BEAM_WIDTH_M;
            beamLayer->size.height = length;
            *beamVisible = 1;
        }
    }

    pthread_mutex_lock(&g_lock);
    g_controllers = c;
    g_pointer = p;
    pthread_mutex_unlock(&g_lock);
}

static void DrawScreen(uint32_t index)
{
    unsigned int serial;

    pthread_mutex_lock(&g_lock);
    serial = g_frameSerial;
    if (serial != g_uploadedSerial && !g_errorColourSet) {
        glBindTexture(GL_TEXTURE_2D, g_frameTexture);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, QUEST_FRAME_WIDTH,
                        QUEST_FRAME_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                        g_frameRgba);
        g_uploadedSerial = serial;
    }
    pthread_mutex_unlock(&g_lock);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_screenChain.framebuffers[index]);
    glViewport(0, 0, SCREEN_PIXELS_W, SCREEN_PIXELS_H);
    if (g_errorColourSet) {
        glClearColor(((g_errorColour >> 16) & 255) / 255.0f,
                     ((g_errorColour >> 8) & 255) / 255.0f,
                     (g_errorColour & 255) / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    } else if (g_uploadedSerial == ~0U) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
    } else {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_frameReadFbo);
        /* Texture row 0 holds the top frame row, but OpenXR treats GL
         * swapchain images with a bottom-left origin: flip vertically. */
        glBlitFramebuffer(0, 0, QUEST_FRAME_WIDTH, QUEST_FRAME_HEIGHT,
                          0, SCREEN_PIXELS_H, SCREEN_PIXELS_W, 0,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

static int DrawKeyboard(XrCompositionLayerQuad *layer)
{
    Vec3 centre, right, up, normal;
    XrQuaternionf q;
    uint32_t index;

    if (!AcquireImage(&g_kbChain, &index))
        return 0;
    /* Swapchain images rotate, so every image is redrawn; the CPU copy is
     * rebuilt only when the hovered key changes. */
    if (g_kbHover != g_kbUploadedHover || g_kbShift != g_kbUploadedShift) {
        int k;

        memcpy(g_kbScratch, g_kbImage,
               QUEST_KEYBOARD_WIDTH * QUEST_KEYBOARD_HEIGHT * 4);
        for (k = 0; g_kbShift && k < QUEST_KEYBOARD_KEY_COUNT; k++) {
            const QuestKeyboardKey *key = &g_questKeyboardKeys[k];
            int x, y;
            if (key->keycode != SDLK_LSHIFT)
                continue;
            for (y = key->y0 + 2; y < key->y1 - 2; y++) {
                unsigned char *row =
                    g_kbScratch + (size_t)(QUEST_KEYBOARD_HEIGHT - 1 - y) *
                                      QUEST_KEYBOARD_WIDTH * 4;
                for (x = key->x0 + 2; x < key->x1 - 2; x++) {
                    unsigned char *px = row + x * 4;
                    px[0] = (unsigned char)(px[0] / 2 + 20);
                    px[1] = (unsigned char)(px[1] / 2 + 110);
                    px[2] = (unsigned char)(px[2] / 2 + 30);
                }
            }
        }
        g_kbUploadedShift = g_kbShift;
        if (g_kbHover >= 0) {
            const QuestKeyboardKey *key = &g_questKeyboardKeys[g_kbHover];
            int x, y;
            for (y = key->y0 + 2; y < key->y1 - 2; y++) {
                unsigned char *row =
                    g_kbScratch + (size_t)(QUEST_KEYBOARD_HEIGHT - 1 - y) *
                                      QUEST_KEYBOARD_WIDTH * 4;
                for (x = key->x0 + 2; x < key->x1 - 2; x++) {
                    unsigned char *px = row + x * 4;
                    px[0] = (unsigned char)(px[0] / 2 + 40);
                    px[1] = (unsigned char)(px[1] / 2 + 100);
                    px[2] = (unsigned char)(px[2] / 2 + 127);
                }
            }
        }
        g_kbUploadedHover = g_kbHover;
    }
    glBindTexture(GL_TEXTURE_2D, g_kbChain.images[index].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, QUEST_KEYBOARD_WIDTH,
                    QUEST_KEYBOARD_HEIGHT, GL_RGBA, GL_UNSIGNED_BYTE,
                    g_kbScratch);
    glBindTexture(GL_TEXTURE_2D, 0);
    ReleaseImage(&g_kbChain);

    KeyboardFrame(&centre, &right, &up, &normal, &q);
    memset(layer, 0, sizeof(*layer));
    layer->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    layer->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer->space = g_localSpace;
    layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer->subImage.swapchain = g_kbChain.handle;
    layer->subImage.imageRect.extent.width = QUEST_KEYBOARD_WIDTH;
    layer->subImage.imageRect.extent.height = QUEST_KEYBOARD_HEIGHT;
    layer->pose.orientation = q;
    layer->pose.position = toXr(centre);
    layer->size.width = KEYBOARD_WIDTH_M;
    layer->size.height = KEYBOARD_HEIGHT_M;
    return 1;
}

static void FillQuadLayer(XrCompositionLayerQuad *layer, Swapchain *chain,
                          const Panel *panel, int alpha)
{
    memset(layer, 0, sizeof(*layer));
    layer->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
    layer->layerFlags = alpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
                              : 0;
    layer->space = g_localSpace;
    layer->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    layer->subImage.swapchain = chain->handle;
    layer->subImage.imageRect.extent.width = chain->width;
    layer->subImage.imageRect.extent.height = chain->height;
    layer->pose.orientation = panel->q;
    layer->pose.position = toXr(panel->centre);
    layer->size.width = panel->width;
    layer->size.height = panel->height;
}

static int LocateEyes(XrTime time)
{
    XrViewLocateInfo info = { XR_TYPE_VIEW_LOCATE_INFO };
    XrViewState state = { XR_TYPE_VIEW_STATE };
    uint32_t count = 0;

    info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    info.displayTime = time;
    info.space = g_localSpace;
    g_views[0].type = XR_TYPE_VIEW;
    g_views[1].type = XR_TYPE_VIEW;
    if (XR_FAILED(xrLocateViews(g_session, &info, &state, 2, &count,
                                g_views)) || count != 2)
        return 0;
    return (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
}

static void RunFrame(void)
{
    XrFrameWaitInfo waitInfo = { XR_TYPE_FRAME_WAIT_INFO };
    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    XrFrameBeginInfo beginInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
    XrCompositionLayerQuad screenLayer;
    XrCompositionLayerQuad beamLayer;
    XrCompositionLayerQuad keyboardLayer;
    XrCompositionLayerQuad dashLayer;
    XrCompositionLayerProjection projLayer;
    XrCompositionLayerProjectionView projViews[2];
    const XrCompositionLayerBaseHeader *layers[6];
    uint32_t layerCount = 0;
    XrPosef headPose;
    int headValid;
    int beamVisible = 0;
    uint32_t index;

    if (XR_FAILED(xrWaitFrame(g_session, &waitInfo, &frameState)))
        return;
    if (XR_FAILED(xrBeginFrame(g_session, &beginInfo)))
        return;

    SyncStereoScene();
    headValid = LocatePose(g_viewSpace, frameState.predictedDisplayTime,
                           &headPose);
    UpdateInput(frameState.predictedDisplayTime, &headPose, headValid,
                &beamLayer, &beamVisible);

    if (frameState.shouldRender && g_anchorValid && g_xrStereo &&
        LocateEyes(frameState.predictedDisplayTime)) {
        float anchorMatrix[16];
        Panel hudPanel, dashPanel;
        int eye;

        MatFromPose(anchorMatrix, g_anchorPos, quatYaw(g_anchorYaw));
        UploadSpriteVertices();
        for (eye = 0; eye < 2; eye++) {
            DrawEye(eye, anchorMatrix);
            memset(&projViews[eye], 0, sizeof(projViews[eye]));
            projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projViews[eye].pose = g_views[eye].pose;
            projViews[eye].fov = g_views[eye].fov;
            projViews[eye].subImage.swapchain = g_eyeChain[eye].handle;
            projViews[eye].subImage.imageRect.extent.width =
                g_eyeChain[eye].width;
            projViews[eye].subImage.imageRect.extent.height =
                g_eyeChain[eye].height;
        }
        memset(&projLayer, 0, sizeof(projLayer));
        projLayer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projLayer.space = g_localSpace;
        projLayer.viewCount = 2;
        projLayer.views = projViews;
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&projLayer;

        BlitPanel(&g_hudChain, g_hudReadFbo);
        CockpitPanel(&hudPanel, g_xrHeader.hudDistance);
        FillQuadLayer(&screenLayer, &g_hudChain, &hudPanel, 1);
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&screenLayer;
        if (g_xrHeader.hasDashboard && !g_xrHeader.cockpit3d) {
            BlitPanel(&g_dashChain, g_dashReadFbo);
            CockpitPanel(&dashPanel, g_xrHeader.dashDistance);
            FillQuadLayer(&dashLayer, &g_dashChain, &dashPanel, 1);
            layers[layerCount++] =
                (const XrCompositionLayerBaseHeader *)&dashLayer;
        }
        if (g_kbVisible && g_kbImage != 0 && DrawKeyboard(&keyboardLayer))
            layers[layerCount++] =
                (const XrCompositionLayerBaseHeader *)&keyboardLayer;
        if (beamVisible)
            layers[layerCount++] =
                (const XrCompositionLayerBaseHeader *)&beamLayer;
    } else if (frameState.shouldRender && g_anchorValid && !g_xrStereo &&
        AcquireImage(&g_screenChain, &index)) {
        Panel flat;

        DrawScreen(index);
        ReleaseImage(&g_screenChain);
        PointerPanel(&flat);
        FillQuadLayer(&screenLayer, &g_screenChain, &flat, 0);
        layers[layerCount++] = (const XrCompositionLayerBaseHeader *)&screenLayer;
        if (g_kbVisible && g_kbImage != 0 && DrawKeyboard(&keyboardLayer))
            layers[layerCount++] =
                (const XrCompositionLayerBaseHeader *)&keyboardLayer;
        if (beamVisible)
            layers[layerCount++] =
                (const XrCompositionLayerBaseHeader *)&beamLayer;
    }

    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = layerCount;
    endInfo.layers = layers;
    xrEndFrame(g_session, &endInfo);
}

static void SignalTick(void)
{
    pthread_mutex_lock(&g_lock);
    g_tickCount++;
    pthread_cond_broadcast(&g_tickCond);
    pthread_mutex_unlock(&g_lock);
}

static void *XrThreadMain(void *unused)
{
    JNIEnv *env = 0;
    (void)unused;

    (*g_vm)->AttachCurrentThread(g_vm, &env, 0);
    if (!InitEgl() || !InitInstance() || !InitSession()) {
        LOGE("OpenXR initialisation failed");
        g_threadRunning = 0;
        SignalTick();
        (*g_vm)->DetachCurrentThread(g_vm);
        return 0;
    }
    LOGI("OpenXR ready");
    while (!g_quit) {
        HandleEvents();
        if (g_sessionRunning) {
            RunFrame();
        } else {
            QuestXrControllers none;
            memset(&none, 0, sizeof(none));
            pthread_mutex_lock(&g_lock);
            g_controllers = none;
            memset(&g_pointer, 0, sizeof(g_pointer));
            pthread_mutex_unlock(&g_lock);
            usleep(10000);
        }
        SignalTick();
    }
    LOGI("OpenXR thread exiting");
    if (g_session != XR_NULL_HANDLE) {
        if (g_sessionRunning)
            xrEndSession(g_session);
        xrDestroySession(g_session);
    }
    if (g_instance != XR_NULL_HANDLE)
        xrDestroyInstance(g_instance);
    ShutdownEgl();
    g_threadRunning = 0;
    SignalTick();
    (*g_vm)->DetachCurrentThread(g_vm);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int QuestXr_Start(void)
{
    JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
    jobject activity = (jobject)SDL_AndroidGetActivity();

    if (g_threadRunning)
        return 1;
    if (env == 0 || activity == 0) {
        LOGE("no JNI environment / activity");
        return 0;
    }
    (*env)->GetJavaVM(env, &g_vm);
    g_activity = (*env)->NewGlobalRef(env, activity);
    (*env)->DeleteLocalRef(env, activity);
    g_quit = 0;
    g_threadRunning = 1;
    if (pthread_create(&g_thread, 0, XrThreadMain, 0) != 0) {
        g_threadRunning = 0;
        return 0;
    }
    return 1;
}

void QuestXr_Shutdown(void)
{
    if (!g_threadRunning)
        return;
    g_quit = 1;
    pthread_join(g_thread, 0);
}

int QuestXr_IsRunning(void)
{
    return g_threadRunning;
}

void QuestXr_SubmitIndexedFrame(const unsigned char *pixels,
                                const unsigned char *palette)
{
    int i;
    unsigned char *out;

    if (pixels == 0 || palette == 0)
        return;
    pthread_mutex_lock(&g_lock);
    out = g_frameRgba;
    for (i = 0; i < QUEST_FRAME_WIDTH * QUEST_FRAME_HEIGHT; i++) {
        const unsigned char *entry = palette + pixels[i] * 4;
        out[0] = entry[2];
        out[1] = entry[1];
        out[2] = entry[0];
        out[3] = 255;
        out += 4;
    }
    g_frameSerial++;
    g_stereoMode = 0;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_WaitForDisplayTick(void)
{
    struct timespec deadline;
    unsigned int start;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 30 * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&g_lock);
    start = g_tickCount;
    while (g_threadRunning && g_tickCount == start) {
        if (pthread_cond_timedwait(&g_tickCond, &g_lock, &deadline) != 0)
            break;
    }
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_GetControllers(QuestXrControllers *controllers)
{
    pthread_mutex_lock(&g_lock);
    *controllers = g_controllers;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_GetPointer(QuestXrPointer *pointer)
{
    pthread_mutex_lock(&g_lock);
    *pointer = g_pointer;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_SetPiloting(int piloting)
{
    g_piloting = piloting ? 1 : 0;
}

void QuestXr_ShowErrorColour(unsigned int rgb)
{
    pthread_mutex_lock(&g_lock);
    g_errorColour = rgb;
    g_errorColourSet = 1;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_Haptic(int hand, float amplitude, float seconds)
{
    if (hand < 0 || hand > 1)
        return;
    pthread_mutex_lock(&g_lock);
    g_hapticAmplitude[hand] = amplitude;
    g_hapticSeconds[hand] = seconds;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_SetKeyboardImage(const unsigned char *rgba)
{
    size_t size = (size_t)QUEST_KEYBOARD_WIDTH * QUEST_KEYBOARD_HEIGHT * 4;
    unsigned char *image = (unsigned char *)malloc(size);
    unsigned char *scratch = (unsigned char *)malloc(size);
    int y;

    if (image == 0 || scratch == 0) {
        free(image);
        free(scratch);
        return;
    }
    /* Store bottom row first: GL swapchain images have a bottom-left origin
     * while key rectangles use a top-left origin. */
    for (y = 0; y < QUEST_KEYBOARD_HEIGHT; y++)
        memcpy(image + (size_t)(QUEST_KEYBOARD_HEIGHT - 1 - y) *
                           QUEST_KEYBOARD_WIDTH * 4,
               rgba + (size_t)y * QUEST_KEYBOARD_WIDTH * 4,
               QUEST_KEYBOARD_WIDTH * 4);
    memcpy(scratch, image, size);
    g_kbScratch = scratch;
    g_kbImage = image;
}

void QuestXr_SetKeyboardVisible(int visible)
{
    if (!visible) {
        g_kbHover = -1;
        pthread_mutex_lock(&g_lock);
        g_kbQueueHead = g_kbQueueTail = 0;
        pthread_mutex_unlock(&g_lock);
    }
    g_kbVisible = visible ? 1 : 0;
}

int QuestXr_PopKeyboardKey(void)
{
    int keycode = 0;

    pthread_mutex_lock(&g_lock);
    if (g_kbQueueHead != g_kbQueueTail) {
        keycode = g_kbQueue[g_kbQueueHead];
        g_kbQueueHead = (g_kbQueueHead + 1) % KEY_QUEUE_SIZE;
    }
    pthread_mutex_unlock(&g_lock);
    return keycode;
}

void QuestXr_SetKeyboardShift(int shift)
{
    g_kbShift = shift ? 1 : 0;
}

void QuestXr_SubmitStereoFrame(const QuestSceneHeader *header,
                               const QuestSceneQuad *quads,
                               const unsigned char *hudRgba,
                               const unsigned char *dashRgba,
                               const unsigned char *palette)
{
    int count = header->quadCount;

    if (count > QUEST_SCENE_MAX_QUADS)
        count = QUEST_SCENE_MAX_QUADS;
    pthread_mutex_lock(&g_lock);
    g_sceneHeader = *header;
    g_sceneHeader.quadCount = count;
    memcpy(g_sceneQuads, quads, sizeof(QuestSceneQuad) * (size_t)count);
    memcpy(g_sceneHud, hudRgba, sizeof(g_sceneHud));
    memcpy(g_sceneDash, dashRgba, sizeof(g_sceneDash));
    memcpy(g_scenePalette, palette, sizeof(g_scenePalette));
    g_sceneSerial++;
    g_stereoMode = 1;
    pthread_mutex_unlock(&g_lock);
}

void QuestXr_QueueAtlasUpload(int layer, int x, int y, int width, int height,
                              unsigned char *pixels)
{
    AtlasUpload *upload = (AtlasUpload *)malloc(sizeof(*upload));

    if (upload == 0) {
        free(pixels);
        return;
    }
    upload->next = 0;
    upload->layer = layer;
    upload->x = x;
    upload->y = y;
    upload->width = width;
    upload->height = height;
    upload->pixels = pixels;
    pthread_mutex_lock(&g_lock);
    if (g_uploadTail != 0)
        g_uploadTail->next = upload;
    else
        g_uploadHead = upload;
    g_uploadTail = upload;
    pthread_mutex_unlock(&g_lock);
}
