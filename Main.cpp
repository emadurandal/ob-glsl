#include "EmacsModule.hpp"
#include "Window.hpp"
#include "GLHelper.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

static const std::string vertexShader = R"(
#version 330 core
const vec4 vertices[4] = vec4[4](
    vec4(-1.0,  1.0, 0.0, 1.0),
    vec4(-1.0, -1.0, 0.0, 1.0),
    vec4( 1.0, -1.0, 0.0, 1.0),
    vec4( 1.0,  1.0, 0.0, 1.0)
);
void main() {
    gl_Position = vertices[gl_VertexID];
})";

static std::vector<Shader> compileShaders(const std::string& fragmentShader) {
    std::vector<Shader> shaders;
    shaders.emplace_back(GL_VERTEX_SHADER, vertexShader);
    shaders.emplace_back(GL_FRAGMENT_SHADER, fragmentShader);
    return shaders;
}

class Renderer {
    struct ReadbackSlot {
        GLuint pbo = 0;
        GLsync fence = nullptr;
        uint64_t sequence = 0;
        bool inFlight = false;
    };

    int _width;
    int _height;
    size_t _pixelBytes;
    FrameBuffer _renderFramebuffer;
    RenderBuffer _renderBuffer;
    FrameBuffer _readbackFramebuffer;
    RenderBuffer _readbackBuffer;
    RenderProgram _program;
    VertexArray _vertexArray;
    GLint _resolutionLocation;
    GLint _timeLocation;
    std::array<ReadbackSlot, 2> _readbacks;
    uint64_t _nextSequence = 1;

    void setupFramebuffer(FrameBuffer& framebuffer, RenderBuffer& renderbuffer) {
        renderbuffer.bind();
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, _width, _height);
        framebuffer.bind();
        framebuffer.renderBuffer(renderbuffer);
        if (!framebuffer.check())
            throw std::runtime_error("OpenGL framebuffer is not complete");
    }

    void renderFrame(double time) {
        _renderFramebuffer.bind();
        glViewport(0, 0, _width, _height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        _vertexArray.bind();
        _program.use();
        if (_resolutionLocation >= 0)
            glUniform2f(_resolutionLocation,
                        static_cast<float>(_width),
                        static_cast<float>(_height));
        if (_timeLocation >= 0)
            glUniform1f(_timeLocation, static_cast<float>(time));
        glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

        // Canvas and PNG rows are top-to-bottom, while OpenGL readback starts
        // at the lower-left. Reverse Y entirely on the GPU.
        _renderFramebuffer.bind(GL_READ_FRAMEBUFFER);
        _readbackFramebuffer.bind(GL_DRAW_FRAMEBUFFER);
        glBlitFramebuffer(0, 0, _width, _height,
                          0, _height, _width, 0,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }

    void bindReadbackFramebuffer() const {
        _readbackFramebuffer.bind(GL_READ_FRAMEBUFFER);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }

    int oldestInFlightSlot() const {
        int oldest = -1;
        uint64_t sequence = std::numeric_limits<uint64_t>::max();
        for (size_t i = 0; i < _readbacks.size(); ++i) {
            if (_readbacks[i].inFlight && _readbacks[i].sequence < sequence) {
                oldest = static_cast<int>(i);
                sequence = _readbacks[i].sequence;
            }
        }
        return oldest;
    }

    int freeSlot() const {
        for (size_t i = 0; i < _readbacks.size(); ++i)
            if (!_readbacks[i].inFlight)
                return static_cast<int>(i);
        return -1;
    }

    bool publishReady(emacs_env* env, emacs_value canvas) {
        const int index = oldestInFlightSlot();
        if (index < 0)
            return false;

        ReadbackSlot& slot = _readbacks[static_cast<size_t>(index)];
        const GLenum status = glClientWaitSync(
            slot.fence, SyncObjectMask::GL_NONE_BIT, 0);
        if (status == GL_TIMEOUT_EXPIRED)
            return false;
        if (status == GL_WAIT_FAILED)
            throw std::runtime_error("failed while polling an OpenGL readback fence");

        uint32_t* canvasPixels = env->canvas_data(env, canvas);
        if (!canvasPixels)
            return false;

        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
                                        static_cast<GLsizeiptr>(_pixelBytes),
                                        GL_MAP_READ_BIT);
        if (!mapped) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            throw std::runtime_error("failed to map completed OpenGL pixel buffer");
        }
        std::memcpy(canvasPixels, mapped, _pixelBytes);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glDeleteSync(slot.fence);
        slot.fence = nullptr;
        slot.inFlight = false;
        return true;
    }

    void queueReadback(double time) {
        const int index = freeSlot();
        if (index < 0)
            return;

        renderFrame(time);
        bindReadbackFramebuffer();

        ReadbackSlot& slot = _readbacks[static_cast<size_t>(index)];
        glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
        glReadPixels(0, 0, _width, _height,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                     reinterpret_cast<void*>(0));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!slot.fence)
            throw std::runtime_error("failed to create OpenGL readback fence");
        slot.sequence = _nextSequence++;
        slot.inFlight = true;
        glFlush();
    }

public:
    Renderer(const std::string& fragmentShader, int width, int height)
        : _width(width),
          _height(height),
          _pixelBytes(static_cast<size_t>(width) * static_cast<size_t>(height) * 4),
          _program(compileShaders(fragmentShader)),
          _resolutionLocation(_program.getUniformLocation("iResolution")),
          _timeLocation(_program.getUniformLocation("iTime")) {
        if (width <= 0 || height <= 0)
            throw std::runtime_error("render dimensions must be positive");
        if (static_cast<size_t>(width) > std::numeric_limits<size_t>::max() /
                                             static_cast<size_t>(height) / 4)
            throw std::runtime_error("render dimensions are too large");

        setupFramebuffer(_renderFramebuffer, _renderBuffer);
        setupFramebuffer(_readbackFramebuffer, _readbackBuffer);

        for (ReadbackSlot& slot : _readbacks) {
            glGenBuffers(1, &slot.pbo);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, slot.pbo);
            glBufferData(GL_PIXEL_PACK_BUFFER,
                         static_cast<GLsizeiptr>(_pixelBytes), nullptr,
                         GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }

    ~Renderer() {
        for (ReadbackSlot& slot : _readbacks) {
            if (slot.fence)
                glDeleteSync(slot.fence);
            if (slot.pbo)
                glDeleteBuffers(1, &slot.pbo);
        }
    }

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    void renderCanvasSync(emacs_env* env, emacs_value canvas, double time) {
        renderFrame(time);
        bindReadbackFramebuffer();
        uint32_t* canvasPixels = env->canvas_data(env, canvas);
        if (!canvasPixels)
            return;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(0, 0, _width, _height,
                     GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                     canvasPixels);
    }

    bool renderCanvasAsync(emacs_env* env, emacs_value canvas, double time) {
        const bool published = publishReady(env, canvas);
        queueReadback(time);
        return published;
    }

    void savePng(const std::string& outputPath, double time) {
        renderFrame(time);
        bindReadbackFramebuffer();

        std::vector<uint8_t> pixels(_pixelBytes);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glReadPixels(0, 0, _width, _height,
                     GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(
            pixels.data(), _width, _height, 32, _width * 4,
            SDL_PIXELFORMAT_RGBA32);
        if (!surface)
            throw std::runtime_error(std::string("failed to create PNG surface: ") +
                                     SDL_GetError());
        const int result = IMG_SavePNG(surface, outputPath.c_str());
        SDL_FreeSurface(surface);
        if (result != 0)
            throw std::runtime_error(std::string("failed to save PNG: ") + IMG_GetError());
    }
};

static void rendererFinalizer(void* data) noexcept {
    try {
        if (data) {
            makeCurrent();
            delete static_cast<Renderer*>(data);
        }
    } catch (...) {
        // Emacs module finalizers cannot report errors.
    }
}

static Renderer* getRenderer(emacs_env* env, emacs_value value) {
    Renderer* renderer = static_cast<Renderer*>(env->get_user_ptr(env, value));
    if (!renderer)
        throw std::runtime_error("GLSL renderer has already been destroyed");
    return renderer;
}

static emacs_value booleanValue(emacs_env* env, bool value) {
    return env->intern(env, value ? "t" : "nil");
}

emacs_value obGlslCreateRenderer(emacs_env* env,
                                 const std::string& shaderCode,
                                 int width,
                                 int height) {
    makeCurrent();
    std::unique_ptr<Renderer> renderer(new Renderer(shaderCode, width, height));
    emacs_value value = env->make_user_ptr(env, rendererFinalizer, renderer.get());
    renderer.release();
    return value;
}

emacs_value obGlslRenderCanvasSync(emacs_env* env,
                                   emacs_value rendererValue,
                                   emacs_value canvas,
                                   double time) {
    makeCurrent();
    getRenderer(env, rendererValue)->renderCanvasSync(env, canvas, time);
    return env->intern(env, "nil");
}

emacs_value obGlslRenderCanvas(emacs_env* env,
                               emacs_value rendererValue,
                               emacs_value canvas,
                               double time) {
    makeCurrent();
    return booleanValue(
        env, getRenderer(env, rendererValue)->renderCanvasAsync(env, canvas, time));
}

emacs_value obGlslDestroyRenderer(emacs_env* env, emacs_value rendererValue) {
    Renderer* renderer = static_cast<Renderer*>(env->get_user_ptr(env, rendererValue));
    if (renderer) {
        env->set_user_finalizer(env, rendererValue, nullptr);
        env->set_user_ptr(env, rendererValue, nullptr);
        makeCurrent();
        delete renderer;
    }
    return env->intern(env, "nil");
}

emacs_value obGlslRun(emacs_env* env,
                      const std::string& shaderCode,
                      int width,
                      int height,
                      const std::string& outputPath,
                      double time) {
    makeCurrent();
    Renderer renderer(shaderCode, width, height);
    renderer.savePng(outputPath, time);
    return env->make_string(env, outputPath.data(), outputPath.size());
}

int emacs_module_init(emacs_runtime* runtime) noexcept {
    if (static_cast<size_t>(runtime->size) < sizeof(*runtime))
        return 1;

    emacs_env* env = runtime->get_environment(runtime);
    if (static_cast<size_t>(env->size) < sizeof(*env))
        return 2;

    try {
        init();
        glbinding::Binding::initialize(nullptr);
        bindFunction(env, obGlslCreateRenderer, "ob-glsl-create-renderer");
        bindFunction(env, obGlslRenderCanvasSync, "ob-glsl-render-canvas-sync");
        bindFunction(env, obGlslRenderCanvas, "ob-glsl-render-canvas");
        bindFunction(env, obGlslDestroyRenderer, "ob-glsl-destroy-renderer");
        bindFunction(env, obGlslRun, "ob-glsl-run");
        provide(env, "ob-glsl-module");
        return 0;
    } catch (const std::exception& error) {
        reportError(env, error);
        return 3;
    }
}
