// xr_session.h — actually TALK to the OpenXR runtime, which nothing did before.
//
// The state of the project until this file existed:
//   xr_try_load() loaded openxr_loader.dll and checked that
//   xrGetInstanceProcAddr was non-NULL, logged "xrGetInstanceProcAddr ok",
//   and stopped there. It never called xrCreateInstance. So there was no XR
//   instance, no system, no session, no HMD pose, and nothing between the
//   headset and stereo.h. "No OpenXR loader, flat rendering" was the honest
//   state and the log said so -- but the search list for the loader was itself
//   wrong, so even that negative was unreliable.
//
// The chain, in the order OpenXR requires, each step checking its result and
// logging the real error code instead of a shrug:
//   xrCreateInstance -> xrGetSystem -> xrCreateSession (+ reference space)
//   -> xrWaitFrame/xrBeginFrame -> xrLocateSpace at the reference space
//
// Everything is resolved through xrGetInstanceProcAddr, so this file adds no
// OpenXR import and the DLL keeps its no-network, three-module import profile.

#ifndef TDVR_XR_SESSION_H
#define TDVR_XR_SESSION_H

// Forward declarations for the helpers in xr_copy.h, which is included from
// teardown_vr.c only AFTER this header -- so by the time the code below calls
// them, the definitions are not in scope yet. Declaring them here breaks the
// include cycle: xr_copy.h still sees the full tdvr_xr, these just need to be
// visible while this header is being read.
// Set by the bind thread in xr_copy.h, read here to decide whether the images
// are safe to write. Declared here because xr_copy.h is included after this
// header, so the definition is not in scope while this file is read.
extern volatile long g_bind_done;

// The window backbuffer, taken off the game's own swapchain in the render hook.
// It is the source for the copy into the XR images.
static void* g_xr_backbuffer = NULL;
static int   tdvr_xr_bind_images(void* Xp);
static void  tdvr_xr_start_bind(void* Xp);
static void  tdvr_xr_copy_images(void* Xp);

// openxr.h declares its functions with __declspec(dllimport) on Windows, so
// merely CALLING one through a function pointer is not enough: the compiler
// emits a direct `call xrGetInstanceProcAddr` and a .def import entry, and the
// link fails with "undefined reference to xrGetInstanceProcAddr". That is what
// happened three times here before the cause was found.
//
// The fix is XR_NO_PROTOTYPES? No -- the correct one is that openxr.h also
// guards the prototypes behind this macro, so defining it before the include
// gives us the TYPES without the imported declarations. Every function is then
// reached only through the loader's own xrGetInstanceProcAddr, which keeps the
// DLL's import profile at KERNEL32/msvcrt/USER32/ADVAPI32 and adds no OpenXR
// dependency.
#ifndef XR_NO_PROTOTYPES
#define XR_NO_PROTOTYPES
#endif
#include "openxr/openxr.h"

// XrGraphicsBindingD3D12KHR lives in openxr_platform.h, not openxr.h, and even
// there it sits behind #ifdef XR_USE_GRAPHICS_API_D3D12, which nothing in this
// build defines. Without the define the whole struct is preprocessed out and
// every field access fails with "unknown type name". The member is called
// `queue`, not `commandQueue` -- that spelling is the D3D11 struct's habit, not
// this one. No library is linked, so the import profile does not grow; the
// pointers arrive from the renderer at runtime.
#ifndef XR_USE_GRAPHICS_API_D3D12
#define XR_USE_GRAPHICS_API_D3D12 1
#endif
// The interfaces in this header are declared only for C++ (`MIDL_INTERFACE`),
// so compiling as C gives an ID3D12Device with no methods at all -- every call
// then fails with "has no member named CreateCommandQueue", and Release
// disappears too. COBJMACROS exposes the same interfaces as vtables, which is
// what C code needs. Tested before committing to it.
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <d3d12.h>
#include "openxr/openxr_platform.h"

// Defined in hook/present_hook.h, which is included AFTER this file. Without this
// the call below is an implicit declaration and the compiler assumes it returns
// int, which silently truncates a 64-bit pointer on the way into a variable.
#if TDVR_PRESENT_HOOK
void* tdvr_factory_from_device(void* dev);
void  tdvr_factory_walk_later(void* dev);
#endif

// The IID_* symbols live in libd3d12.a on mingw, and linking that to get them
// would add a D3D12 import to a DLL that deliberately has none (the import
// profile is KERNEL32/msvcrt/USER32/ADVAPI32 and everything else is resolved by
// name at runtime). The GUIDs are fixed constants, so declaring them here is
// exact, not a workaround.
#ifndef __IID_ID3D12Device_DEFINED__
#define __IID_ID3D12Device_DEFINED__
static const GUID IID_ID3D12Device_local =
    {0x189819f1,0x1db6,0x4b57,{0xbe,0x54,0x18,0x21,0x33,0x9b,0x85,0xf7}};
#undef IID_ID3D12Device
#define IID_ID3D12Device IID_ID3D12Device_local
#endif
#ifndef __IID_ID3D12CommandQueue_DEFINED__
#define __IID_ID3D12CommandQueue_DEFINED__
// IID_ID3D12CommandQueue = 0ec870a6-5d7e-4c22-8cfc-5baae0769903
//
// The tail was wrong here (0x16,0xed instead of 0x99,0x03) and the symptom was
// silent and expensive: QueryInterface answered E_NOINTERFACE on a queue the
// game had just handed us, so the code concluded the object was not a queue and
// the session was created with no command queue. Copy IIDs from the SDK header;
// never retype them from memory.
static const GUID IID_ID3D12CommandQueue_local =
    {0x0ec870a6,0x5d7e,0x4c22,{0x8c,0xfc,0x5b,0xaa,0xe0,0x76,0x99,0x03}};
#undef IID_ID3D12CommandQueue
#define IID_ID3D12CommandQueue IID_ID3D12CommandQueue_local
#endif

// XR_NO_PROTOTYPES above removed this declaration along with the rest, so put
// it back. It is the ONE function we hold as a real pointer -- obtained from
// GetProcAddress on the loader module we loaded by name -- and declaring it
// here is what stops the compiler from inventing an import for it.
typedef XrResult (XRAPI_PTR *tdvr_gipa_t)(
    XrInstance instance, const char* name, PFN_xrVoidFunction* function);
static tdvr_gipa_t volatile g_xr_gipa = NULL;

typedef struct tdvr_xr {
    int      have_loader;      // the DLL loaded and xrGetInstanceProcAddr exists
    int      have_instance;
    int      have_system;
    int      have_session;
    int      have_space;

    // xrCreateSession needs a graphics device and xrCreateInstance does not, so
    // the path is split in two. Everything above happens on the DLL-load thread
    // (device-free, and measured to work there). The session is created later,
    // from the render thread, where a swapchain and its device actually exist.
    // Before this split every run ended in XR_ERROR_GRAPHICS_DEVICE_INVALID
    // because the session was requested before the renderer had made anything.
    int      want_session;     // set once the instance is ready for a session
    int      session_tried;    // only try once; a refusal will not change
    void*    gfx_device;       // ID3D12Device* from the renderer, for the binding
    void*    gfx_queue;        // ID3D12CommandQueue*
    XrViewConfigurationType view_config;  // PRIMARY_STEREO, chosen during init

    // The frame loop's state. xr_time is the predicted display time handed back
    // by xrWaitFrame and is what xrLocateSpace must be asked about -- passing a
    // literal 0 asks for "most recent", which is legal but samples the wrong
    // instant. The counters exist so a stalled or desynced loop is visible in
    // the log instead of silently producing a frozen pose.
    XrTime   xr_time;
    int      frame_ready;      // 1 when the runtime said this frame should render
    unsigned frame_waits;
    unsigned frame_ends;       // must track frame_waits that were renderable
    unsigned frame_errors;
    unsigned state_changes;   // session-state events seen, so far
    int      announced_ready; // the app has told the runtime it is ready
    int      announced_noloop; // NOLOOP diagnostic banner already printed
    int      last_state;      // previous XrSessionState, this SDK has no oldState

    XrInstance instance;
    XrSystemId system;
    XrSession  session;
    XrSpace    space;          // LOCAL space, the play area

    // One colour swapchain per eye plus the projection layer that carries them.
    // A single mono swapchain cannot satisfy XR_TYPE_COMPOSITION_LAYER_PROJECTION:
    // the spec requires as many swapchains in the layer as the view
    // configuration has views, and the runtime says so out loud --
    // "presentProjection: Left swapchain not found".
    XrSwapchain sc[2];
    int         sc_made[2];
    int32_t     sc_image[2];       // index of the image currently held
    int         sc_held[2];        // an image is acquired and not yet released
    int         sc_image_ready[2]; // acquired and usable for drawing
    // The colour target behind each swapchain image. Filled in by
    // tdvr_xr_bind_images(), which calls xrEnumerateSwapchainImages -- the swapchain
    // handle alone is not drawable, only the resources behind it are.
    void*       sc_res[2];         // ID3D12Resource* per eye
    void*       sc_srv[2];         // ID3D12ShaderResourceView* per eye, for the copy
    // A fence that says the copy commands have actually executed on the GPU, so
    // the images are not released while the copy is still reading the game's
    // backbuffer. Rendering and EndFrame happen on different threads, and
    // releasing an image whose copy has not landed is exactly the kind of race
    // that shows up as a torn or one-frame-late preview.
    void*       gfx_fence;
    uint64_t    fence_value;       // the value the last copy will signal
    // The game's backbuffer, read once so the copy has a source that cannot
    // move under it.
    void*       game_res;          // ID3D12Resource* of the window backbuffer
    void*       game_srv;
    unsigned    copy_frames;       // frames actually copied to the XR images
    // The command list the copy runs through, created once by the bind thread.
    void*       cmd_alloc;         // ID3D12CommandAllocator*
    void*       cmd_list;          // ID3D12GraphicsCommandList*, reused per frame
    // xrEndFrame blocks forever when called from the render thread: the runtime
    // composites on the same thread we are holding, and the game's own device
    // has a frame in flight, so it waits for GPU work that cannot complete
    // because we never release the render thread. Measured: the process froze
    // with every thread waiting and CPU flat, on the first EndFrame, for as
    // long as the game was left alone.
    //
    // So EndFrame runs on its own thread. The swapchain image must stay acquired
    // until it returns, which is exactly what the spec wants: release after
    // EndFrame, never before.
    HANDLE      end_thread;
    volatile long end_pending;   // a frame is waiting to be ended
    volatile long end_done;      // frames completed by the thread
    volatile long end_errors;
    volatile long end_dropped;   // frames skipped because EndFrame was busy
    volatile long frame_skipped;  // frames not started: previous EndFrame busy
    XrCompositionLayerProjection* proj;
    XrCompositionLayerProjectionView pview[2];
    int      proj_made;
    int      sc_width, sc_height;
    int      sc_format;            // the colour format we settled on
    int      sc_array;             // 2: one array slice per eye
    unsigned proj_submissions;     // frames that carried a projection layer
    int      locate_failed;

    // Resolved entry points
    PFN_xrGetSystem          GetSystem;
    PFN_xrCreateSession      CreateSession;
    PFN_xrDestroySession     DestroySession;
    PFN_xrCreateReferenceSpace CreateReferenceSpace;
    PFN_xrLocateSpace        LocateSpace;
    PFN_xrWaitFrame          WaitFrame;
    PFN_xrBeginFrame         BeginFrame;
    PFN_xrEndFrame           EndFrame;
    PFN_xrPollEvent          PollEvent;
    // Mandatory to leave READY. Without xrBeginSession the runtime never moves
    // the session to SYNCHRONIZING/VISIBLE/FOCUSED, and it stays in state 2
    // forever no matter how many frames the app submits (measured).
    PFN_xrBeginSession       BeginSession;
    PFN_xrResultToString     ResultToString;
    PFN_xrGetSystemProperties GetSystemProperties;
    // The projection layer. Without these the runtime has no colour target to
    // composite: it logs "no projection layer - 2D-only frame" and the preview
    // sits on "waiting for stereo projection" forever, however many frames the
    // app submits (measured).
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
    PFN_xrCreateSwapchain          CreateSwapchain;
    PFN_xrDestroySwapchain         DestroySwapchain;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
    PFN_xrAcquireSwapchainImage    AcquireSwapchainImage;
    PFN_xrWaitSwapchainImage       WaitSwapchainImage;
    PFN_xrReleaseSwapchainImage    ReleaseSwapchainImage;
    PFN_xrLocateViews              LocateViews;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews;
    // Mandatory for D3D12, and the runtime enforces it: without this call
    // xrCreateSession answers XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING
    // (measured, not guessed) even though the device and queue are perfectly
    // valid. The spec requires asking the runtime which feature level the
    // binding must support before making a device for it.
    // The typedef name in the SDK is PFN_xrGetD3D12GraphicsRequirementsKHR,
    // not PFN_xrGetGraphicsRequirementsD3D12. The exported function name is
    // still xrGetGraphicsRequirementsD3D12 and that is what the string
    // below must be.
    PFN_xrGetD3D12GraphicsRequirementsKHR GetGFXD3D12;


    // The pose, filled by tdvr_xr_poll(). Kept in the renderer's units:
    // metres, and the engine's Y-up left/right/front basis.
    float head_x, head_y, head_z;
    float yaw, pitch, roll;       // radians
    int   tracking_valid;         // 0 when the runtime says the pose is not tracked

} tdvr_xr;

// The one runtime state, defined in teardown_vr.c before this header is
// included. Declared here because the binding helpers below live in this header
// and are called from the render hook, and they need to reach the same state the
// init path fills in.
extern tdvr_xr g_xr;

// Defined below but called from the init path above: once a session exists the
// LOCAL reference space has to be made, and both the load thread and
// tdvr_xr_try_session() need to reach that code.
static int tdvr_xr_after_session(tdvr_xr* X);
static void tdvr_xr_make_projection(tdvr_xr* X, int w, int h);
// Defined below, next to the thread it starts; declared here because the
// projection layer is what makes the threaded path worth using.
static void tdvr_xr_start_end_thread(tdvr_xr* X);
// A monotonic millisecond counter, so a blocking runtime call can be reported
// with a duration instead of just "it did not come back". GetTickCount64 is
// monotonic and needs no synchronisation.
static unsigned long long td_ms_now(void) {
    return (unsigned long long)GetTickCount64();
}

static int tdvr_xr_backbuffer_size(int* w, int* h);
static void tdvr_xr_acquire_views(tdvr_xr* X, XrTime t);
static void tdvr_xr_release_views(tdvr_xr* X);

static const char* tdvr_xr_err(tdvr_xr* X, XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (r == XR_SUCCESS) return "XR_SUCCESS";
    // The function is xrResultToString (lowercase xr, no leading Xr type), and
    // it is resolved through xrGetInstanceProcAddr, not called by name -- the
    // loader is loaded by name, so nothing is imported.
    //
    // It is ALSO useless for the failure we care about most: xrResultToString
    // needs a valid instance handle, and the one error worth naming is
    // xrCreateInstance's own. When that call fails there is no instance yet, so
    // the "converted" name came out as the bare string "XR_RESULT_-10". That hid
    // XR_ERROR_LIMIT_REACHED behind a number, which is exactly backwards -- the
    // number was the only clue and it was unreadable.
    //
    // So the codes are spelled out here. Checked against third_party/openxr/
    // openxr.h line by line, not from memory: -10 is LIMIT_REACHED, and
    // GRAPHICS_REQUIREMENTS_CALL_MISSING is -50. Those two are easy to swap and
    // swapping them would have sent the fix in the wrong direction entirely.
    struct { XrResult v; const char* n; } tbl[] = {
        { XR_ERROR_VALIDATION_FAILURE,                 "XR_ERROR_VALIDATION_FAILURE" },
        { XR_ERROR_RUNTIME_FAILURE,                    "XR_ERROR_RUNTIME_FAILURE" },
        { XR_ERROR_OUT_OF_MEMORY,                      "XR_ERROR_OUT_OF_MEMORY" },
        { XR_ERROR_API_VERSION_UNSUPPORTED,            "XR_ERROR_API_VERSION_UNSUPPORTED" },
        { XR_ERROR_INITIALIZATION_FAILED,              "XR_ERROR_INITIALIZATION_FAILED" },
        { XR_ERROR_FUNCTION_UNSUPPORTED,               "XR_ERROR_FUNCTION_UNSUPPORTED" },
        { XR_ERROR_FEATURE_UNSUPPORTED,                "XR_ERROR_FEATURE_UNSUPPORTED" },
        { XR_ERROR_EXTENSION_NOT_PRESENT,              "XR_ERROR_EXTENSION_NOT_PRESENT" },
        { XR_ERROR_LIMIT_REACHED,                      "XR_ERROR_LIMIT_REACHED" },
        { XR_ERROR_SIZE_INSUFFICIENT,                  "XR_ERROR_SIZE_INSUFFICIENT" },
        { XR_ERROR_HANDLE_INVALID,                     "XR_ERROR_HANDLE_INVALID" },
        { XR_ERROR_INSTANCE_LOST,                      "XR_ERROR_INSTANCE_LOST" },
        { XR_ERROR_GRAPHICS_DEVICE_INVALID,            "XR_ERROR_GRAPHICS_DEVICE_INVALID" },
        { XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING, "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING" },
        { XR_ERROR_SYSTEM_INVALID,                     "XR_ERROR_SYSTEM_INVALID" },
        { XR_ERROR_FORM_FACTOR_UNSUPPORTED,            "XR_ERROR_FORM_FACTOR_UNSUPPORTED" },
        { XR_ERROR_FORM_FACTOR_UNAVAILABLE,            "XR_ERROR_FORM_FACTOR_UNAVAILABLE" },
        { XR_ERROR_API_LAYER_NOT_PRESENT,              "XR_ERROR_API_LAYER_NOT_PRESENT" },
        { XR_ERROR_CALL_ORDER_INVALID,                 "XR_ERROR_CALL_ORDER_INVALID" },
        { XR_ERROR_RUNTIME_UNAVAILABLE,                "XR_ERROR_RUNTIME_UNAVAILABLE" },
        { XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED,   "XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED" },
    };
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (tbl[i].v == r) return tbl[i].n;
    if (X->ResultToString && X->have_instance) {
        if (X->ResultToString(X->instance, r, buf) == XR_SUCCESS) return buf;
    }
    snprintf(buf, sizeof buf, "XR_RESULT_%d", (int)r);
    return buf;
}

// Same table, no instance needed. Split out because the failure that matters
// most is xrCreateInstance's own, and at that point there is no instance to pass
// -- so the callers that only have a result code (the diagnostic probe) would
// otherwise have no way to name it.
static const char* tdvr_xr_err_name_only(int r) {
    static const struct { int v; const char* n; } tbl[] = {
        { -1, "XR_ERROR_VALIDATION_FAILURE" },
        { -2, "XR_ERROR_RUNTIME_FAILURE" },
        { -3, "XR_ERROR_OUT_OF_MEMORY" },
        { -4, "XR_ERROR_API_VERSION_UNSUPPORTED" },
        { -6, "XR_ERROR_INITIALIZATION_FAILED" },
        { -7, "XR_ERROR_FUNCTION_UNSUPPORTED" },
        { -8, "XR_ERROR_FEATURE_UNSUPPORTED" },
        { -9, "XR_ERROR_EXTENSION_NOT_PRESENT" },
        { -10, "XR_ERROR_LIMIT_REACHED" },
        { -11, "XR_ERROR_SIZE_INSUFFICIENT" },
        { -12, "XR_ERROR_HANDLE_INVALID" },
        { -13, "XR_ERROR_INSTANCE_LOST" },
        { -14, "XR_ERROR_SESSION_RUNNING" },
        { -16, "XR_ERROR_SESSION_NOT_RUNNING" },
        { -17, "XR_ERROR_SESSION_LOST" },
        { -18, "XR_ERROR_SYSTEM_INVALID" },
        { -19, "XR_ERROR_PATH_INVALID" },
        { -22, "XR_ERROR_PATH_UNSUPPORTED" },
        { -23, "XR_ERROR_LAYER_INVALID" },
        { -25, "XR_ERROR_SWAPCHAIN_RECT_INVALID" },
        { -26, "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED" },
        { -28, "XR_ERROR_SESSION_NOT_READY" },
        { -29, "XR_ERROR_SESSION_NOT_STOPPING" },
        { -30, "XR_ERROR_TIME_INVALID" },
        { -31, "XR_ERROR_REFERENCE_SPACE_UNSUPPORTED" },
        { -34, "XR_ERROR_FORM_FACTOR_UNSUPPORTED" },
        { -35, "XR_ERROR_FORM_FACTOR_UNAVAILABLE" },
        { -36, "XR_ERROR_API_LAYER_NOT_PRESENT" },
        { -37, "XR_ERROR_CALL_ORDER_INVALID" },
        { -38, "XR_ERROR_GRAPHICS_DEVICE_INVALID" },
        { -39, "XR_ERROR_POSE_INVALID" },
        { -40, "XR_ERROR_INDEX_OUT_OF_RANGE" },
        { -41, "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED" },
        { -42, "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED" },
        { -44, "XR_ERROR_NAME_DUPLICATED" },
        { -45, "XR_ERROR_NAME_INVALID" },
        { -46, "XR_ERROR_ACTIONSET_NOT_ATTACHED" },
        { -47, "XR_ERROR_ACTIONSETS_ALREADY_ATTACHED" },
        { -48, "XR_ERROR_LOCALIZED_NAME_DUPLICATED" },
        { -49, "XR_ERROR_LOCALIZED_NAME_INVALID" },
        { -50, "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING" },
        { -51, "XR_ERROR_RUNTIME_UNAVAILABLE" },
        // The two that are easy to confuse and were confused once already:
        // -10 is LIMIT_REACHED, -50 is GRAPHICS_REQUIREMENTS_CALL_MISSING.
        // The spec requires the graphics-requirements call before
        // xrCreateSession, NOT before xrCreateInstance, so a -10 at instance
        // creation is a limit and never a call-order problem.
    };
    static char buf[48];
    for (size_t i = 0; i < sizeof tbl / sizeof tbl[0]; i++)
        if (tbl[i].v == r) return tbl[i].n;
    snprintf(buf, sizeof buf, "XR_RESULT_%d", r);
    return buf;
}

// Every OpenXR entry point is reached through the loader's own
// xrGetInstanceProcAddr, obtained with GetProcAddress on the module we loaded by
// name. Nothing here is imported: calling xrGetSystemProperties or
// xrCreateInstance directly is what produced the "undefined reference" link
// errors, because openxr.h declares them as real symbols and the mingw link
// then wants a real import library -- which would add an OpenXR dependency to
// the DLL and change its import profile.
//
// So: xrGetSystemProperties is a global function, so it comes from the
// XR_NULL_HANDLE lookup, and every other one is resolved per instance.
// The target of the global lookup is always the struct member `name`, so the
// concatenation cannot be g_xr_##name -- that produced g_xr_xrGetSystemProperties
// and the compiler said so. The member is written out instead.
// Resolves a function against a real handle. The handle matters: SteamVR's
// xrGetInstanceProcAddr returns XR_RESULT_HANDLE_INVALID (-12) for
// xrGetSystemProperties when asked with XR_NULL_HANDLE, even though the same
// call with the instance returns XR_SUCCESS. The first working run hit exactly
// that and the log blamed the runtime; it was this macro's NULL.
#define TDVR_XR_GLOBAL(X, member, sym)                                        \
    do {                                                                       \
        XrResult _r = g_xr_gipa((X)->instance, sym,                           \
                                (PFN_xrVoidFunction*)&(X)->member);          \
        if (_r != XR_SUCCESS) {                                                \
            vr_log("OpenXR: cannot resolve %s (%s)", sym,                     \
                   tdvr_xr_err(X, _r));                                        \
            return 0;                                                          \
        }                                                                      \
    } while (0)

// Resolves a function against `inst`. The name is a separate argument because
// the target is written as a struct member expression (X->CreateSession), and
// stringifying that expression -- the #fn this used to do -- produced the
// lookup key "X->CreateSession", which of course does not exist. The log line
// said "cannot resolve X->CreateSession (XR_RESULT_-7)", which is the giveaway:
// a real missing function would have been named xrCreateSession.
// FORMAT_FUNCTION is not portable, so the literal name is passed in.
#define TDVR_XR_GET(X, inst, fn, sym)                                         \
    do {                                                                       \
        XrResult _r = g_xr_gipa((inst), sym,                                  \
                                (PFN_xrVoidFunction*)&(fn));                  \
        if (_r != XR_SUCCESS) {                                                \
            vr_log("OpenXR: cannot resolve %s (%s)", sym,                     \
                   tdvr_xr_err(X, _r));                                        \
            return 0;                                                          \
        }                                                                      \
    } while (0)

// Walk the whole chain. Returns 1 only if a session and a reference space exist,
// which is the first point at which a pose can be read at all.
// The loader's own xrGetInstanceProcAddr, needed by the resolution macros, which
// are used before X is fully set up. File-scope, set once, never cleared.

// Forward declaration: tdvr_xr_make_device_and_queue is defined ~400 lines
// below this point, and the INSTANCE_ONLY diagnostic inside tdvr_xr_init calls
// it. It has to be at FILE scope, above the function -- declaring it inside a
// block is an "invalid storage class" error, and a non-static block-scope
// declaration makes the later `static` definition a conflicting one. The
// INSTANCE_ONLY block is inside tdvr_xr_init(), which is why moving the
// declaration to just before that #if did not help.
static int tdvr_xr_make_device_and_queue(void** out_dev, void** out_queue);

static int tdvr_xr_init(tdvr_xr* X, HMODULE loader, tdvr_gipa_t gipa) {
    vr_log("OpenXR: tdvr_xr_init entered");
    memset(X, 0, sizeof *X);
    g_xr_gipa = gipa;
    if (!gipa) { vr_log("OpenXR: init called with a NULL gipa"); return 0; }
    X->instance = XR_NULL_HANDLE;
    X->system   = XR_NULL_SYSTEM_ID;
    X->session  = XR_NULL_HANDLE;
    X->space    = XR_NULL_HANDLE;

    if (!gipa) { vr_log("OpenXR: no xrGetInstanceProcAddr"); return 0; }
    X->have_loader = 1;

    // xrEnumerateInstanceExtensionProperties is a global function, reachable
    // through xrGetInstanceProcAddr with XR_NULL_HANDLE.
    {
        PFN_xrResultToString rt = NULL;
        g_xr_gipa(XR_NULL_HANDLE, "xrResultToString",
                  (PFN_xrVoidFunction*)&rt);
        X->ResultToString = rt;
    }
    // NOTE: xrGetSystemProperties is deliberately NOT resolved here. It was
    // resolved before xrCreateInstance, which is why the first working run
    // reported "cannot resolve xrGetSystemProperties (XR_RESULT_-12)"
    // (HANDLE_INVALID): it was asked with a NULL instance, before one existed.
    // It is resolved after the instance, below, with the real handle.

    // -- 1. instance
    {
        PFN_xrCreateInstance ci = NULL;
        g_xr_gipa(XR_NULL_HANDLE, "xrCreateInstance",
                  (PFN_xrVoidFunction*)&ci);
        if (!ci) { vr_log("OpenXR: loader exports no xrCreateInstance"); return 0; }
        (void)loader;

        XrInstanceCreateInfo cii = {0};
        cii.type = XR_TYPE_INSTANCE_CREATE_INFO;
        // These were left zeroed, and calling xrCreateInstance with an empty
        // application name is what killed the game on the first attempt: the log
        // stopped dead after "xrGetInstanceProcAddr ok" with no result code at
        // all, and the process was gone. OpenXR requires them; a runtime that
        // trusts its loader to have checked is entitled to fault.
        cii.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        snprintf(cii.applicationInfo.applicationName,
                 XR_MAX_APPLICATION_NAME_SIZE, "teardown_vr");
        cii.applicationInfo.applicationVersion = 1;
        snprintf(cii.applicationInfo.engineName,
                 XR_MAX_ENGINE_NAME_SIZE, "none");
        cii.applicationInfo.engineVersion = 1;
        // No XR_MND_HEADLESS: that extension needs to be requested in
        // enabledApiLayerNames or enabledExtensionNames or the runtime may
        // fault, and there is no headless HMD to talk to anyway.

        // XR_KHR_D3D12_enable must be requested at instance creation, or the
        // runtime will not expose xrGetD3D12GraphicsRequirementsKHR and
        // xrCreateSession refuses with "failed to call xr*GetGraphicsRequirements
        // before xrCreateSession" (that is the runtime's own wording, from
        // xrclient_teardown.txt). The function IS present in vrclient_x64.dll --
        // it just is not reachable until the extension is enabled. Enabling an
        // extension the runtime lacks is not an error: unknown names are
        // ignored, so this is safe either way.
        // Only D3D12. D3D11's name macro is not defined here because
        // openxr_platform.h guards it behind XR_USE_GRAPHICS_API_D3D11, which
        // this build does not set -- asking for a graphics API the binding will
        // not use is pointless anyway.
#if TDVR_XR_NO_D3D12_EXT
        // Bisect: ask for no graphics extension at all.
        //
        // The bisect ladder (docs/XR_CRASH_BISECT_2026-09-29.md) puts the GPU
        // reset at xrCreateInstance: the runtime is idle in the game's process
        // and harmless, but the moment an instance exists the GPU stops
        // answering and Windows resets it -- and that survives switching the
        // image copy off, setting layerCount=0, and stopping before
        // xrGetSystem. The one thing instance creation still asks for is
        // XR_KHR_D3D12_enable, and the runtime's own log brackets the reset
        // with "Settings restored: profile=Quest 3" inside xrCreateInstance.
        // So: does asking for D3D12 at instance time make the runtime set up a
        // graphics device inside the game process, and is that the hang?
        //
        // A session cannot be created afterwards without the extension, so this
        // build is expected to stop at "no D3D12 requirements function" -- the
        // point is only whether the GPU survives.
        cii.enabledExtensionCount = 0;
        cii.enabledExtensionNames  = NULL;
        vr_log("OpenXR: XR_NO_D3D12_EXT build -- requesting NO graphics extension");
#else
        static const char* kExt[] = { XR_KHR_D3D12_ENABLE_EXTENSION_NAME };
        cii.enabledExtensionCount = 1;
        cii.enabledExtensionNames  = kExt;
        vr_log("OpenXR: requesting extension %s", kExt[0]);
#endif

        vr_log("OpenXR: calling xrCreateInstance (api 1.0.34, app teardown_vr)");
        XrResult r = ci(&cii, &X->instance);
        vr_log("OpenXR: xrCreateInstance returned %d", (int)r);
        if (r != XR_SUCCESS) {
            vr_log("OpenXR: xrCreateInstance(1.1.0) -> %s", tdvr_xr_err(X, r));
            // Fall back to 1.0, some runtimes refuse 1.1 outright.
            cii.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
            r = ci(&cii, &X->instance);
            if (r != XR_SUCCESS) {
                vr_log("OpenXR: xrCreateInstance(1.0.34) -> %s; giving up",
                       tdvr_xr_err(X, r));
                return 0;
            }
            vr_log("OpenXR: instance created at API 1.0.34 (1.1 was refused)");
        } else {
            vr_log("OpenXR: instance created at API 1.1.0");
        }
        X->have_instance = 1;
    }

#if TDVR_XR_INSTANCE_ONLY
    // Bisect point: the instance exists, and nothing else happens. No system, no
    // session, no reference space, no loop.
    //
    // The measured picture so far, all with the same three crash dumps
    // ("GPU hung/removed/reset, HRESULT=887a0005" = DXGI_ERROR_DEVICE_RESET,
    // fault at teardown.exe rva 0x4ffcc8, identical every time):
    //
    //   no loader at all (TDVR_NO_XR=1) ....... game ALIVE
    //   loader resident, no instance .......... game ALIVE
    //   instance + system + session + loop ... game CRASHES
    //
    // and neither the image copy nor layerCount changes the result, so the
    // trigger is inside the chain rather than in what we hand the runtime. This
    // switch splits "instance" from "system and session", which is the next
    // boundary down.
    vr_log("XR_INSTANCE_ONLY build: instance created, stopping before xrGetSystem");

    // UPDATE 6 left one cell in the table untested: does D3D12 device creation
    // hang in a process that has an OpenXR instance but nothing else? Everything
    // measured so far had a system and usually a session, so a stall in
    // D3D12CreateDevice could have been the runtime's session/swapchain state
    // rather than the instance. This is the cheapest possible version of that
    // question -- instance only, no system, no session, no graphics binding
    // requested at all -- and it is the one case where a hang would be almost
    // impossible to explain by anything we are doing.
    {
        vr_log("OpenXR: INSTANCE_ONLY -- now asking D3D12CreateDevice for a "
               "private device, with an instance alive and no session "
               "(NO_D3D12_EXT=%d: %s)", TDVR_XR_NO_D3D12_EXT,
               TDVR_XR_NO_D3D12_EXT ? "no graphics extension was requested"
                                    : "XR_KHR_D3D12_enable WAS requested");
        // (forward-declared at file scope just above the #if)
        void* odev = NULL; void* oq = NULL;
        if (tdvr_xr_make_device_and_queue(&odev, &oq)) {
            vr_log("OpenXR: INSTANCE_ONLY -- private device CREATED: dev=%p "
                   "queue=%p. An instance alone does not block D3D12.", odev, oq);
        } else {
            vr_log("OpenXR: INSTANCE_ONLY -- D3D12CreateDevice FAILED or hung; "
                   "hr was not returned if it hung, so treat this as the answer");
        }
    }
    return 1;
#endif

    // -- 1b. global functions that need a real instance handle. Resolving these
    //    before xrCreateInstance is why the first run died with
    //    XR_RESULT_-12 on xrGetSystemProperties: gipa with XR_NULL_HANDLE only
    //    ever returns the global-function table, and for anything else it is a
    //    handle-validation error. With the instance in hand it resolves.
    //
    //    It still did not. So log the whole set rather than guess which of the
    //    two causes it is: HANDLE_INVALID because SteamVR's gipa validates the
    //    instance differently, or the function simply is not exported.
    {
        static const char* names[] = {
            "xrGetSystemProperties", "xrCreateSession", "xrDestroyInstance",
            "xrGetInstanceProperties", "xrPollEvent", "xrResultToString",
            "xrGetSystem", "xrEnumerateEnvironmentBlendModes", NULL
        };
        for (int i = 0; names[i]; i++) {
            PFN_xrVoidFunction f = NULL;
            XrResult rr = g_xr_gipa(X->instance, names[i], &f);
            vr_log("OpenXR: gipa(%s) = %d  %s", names[i], (int)rr, f ? "OK" : "null");
        }
    }
    TDVR_XR_GLOBAL(X, GetSystemProperties, "xrGetSystemProperties");

    // The exported name is xrGetD3D12GraphicsRequirementsKHR, not
    // xrGetGraphicsRequirementsD3D12. Both the typedef and the export word it
    // D3D12-first, and the runtime only answers to the KHR spelling -- asking
    // for the other one resolves nothing, which is what the log showed.
    {
        PFN_xrVoidFunction f = NULL;
        if (g_xr_gipa(X->instance, "xrGetD3D12GraphicsRequirementsKHR", &f)
                == XR_SUCCESS && f) {
            X->GetGFXD3D12 = (PFN_xrGetD3D12GraphicsRequirementsKHR)f;
        } else {
            vr_log("OpenXR: cannot resolve xrGetD3D12GraphicsRequirementsKHR");
        }
    }

    // Bind xrResultToString for real. It was being resolved once against
    // XR_NULL_HANDLE before the instance existed, so every error string in the
    // log fell back to the numeric "XR_RESULT_-38" instead of the runtime's own
    // name. That name is the whole point of the next experiment: -38 has to be
    // confirmed as XR_RESULT_LIMIT_REACHED by the runtime, not by me.
    {
        PFN_xrVoidFunction f = NULL;
        if (g_xr_gipa(X->instance, "xrResultToString", &f) == XR_SUCCESS && f) {
            X->ResultToString = (PFN_xrResultToString)f;
            char nm[XR_MAX_RESULT_STRING_SIZE];
            if (X->ResultToString(X->instance, (XrResult)-38, nm) == XR_SUCCESS) {
                vr_log("OpenXR: runtime names -38 as '%s'", nm);
            }
        } else {
            vr_log("OpenXR: xrResultToString unavailable, errors stay numeric");
        }
    }

    // -- 2. system. In OpenXR 1.1 systemId is an OUT param and must not be
    //    XR_NULL_HANDLE; in 1.0 the call takes no id at all. Get both and let
    //    the instance's real version pick which one is legal, because calling
    //    the 1.0 shape against a 1.1 runtime is a validation error.
    {
        PFN_xrGetSystem GetSystem = NULL;
        g_xr_gipa(X->instance, "xrGetSystem",
                  (PFN_xrVoidFunction*)&GetSystem);
        if (!GetSystem) {
            vr_log("OpenXR: cannot resolve xrGetSystem");
            return 0;
        }
        XrSystemGetInfo gi = {0};
        gi.type       = XR_TYPE_SYSTEM_GET_INFO;
        // The field is `formFactor`, not `form`. Same class of mistake as the
        // earlier `sp.trackingProperties` one this project already paid for.
        gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

        XrSystemProperties props = {0};
        props.type = XR_TYPE_SYSTEM_PROPERTIES;

        XrResult r = GetSystem(X->instance, &gi, &X->system);
        if (r != XR_SUCCESS) {
            vr_log("OpenXR: xrGetSystem -> %s", tdvr_xr_err(X, r));
            X->system = XR_NULL_SYSTEM_ID;
            return 0;
        }
        r = X->GetSystemProperties(X->instance, X->system, &props);
        if (r == XR_SUCCESS) {
            vr_log("OpenXR: system %llu  vendor=%u  tracking pos=%d orient=%d",
                   (unsigned long long)X->system, props.vendorId,
                   (int)props.trackingProperties.positionTracking,
                   (int)props.trackingProperties.orientationTracking);
        } else {
            vr_log("OpenXR: xrGetSystemProperties -> %s", tdvr_xr_err(X, r));
        }
        X->have_system = 1;
    }

    // -- 1c. What does this runtime actually offer? Two things decide whether a
    //     session can exist at all and neither is guessable:
    //       - the extension list (a runtime may require one to be enabled)
    //       - the reference spaces (no LOCAL space = nothing to track against,
    //         and SteamVR reports that as a session failure, not a space
    //         failure -- so it would look exactly like our -38)
    {
        PFN_xrEnumerateInstanceExtensionProperties eiep = NULL;
        g_xr_gipa(X->instance, "xrEnumerateInstanceExtensionProperties",
                  (PFN_xrVoidFunction*)&eiep);
        if (eiep) {
            uint32_t en = 0;
            if (eiep("teardown_vr", 0, &en, NULL) == XR_SUCCESS && en) {
                vr_log("OpenXR: %u instance extensions offered", en);
                XrExtensionProperties* ex =
                    (XrExtensionProperties*)malloc(en * sizeof *ex);
                if (ex) {
                    if (eiep("teardown_vr", en, &en, ex) == XR_SUCCESS) {
                        for (uint32_t i = 0; i < en; i++) {
                            vr_log("OpenXR:   ext %s", ex[i].extensionName);
                        }
                    }
                    free(ex);
                }
            }
        }

        // xrEnumerateReferenceSpaces takes a SESSION, and we have no session
        // (xrCreateSession returns -38), so calling it here would only ever
        // report a handle error. Dropped deliberately: it cannot answer why
        // the session itself is refused. Reference spaces are re-checked once
        // a session exists.
    }


    TDVR_XR_GET(X, X->instance, X->CreateSession, "xrCreateSession");
    TDVR_XR_GET(X, X->instance, X->CreateReferenceSpace, "xrCreateReferenceSpace");
    TDVR_XR_GET(X, X->instance, X->LocateSpace, "xrLocateSpace");
    TDVR_XR_GET(X, X->instance, X->WaitFrame, "xrWaitFrame");
    TDVR_XR_GET(X, X->instance, X->BeginFrame, "xrBeginFrame");
    TDVR_XR_GET(X, X->instance, X->EndFrame, "xrEndFrame");
    TDVR_XR_GET(X, X->instance, X->PollEvent, "xrPollEvent");
    TDVR_XR_GET(X, X->instance, X->BeginSession, "xrBeginSession");
    TDVR_XR_GET(X, X->instance, X->DestroySession, "xrDestroySession");
    TDVR_XR_GET(X, X->instance, X->EnumerateSwapchainFormats, "xrEnumerateSwapchainFormats");
    TDVR_XR_GET(X, X->instance, X->CreateSwapchain, "xrCreateSwapchain");
    TDVR_XR_GET(X, X->instance, X->EnumerateSwapchainImages, "xrEnumerateSwapchainImages");
    TDVR_XR_GET(X, X->instance, X->DestroySwapchain, "xrDestroySwapchain");

    TDVR_XR_GET(X, X->instance, X->AcquireSwapchainImage, "xrAcquireSwapchainImage");
    TDVR_XR_GET(X, X->instance, X->WaitSwapchainImage, "xrWaitSwapchainImage");
    TDVR_XR_GET(X, X->instance, X->ReleaseSwapchainImage, "xrReleaseSwapchainImage");
    TDVR_XR_GET(X, X->instance, X->LocateViews, "xrLocateViews");
    TDVR_XR_GET(X, X->instance, X->EnumerateViewConfigurationViews,
                "xrEnumerateViewConfigurationViews");

    // -- 3. session
    {
        XrSessionCreateInfo si = {0};
        si.type = XR_TYPE_SESSION_CREATE_INFO;
        // A plain HMD session with no view or swapchain. We are not rendering
        // through OpenXR -- the game already owns its D3D12 swapchain and we
        // are not going to hand it over -- we only want the POSE.
        si.next = NULL;
        // systemId is REQUIRED and is what xrGetSystem handed back. It was left
        // zero, which is exactly what XR_FORM_FACTOR_UNSUPPORTED (-18) means:
        // a session cannot be created for a system that does not exist. The
        // earlier log blamed the runtime; this was a missing field.
        si.systemId = X->system;
        // XrSessionCreateInfo has NO viewConfigurationType field -- that is the
        // mistake this comment used to encode. Verified in the 1.1.51 header:
        // it holds only type, next, createFlags, systemId. The field lives in
        // XrViewConfigurationProperties, which is what you pass in the chain
        // to ask which configurations exist. So: enumerate the real ones and
        // put the first stereo one in the chain. That also stops guessing at
        // enum values, which is how -38 was arrived at in the first place.
        // Declared out here because the block below only has function scope, and
        // the session attempt that consumes it sits after the block ends.
        XrViewConfigurationType pick = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        {
            PFN_xrEnumerateViewConfigurations evc = NULL;
            g_xr_gipa(X->instance, "xrEnumerateViewConfigurations",
                      (PFN_xrVoidFunction*)&evc);
            if (!evc) {
                vr_log("OpenXR: runtime has no xrEnumerateViewConfigurations");
                return 0;
            }
            uint32_t n = 0;
            XrResult e = evc(X->instance, X->system, 0, &n, NULL);
            if (e != XR_SUCCESS || n == 0) {
                vr_log("OpenXR: view config count -> %s (n=%u)",
                       tdvr_xr_err(X, e), n);
                return 0;
            }
            XrViewConfigurationType* list =
                (XrViewConfigurationType*)malloc(n * sizeof *list);
            if (!list) return 0;
            e = evc(X->instance, X->system, n, &n, list);
            if (e != XR_SUCCESS) {
                vr_log("OpenXR: enumerate view configs -> %s", tdvr_xr_err(X, e));
                free(list);
                return 0;
            }
            pick = list[0];
            for (uint32_t i = 0; i < n; i++) {
                vr_log("OpenXR: view config offered: %d", (int)list[i]);
                if (list[i] == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
                    pick = list[i];
                }
            }
            free(list);

            // Which blend modes does the runtime actually allow for this view
            // configuration? A stereo config with no supported blend mode is
            // exactly the situation a runtime reports as XR_ERROR_LIMIT_REACHED,
            // and it is the one thing the spec says the app must negotiate
            // before creating a session.
            {
                PFN_xrEnumerateEnvironmentBlendModes ebm = NULL;
                g_xr_gipa(X->instance, "xrEnumerateEnvironmentBlendModes",
                          (PFN_xrVoidFunction*)&ebm);
                if (ebm) {
                    uint32_t bn = 0;
                    XrResult be = ebm(X->instance, X->system, pick, 0, &bn, NULL);
                    vr_log("OpenXR: blend modes for view %d -> %s (n=%u)",
                           (int)pick, tdvr_xr_err(X, be), bn);
                    if (be == XR_SUCCESS && bn > 0) {
                        XrEnvironmentBlendMode* bl =
                            (XrEnvironmentBlendMode*)malloc(bn * sizeof *bl);
                        if (bl) {
                            if (ebm(X->instance, X->system, pick, bn, &bn, bl)
                                    == XR_SUCCESS) {
                                for (uint32_t k = 0; k < bn; k++) {
                                    vr_log("OpenXR:   blend mode %u", (unsigned)bl[k]);
                                }
                            }
                            free(bl);
                        }
                    }
                } else {
                    vr_log("OpenXR: no xrEnumerateEnvironmentBlendModes");
                }
            }
        }

        // The session is NOT created here. SteamVR's own client log
        // (xrclient_teardown.txt) says why, verbatim:
        //     [Error] xrCreateSession: No binding struct was provided
        //     [Warning] Ignoring unsupported structs in next chain of type: 45
        // and 45 is XR_TYPE_VIEW_CONFIGURATION_PROPERTIES, not a graphics
        // binding. So the view-configuration props I used to chain here were
        // being discarded, and with nothing else in the chain the runtime found
        // no binding at all and returned XR_ERROR_GRAPHICS_DEVICE_INVALID
        // (-38, confirmed by binding xrResultToString; the session-limit code
        // is -10 and lives on a different path in vrclient_x64.dll).
        //
        // The accepted chain types are a closed set, from the binary: D3D11
        // (0x3b9b3378), Vulkan (0x3b9b2ba8), D3D12 (0x3b9b3760), OpenGL
        // win32/xlib/xcb/wayland (0x3b9b23d8..db), GLES (0x3b9b27c1), EGL_MNDX
        // (0x3b9b8584). A binding with a NULL device member is as fatal as no
        // binding (0x18006bc9b: cmp QWORD PTR [rdx+0x10],0 -> -38).
        //
        // The game's own log says it runs D3D12 when options.xml has gfxapi=1
        // (default is OpenGL, which cannot do its compute-shader lighting), so
        // the binding is XrGraphicsBindingD3D12KHR and it must carry the real
        // device and queue. Both only exist on the render thread, once a frame
        // is being drawn -- which is exactly where tdvr_xr_try_session() runs.
        X->view_config = pick;
        X->want_session = 1;
        vr_log("OpenXR: instance+system ready, session deferred to the render thread");
    }

    if (!X->have_session) {
        // No session and no device to bind one to: nothing more is possible on
        // this thread. The render hook calls tdvr_xr_try_session() when it has
        // captured a device.
        return 0;
    }

    return tdvr_xr_after_session(X);
}

// ---------------------------------------------------------------------------
// tdvr_xr_try_session - create the session once a graphics device exists.
//
// Called from the render thread, not the load thread, because that is the only
// place a device exists. The binding is the whole point: SteamVR's client log
// refuses the session with "No binding struct was provided" and returns
// XR_ERROR_GRAPHICS_DEVICE_INVALID (-38) when the chain has no graphics binding.
// A binding with a NULL device is equally fatal, so the pointers must be real.
//
// The game runs D3D12 when options.xml has gfxapi=1 (its own log says so; the
// default OpenGL path cannot do compute-shader lighting), so the D3D12 binding
// is the one to use. The view-configuration properties are chained BEHIND the
// binding: the runtime walks the chain and ignores what it does not recognise,
// and on its own it proved to be ignored and to be the only thing in the chain.
// ---------------------------------------------------------------------------
static int tdvr_xr_try_session(tdvr_xr* X, void* device, void* queue) {
    if (X->have_session)     return 1;
    if (!X->want_session)    return 0;
    if (!X->CreateSession)   return 0;
    if (X->session_tried)    return 0;
    if (!device || !queue)   return 0;   // nothing valid to bind yet

    X->session_tried = 1;
    X->gfx_device = device;
    X->gfx_queue  = queue;

    // Ask the runtime what it requires, and honour the answer. Skipping this is
    // not a formality: the runtime answered
    // XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING with a valid device and queue
    // in hand. minFeatureLevel comes back as a packed version, and
    // D3D12CreateDevice must be asked for at least that. This uses the
    // renderer's existing device, so the level is informational here -- the real
    // use is the check below that keeps the call from being skipped again.
    if (X->GetGFXD3D12) {
        XrGraphicsRequirementsD3D12KHR req = {0};
        req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR;
        XrResult rq = X->GetGFXD3D12(X->instance, X->system, &req);
        if (rq != XR_SUCCESS) {
            vr_log("OpenXR: xrGetGraphicsRequirementsD3D12 -> %s",
                   tdvr_xr_err(X, rq));
            X->gfx_device = NULL; X->gfx_queue = NULL;
            return 0;
        }
        D3D_FEATURE_LEVEL want = (D3D_FEATURE_LEVEL)req.minFeatureLevel;
        vr_log("OpenXR: graphics requirements minFeatureLevel=0x%X "
               "(the binding's device is feature level 0x%X)",
               (unsigned)want, (unsigned)D3D_FEATURE_LEVEL_11_0);
    } else {
        vr_log("OpenXR: xrGetGraphicsRequirementsD3D12 unavailable; the runtime "
               "will refuse the session with REQUIREMENTS_CALL_MISSING");
        X->gfx_device = NULL; X->gfx_queue = NULL;
        return 0;
    }

    vr_log("OpenXR: creating session on the render thread, D3D12 binding "
           "device=%p queue=%p", device, queue);

    XrGraphicsBindingD3D12KHR gb = {0};
    gb.type   = XR_TYPE_GRAPHICS_BINDING_D3D12_KHR;
    gb.device = (ID3D12Device*)device;
    gb.queue  = (ID3D12CommandQueue*)queue;   // `queue`, not commandQueue

    XrViewConfigurationProperties vp = {0};
    vp.type                 = XR_TYPE_VIEW_CONFIGURATION_PROPERTIES;
    vp.viewConfigurationType = X->view_config;

    XrSessionCreateInfo si = {0};
    si.type           = XR_TYPE_SESSION_CREATE_INFO;
    si.next           = &gb;          // binding first, props behind it
    si.createFlags    = 0;
    si.systemId       = X->system;

    // The view-configuration properties are NOT chained. SteamVR logged
    //     [Warning] xrCreateSession: Ignoring unsupported structs in next
    //               chain of type: 45
    // on every single run, and 45 is exactly XR_TYPE_VIEW_CONFIGURATION_PROPERTIES
    // -- the block below. The runtime discards it, so it contributes nothing but
    // a warning, and the view configuration is negotiated later through
    // xrEnumerateViewConfigurationViews instead. The D3D12 binding is the only
    // struct the runtime actually reads.
    (void)vp;
    gb.next = NULL;

    XrResult r = X->CreateSession(X->instance, &si, &X->session);
    if (r != XR_SUCCESS) {
        vr_log("OpenXR: xrCreateSession with D3D12 binding -> %s",
               tdvr_xr_err(X, r));
        // Release the pointers: they belong to the renderer and this attempt is
        // done, and a stale pointer in the log would mislead the next run.
        X->gfx_device = NULL;
        X->gfx_queue  = NULL;
        return 0;
    }

    X->have_session = 1;
    vr_log("OpenXR: SESSION CREATED (was impossible from the load thread)");

    int ok = tdvr_xr_after_session(X);
    if (ok) {
        // The projection layer needs a size. Prefer the renderer's real
        // backbuffer so the colour target matches what the game draws, and fall
        // back to a sane stereo default only if the swapchain has not been
        // seen yet -- a projection layer with a guessed size is still far
        // better than none, because without it the runtime stays 2D-only.
        // tdvr_xr_backbuffer_size() is defined above, next to the swapchain
        // vtable read that fills it in.
        int w = 0, h = 0;
        if (!tdvr_xr_backbuffer_size(&w, &h) || w <= 0 || h <= 0) {
            w = 1280; h = 720;
        }
        tdvr_xr_make_projection(X, w, h);
    }
    return ok;
}

// Everything that can only happen once there IS a session: the LOCAL reference
// space, which is what makes the head pose readable. Split out of the init path
// so both the load thread and tdvr_xr_try_session() can reach it.
// ---------------------------------------------------------------------------
// The command queue.
//
// An ID3D12Device does not hand out the queue it was created with, and
// IDXGISwapChain has no GetCommandQueue either, so the renderer's own queue is
// not reachable from the swapchain by any public call. Scanning memory for
// command-queue objects was rejected: an unbounded scan of the heap is tens of
// thousands of VirtualQuery calls, and doing that inside a frame stalled the
// game once already in this project.
//
// So the binding gets its own device and queue. That is legitimate and
// self-consistent -- they must come from the SAME device, and a pair made here
// is a pair -- and it is enough to make a session, because a session needs a
// capable device to talk to the compositor, not the game's own one. Using the
// game's device with a foreign queue would be invalid, so it is all or nothing.
//
// Nothing is imported: d3d12.dll is loaded by name and D3D12CreateDevice is
// resolved with GetProcAddress, which keeps the import profile at
// KERNEL32/msvcrt/USER32/ADVAPI32.
// ---------------------------------------------------------------------------
// The exact D3D12CreateDevice signature. Spelling it approximately is what
// produced three errors at once: too many arguments, a pointer from an integer,
// and then a cascade about CreateCommandQueue and Release missing, because the
// device came back as a value instead of a real ID3D12Device* and every later
// use of it failed in turn.
typedef HRESULT (WINAPI* tdvr_d3d12_create_t)(
    IUnknown* pAdapter,
    const D3D_FEATURE_LEVEL* pMinimumFeatureLevel,
    UINT FeatureLevels,
    REFIID riid,
    void** ppDevice);

static int tdvr_xr_make_device_and_queue(void** out_dev, void** out_queue) {
    *out_dev = NULL; *out_queue = NULL;

    HMODULE d3d = LoadLibraryA("d3d12.dll");
    if (!d3d) { vr_log("OpenXR: d3d12.dll not loadable"); return 0; }
    tdvr_d3d12_create_t create =
        (tdvr_d3d12_create_t)(void*)GetProcAddress(d3d, "D3D12CreateDevice");
    if (!create) { vr_log("OpenXR: D3D12CreateDevice missing"); return 0; }

    // D3D_FEATURE_LEVEL_11_0. The box has an RTX 4070, so this is not the
    // binding constraint; the runtime's requirement is, and that is separate.
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
    };
    void* dev = NULL;
    HRESULT hr = create(NULL, levels, 3, &IID_ID3D12Device, &dev);
    if (FAILED(hr) || !dev) {
        vr_log("OpenXR: D3D12CreateDevice -> hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
    ID3D12Device* d = (ID3D12Device*)dev;

    D3D12_COMMAND_QUEUE_DESC qd = {0};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    ID3D12CommandQueue* q = NULL;
    // lpVtbl-> everywhere: with COBJMACROS the C interface is the vtable, not a
    // C++ class with methods.
    hr = d->lpVtbl->CreateCommandQueue(d, &qd, &IID_ID3D12CommandQueue, (void**)&q);
    if (FAILED(hr) || !q) {
        vr_log("OpenXR: CreateCommandQueue -> hr=0x%08lX", (unsigned long)hr);
        d->lpVtbl->Release(d);
        return 0;
    }

    vr_log("OpenXR: made a device+queue pair for the binding: dev=%p queue=%p",
           dev, (void*)q);
    *out_dev = dev;
    *out_queue = q;
    return 1;
}

// ---------------------------------------------------------------------------
// tdvr_xr_bind_device_from_swapchain - get a device and queue for the binding.
//
// Called from endRender once a frame is being drawn, because that is the first
// point at which the renderer is known to be running D3D12. SteamVR's client log
// refuses the session with "No binding struct was provided" and returns
// XR_ERROR_GRAPHICS_DEVICE_INVALID (-38) when the chain has no graphics binding,
// and a binding with a NULL device or queue is equally fatal -- so nothing is
// attempted half-formed.
// ---------------------------------------------------------------------------
// Take the window's backbuffer resource, so there is something to copy FROM.
//
// Called from the render hook, where a live IDXGISwapChain is in hand. GetBuffer
// is vtable slot 9 and GetDesc is slot 12 on IDXGISwapChain (and 18 on
// IDXGISwapChain1, which is a different interface with the same leading
// entries -- 0..9 are identical, so slot 9 is safe for both).
//
// This must be read off the swapchain and not out of the earlier device-bind
// path: buffers are created with the swapchain, and the one-shot bind runs
// before the renderer has filled them in.
static void tdvr_xr_take_backbuffer(void* chain) {
    if (g_xr_backbuffer) return;      // once is enough
    if (!chain) return;

    void* vt = NULL;
    if (!td_read((uint64_t)(uintptr_t)chain, &vt, sizeof vt) || !vt) return;

    typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(void*, UINT, void*);
    PFN_GetBuffer getBuffer = NULL;
    if (!td_read((uint64_t)(uintptr_t)vt + 9 * sizeof(void*),
                 &getBuffer, sizeof getBuffer) || !getBuffer) {
        vr_log("OpenXR: GetBuffer is not readable on the swapchain vtable");
        return;
    }
    void* buf = NULL;
    HRESULT hb = getBuffer(chain, 0, &buf);
    vr_log("OpenXR: GetBuffer(slot 9) -> hr=0x%08lX backbuffer=%p",
           (unsigned long)hb, buf);
    if (SUCCEEDED(hb) && buf) g_xr_backbuffer = buf;
}

// The renderer's backbuffer size, read off the game's own swapchain. The
// projection layer must match it or the runtime composites a stretched image,
// and 0 means "not seen yet", which is a real state here: the session is
// created from the render hook, so the size is known by then but is not known
// any earlier.
static int g_xr_bb_w = 0, g_xr_bb_h = 0;

static int tdvr_xr_backbuffer_size(int* w, int* h) {
    if (!w || !h) return 0;
    *w = g_xr_bb_w;
    *h = g_xr_bb_h;
    return g_xr_bb_w > 0 && g_xr_bb_h > 0;
}

static void tdvr_xr_bind_device_from_swapchain(void* chain) {
#if TDVR_XR_OWN_DEVICE
    // Forward declarations for the mode-14 handoff. present_hook.h declares
    // these, but xr_session.h is included BEFORE it, so at this point in the
    // translation unit they are not visible yet. Without these the build fails
    // with implicit-declaration, and with the wrong-signature guesses that came
    // first, it fails with a conflicting-type error instead.
    extern volatile LONG tdvr_own_device_requested;
    int  tdvr_sc_worker_running(void);
    void tdvr_sc_wake_event(void);
#endif
    if (!g_xr.have_instance) return;
    if (g_xr.have_session) return;
    if (g_xr.session_tried) return;

#if TDVR_XR_OWN_DEVICE
    // Diagnostic mode 14, and it runs FIRST, before `chain` is touched at all.
    //
    // UPDATE 5 established that `chain` is not an IDXGISwapChain at all: its
    // vtable is in D3D12Core.dll, its GetDesc returns S_OK with a zeroed
    // descriptor and its GetBuffer returns S_OK with a null buffer. Everything
    // this function derives from it is therefore suspect, including the device
    // that was being handed to xrCreateSession. This mode's entire purpose is to
    // bind a session WITHOUT a swapchain, so putting it after the chain handling
    // guaranteed it could never run.
    //
    // It also puts NOLOOP and OWN_DEVICE on a different footing: NOLOOP still
    // went through this function and still handed the runtime a device reached
    // through the fake chain, so it never actually tested device sharing. This
    // run does.
    {
        vr_log("OpenXR: TDVR_XR_OWN_DEVICE=1 -- asking D3D12CreateDevice for a "
               "private device (no swapchain will be touched)");
        // D3D12CreateDevice takes a driver lock, and this runs on the RENDER
        // THREAD with a frame in flight. That is the same shape of stall the
        // factory walk was moved off for, and a device creation is a heavier
        // lock than GetAdapter was. Let the factory-walk worker do it instead --
        // it already exists, already outlives this call and already AddRefs what
        // it touches. g_walk_device = NULL tells it there is no device to walk,
        // which is the signal to create one instead.
        tdvr_own_device_requested = 1;
        // The worker already exists in every run where PRESENT_HOOK is on, but do
        // not assume it: if it was never started there is nothing to consume the
        // flag and the mode would silently do nothing at all, which is exactly
        // how the first attempt at mode 14 looked.
        if (tdvr_sc_worker_running()) {
            tdvr_sc_wake_event();
        } else {
            vr_log("OpenXR: TDVR_XR_OWN_DEVICE=1 but the worker is not running; "
                   "there is no thread that can create the device off the render "
                   "thread, so this mode cannot run here");
        }
        return;
    }
#endif

    if (!chain) return;

    // GetDevice is vtable slot 7 (offset 0x38) on IDXGISwapChain, counted
    // through the whole inheritance chain:
    //   0 QueryInterface  1 AddRef  2 Release
    //   3 SetPrivateData  4 SetPrivateDataInterface  5 GetPrivateData
    //   6 GetParent       7 GetDevice (0x38)          8 Present (0x40)
    //   9 GetBuffer (0x48) ...
    // Slot 8 is Present, and calling it through a GetDevice signature returned
    // S_OK with a NULL device -- a success that meant nothing, which is exactly
    // the kind of false-positive this project has been eating all along.
    //
    // Use the RENDERER's own device for the binding, not a private one. The
    // swapchain's device is the one the compositor will actually be talking to,
    // and GetDevice hands back a referenced pointer, so it is the correct thing
    // to bind. The queue still has to come from somewhere: a device does not
    // expose the queue it was created with and no public call reaches it, so a
    // queue is created on the SAME device, which keeps the pair consistent (a
    // foreign queue with this device would be invalid).
    ID3D12Device* rdev12 = NULL;
    {
        typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDevice)(void*, REFIID, void**);
        PFN_GetDevice getDevice = NULL;
        const int kGetDeviceSlot = 7;
        void** vt = NULL;
        if (!td_read((uint64_t)(uintptr_t)chain, &vt, sizeof vt) || !vt) {
            vr_log("OpenXR: swapchain %p has no vtable", chain);
            return;
        }
        if (!td_read((uint64_t)(uintptr_t)vt + kGetDeviceSlot * sizeof(void*),
                     &getDevice, sizeof getDevice) || !getDevice) {
            vr_log("OpenXR: GetDevice (slot %d) unreadable", kGetDeviceSlot);
            return;
        }
        void* any = NULL;
        // In C, a macro-expanded IID_ used as an argument must be address-taken:
        // MIDL_INTERFACE's REFIID is `const GUID*` here, and passing the GUID
        // itself is a type error. & makes it a pointer either way, and it also
        // works if the header later provides a real extern symbol.
        HRESULT hr = getDevice(chain, &IID_ID3D12Device, &any);
        vr_log("OpenXR: swapchain GetDevice(slot %d) -> hr=0x%08lX dev=%p",
               kGetDeviceSlot, (unsigned long)hr, any);
        if (FAILED(hr) || !any) {
            vr_log("OpenXR: no D3D12 device on the swapchain; the game may be on "
                   "OpenGL (options.xml gfxapi), and the D3D12 binding cannot "
                   "be built");
            return;
        }
        rdev12 = (ID3D12Device*)any;   // one reference, deliberately kept

#if TDVR_PRESENT_HOOK
        // We now hold a REAL ID3D12Device -- from GetDevice on the renderer's own
        // swapchain, not pattern-matched. That is the one handle that can lead to
        // the factory the game actually uses, which dxgi.dll!CreateDXGIFactory1
        // does not give us:
        //
        //   measured: patching dxgi.dll's shared factory vtable (slots 10 and 15,
        //   both read back VERIFIED, hook installed two log lines before the
        //   game's first frame, D3D12 confirmed on) never fires even once.
        //
        // DO NOT walk the graph here. This runs on the RENDER THREAD, and the
        // first version did exactly that: the log ended dead at
        //     [SC] device=... vtable=...
        // with nothing after it, every run, no crash, no minidump -- the thread
        // was inside GetAdapter and never came back, which is the GPU-stall
        // STALLED verdict rather than a crash. GetAdapter takes a driver lock and
        // can block behind the frame being presented.
        //
        // Hand the pointer to the worker instead. The device is kept alive by the
        // reference already held above, and the worker adds its own AddRef before
        // touching it so a release on another thread cannot pull it out from under.
        tdvr_factory_walk_later(rdev12);
#endif

        // While we are already holding the vtable, read the backbuffer size off
        // the same swapchain. GetDesc is slot 12 on IDXGISwapChain -- slot 6 is
        // GetParent, which also succeeds and returns a pointer, so calling the
        // wrong one gives a DXGI object where a DXGI_SWAPCHAIN_DESC was expected
        // and the "size" that comes back is whatever the first bytes of it mean.
        // That is exactly what happened: the size looked plausible and the
        // backbuffer never came back, because the GetBuffer below ran on the
        // same wrong base.
        {
            typedef HRESULT (STDMETHODCALLTYPE *PFN_GetDesc)(void*, void*);
            PFN_GetDesc getDesc = NULL;
            if (td_read((uint64_t)(uintptr_t)vt + 12 * sizeof(void*),
                        &getDesc, sizeof getDesc) && getDesc) {
                // DXGI_SWAPCHAIN_DESC, the real one. BufferDesc is at offset 0,
                // and its Width/Height are the first two UINTs.
                unsigned char desc[128] = {0};
                HRESULT hd = getDesc(chain, desc);
                if (SUCCEEDED(hd)) {
                    uint32_t ww = 0, hh = 0;
                    memcpy(&ww, desc + 0, sizeof ww);
                    memcpy(&hh, desc + 4, sizeof hh);
                    vr_log("OpenXR: swapchain GetDesc -> %ux%u (BufferDesc first "
                           "words: %08x %08x %08x %08x)", ww, hh, desc[0], desc[1],
                           desc[2], desc[3]);
                    if (ww > 0 && hh > 0) {
                        g_xr_bb_w = (int)ww;
                        g_xr_bb_h = (int)hh;

                        // While the vtable is in hand, also grab the backbuffer
                        // itself. GetBuffer is slot 9, and it needs the buffer index --
                        // 0 is the backbuffer, the rest are the flip-model staging
                        // buffers, which are not what the compositor wants.
                        typedef HRESULT (STDMETHODCALLTYPE *PFN_GetBuffer)(void*, UINT, void*);
                        PFN_GetBuffer getBuffer = NULL;
                        if (td_read((uint64_t)(uintptr_t)vt + 9 * sizeof(void*),
                                    &getBuffer, sizeof getBuffer) && getBuffer) {
                            void* buf = NULL;
                            HRESULT hb = getBuffer(chain, 0, &buf);
                            vr_log("OpenXR: GetBuffer(slot 9) -> hr=0x%08lX backbuffer=%p",
                                   (unsigned long)hb, buf);
                            if (SUCCEEDED(hb) && buf) g_xr_backbuffer = buf;
                        }
                    }
                }
            }
        }
    }

    void* queue = NULL;
    D3D12_COMMAND_QUEUE_DESC qd = {0};
    qd.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    HRESULT hq = rdev12->lpVtbl->CreateCommandQueue(
        rdev12, &qd, &IID_ID3D12CommandQueue, &queue);
    vr_log("OpenXR: command queue on the renderer's device -> hr=0x%08lX queue=%p",
           (unsigned long)hq, queue);
    if (FAILED(hq) || !queue) {
        rdev12->lpVtbl->Release(rdev12);
        vr_log("OpenXR: no queue, so no binding; a null queue is refused exactly "
               "like no binding at all");
        return;
    }

    // Mode 14 (TDVR_XR_OWN_DEVICE) returns at the TOP of this function, before
    // `chain` is ever read. An earlier version of it sat here instead, after the
    // queue had already been built from the renderer's device -- which is why the
    // first attempt logged "command queue on the renderer's device" and then never
    // printed the mode banner at all. Do not move it back down here.

    tdvr_xr_try_session(&g_xr, rdev12, queue);
}

// Everything that can only happen once there IS a session: the LOCAL reference
// space, which is what makes the head pose readable. Split out of the init path
// so both the load thread and tdvr_xr_try_session() can reach it.
// Build the stereo colour path: one array-2 swapchain per eye, a projection
// layer that points at both of them, and a per-frame acquire/release plus view
// locate. Without this the runtime never gets a colour target and stays on
// "waiting for stereo projection" -- it logs "no projection layer - 2D-only
// frame" for every frame the app submits without one (measured).
static void tdvr_xr_make_projection(tdvr_xr* X, int w, int h) {
    if (X->proj_made || !X->CreateSwapchain) return;
    if (w <= 0 || h <= 0) return;
    X->sc_width  = w;
    X->sc_height = h;

    // Ask the runtime what it can render into rather than guessing a format.
    // A format the runtime does not offer is a hard error at xrCreateSwapchain,
    // and the answer differs per runtime, so this query is the only portable
    // source of truth. Note the int64_t: this SDK's swapchain formats are
    // 64-bit, not the int32_t that xrCreateSwapchain itself takes.
    int64_t formats[32] = {0};
    uint32_t nfmt = 32;
    XrResult rf = X->EnumerateSwapchainFormats
                    ? X->EnumerateSwapchainFormats(X->session, 32, &nfmt, formats)
                    : XR_ERROR_FUNCTION_UNSUPPORTED;
    if (XR_FAILED(rf) || nfmt == 0) {
        vr_log("OpenXR: xrEnumerateSwapchainFormats -> %s; falling back to B8G8R8A8_SRGB",
               tdvr_xr_err(X, rf));
        formats[0] = 31;  // XR_FORMAT_B8G8R8A8_SRGB
        nfmt = 1;
    }
    // Prefer an 8-bit UNORM or SRGB format. The runtime's own note says float
    // swapchains hold linear light and need the transfer curve applied, and we
    // are handing it a plain 8-bit colour target from the game's backbuffer.
    int32_t pick = (int32_t)formats[0];
    for (uint32_t i = 0; i < nfmt; i++) {
        int64_t f = formats[i];
        if (f == 33 || f == 31 || f == 43 || f == 37) { pick = (int32_t)f; break; }
    }
    X->sc_format = pick;
    vr_log("OpenXR: %u swapchain formats offered, using %d (%dx%d)",
           nfmt, pick, w, h);

    // How many views does the stereo configuration actually have? Using 2
    // blindly is wrong if the runtime reports 1, and a projection layer whose
    // viewCount does not match the configuration is rejected. This query takes
    // the INSTANCE and the systemId, not the session.
    uint32_t nviews = 2;
    if (X->EnumerateViewConfigurationViews) {
        // Every element of the output array must carry its own `type`. A
        // zero-initialised array leaves type == 0, which is not a valid
        // XrStructureType, and the Meta runtime rejects it outright:
        //     [E] views[i] should have type 'XR_TYPE_VIEW_CONFIGURATION_VIEW'
        // Khronos' simulator never validated this, so the bug survived the whole
        // Khronos phase unnoticed. With the type wrong the call still "succeeded"
        // but the runtime left the session stuck in READY and never reached
        // RUNNING, so xrWaitFrame answered "Session is not running".
        XrViewConfigurationView vcs[2];
        ZeroMemory(vcs, sizeof(vcs));
        for (int i = 0; i < 2; i++) vcs[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
        nviews = 0;
        XrResult rv = X->EnumerateViewConfigurationViews(
            X->instance, X->system, X->view_config, 2, &nviews, vcs);
        if (XR_FAILED(rv) || nviews == 0) nviews = 2;
        vr_log("OpenXR: xrEnumerateViewConfigurationViews rv=%d count=%u", (int)rv, nviews);
    }
    X->sc_array = (int)nviews;
    vr_log("OpenXR: stereo view configuration has %u view(s)", nviews);

    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo sci = {0};
        sci.type              = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        sci.usageFlags        = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format            = pick;
        sci.sampleCount       = 1;
        sci.width             = (int32_t)w;
        sci.height            = (int32_t)h;
        sci.faceCount         = 1;
        sci.arraySize         = nviews;
        sci.mipCount          = 1;
        XrSwapchain sc = XR_NULL_HANDLE;
        vr_log("OpenXR: about to xrCreateSwapchain(eye %d) %dx%d", e, w, h);
        XrResult rc = X->CreateSwapchain(X->session, &sci, &sc);
        vr_log("OpenXR: xrCreateSwapchain(eye %d) returned %s", e, tdvr_xr_err(X, rc));
        if (XR_FAILED(rc)) {
            // Transfer-dst is a hint, not a requirement; drop it and retry once
            // rather than failing outright on a runtime that rejects the flag.
            sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
            rc = X->CreateSwapchain(X->session, &sci, &sc);
        }
        if (XR_FAILED(rc)) {
            vr_log("OpenXR: xrCreateSwapchain(eye %d) -> %s", e, tdvr_xr_err(X, rc));
            X->sc[e] = XR_NULL_HANDLE;
            X->sc_made[e] = 0;
            continue;
        }
        X->sc[e] = sc;
        X->sc_made[e] = 1;
        X->sc_image[e] = -1;
        X->sc_held[e]  = 0;
        vr_log("OpenXR: xrCreateSwapchain(eye %d) -> %s %dx%d array=%u", e,
               tdvr_xr_err(X, rc), w, h, nviews);
    }
    if (!X->sc_made[0] && !X->sc_made[1]) return;

    // The size the copy has to cover, and the resources behind both swapchains.
    // The bind runs on its own thread: xrEnumerateSwapchainImages blocks on the
    // render thread for the same reason xrEndFrame does, and the frame loop must
    // not wait for it. The acquire path skips the copy until the bind is done.
    tdvr_xr_start_bind(X);
    vr_log("OpenXR: %dx%d per eye, fence=%p, backbuffer=%p", w, h,
           X->gfx_fence, g_xr_backbuffer);

    X->proj = (XrCompositionLayerProjection*)calloc(1, sizeof *X->proj);
    if (!X->proj) return;
    X->proj->type  = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    X->proj->space = X->space;
    // This SDK calls the per-eye array `views` with a `viewCount`, not the
    // 1.1 `projection` spelling. Getting the field name wrong is a compile
    // error, which is at least honest.
    X->proj->viewCount = 2;
    X->proj->views = &X->pview[0];
    for (int e = 0; e < 2; e++) {
        X->pview[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        // This SDK carries the swapchain inside subImage, not as a direct
        // `swapchain` member -- the 1.1 flattening is not present here.
        X->pview[e].subImage.swapchain    = X->sc[e];
        X->pview[e].subImage.imageRect    = (XrRect2Di){0, 0, 0, 0};
        X->pview[e].subImage.imageArrayIndex = 0;   // slice 0 = this eye
        // A zero FOV is not a valid projection: the runtime divides by these
        // values, so fill in a real horizontal FOV per eye. The real per-eye
        // values get filled in from xrLocateViews once the runtime reports them.
        X->pview[e].fov.angleLeft   = -0.9f;
        X->pview[e].fov.angleRight  =  0.9f;
        X->pview[e].fov.angleUp     =  0.9f;
        X->pview[e].fov.angleDown   = -0.9f;
    }
    X->proj_made = 1;

    // From here on every frame has a projection layer, and xrEndFrame is the
    // call that froze the game when it ran on the render thread. Give it a
    // thread of its own.
    tdvr_xr_start_end_thread(X);
    vr_log("OpenXR: projection layer ready (2 swapchains, the runtime can composite)");
}

// Acquire both eyes' images and ask the runtime where each view is looking.
// Called once per renderable frame, between xrBeginFrame and xrEndFrame.
static void tdvr_xr_acquire_views(tdvr_xr* X, XrTime t) {
    if (!X->proj_made) return;

    XrViewLocateInfo vli = {0};
    vli.type            = XR_TYPE_VIEW_LOCATE_INFO;
    vli.viewConfigurationType = X->view_config;
    vli.displayTime     = t;

    for (int e = 0; e < 2; e++) {
        if (!X->sc_made[e]) continue;
        if (X->sc_held[e]) continue;              // still held from last frame
        if (!X->AcquireSwapchainImage || !X->WaitSwapchainImage) break;
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai = {0};
        ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
        vr_log("OpenXR: >> xrAcquireSwapchainImage(eye %d)", e);
        XrResult ra = X->AcquireSwapchainImage(X->sc[e], &ai, &idx);
        vr_log("OpenXR: << xrAcquireSwapchainImage(eye %d) -> %s idx=%u",
               e, tdvr_xr_err(X, ra), idx);
        if (XR_FAILED(ra)) { X->sc_held[e] = 0; continue; }
        X->sc_image[e] = (int32_t)idx;
        X->sc_held[e]  = 1;
        X->sc_image_ready[e] = 1;

        // Tell the layer which array slice to read for this eye.
        X->pview[e].subImage.imageArrayIndex = (int32_t)idx;

        // xrWaitSwapchainImage is deliberately NOT called. It blocks until the
        // runtime has finished its own work on the image, and from the render
        // hook that is a deadlock: we are the ones who have to draw into it, and
        // we are blocked before we can. Measured -- with the wait in place the
        // process froze here on the very first frame, every thread waiting, CPU
        // flat, and the simulator's log stopped growing.
        //
        // Skipping it is legal: the spec allows acquiring an image that is not
        // yet ready, and the runtime is what decides when it is safe to
        // release. For a colour target we overwrite completely, there is no
        // content to wait for.
        X->sc_image_ready[e] = 1;
    }

    if (X->LocateViews) {
        XrViewState vs = {0};
        vs.type = XR_TYPE_VIEW_STATE;
        XrView views[2] = {0};
        views[0].type = XR_TYPE_VIEW;
        views[1].type = XR_TYPE_VIEW;
        uint32_t got = 0, cap = 2;
        // This SDK's xrLocateViews takes the SESSION plus an XrViewLocateInfo;
        // the 1.1 flat (space, viewConfigType, displayTime, ...) form does not
        // exist here.
        XrViewLocateInfo vli = {0};
        vli.type                    = XR_TYPE_VIEW_LOCATE_INFO;
        vli.viewConfigurationType   = X->view_config;
        vli.displayTime             = t;
        XrResult rl = X->LocateViews(X->session, &vli, &vs, cap, &got, views);
        vr_log("OpenXR: >> xrLocateViews returned %s got=%u", tdvr_xr_err(X, rl), got);
        if (XR_SUCCEEDED(rl) && got > 0) {
            for (uint32_t i = 0; i < got && i < 2; i++) {
                X->pview[i].pose = views[i].pose;
                // Adopt the runtime's own FOV when it gives one: these are the
                // numbers the compositor projects with, so guessing them here
                // would fight the runtime rather than agree with it.
                if (views[i].fov.angleLeft  != 0.0f ||
                    views[i].fov.angleRight != 0.0f) {
                    X->pview[i].fov = views[i].fov;
                }
            }
            X->locate_failed = 0;
        } else if (!X->locate_failed) {
            X->locate_failed = 1;
            vr_log("OpenXR: xrLocateViews -> %s (keeping the fallback FOV)",
                   tdvr_xr_err(X, rl));
        }
    }
}

// ------------------------------------------------------------------
// Acquire both images, put pixels in them, and point the layer at them.
//
// The copy happens HERE, on the render thread, between acquire and EndFrame --
// which is where the spec requires the image to be written. It is one GPU copy
// on the game's own queue, so it is ordered against the frame in flight and
// needs no fence wait of its own.
// ------------------------------------------------------------------
static void tdvr_xr_prepare_views(tdvr_xr* X) {
    // Acquire only. The pixels go in later, on the frame-end thread, just before
    // xrEndFrame -- the image stays acquired across the handoff, which is what the
    // spec allows and what makes the write legal from the other thread.
    tdvr_xr_acquire_views(X, X->xr_time);
}

static void tdvr_xr_release_views(tdvr_xr* X) {
    for (int e = 0; e < 2; e++) {
        if (X->sc_made[e] && X->sc_held[e] && X->ReleaseSwapchainImage) {
            XrSwapchainImageReleaseInfo ri = {0};
            ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            X->ReleaseSwapchainImage(X->sc[e], &ri);
            X->sc_held[e] = 0;
        }
    }
}

// ---------------------------------------------------------------------------
// The frame-ending thread.
//
// xrEndFrame never returns when it is called on the render thread (measured:
// the process freezes on the very first frame, every thread in UserRequest,
// CPU flat, and it stays that way indefinitely). The runtime composites the
// layer we hand it, and that work needs the same device the game is using; held
// on the render thread, it waits for GPU work that cannot land because we never
// let the thread go.
//
// Moving it to its own thread costs one rule: the swapchain images must stay
// acquired until EndFrame returns, and the layer state (poses, FOV, swapchain
// pointers) must not be mutated while the runtime reads it. So this thread owns
// the images and the layer, and the render hook only fills in the poses.
// ---------------------------------------------------------------------------

// The frame the render thread hands over.
//
// Each slot is a COMPLETE snapshot: its own copy of the projection layer and of
// the two views. Sharing one layer with the render thread is a data race -- the
// game overwrites the poses while the runtime is still compositing them, and the
// projection then flickers between two head positions. One slot is enough, and
// it is not a queue but a handoff: the spec requires EndFrame to return before
// the next BeginFrame, so the producer waits rather than stacking frames.
#define TDVR_ENDQ 1

typedef struct {
    XrFrameEndInfo                  ei;
    XrCompositionLayerProjection    proj;
    XrCompositionLayerProjectionView pv[2];
    const XrCompositionLayerBaseHeader* layers[1];
} tdvr_end_slot;

static tdvr_xr*          g_endq_X = NULL;
static tdvr_end_slot     g_endq_slot[TDVR_ENDQ];
static volatile long     g_endq_busy = 0;   // 0 = free, 1 = the XR thread owns it
static HANDLE            g_endq_ev = NULL;

static DWORD WINAPI tdvr_xr_end_thread(LPVOID arg) {
    (void)arg;
    tdvr_xr* X = g_endq_X;
    if (!X) return 0;

    for (;;) {
        // Wake for a queued frame, or periodically so shutdown is never stuck
        // behind an empty slot.
        WaitForSingleObject(g_endq_ev, 100);
        if (!g_endq_busy) continue;

        tdvr_end_slot s = g_endq_slot[0];      // take our own copy
        s.ei.layers = s.layers;

        // Log the state of the frame as this thread sees it, BEFORE any work.
        // The previous run produced a log that stopped cleanly at
        //     OpenXR: xrLocateViews returned XR_SUCCESS got=2
        //     OpenXR: endRender 2 survived the original call
        // with no line from this thread at all, so it was impossible to tell
        // "the thread was never woken" from "it woke and hung inside EndFrame"
        // from "it woke and the slot was empty". Those are three different bugs
        // and the log had nothing to tell them apart.
        vr_log("[END] woke: busy=%ld pending=%ld done=%ld errors=%ld dropped=%ld",
               (long)InterlockedCompareExchange(&g_endq_busy, 0, 0),
               (long)X->end_pending, (long)X->end_done,
               (long)X->end_errors, (long)X->end_dropped);
        vr_log("[END] slot: type=0x%X layerCount=%u viewCount=%u time=%llu",
               (unsigned)s.ei.type, s.ei.layerCount,
               (unsigned)s.proj.viewCount,
               (unsigned long long)s.ei.displayTime);

        // Put the game's pixels in BEFORE the runtime reads the layer. The image
        // was acquired on the render thread and is still held, so this is where
        // it is legal to write it -- the spec ties the write to the acquire, and
        // the release below to the EndFrame that has not happened yet.
        //
        // The copy has to be here, not on the render thread: it submits work to
        // the game's own queue, and doing that from the thread that is holding
        // the frame deadlocks exactly like xrEndFrame did. Measured.
        if (g_bind_done) tdvr_xr_copy_images(X);

        unsigned long long t0 = td_ms_now();
        XrResult re = X->EndFrame ? X->EndFrame(X->session, &s.ei)
                                  : XR_ERROR_HANDLE_INVALID;
        unsigned long long dt = td_ms_now() - t0;

        if (XR_SUCCEEDED(re)) {
            X->end_done++;
        } else {
            X->end_errors++;
            if (X->end_errors < 5) {
                vr_log("OpenXR: xrEndFrame (thread) -> %s after %llu ms",
                       tdvr_xr_err(X, re), (unsigned long long)dt);
            }
        }
        X->frame_ends++;

        // Only now may the images be released: the runtime has finished reading
        // the layer, which is what the spec ties the release to.
        for (int e = 0; e < 2; e++) {
            if (X->sc_held[e] && X->ReleaseSwapchainImage) {
                XrSwapchainImageReleaseInfo ri = {0};
                ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
                X->ReleaseSwapchainImage(X->sc[e], &ri);
                X->sc_held[e] = 0;
            }
        }

        // Free the slot last: the producer must not touch it until this point,
        // and setting busy=0 is what hands it back.
        InterlockedExchange(&g_endq_busy, 0);
    }
    return 0;
}

// True while the previous frame is still being composited. The caller must not
// start a new one: xrBeginFrame is not allowed before xrEndFrame has returned,
// and skipping the whole frame here is what keeps the game running at full
// speed instead of blocking inside EndFrame on the render thread.
static int tdvr_xr_end_busy(void) {
    return InterlockedCompareExchange(&g_endq_busy, 0, 0) != 0;
}

// Start the frame-ending thread once the session exists. Failing here is not
// fatal: the loop still runs, it just has to call EndFrame inline.
static void tdvr_xr_start_end_thread(tdvr_xr* X) {
    if (X->end_thread) return;
    g_endq_X = X;
    g_endq_busy = 0;
    if (!g_endq_ev) {
        g_endq_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    }
    if (!g_endq_ev) { vr_log("OpenXR: no event for the frame-end thread"); return; }
    X->end_thread = CreateThread(NULL, 0, tdvr_xr_end_thread, X, 0, NULL);
    if (X->end_thread) {
        vr_log("OpenXR: frame-end thread started (xrEndFrame cannot run on the "
               "render thread)");
    } else {
        vr_log("OpenXR: could not start the frame-end thread; EndFrame will be "
               "called inline and may stall");
    }
}

static int tdvr_xr_after_session(tdvr_xr* X) {
    if (X->have_space) return 1;
    XrReferenceSpaceCreateInfo ri = {0};
    ri.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    ri.referenceSpaceType   = XR_REFERENCE_SPACE_TYPE_LOCAL;
    ri.poseInReferenceSpace.orientation.w = 1.0f;   // identity, not all-zero:
                                                    // a zero quaternion is
                                                    // not a valid pose and some
                                                    // runtimes reject it.
    XrResult r = X->CreateReferenceSpace(X->session, &ri, &X->space);
    if (r != XR_SUCCESS) {
        vr_log("OpenXR: xrCreateReferenceSpace -> %s", tdvr_xr_err(X, r));
        X->space = XR_NULL_HANDLE;
        return 0;
    }
    vr_log("OpenXR: LOCAL space created -- HMD pose is now readable");
    X->have_space = 1;
    return 1;
}

// Read the head pose for this frame. Cheap on purpose: it is called from the
// render hook, so it must not allocate, log, or block.
static void tdvr_xr_poll(tdvr_xr* X) {
    if (!X->have_space) return;

    // WaitFrame is called first, and it may block until the runtime wants a
    // frame. If the previous EndFrame has not come back yet, a new one cannot
    // legally be started, so skip this frame entirely rather than queueing it.
    if (X->end_thread && tdvr_xr_end_busy()) {
        X->frame_skipped++;
        return;
    }

    // The frame loop, in the order the spec requires: WAIT -> (maybe render)
    // -> BEGIN -> ... -> END. Three things were wrong here and each one alone
    // stops it working:
    //
    //   1. shouldRender was never checked, so END was called for frames the
    //      runtime had not marked renderable. A session is allowed to skip
    //      frames, and skipping is not an error.
    //   2. LocateSpace was passed a literal 0, i.e. "most recent", instead of
    //      the predicted display time that xrWaitFrame just handed back. That
    //      is legal but it is the wrong sample, and with a null driver there is
    //      nothing to guess about -- use the time the runtime says.
    //   3. xrWaitFrame BLOCKS until the runtime is ready for a frame. Calling it
    //      from the render hook, which is the only place with a device, means
    //      the game's own frame waits on the runtime. That is the correct call
    //      order but it can stall the game, so it is bounded: at most once per
    //      frame, and only while the session is READY.
    XrFrameWaitInfo wi = {0};
    wi.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState fs = {0};
    fs.type = XR_TYPE_FRAME_STATE;

    // Drain the event queue first. A session sitting in READY without ever
    // reaching FOCUSED is the state SteamVR leaves it in when nothing is
    // looking at it, and the reason is always in the events: this is the only
    // way to see what the runtime is actually asking for.
    //
    // The buffer type is XrEventDataBuffer -- an event is delivered by writing
    // into a caller-supplied buffer, and there is no `XrEventData` type to
    // declare one; the union member is named `buffer`. Getting that wrong is
    // what made the first attempt fail to compile.
    if (X->PollEvent) {
        for (int k = 0; k < 8; k++) {
            XrEventDataBuffer ev = {0};
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
            if (X->PollEvent(X->instance, &ev) != XR_SUCCESS) break;
            if (ev.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) continue;
            const XrEventDataSessionStateChanged* sc =
                (const XrEventDataSessionStateChanged*)&ev;
            if (X->state_changes++ >= 8) continue;
            // There is no `oldState` in this event -- only type, next, session,
            // state, time. A previous build of this struct carried the old state
            // in a 1.1 revision; SDK 1.1.51 here does not, so the transition is
            // logged as "now in state N" and the previous value is tracked
            // locally instead.
            vr_log("OpenXR: session is now in state %d (was %d) at time %lld",
                   (int)sc->state, (int)X->last_state, (long long)sc->time);
            X->last_state = sc->state;
            if (sc->state == XR_SESSION_STATE_READY && !X->announced_ready) {
                // The spec requires the app to tell the runtime the session is
                // ready to start rendering. Skipping this leaves the session in
                // READY with shouldRender never true -- measured: waits
                // climbing, ends stuck at 0, no frame ever produced.
                //
                // This was logging "session READY" and never calling
                // xrBeginSession, which is why the session stayed in state 2
                // and never reached FOCUSED. xrBeginSession is what moves
                // READY -> SYNCHRONIZING -> VISIBLE -> FOCUSED.
                X->announced_ready = 1;
                if (X->BeginSession) {
                    // beginInfo is NOT optional in practice. The Meta runtime
                    // validates it and logs
                    //     [E] beginInfo should not be nullptr
                    // then leaves the session in READY, so the following
                    // xrWaitFrame answers "Session is not running". Khronos'
                    // simulator accepted a NULL here, which is why the Khronos
                    // phase never surfaced it. Per the spec the primary
                    // viewConfigurationType is what the runtime cross-checks
                    // against the configuration the session was created with.
                    XrSessionBeginInfo bi = {0};
                    bi.type                     = XR_TYPE_SESSION_BEGIN_INFO;
                    bi.primaryViewConfigurationType = X->view_config;
                    vr_log("OpenXR: xrBeginSession primaryViewConfigurationType=%d "
                           "(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO=%d), state=%d",
                           (int)X->view_config,
                           (int)XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                           (int)X->last_state);
                    XrResult rb = X->BeginSession(X->session, &bi);
                    vr_log("OpenXR: xrBeginSession -> %s (from READY, viewConfigType=%d)",
                           tdvr_xr_err(X, rb), (int)X->view_config);
                } else {
                    vr_log("OpenXR: xrBeginSession was never resolved; "
                           "the session cannot leave READY");
                }
            }
        }
    }

#if TDVR_XR_NOLOOP
    // Diagnostic mode 12: the session and the state machine run, the frame loop
    // does not. xrEndFrame is what hangs, so the question this answers is whether
    // EndFrame is unreachable in this process at all -- or only unreachable while
    // the game's own Present is still outstanding. The difference between this
    // build and a normal run is exactly the WaitFrame/BeginFrame/EndFrame chain,
    // which is what makes it a usable control.
    if (!X->announced_noloop) {
        X->announced_noloop = 1;
        vr_log("OpenXR: TDVR_XR_NOLOOP=1 -- session and events run, the frame "
               "loop is not entered at all");
    }
    return;
#endif

    X->frame_waits++;
    vr_log("OpenXR: >> xrWaitFrame (wait %u, layer=%d)", X->frame_waits, X->proj_made);
    XrResult rw = XR_SUCCESS;
    if (X->WaitFrame) rw = X->WaitFrame(X->session, &wi, &fs);
    vr_log("OpenXR: << xrWaitFrame -> %s (shouldRender=%d)", tdvr_xr_err(X, rw), (int)fs.shouldRender);
    if (rw != XR_SUCCESS) {
        // XR_ERROR_SESSION_NOT_RUNNING is the normal answer while the runtime
        // has the session stopped. There is no SESSION_NOT_FOCUSED result --
        // that constant is XR_SESSION_NOT_FOCUSED, a session STATE, not an
        // error code, and comparing a result against it does not compile.
        // Counting a couple of these and staying quiet keeps a stopped session
        // from spamming the log every frame.
        if (rw != XR_ERROR_SESSION_NOT_RUNNING) {
            X->frame_errors++;
            if (X->frame_errors < 5) {
                vr_log("OpenXR: xrWaitFrame -> %s", tdvr_xr_err(X, rw));
            }
        }
        X->frame_ready = 0;
        return;
    }
    X->frame_ready = (fs.shouldRender != XR_FALSE);

    if (X->BeginFrame) {
        vr_log("OpenXR: >> xrBeginFrame");
        XrFrameBeginInfo bi = {0};
        bi.type = XR_TYPE_FRAME_BEGIN_INFO;
        XrResult rb = X->BeginFrame(X->session, &bi);
        vr_log("OpenXR: << xrBeginFrame -> %s", tdvr_xr_err(X, rb));
        if (rb != XR_SUCCESS) {
            X->frame_errors++;
            if (X->frame_errors < 5) {
                vr_log("OpenXR: xrBeginFrame -> %s", tdvr_xr_err(X, rb));
            }
            X->frame_ready = 0;
            return;
        }
    }

    X->xr_time = fs.predictedDisplayTime;   // <- was missing entirely

    XrSpaceLocation loc = {0};
    loc.type = XR_TYPE_SPACE_LOCATION;
    // The signature is xrLocateSpace(space, baseSpace, time, location) -- there
    // is no `space` or `time` member on the struct, and the time is a separate
    // argument, not a field.
    vr_log("OpenXR: >> xrLocateSpace");
    if (X->LocateSpace(X->space, X->space, X->xr_time, &loc) != XR_SUCCESS) {
        vr_log("OpenXR: << xrLocateSpace FAILED");
        X->tracking_valid = 0;
    } else {
        vr_log("OpenXR: << xrLocateSpace OK flags=0x%llx",
               (unsigned long long)loc.locationFlags);
        X->tracking_valid = (loc.locationFlags &
                             (XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ==
                            (XR_SPACE_LOCATION_POSITION_VALID_BIT |
                             XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    }

    XrPosef* p = &loc.pose;
    X->head_x = p->position.x;
    X->head_y = p->position.y;
    X->head_z = p->position.z;

    // Orientation as a quaternion -> yaw/pitch/roll, enough to aim a camera.
    float qx = p->orientation.x, qy = p->orientation.y;
    float qz = p->orientation.z, qw = p->orientation.w;
    float n = qx * qx + qy * qy + qz * qz + qw * qw;
    if (n > 1e-8f) {
        float s = 2.0f / n;
        X->yaw   = atan2f(s * (qw * qz + qx * qy),
                          1.0f - s * (qy * qy + qz * qz));
        X->pitch = asinf(2.0f / n * (qw * qy - qx * qz));
        X->roll  = atan2f(s * (qw * qx + qy * qz),
                          1.0f - s * (qx * qx + qy * qy));
    }

    // xrEndFrame closes the frame that xrBeginFrame opened, so it has to be the
    // last of the four -- and only for a frame the runtime said to render. It
    // was previously called immediately after BeginFrame, before any rendering
    // could have happened, which closes the frame early and would eventually
    // desync the loop even if everything else worked.
    if (X->frame_ready && X->EndFrame) {
        XrFrameEndInfo ei = {0};
        ei.type = XR_TYPE_FRAME_END_INFO;

        // Hand the runtime something to composite. An empty layerHeaderCount is
        // a legal frame, but it is a 2D-only frame: the runtime has no colour
        // target, logs "no projection layer - 2D-only frame", and its preview
        // never leaves "waiting for stereo projection" (measured).
        if (X->proj_made && X->end_thread) {
            tdvr_xr_prepare_views(X);
            ei.displayTime   = X->xr_time;      // required, and it is the same
                                                // time xrLocateViews just used
#if TDVR_XR_LAYER0
            // Diagnostic build: the loop runs and the session goes FOCUSED, but
            // no layer is handed over, so the runtime never composites anything.
            //
            // This is the bisect point between "the XR loop is unstable" and
            // "the runtime's compositing is what resets the GPU". Both crash runs
            // so far reported "GPU hung/removed/reset, HRESULT=887a0005"
            // (DXGI_ERROR_DEVICE_RESET) with the fault in teardown.exe at the
            // same rva 0x4ffcc8 -- identical whether the image copy was on or
            // off, so the copy is not the trigger. What differs between those
            // runs and this one is only whether the simulator draws our layer.
            // The suspicion worth testing is that the simulator composites with
            // the device and queue we handed it in the graphics binding, which
            // are Teardown's own -- so its composition contends with the game's
            // in-flight frame on the same device.
            ei.layerCount = 0;
#else
            ei.layerCount    = 1;
#endif
            X->proj_submissions++;

            // Hand over a full snapshot. The runtime reads the layer during
            // EndFrame, which happens on the XR thread, so the render thread
            // must not share the struct it is about to overwrite next frame.
            if (InterlockedCompareExchange(&g_endq_busy, 1, 0) == 0) {
                tdvr_end_slot* s = &g_endq_slot[0];
                s->ei   = ei;
                s->proj = *X->proj;
                s->pv[0] = X->pview[0];
                s->pv[1] = X->pview[1];
                // The copy's `views` still points at the originals; repoint it
                // at the copies, or the runtime would read poses the render
                // thread is about to overwrite.
                s->proj.views     = s->pv;
                s->proj.viewCount = 2;
                s->layers[0] = (const XrCompositionLayerBaseHeader*)(void*)&s->proj;
                s->ei.layers = s->layers;
                SetEvent(g_endq_ev);
                X->end_pending++;
                // Log the handover from the PRODUCER side. Combined with the
                // [END] woke line on the consumer side, this pair distinguishes
                // all four possible failures: no handoff (never logged here),
                // handoff with no wake-up, wake with a corrupt slot, and
                // EndFrame that never returns.
                if (X->end_pending <= 3 || (X->frame_waits % 30) == 0) {
                    vr_log("[END] handed over: wait=%u pending=%ld layerCount=%u "
                           "views=%u time=%llu", X->frame_waits,
                           (long)X->end_pending, (unsigned)ei.layerCount,
                           (unsigned)X->proj->viewCount,
                           (unsigned long long)ei.displayTime);
                }
            } else {
                // The previous frame is still being composited. The spec does
                // not allow a new BeginFrame before EndFrame has returned, so
                // the frame is dropped -- the images go back and the loop moves
                // on. A dropped frame is legal; blocking here would freeze the
                // game, which is the failure this whole thread exists to avoid.
                if (X->end_dropped == 0 || (X->frame_waits % 30) == 0) {
                    vr_log("OpenXR: dropping frame, the previous xrEndFrame is "
                           "still running (%ld dropped so far)", X->end_dropped + 1);
                }
                X->end_dropped++;
                tdvr_xr_release_views(X);
            }
        } else if (X->EndFrame) {
            // No thread, or no projection layer: a plain 2D frame inline. This
            // is the path that used to run for every frame, and it works -- the
            // loop was measured running at full rate with it.
            ei.displayTime = X->xr_time;
            XrResult re = X->EndFrame(X->session, &ei);
            if (re != XR_SUCCESS) {
                X->frame_errors++;
                if (X->frame_errors < 5) {
                    vr_log("OpenXR: xrEndFrame -> %s", tdvr_xr_err(X, re));
                }
            } else {
                X->frame_ends++;
            }
        }
    }
}

#endif // TDVR_XR_SESSION_H
