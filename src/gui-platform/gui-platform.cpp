#include <limits.h>
#include "SDL2/SDL.h"
#include "SDL2/SDL_opengl.h"
#include "skia/core/SkCanvas.h"
#include "skia/core/SkColorSpace.h"
#include "skia/core/SkFont.h"
#include "skia/core/SkFontMgr.h"
#include "skia/core/SkSurface.h"
#include "skia/private/base/SkTArray.h"
#include "skia/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "skia/gpu/ganesh/gl/GrGLDirectContext.h"
#include "skia/gpu/ganesh/SkSurfaceGanesh.h"
#include "skia/gpu/GrRecordingContext.h"
#include "skia/gpu/GrDirectContext.h"
#include "skia/gpu/GrBackendSurface.h"
#include "skia/src/base/SkRandom.h"
#include "skia/src/gpu/ganesh/gl/GrGLUtil.h"
#include "skia/codec/SkCodec.h"
#include "runtime.h"
#include "skia-bind.h"

#if defined(SK_BUILD_FOR_ANDROID)
#   include <GLES/gl.h>
#elif defined(SK_BUILD_FOR_UNIX)
#   include <GL/gl.h>
#elif defined(SK_BUILD_FOR_MAC)
#   include <OpenGL/gl.h>
#elif defined(SK_BUILD_FOR_IOS)
#   include <OpenGLES/ES2/gl.h>
#endif

#if defined(__linux__)
#include "skia/ports/SkFontConfigInterface.h"
#include "skia/ports/SkFontMgr_FontConfigInterface.h"
#elif defined(__APPLE__)
#include "skia/ports/SkFontMgr_mac_ct.h"
#elif defined(WIN32)
#include <skia/ports/SkTypeface_win.h>
#endif

bool gCheckErrorGL = false;
bool gLogCallsGL = false;

extern "C" {
    typedef struct {
        AgObject head;
    } GuiPlatformApp;

    typedef struct {
        void (*run)(
            GuiPlatformApp* thiz,
            AgString* appName,
            int64_t fps,
            void* initalizerContext,
            void(*initializer)(void* ctx, GuiPlatformApp*));
        void (*onFocused)(
            GuiPlatformApp* thiz,
            bool isFocused);
        void (*onPaused)(
            GuiPlatformApp* thiz,
            bool isPaused);
        void (*onResized)(
            GuiPlatformApp* thiz,
            int64_t width,
            int64_t height);
        void (*onKey)(
            GuiPlatformApp* thiz,
            bool down,
            int32_t key,
            int32_t shifts);
        void (*onCursor)(
            GuiPlatformApp* thiz,
            int64_t x,
            int64_t y);
        void (*onScroll)(
            GuiPlatformApp* thiz,
            int64_t dx,
            int64_t dy,
            int64_t dZoom);
        void (*pausePaints)(
            GuiPlatformApp* thiz,
            bool isPaused);
        void (*onPaint)(
            GuiPlatformApp* thiz,
            AgSkCanvas* canvas);
        void (*onQuit)(
            GuiPlatformApp* thiz);
        void (*onLowMemory)(
            GuiPlatformApp* thiz);
        void* cast_info;
        AgVmt base;
    } GuiPlatformAppVmt;

    int    ag_main(void);
    void** ag_disp_guiPlatform_Canvas(uint64_t interface_and_method_ordinal);

    void ag_fn_guiPlatform_disposeApp(GuiPlatformApp* thiz);
    void ag_m_guiPlatform_App_guiPlatform_pausePaints(GuiPlatformApp* thiz, bool isPaused);
    void ag_m_guiPlatform_App_guiPlatform_handleTick(GuiPlatformApp* thiz);
    void ag_m_guiPlatform_App_guiPlatform_runInternal(
        GuiPlatformApp* thiz,
        AgString* appName,
        int64_t fps,
        void* initalizerContext,
        void(*initializer)(void* ctx, GuiPlatformApp*));
}

int window_width = 0;
int window_height = 0;
SDL_DisplayMode sdl_display_mode;
SDL_Window* window = nullptr;
SDL_GLContext gl_context;
int64_t mouse_buttons = 0;
int64_t shifts = 0;
bool repaints_are_paused = false;
bool app_is_in_background = false;
AgSkCanvas gui_platform_canvas;
int64_t frame_duration_ms = 1000 / 60;

sk_sp<const GrGLInterface> gr_gl_interface;
sk_sp<GrDirectContext> gr_gl_context;
GrGLFramebufferInfo sk_framebuffer_info;
sk_sp<SkSurface> sk_framebuffer_surface;
GrBackendRenderTarget sk_render_target;
SkSurfaceProps sk_famebuffer_props(SkSurfaceProps::kUseDeviceIndependentFonts_Flag, kUnknown_SkPixelGeometry);
GuiPlatformApp* app = nullptr;

void handle_error() {
    const char* error = SDL_GetError();
    SkDebugf("SDL Error: %s\n", error);
    exit(-1);
}

#if defined(SK_BUILD_FOR_ANDROID)
int SDL_main(int argc, char** argv) {
    return ag_main();
}
#endif

void ag_m_guiPlatform_App_guiPlatform_pausePaints(GuiPlatformApp* thiz, bool isPaused) {
    repaints_are_paused = isPaused;
}

void ag_m_guiPlatform_App_guiPlatform_handleTick(GuiPlatformApp* thiz) {
    if (thiz != app) return;
    auto start_frame_ms = ag_fn_sys_nowMs();
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_MOUSEMOTION:
            if (event.motion.state == SDL_PRESSED)
                reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onCursor(thiz, event.motion.x, event.motion.y);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            if (event.button.state == SDL_PRESSED || event.button.state == SDL_RELEASED)
                reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onKey(thiz, event.button.state == SDL_PRESSED, event.button.button, shifts);
            break;
        case SDL_MOUSEWHEEL:
            reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onScroll(thiz, event.wheel.x, event.wheel.y, 0);            
            break;
        case SDL_WINDOWEVENT:
            if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || event.window.event == SDL_WINDOWEVENT_RESIZED) {
                window_width = event.window.data1;
                window_height = event.window.data2;
                reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onResized(thiz, window_width, window_height);
            } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED || event.window.event == SDL_WINDOWEVENT_FOCUS_LOST){
                reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onFocused(thiz, event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED);
            } else if (event.window.event == SDL_WINDOWEVENT_SHOWN || event.window.event == SDL_WINDOWEVENT_HIDDEN){
                app_is_in_background = event.window.event == SDL_WINDOWEVENT_HIDDEN;
                reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onPaused(thiz, event.window.event == SDL_WINDOWEVENT_HIDDEN);
            } else if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                SDL_Event e;
                e.type = SDL_QUIT;
                SDL_PushEvent(&e);
            }
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onKey(thiz,
                event.type == SDL_KEYDOWN,
                event.key.keysym.scancode,
                shifts = event.key.keysym.mod);
            break;
        case SDL_QUIT:
            reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onQuit(thiz);
            ag_fn_sys_setMainObject(nullptr);
            return;
        case SDL_APP_LOWMEMORY:
            reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onLowMemory(thiz);
            break;
        default:
            break;
        }
    }
    if (app_is_in_background) {
        ag_fn_sys_postTimer(
            start_frame_ms + 500, // when in bg, check for events every 1/2 sec
            ag_mk_weak(&thiz->head),
            (ag_fn)ag_m_guiPlatform_App_guiPlatform_handleTick);
        return;
    }
    //if (!repaints_are_paused) {
        SkCanvas* canvas = sk_framebuffer_surface->getCanvas();
        gui_platform_canvas.canvas = canvas;
        //canvas->scale(
        //    (float)window_width / sdl_display_mode.w,
        //    (float)window_height / sdl_display_mode.h);
        reinterpret_cast<GuiPlatformAppVmt*>(thiz->head.dispatcher)[-1].onPaint(thiz, &gui_platform_canvas);
        if (auto as_direct_context = GrAsDirectContext(canvas->recordingContext()))
            as_direct_context->flushAndSubmit();
        SDL_GL_SwapWindow(window);
    //}
    ag_fn_sys_postTimer(
        start_frame_ms + frame_duration_ms,
        ag_mk_weak(&thiz->head),
        (ag_fn)ag_m_guiPlatform_App_guiPlatform_handleTick);
}

void ag_m_guiPlatform_App_guiPlatform_runInternal(
        GuiPlatformApp* thiz,
        AgString* appName,
        int64_t fps,
        void* initalizerContext,
        void(*initializer)(void* ctx, GuiPlatformApp*)) {
    if (app) return;
    app = thiz;
    frame_duration_ms = 1000 / fps;
    gui_platform_canvas.header.dispatcher = ag_disp_guiPlatform_Canvas;
    gui_platform_canvas.header.ctr_mt = AG_CTR_STEP;
    gui_platform_canvas.header.wb_p = AG_IN_STACK | AG_F_PARENT;
    gui_platform_canvas.canvas = nullptr;

    uint32_t window_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

#if defined(SK_BUILD_FOR_ANDROID) || defined(SK_BUILD_FOR_IOS)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    windowFlags |= SDL_WINDOW_BORDERLESS | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI;
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);   // TODO: make configurable
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // Skia requirement
    SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) handle_error();
    if (SDL_GetDesktopDisplayMode(0, &sdl_display_mode) != 0) handle_error();
    window = SDL_CreateWindow(
        appName->chars,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        sdl_display_mode.w,
        sdl_display_mode.h,
        window_flags);
    if (!window) handle_error();

    gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) handle_error();

    uint32_t window_format = SDL_GetWindowPixelFormat(window);
    int context_type;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &context_type);

    SDL_GL_GetDrawableSize(window, &window_width, &window_height);
    gr_gl_interface = GrGLMakeNativeInterface();
    gr_gl_context = GrDirectContexts::MakeGL(gr_gl_interface);
    SkASSERT(gr_gl_context);
    //GR_GL_GetIntegerv(gr_gl_interface.get(), GR_GL_FRAMEBUFFER_BINDING, (GrGLint*)&sk_framebuffer_info.fFBOID);
    gr_gl_interface.get()->fFunctions.fGetIntegerv(GR_GL_FRAMEBUFFER_BINDING, (GrGLint*)&sk_framebuffer_info.fFBOID);
    sk_framebuffer_info.fFormat = window_format == SDL_PIXELFORMAT_RGBA8888 || context_type != SDL_GL_CONTEXT_PROFILE_ES
        ? GR_GL_RGBA8
        : GR_GL_BGRA8;
    sk_render_target = GrBackendRenderTargets::MakeGL(
        window_width, window_height,
        0, // MSAA Sample Count
        8, // Stencil Bits
        sk_framebuffer_info);
    sk_framebuffer_surface = SkSurfaces::WrapBackendRenderTarget(
        (GrRecordingContext*)gr_gl_context.get(),
        sk_render_target,
        kBottomLeft_GrSurfaceOrigin,
        window_format == SDL_PIXELFORMAT_RGBA8888 ? kRGBA_8888_SkColorType : kBGRA_8888_SkColorType,
        nullptr,
        &sk_famebuffer_props);
    SkASSERT(sk_framebuffer_surface);
    ag_retain_pin(&thiz->head);
    ag_fn_sys_setMainObject(&thiz->head);
    ag_fn_sys_postTimer(
        ag_fn_sys_nowMs(),
        ag_mk_weak(&thiz->head),
        (ag_fn)ag_m_guiPlatform_App_guiPlatform_handleTick);
}

void ag_fn_guiPlatform_disposeApp(GuiPlatformApp* thiz) {
    if (thiz != app) return;
    sk_framebuffer_surface = nullptr;
    sk_render_target.GrBackendRenderTarget::~GrBackendRenderTarget();
    new(&sk_render_target)GrBackendRenderTarget;
    gr_gl_context = nullptr;
    gr_gl_interface = nullptr;
    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
}
