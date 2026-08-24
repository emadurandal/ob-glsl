#include "Window.hpp"
#include <SDL2/SDL.h>
#include <string>
#include <stdexcept>

static SDL_Window* win = nullptr;
static SDL_GLContext ctx = nullptr;

void init() {
    if (win && ctx) {
        makeCurrent();
        return;
    }

    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        throw std::runtime_error(std::string("failed to initialize SDL video: ") + SDL_GetError());
    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE); 

    win = SDL_CreateWindow("ob-glsl", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        1, 1, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!win)
        throw std::runtime_error(std::string("failed to create hidden SDL window: ") + SDL_GetError());
    ctx = SDL_GL_CreateContext(win);
    if (!ctx)
        throw std::runtime_error(std::string("failed to create OpenGL context: ") + SDL_GetError());
    makeCurrent();
}

void makeCurrent() {
    if (!win || !ctx)
        throw std::runtime_error("OpenGL context has not been initialized");
    if (SDL_GL_MakeCurrent(win, ctx) != 0)
        throw std::runtime_error(std::string("failed to make OpenGL context current: ") + SDL_GetError());
}
