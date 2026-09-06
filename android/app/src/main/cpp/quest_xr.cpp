#include <jni.h>

#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kLogTag = "QuestXR";
constexpr int32_t kSwapchainWidth = 1280;
constexpr int32_t kSwapchainHeight = 720;
constexpr float kDefaultWidthMeters = 2.40f;
constexpr float kDefaultDistanceMeters = 2.00f;
constexpr float kPi = 3.14159265358979323846f;

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, kLogTag, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, kLogTag, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, kLogTag, __VA_ARGS__)

JavaVM* gVm = nullptr;
jobject gActivity = nullptr;
std::thread gXrThread;
std::mutex gLifecycleMutex;
std::atomic<bool> gRunning{false};
std::atomic<bool> gResumed{false};
std::atomic<float> gScreenWidthMeters{kDefaultWidthMeters};
std::atomic<float> gScreenDistanceMeters{kDefaultDistanceMeters};
std::atomic<float> gScreenTiltDegrees{0.0f};
std::atomic<bool> gHeadLocked{false};

struct VideoFrameCpu {
    int width = 0;
    int height = 0;
    int rotation = 0;
    int64_t timestampNs = 0;
    uint64_t sequence = 0;
    bool dirty = false;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
};

std::mutex gFrameMutex;
VideoFrameCpu gLatestFrame;
std::atomic<uint64_t> gBridgeFramesReceived{0};
std::atomic<uint64_t> gBridgeFramesOverwritten{0};
std::atomic<uint64_t> gBridgeFramesUploaded{0};

struct EglState {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
};

struct GlState {
    GLuint framebuffer = 0;
    GLuint program = 0;
    GLuint textureY = 0;
    GLuint textureU = 0;
    GLuint textureV = 0;
    GLint hasVideoLoc = -1;
    GLint yLoc = -1;
    GLint uLoc = -1;
    GLint vLoc = -1;
    int videoWidth = 0;
    int videoHeight = 0;
    uint64_t uploadedSequence = 0;
};

struct XrState {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain quadSwapchain = XR_NULL_HANDLE;
    std::vector<XrSwapchainImageOpenGLESKHR> quadImages;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
};

bool xrSucceeded(XrInstance instance, XrResult result, const char* what) {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    char buffer[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE) {
        xrResultToString(instance, result, buffer);
    } else {
        std::snprintf(buffer, sizeof(buffer), "result=%d", static_cast<int>(result));
    }
    LOGE("%s failed: %s", what, buffer);
    return false;
}

bool hasExtension(const std::vector<XrExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool initializeLoader() {
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    XrResult getResult = xrGetInstanceProcAddr(
        XR_NULL_HANDLE,
        "xrInitializeLoaderKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&initializeLoader));
    if (XR_FAILED(getResult) || initializeLoader == nullptr) {
        LOGE("xrInitializeLoaderKHR is unavailable");
        return false;
    }

    XrLoaderInitInfoAndroidKHR initInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    initInfo.applicationVM = gVm;
    initInfo.applicationContext = gActivity;
    if (!xrSucceeded(XR_NULL_HANDLE, initializeLoader(
            reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&initInfo)),
            "xrInitializeLoaderKHR")) {
        return false;
    }
    return true;
}

bool createInstance(XrState& xr) {
    uint32_t extensionCount = 0;
    if (!xrSucceeded(XR_NULL_HANDLE,
                     xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr),
                     "xrEnumerateInstanceExtensionProperties(count)")) {
        return false;
    }

    std::vector<XrExtensionProperties> extensions(extensionCount);
    for (auto& extension : extensions) {
        extension.type = XR_TYPE_EXTENSION_PROPERTIES;
        extension.next = nullptr;
    }
    if (!xrSucceeded(XR_NULL_HANDLE,
                     xrEnumerateInstanceExtensionProperties(
                         nullptr, extensionCount, &extensionCount, extensions.data()),
                     "xrEnumerateInstanceExtensionProperties(data)")) {
        return false;
    }

    const char* requiredExtensions[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };
    for (const char* required : requiredExtensions) {
        if (!hasExtension(extensions, required)) {
            LOGE("Required OpenXR extension missing: %s", required);
            return false;
        }
    }

    XrInstanceCreateInfoAndroidKHR androidCreateInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidCreateInfo.applicationVM = gVm;
    androidCreateInfo.applicationActivity = gActivity;

    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.next = &androidCreateInfo;
    std::strncpy(createInfo.applicationInfo.applicationName,
                 "XStreaming Quest Spatial",
                 XR_MAX_APPLICATION_NAME_SIZE - 1);
    createInfo.applicationInfo.applicationVersion = 1;
    std::strncpy(createInfo.applicationInfo.engineName,
                 "XStreaming",
                 XR_MAX_ENGINE_NAME_SIZE - 1);
    createInfo.applicationInfo.engineVersion = 1;
    // Quest 2 runtimes are OpenXR 1.0 conformant. Request 1.0 even when
    // compiling against newer 1.1 headers/loader for maximum compatibility.
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    createInfo.enabledExtensionCount = 2;
    createInfo.enabledExtensionNames = requiredExtensions;

    if (!xrSucceeded(XR_NULL_HANDLE, xrCreateInstance(&createInfo, &xr.instance), "xrCreateInstance")) {
        return false;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrSucceeded(xr.instance, xrGetSystem(xr.instance, &systemInfo, &xr.systemId), "xrGetSystem")) {
        return false;
    }

    XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(xr.instance, xr.systemId, &props))) {
        LOGI("OpenXR system: %s vendor=%u", props.systemName, props.vendorId);
    }
    return true;
}

bool chooseEglConfig(EglState& egl) {
    egl.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (eglInitialize(egl.display, &major, &minor) == EGL_FALSE) {
        LOGE("eglInitialize failed: 0x%x", eglGetError());
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint numConfigs = 0;
    if (eglGetConfigs(egl.display, nullptr, 0, &numConfigs) == EGL_FALSE || numConfigs <= 0) {
        LOGE("eglGetConfigs(count) failed: 0x%x", eglGetError());
        return false;
    }
    std::vector<EGLConfig> configs(static_cast<size_t>(numConfigs));
    if (eglGetConfigs(egl.display, configs.data(), numConfigs, &numConfigs) == EGL_FALSE) {
        LOGE("eglGetConfigs(data) failed: 0x%x", eglGetError());
        return false;
    }

    for (EGLConfig config : configs) {
        EGLint renderable = 0;
        EGLint surfaceType = 0;
        EGLint red = 0;
        EGLint green = 0;
        EGLint blue = 0;
        EGLint alpha = 0;
        EGLint depth = 0;
        EGLint samples = 0;
        eglGetConfigAttrib(egl.display, config, EGL_RENDERABLE_TYPE, &renderable);
        eglGetConfigAttrib(egl.display, config, EGL_SURFACE_TYPE, &surfaceType);
        eglGetConfigAttrib(egl.display, config, EGL_RED_SIZE, &red);
        eglGetConfigAttrib(egl.display, config, EGL_GREEN_SIZE, &green);
        eglGetConfigAttrib(egl.display, config, EGL_BLUE_SIZE, &blue);
        eglGetConfigAttrib(egl.display, config, EGL_ALPHA_SIZE, &alpha);
        eglGetConfigAttrib(egl.display, config, EGL_DEPTH_SIZE, &depth);
        eglGetConfigAttrib(egl.display, config, EGL_SAMPLES, &samples);

        const bool es3 = (renderable & EGL_OPENGL_ES3_BIT_KHR) != 0;
        const bool pbuffer = (surfaceType & EGL_PBUFFER_BIT) != 0;
        if (es3 && pbuffer && red == 8 && green == 8 && blue == 8 && alpha == 8 &&
            depth == 0 && samples == 0) {
            egl.config = config;
            break;
        }
    }

    if (egl.config == nullptr) {
        LOGE("No suitable RGBA8 OpenGL ES 3 EGL config found");
        return false;
    }

    const EGLint contextAttributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    egl.context = eglCreateContext(
        egl.display, egl.config, EGL_NO_CONTEXT, contextAttributes);
    if (egl.context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed: 0x%x", eglGetError());
        return false;
    }

    const EGLint surfaceAttributes[] = {
        EGL_WIDTH, 16,
        EGL_HEIGHT, 16,
        EGL_NONE,
    };
    egl.surface = eglCreatePbufferSurface(egl.display, egl.config, surfaceAttributes);
    if (egl.surface == EGL_NO_SURFACE) {
        LOGE("eglCreatePbufferSurface failed: 0x%x", eglGetError());
        return false;
    }

    if (eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context) == EGL_FALSE) {
        LOGE("eglMakeCurrent failed: 0x%x", eglGetError());
        return false;
    }

    LOGI("EGL initialized %d.%d, GLES=%s", major, minor, glGetString(GL_VERSION));
    return true;
}

bool createSession(XrState& xr, const EglState& egl) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    if (!xrSucceeded(xr.instance,
                     xrGetInstanceProcAddr(
                         xr.instance,
                         "xrGetOpenGLESGraphicsRequirementsKHR",
                         reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements)),
                     "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)") ||
        getRequirements == nullptr) {
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!xrSucceeded(xr.instance,
                     getRequirements(xr.instance, xr.systemId, &requirements),
                     "xrGetOpenGLESGraphicsRequirementsKHR")) {
        return false;
    }

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = egl.display;
    binding.config = egl.config;
    binding.context = egl.context;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = xr.systemId;
    if (!xrSucceeded(xr.instance, xrCreateSession(xr.instance, &sessionInfo, &xr.session), "xrCreateSession")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!xrSucceeded(xr.instance,
                     xrCreateReferenceSpace(xr.session, &spaceInfo, &xr.localSpace),
                     "xrCreateReferenceSpace(LOCAL)")) {
        return false;
    }
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xrSucceeded(xr.instance,
                     xrCreateReferenceSpace(xr.session, &spaceInfo, &xr.viewSpace),
                     "xrCreateReferenceSpace(VIEW)")) {
        return false;
    }

    uint32_t blendModeCount = 0;
    xrEnumerateEnvironmentBlendModes(
        xr.instance,
        xr.systemId,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0,
        &blendModeCount,
        nullptr);
    if (blendModeCount > 0) {
        std::vector<XrEnvironmentBlendMode> blendModes(blendModeCount);
        xrEnumerateEnvironmentBlendModes(
            xr.instance,
            xr.systemId,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
            blendModeCount,
            &blendModeCount,
            blendModes.data());
        if (std::find(blendModes.begin(), blendModes.end(), XR_ENVIRONMENT_BLEND_MODE_OPAQUE) !=
            blendModes.end()) {
            xr.blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        } else {
            xr.blendMode = blendModes.front();
        }
    }

    return true;
}

bool createQuadSwapchain(XrState& xr) {
    uint32_t formatCount = 0;
    if (!xrSucceeded(xr.instance,
                     xrEnumerateSwapchainFormats(xr.session, 0, &formatCount, nullptr),
                     "xrEnumerateSwapchainFormats(count)")) {
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    if (!xrSucceeded(xr.instance,
                     xrEnumerateSwapchainFormats(
                         xr.session, formatCount, &formatCount, formats.data()),
                     "xrEnumerateSwapchainFormats(data)")) {
        return false;
    }

    int64_t selectedFormat = 0;
    const int64_t preferredFormats[] = {
        static_cast<int64_t>(GL_RGBA8),
        static_cast<int64_t>(GL_SRGB8_ALPHA8),
    };
    for (int64_t preferred : preferredFormats) {
        if (std::find(formats.begin(), formats.end(), preferred) != formats.end()) {
            selectedFormat = preferred;
            break;
        }
    }
    if (selectedFormat == 0) {
        LOGE("OpenXR runtime did not expose RGBA8/SRGB8 swapchain formats");
        return false;
    }

    XrSwapchainCreateInfo swapchainInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchainInfo.createFlags = 0;
    swapchainInfo.usageFlags =
        XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchainInfo.format = selectedFormat;
    swapchainInfo.sampleCount = 1;
    swapchainInfo.width = kSwapchainWidth;
    swapchainInfo.height = kSwapchainHeight;
    swapchainInfo.faceCount = 1;
    swapchainInfo.arraySize = 1;
    swapchainInfo.mipCount = 1;
    if (!xrSucceeded(xr.instance,
                     xrCreateSwapchain(xr.session, &swapchainInfo, &xr.quadSwapchain),
                     "xrCreateSwapchain(quad)")) {
        return false;
    }

    uint32_t imageCount = 0;
    if (!xrSucceeded(xr.instance,
                     xrEnumerateSwapchainImages(xr.quadSwapchain, 0, &imageCount, nullptr),
                     "xrEnumerateSwapchainImages(count)")) {
        return false;
    }
    xr.quadImages.resize(imageCount);
    for (auto& image : xr.quadImages) {
        image.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        image.next = nullptr;
        image.image = 0;
    }
    if (!xrSucceeded(xr.instance,
                     xrEnumerateSwapchainImages(
                         xr.quadSwapchain,
                         imageCount,
                         &imageCount,
                         reinterpret_cast<XrSwapchainImageBaseHeader*>(xr.quadImages.data())),
                     "xrEnumerateSwapchainImages(data)")) {
        return false;
    }
    LOGI("Created OpenXR quad swapchain %dx%d with %u images", kSwapchainWidth, kSwapchainHeight, imageCount);
    return true;
}

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(std::max(1, logLength)));
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        LOGE("Shader compile failed: %s", log.data());
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

bool createGlState(GlState& gl) {
    static const char* vertexSource = R"GLSL(#version 300 es
precision highp float;
out vec2 vUv;
void main() {
    const vec2 positions[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0));
    const vec2 uvs[4] = vec2[4](
        vec2(0.0, 1.0),
        vec2(1.0, 1.0),
        vec2(0.0, 0.0),
        vec2(1.0, 0.0));
    gl_Position = vec4(positions[gl_VertexID], 0.0, 1.0);
    vUv = uvs[gl_VertexID];
}
)GLSL";

    static const char* fragmentSource = R"GLSL(#version 300 es
precision highp float;
in vec2 vUv;
out vec4 outColor;
uniform int uHasVideo;
uniform sampler2D uTexY;
uniform sampler2D uTexU;
uniform sampler2D uTexV;
void main() {
    if (uHasVideo == 0) {
        vec2 cell = floor(vUv * vec2(16.0, 9.0));
        float checker = mod(cell.x + cell.y, 2.0);
        vec3 a = vec3(0.015, 0.020, 0.018);
        vec3 b = vec3(0.035, 0.110, 0.050);
        vec3 color = mix(a, b, checker * 0.55);
        float edge = step(0.985, max(abs(vUv.x * 2.0 - 1.0), abs(vUv.y * 2.0 - 1.0)));
        outColor = vec4(mix(color, vec3(0.06, 0.55, 0.13), edge), 1.0);
        return;
    }

    float y = texture(uTexY, vUv).r;
    float u = texture(uTexU, vUv).r - 0.5;
    float v = texture(uTexV, vUv).r - 0.5;
    float yy = 1.16438356 * (y - 0.0625);
    vec3 rgb;
    rgb.r = yy + 1.79274107 * v;
    rgb.g = yy - 0.21324861 * u - 0.53290933 * v;
    rgb.b = yy + 2.11240179 * u;
    outColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
)GLSL";

    GLuint vertex = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return false;
    }

    gl.program = glCreateProgram();
    glAttachShader(gl.program, vertex);
    glAttachShader(gl.program, fragment);
    glLinkProgram(gl.program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(gl.program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        GLint logLength = 0;
        glGetProgramiv(gl.program, GL_INFO_LOG_LENGTH, &logLength);
        std::vector<char> log(static_cast<size_t>(std::max(1, logLength)));
        glGetProgramInfoLog(gl.program, logLength, nullptr, log.data());
        LOGE("Program link failed: %s", log.data());
        return false;
    }

    glGenFramebuffers(1, &gl.framebuffer);
    glGenTextures(1, &gl.textureY);
    glGenTextures(1, &gl.textureU);
    glGenTextures(1, &gl.textureV);
    for (GLuint texture : {gl.textureY, gl.textureU, gl.textureV}) {
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    gl.hasVideoLoc = glGetUniformLocation(gl.program, "uHasVideo");
    gl.yLoc = glGetUniformLocation(gl.program, "uTexY");
    gl.uLoc = glGetUniformLocation(gl.program, "uTexU");
    gl.vLoc = glGetUniformLocation(gl.program, "uTexV");
    return true;
}

void destroyGlState(GlState& gl) {
    if (gl.textureY) glDeleteTextures(1, &gl.textureY);
    if (gl.textureU) glDeleteTextures(1, &gl.textureU);
    if (gl.textureV) glDeleteTextures(1, &gl.textureV);
    if (gl.framebuffer) glDeleteFramebuffers(1, &gl.framebuffer);
    if (gl.program) glDeleteProgram(gl.program);
    gl = {};
}

bool uploadLatestFrame(GlState& gl) {
    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (gLatestFrame.width <= 0 || gLatestFrame.height <= 0 || gLatestFrame.y.empty()) {
        return gl.uploadedSequence != 0;
    }

    if (!gLatestFrame.dirty && gl.uploadedSequence != 0) {
        return true;
    }

    const int width = gLatestFrame.width;
    const int height = gLatestFrame.height;
    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl.textureY);
    if (gl.videoWidth != width || gl.videoHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.y.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.y.data());
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gl.textureU);
    if (gl.videoWidth != width || gl.videoHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uvWidth, uvHeight, 0, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.u.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.u.data());
    }

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gl.textureV);
    if (gl.videoWidth != width || gl.videoHeight != height) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uvWidth, uvHeight, 0, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.v.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight, GL_RED, GL_UNSIGNED_BYTE, gLatestFrame.v.data());
    }

    gl.videoWidth = width;
    gl.videoHeight = height;
    gl.uploadedSequence = gLatestFrame.sequence;
    gLatestFrame.dirty = false;
    gBridgeFramesUploaded.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void renderQuadTexture(GlState& gl, GLuint destinationTexture) {
    glBindFramebuffer(GL_FRAMEBUFFER, gl.framebuffer);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, destinationTexture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOGE("OpenXR quad framebuffer is incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }

    glViewport(0, 0, kSwapchainWidth, kSwapchainHeight);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    const bool hasVideo = uploadLatestFrame(gl);
    glUseProgram(gl.program);
    glUniform1i(gl.hasVideoLoc, hasVideo ? 1 : 0);
    glUniform1i(gl.yLoc, 0);
    glUniform1i(gl.uLoc, 1);
    glUniform1i(gl.vLoc, 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, gl.textureY);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, gl.textureU);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, gl.textureV);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

XrQuaternionf quaternionFromPitch(float degrees) {
    const float radians = degrees * kPi / 180.0f;
    const float half = radians * 0.5f;
    XrQuaternionf q{};
    q.x = std::sin(half);
    q.y = 0.0f;
    q.z = 0.0f;
    q.w = std::cos(half);
    return q;
}

bool beginSessionIfReady(XrState& xr) {
    if (xr.sessionRunning || xr.sessionState != XR_SESSION_STATE_READY || !gResumed.load()) {
        return true;
    }
    XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO};
    beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (!xrSucceeded(xr.instance, xrBeginSession(xr.session, &beginInfo), "xrBeginSession")) {
        return false;
    }
    xr.sessionRunning = true;
    LOGI("OpenXR session started");
    return true;
}

void pollEvents(XrState& xr) {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(xr.instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* stateChanged = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            xr.sessionState = stateChanged->state;
            LOGI("OpenXR session state=%d", static_cast<int>(xr.sessionState));

            if (xr.sessionState == XR_SESSION_STATE_STOPPING && xr.sessionRunning) {
                xrEndSession(xr.session);
                xr.sessionRunning = false;
                LOGI("OpenXR session stopped");
            } else if (xr.sessionState == XR_SESSION_STATE_EXITING ||
                       xr.sessionState == XR_SESSION_STATE_LOSS_PENDING) {
                gRunning.store(false);
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOGW("OpenXR instance loss pending");
            gRunning.store(false);
        }
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool renderFrame(XrState& xr, GlState& gl) {
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    if (!xrSucceeded(xr.instance, xrWaitFrame(xr.session, &waitInfo, &frameState), "xrWaitFrame")) {
        return false;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    if (!xrSucceeded(xr.instance, xrBeginFrame(xr.session, &beginInfo), "xrBeginFrame")) {
        return false;
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    std::vector<const XrCompositionLayerBaseHeader*> layers;

    if (frameState.shouldRender) {
        uint32_t imageIndex = 0;
        XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (!xrSucceeded(xr.instance,
                         xrAcquireSwapchainImage(xr.quadSwapchain, &acquireInfo, &imageIndex),
                         "xrAcquireSwapchainImage")) {
            return false;
        }

        XrSwapchainImageWaitInfo imageWait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        imageWait.timeout = XR_INFINITE_DURATION;
        if (!xrSucceeded(xr.instance,
                         xrWaitSwapchainImage(xr.quadSwapchain, &imageWait),
                         "xrWaitSwapchainImage")) {
            return false;
        }

        renderQuadTexture(gl, xr.quadImages[imageIndex].image);
        glFlush();

        XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (!xrSucceeded(xr.instance,
                         xrReleaseSwapchainImage(xr.quadSwapchain, &releaseInfo),
                         "xrReleaseSwapchainImage")) {
            return false;
        }

        const float widthMeters = std::clamp(gScreenWidthMeters.load(), 0.6f, 6.0f);
        const float distanceMeters = std::clamp(gScreenDistanceMeters.load(), 0.6f, 8.0f);
        const float tiltDegrees = std::clamp(gScreenTiltDegrees.load(), -45.0f, 45.0f);

        quad.layerFlags = 0;
        quad.space = gHeadLocked.load() ? xr.viewSpace : xr.localSpace;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = xr.quadSwapchain;
        quad.subImage.imageRect.offset = {0, 0};
        quad.subImage.imageRect.extent = {kSwapchainWidth, kSwapchainHeight};
        quad.subImage.imageArrayIndex = 0;
        quad.pose.orientation = quaternionFromPitch(tiltDegrees);
        quad.pose.position = {0.0f, 0.0f, -distanceMeters};
        quad.size = {widthMeters, widthMeters * 9.0f / 16.0f};
        layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = xr.blendMode;
    endInfo.layerCount = static_cast<uint32_t>(layers.size());
    endInfo.layers = layers.empty() ? nullptr : layers.data();
    return xrSucceeded(xr.instance, xrEndFrame(xr.session, &endInfo), "xrEndFrame");
}

void destroyXr(XrState& xr, EglState& egl, GlState& gl) {
    if (egl.display != EGL_NO_DISPLAY && egl.context != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl.display, egl.surface, egl.surface, egl.context);
        destroyGlState(gl);
    }

    if (xr.quadSwapchain != XR_NULL_HANDLE) xrDestroySwapchain(xr.quadSwapchain);
    if (xr.viewSpace != XR_NULL_HANDLE) xrDestroySpace(xr.viewSpace);
    if (xr.localSpace != XR_NULL_HANDLE) xrDestroySpace(xr.localSpace);
    if (xr.session != XR_NULL_HANDLE) xrDestroySession(xr.session);
    if (xr.instance != XR_NULL_HANDLE) xrDestroyInstance(xr.instance);

    if (egl.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl.surface != EGL_NO_SURFACE) eglDestroySurface(egl.display, egl.surface);
        if (egl.context != EGL_NO_CONTEXT) eglDestroyContext(egl.display, egl.context);
        eglTerminate(egl.display);
    }
}

void xrThreadMain() {
    LOGI("Quest OpenXR V1 thread starting");
    XrState xr;
    EglState egl;
    GlState gl;

    bool ok = initializeLoader();
    ok = ok && createInstance(xr);
    ok = ok && chooseEglConfig(egl);
    ok = ok && createSession(xr, egl);
    ok = ok && createQuadSwapchain(xr);
    ok = ok && createGlState(gl);

    if (!ok) {
        LOGE("Quest OpenXR initialization failed");
        destroyXr(xr, egl, gl);
        gRunning.store(false);
        return;
    }

    LOGI("Quest OpenXR V1 initialized. Waiting for session READY.");
    auto lastStats = std::chrono::steady_clock::now();

    while (gRunning.load()) {
        pollEvents(xr);
        if (!gRunning.load()) break;
        if (!beginSessionIfReady(xr)) {
            break;
        }

        if (xr.sessionRunning) {
            if (!renderFrame(xr, gl)) {
                LOGW("OpenXR frame failed; continuing while runtime is alive");
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= std::chrono::seconds(5)) {
            LOGI("[QuestXRBridge] received=%llu uploaded=%llu overwritten=%llu",
                 static_cast<unsigned long long>(gBridgeFramesReceived.load()),
                 static_cast<unsigned long long>(gBridgeFramesUploaded.load()),
                 static_cast<unsigned long long>(gBridgeFramesOverwritten.load()));
            lastStats = now;
        }
    }

    if (xr.sessionRunning) {
        xrRequestExitSession(xr.session);
    }
    destroyXr(xr, egl, gl);
    LOGI("Quest OpenXR V1 thread stopped");
}

bool copyPlane(JNIEnv* env,
               jobject buffer,
               int stride,
               int rowBytes,
               int rows,
               std::vector<uint8_t>& out) {
    if (buffer == nullptr || stride <= 0 || rowBytes <= 0 || rows <= 0) {
        return false;
    }
    auto* source = static_cast<uint8_t*>(env->GetDirectBufferAddress(buffer));
    const jlong capacity = env->GetDirectBufferCapacity(buffer);
    if (source == nullptr || capacity <= 0) {
        return false;
    }
    const int64_t required = static_cast<int64_t>(rows - 1) * stride + rowBytes;
    if (required > capacity) {
        LOGW("I420 plane too small: required=%lld capacity=%lld",
             static_cast<long long>(required), static_cast<long long>(capacity));
        return false;
    }
    out.resize(static_cast<size_t>(rowBytes) * rows);
    for (int row = 0; row < rows; ++row) {
        std::memcpy(out.data() + static_cast<size_t>(row) * rowBytes,
                    source + static_cast<size_t>(row) * stride,
                    static_cast<size_t>(rowBytes));
    }
    return true;
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT void JNICALL
Java_com_xstreaming_QuestOpenXrActivity_nativeStart(JNIEnv* env, jclass, jobject activity) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    if (gRunning.load()) {
        return;
    }
    if (gXrThread.joinable()) {
        gXrThread.join();
    }
    if (gActivity != nullptr) {
        env->DeleteGlobalRef(gActivity);
        gActivity = nullptr;
    }
    gActivity = env->NewGlobalRef(activity);
    gRunning.store(true);
    gXrThread = std::thread(xrThreadMain);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xstreaming_QuestOpenXrActivity_nativeSetResumed(JNIEnv*, jclass, jboolean resumed) {
    gResumed.store(resumed == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xstreaming_QuestOpenXrActivity_nativeSetScreenConfig(
    JNIEnv*, jclass, jfloat widthMeters, jfloat distanceMeters, jfloat tiltDegrees, jboolean headLocked) {
    gScreenWidthMeters.store(widthMeters);
    gScreenDistanceMeters.store(distanceMeters);
    gScreenTiltDegrees.store(tiltDegrees);
    gHeadLocked.store(headLocked == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xstreaming_QuestOpenXrActivity_nativeSubmitI420Frame(
    JNIEnv* env,
    jclass,
    jobject yBuffer,
    jobject uBuffer,
    jobject vBuffer,
    jint width,
    jint height,
    jint strideY,
    jint strideU,
    jint strideV,
    jint rotation,
    jlong timestampNs) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096) {
        return;
    }

    const int uvWidth = (width + 1) / 2;
    const int uvHeight = (height + 1) / 2;

    std::lock_guard<std::mutex> lock(gFrameMutex);
    if (gLatestFrame.dirty) {
        gBridgeFramesOverwritten.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;
    if (!copyPlane(env, yBuffer, strideY, width, height, y) ||
        !copyPlane(env, uBuffer, strideU, uvWidth, uvHeight, u) ||
        !copyPlane(env, vBuffer, strideV, uvWidth, uvHeight, v)) {
        return;
    }

    gLatestFrame.width = width;
    gLatestFrame.height = height;
    gLatestFrame.rotation = rotation;
    gLatestFrame.timestampNs = timestampNs;
    gLatestFrame.sequence += 1;
    gLatestFrame.dirty = true;
    gLatestFrame.y.swap(y);
    gLatestFrame.u.swap(u);
    gLatestFrame.v.swap(v);
    gBridgeFramesReceived.fetch_add(1, std::memory_order_relaxed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_xstreaming_QuestOpenXrActivity_nativeStop(JNIEnv* env, jclass) {
    std::lock_guard<std::mutex> lock(gLifecycleMutex);
    gRunning.store(false);
    if (gXrThread.joinable()) {
        gXrThread.join();
    }
    if (gActivity != nullptr) {
        env->DeleteGlobalRef(gActivity);
        gActivity = nullptr;
    }
}
