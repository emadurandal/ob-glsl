#pragma once

#include <glbinding/Binding.h>
#include <glbinding/gl33core/gl.h>
#include <vector>
#include <string>

using namespace gl33core;

class Shader {
    GLuint _handle = 0;
public:
    Shader(GLenum type, const std::string& src) {
        _handle = glCreateShader(type);
        const char* srcList[] = {src.c_str()};
        glShaderSource(_handle, 1, srcList, nullptr);
        glCompileShader(_handle);
        GLint compilationOk;
        glGetShaderiv(_handle, GL_COMPILE_STATUS, &compilationOk);
        if (!compilationOk)
        {
            GLint errLength;
            glGetShaderiv(_handle, GL_INFO_LOG_LENGTH, &errLength);
            std::string errLog(errLength, '\0');
            glGetShaderInfoLog(_handle, errLength, &errLength, errLog.data());
            throw std::runtime_error("shader compilation failed:" + errLog);
        }
    }
    Shader(Shader&& other) noexcept {
        _handle = other._handle;
        other._handle = 0;
    }
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    virtual ~Shader() {
        if (_handle) {
            glDeleteShader(_handle);
        }
    }
    GLuint handle() const {return _handle;}
};

class RenderProgram {
    GLuint _handle = 0;
public:
    RenderProgram(const std::vector<Shader>& shaders)
    {
        _handle = glCreateProgram();
        for(const auto& shader: shaders) {
            glAttachShader(_handle, shader.handle());
        }
        glLinkProgram(_handle);
        GLint isLinked = 0;
        glGetProgramiv(_handle, GL_LINK_STATUS, &isLinked);
        if(!isLinked) {
            GLint maxLength = 0;
            glGetProgramiv(_handle, GL_INFO_LOG_LENGTH, &maxLength);
            std::string infoLog(static_cast<size_t>(maxLength > 0 ? maxLength : 1), '\0');
            glGetProgramInfoLog(_handle, maxLength, nullptr, infoLog.data());
            glDeleteProgram(_handle);
            _handle = 0;
            throw std::runtime_error("failed to link program: " + infoLog);
        }
    }
    RenderProgram(RenderProgram&& other) noexcept {
        _handle = other._handle;
        other._handle = 0;
    }
    RenderProgram(const RenderProgram&) = delete;
    RenderProgram& operator=(const RenderProgram&) = delete;
    virtual ~RenderProgram()
    {
        if (_handle)
            glDeleteProgram(_handle);
    }
    void use() const
    {
        glUseProgram(_handle);
    }
    GLuint getUniformLocation(const char* name) const
    {
        return glGetUniformLocation(_handle, name);
    }
};

class VertexArray {
    GLuint _handle = 0;
public:
    VertexArray() {
        glGenVertexArrays(1, &_handle);
    }
    VertexArray(VertexArray&& other) noexcept {
        _handle = other._handle;
        other._handle = 0;
    }
    VertexArray(const VertexArray&) = delete;
    VertexArray& operator=(const VertexArray&) = delete;
    virtual ~VertexArray() {
        if (_handle) {
            glDeleteVertexArrays(1, &_handle);
        }
    }
    void bind() {
        glBindVertexArray(_handle);
    }
};

class RenderBuffer {
    GLuint _handle = 0;
    friend class FrameBuffer;
public:
    RenderBuffer() {
        glGenRenderbuffers(1, &_handle);
    }
    virtual ~RenderBuffer() {
        if (_handle)
            glDeleteRenderbuffers(1, &_handle);
    }
    RenderBuffer(RenderBuffer&& other) noexcept {
        _handle = other._handle;
        other._handle = 0;
    }
    RenderBuffer(const RenderBuffer&) = delete;
    RenderBuffer& operator=(const RenderBuffer&) = delete;
    void bind() {
        glBindRenderbuffer(GL_RENDERBUFFER, _handle);
    }
};

class FrameBuffer {
    GLuint _handle = 0;
public:
    FrameBuffer() {
        glGenFramebuffers(1, &_handle);
    }
    virtual ~FrameBuffer() {
        if (_handle)
            glDeleteFramebuffers(1, &_handle);
    }
    FrameBuffer(FrameBuffer&& other) noexcept {
        _handle = other._handle;
        other._handle = 0;
    }
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
    void bind(GLenum target = GL_FRAMEBUFFER) const {
        glBindFramebuffer(target, _handle);
    }
    void renderBuffer(const RenderBuffer& rb) {
        glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb._handle);
    }
    bool check() {
        return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    GLuint handle() const { return _handle; }
};
