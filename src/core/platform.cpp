#include "core/platform.h"
#include <SDL3/SDL.h>
#include <cstdio>

struct Platform::Impl {
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    bool running = false;
    uint64_t frameCount = 0;
    ResizeCallback resizeCb;
};

Platform::Platform() : impl(new Impl) {}
Platform::~Platform() { delete impl; }

bool Platform::init(const PlatformConfig& config) {
    // Force X11 backend for GLX compatibility with GLEW
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL3 init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    if (config.msaaSamples > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, config.msaaSamples);
    }

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (config.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    flags |= SDL_WINDOW_RESIZABLE;

    impl->window = SDL_CreateWindow(config.title.c_str(), config.width, config.height, flags);
    if (!impl->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    impl->glContext = SDL_GL_CreateContext(impl->window);
    if (!impl->glContext) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);

    impl->running = true;
    running = true;
    return true;
}

void Platform::shutdown() {
    if (impl->glContext) SDL_GL_DestroyContext(impl->glContext);
    if (impl->window) SDL_DestroyWindow(impl->window);
    SDL_Quit();
    running = false;
}

bool Platform::processEvents() {
    inputState.mouseDeltaX = 0;
    inputState.mouseDeltaY = 0;
    inputState.mouseWheel = 0;
    inputState.textInput.clear();

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                running = false;
                return false;
            case SDL_EVENT_TEXT_INPUT:
                inputState.textInput += e.text.text;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.scancode < 512) inputState.keysDown[e.key.scancode] = true;
                if (e.key.key == SDLK_ESCAPE) { running = false; return false; }
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode < 512) inputState.keysDown[e.key.scancode] = false;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button < 8) inputState.mouseButtons[e.button.button] = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button < 8) inputState.mouseButtons[e.button.button] = false;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                inputState.mouseDeltaX += e.motion.xrel;
                inputState.mouseDeltaY += e.motion.yrel;
                inputState.mouseX = e.motion.x;
                inputState.mouseY = e.motion.y;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                inputState.mouseWheel = e.wheel.y;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                if (impl->resizeCb) impl->resizeCb(e.window.data1, e.window.data2);
                break;
        }
    }
    impl->frameCount++;
    return true;
}

void Platform::swapBuffers() { SDL_GL_SwapWindow(impl->window); }
bool Platform::isRunning() const { return running; }

int32_t Platform::width() const {
    int w; SDL_GetWindowSize(impl->window, &w, nullptr); return w;
}

int32_t Platform::height() const {
    int h; SDL_GetWindowSize(impl->window, nullptr, &h); return h;
}

float Platform::aspect() const { return (float)width() / (float)height(); }
double Platform::time() const { return SDL_GetTicks() / 1000.0; }
uint64_t Platform::frameCount() const { return impl->frameCount; }

void Platform::setTitle(const char* title) { SDL_SetWindowTitle(impl->window, title); }
void Platform::showMouse(bool show) { if (show) SDL_ShowCursor(); else SDL_HideCursor(); }
void Platform::setMousePos(int32_t x, int32_t y) { SDL_WarpMouseInWindow(impl->window, x, y); }
void Platform::setRelativeMouse(bool relative) { SDL_SetWindowRelativeMouseMode(impl->window, relative); }

void* Platform::nativeWindow() { return impl->window; }
void* Platform::nativeGLContext() { return impl->glContext; }

void Platform::setResizeCallback(ResizeCallback cb) { impl->resizeCb = std::move(cb); }

void Platform::startTextInput() { SDL_StartTextInput(impl->window); }
void Platform::stopTextInput() { SDL_StopTextInput(impl->window); }
