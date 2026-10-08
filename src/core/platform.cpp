#include "core/platform.h"
#include "core/input_parity.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstdio>
#include <iterator>

struct Platform::Impl {
    SDL_Window* window = nullptr;
    SDL_GLContext glContext = nullptr;
    SDL_Joystick* joystick = nullptr;
    SDL_JoystickID joystickId = 0;
    bool running = false;
    uint64_t frameCount = 0;
    ResizeCallback resizeCb;
};

static void openFirstJoystick(SDL_Joystick*& current, SDL_JoystickID& currentId, InputState& input) {
    if (current) return;
    int count = 0;
    SDL_JoystickID* devices = SDL_GetJoysticks(&count);
    for (int i = 0; devices && i < count; ++i) {
        SDL_Joystick* joystick = SDL_OpenJoystick(devices[i]);
        if (!joystick) continue;
        current = joystick;
        currentId = SDL_GetJoystickID(joystick);
        input.joystickConnected = true;
        std::fill(std::begin(input.joystickAxes), std::end(input.joystickAxes), 0.0f);
        std::fill(std::begin(input.joystickButtons), std::end(input.joystickButtons), false);
        std::fill(std::begin(input.consumedJoystickButtons), std::end(input.consumedJoystickButtons), false);
        std::fill(std::begin(input.consumedJoystickAxes), std::end(input.consumedJoystickAxes), false);
        input.joystickButtonPressQueue.clear();
        input.joystickHat = 0;
        input.consumedJoystickHat = false;
        for (int axis = 0; axis < std::min(SDL_GetNumJoystickAxes(joystick), JoystickInput::MaxAxes); ++axis) {
            Sint16 value = 0;
            if (SDL_GetJoystickAxisInitialState(joystick, axis, &value))
                input.joystickAxes[axis] = JoystickInput::normalizeAxis(value);
        }
        for (int button = 0; button < std::min(SDL_GetNumJoystickButtons(joystick), JoystickInput::MaxButtons); ++button)
            input.joystickButtons[button] = SDL_GetJoystickButton(joystick, button);
        if (SDL_GetNumJoystickHats(joystick) > 0)
            input.joystickHat = SDL_GetJoystickHat(joystick, 0);
        break;
    }
    SDL_free(devices);
}

static void closeJoystick(SDL_Joystick*& current, SDL_JoystickID& currentId, InputState& input) {
    if (current) SDL_CloseJoystick(current);
    current = nullptr;
    currentId = 0;
    input.joystickConnected = false;
    std::fill(std::begin(input.joystickAxes), std::end(input.joystickAxes), 0.0f);
    std::fill(std::begin(input.joystickButtons), std::end(input.joystickButtons), false);
    std::fill(std::begin(input.consumedJoystickButtons), std::end(input.consumedJoystickButtons), false);
    std::fill(std::begin(input.consumedJoystickAxes), std::end(input.consumedJoystickAxes), false);
    input.joystickButtonPressQueue.clear();
    input.joystickHat = 0;
    input.consumedJoystickHat = false;
}

Platform::Platform() : impl(new Impl) {}
Platform::~Platform() { shutdown(); delete impl; }

bool Platform::init(const PlatformConfig& config) {
    // Force X11 backend for GLX compatibility with GLEW
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK)) {
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
        SDL_Quit();
        return false;
    }
    SDL_RaiseWindow(impl->window);

    impl->glContext = SDL_GL_CreateContext(impl->window);
    if (!impl->glContext) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(impl->window);
        impl->window = nullptr;
        SDL_Quit();
        return false;
    }

    SDL_GL_SetSwapInterval(config.vsync ? 1 : 0);

    impl->running = true;
    running = true;
    mouseEnabled = true;
    SDL_SetJoystickEventsEnabled(true);
    openFirstJoystick(impl->joystick, impl->joystickId, inputState);
    return true;
}

void Platform::shutdown() {
    if (!impl) return;
    closeJoystick(impl->joystick, impl->joystickId, inputState);
    if (impl->glContext) SDL_GL_DestroyContext(impl->glContext);
    impl->glContext = nullptr;
    if (impl->window) SDL_DestroyWindow(impl->window);
    impl->window = nullptr;
    SDL_Quit();
    impl->running = false;
    running = false;
    mouseEnabled = false;
}

bool Platform::processEvents() {
    inputState.mouseDeltaX = 0;
    inputState.mouseDeltaY = 0;
    inputState.mouseWheel = 0;
    inputState.textInput.clear();
    inputState.keyPressQueue.clear();
    inputState.joystickButtonPressQueue.clear();
    inputState.focusLost = false;

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
                if (e.key.scancode >= 0 && e.key.scancode < 512) {
                    const bool wasDown = inputState.keysDown[e.key.scancode];
                    inputState.keysDown[e.key.scancode] = true;
                    if (!wasDown) inputState.keyPressQueue.push_back(e.key.scancode);
                }
                break;
            case SDL_EVENT_KEY_UP:
                if (e.key.scancode >= 0 && e.key.scancode < 512) {
                    inputState.keysDown[e.key.scancode] = false;
                    inputState.consumedSc[e.key.scancode] = false;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (!mouseEnabled) break;
                inputState.mouseX = e.button.x;
                inputState.mouseY = e.button.y;
                if (e.button.button > 0 && e.button.button < 8) inputState.mouseButtons[e.button.button] = true;
                break;
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (!mouseEnabled) break;
                inputState.mouseX = e.button.x;
                inputState.mouseY = e.button.y;
                if (e.button.button > 0 && e.button.button < 8) {
                    inputState.mouseButtons[e.button.button] = false;
                    inputState.consumedMouse[e.button.button] = false;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (!mouseEnabled) break;
                inputState.mouseDeltaX += e.motion.xrel;
                inputState.mouseDeltaY += e.motion.yrel;
                inputState.mouseX = e.motion.x;
                inputState.mouseY = e.motion.y;
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (!mouseEnabled) break;
                inputState.mouseX = e.wheel.mouse_x;
                inputState.mouseY = e.wheel.mouse_y;
                // SDL3 reports natural-scrolling devices as flipped. Convert
                // that back to the physical wheel direction expected by the
                // Torque input map, so weapon cycling stays consistent.
                inputState.mouseWheel += mouseWheelDelta(
                    e.wheel.y, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
                if (!impl->joystick) openFirstJoystick(impl->joystick, impl->joystickId, inputState);
                break;
            case SDL_EVENT_JOYSTICK_REMOVED:
                if (impl->joystick && e.jdevice.which == impl->joystickId) {
                    closeJoystick(impl->joystick, impl->joystickId, inputState);
                    openFirstJoystick(impl->joystick, impl->joystickId, inputState);
                }
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                if (impl->joystick && e.jaxis.which == impl->joystickId &&
                    e.jaxis.axis < JoystickInput::MaxAxes)
                    inputState.joystickAxes[e.jaxis.axis] = JoystickInput::normalizeAxis(e.jaxis.value);
                break;
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                if (impl->joystick && e.jbutton.which == impl->joystickId &&
                    e.jbutton.button < JoystickInput::MaxButtons) {
                    const bool wasDown = inputState.joystickButtons[e.jbutton.button];
                    inputState.joystickButtons[e.jbutton.button] = e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN;
                    if (e.type == SDL_EVENT_JOYSTICK_BUTTON_DOWN && !wasDown)
                        inputState.joystickButtonPressQueue.push_back(e.jbutton.button);
                    if (e.type == SDL_EVENT_JOYSTICK_BUTTON_UP)
                        inputState.consumedJoystickButtons[e.jbutton.button] = false;
                }
                break;
            case SDL_EVENT_JOYSTICK_HAT_MOTION:
                if (impl->joystick && e.jhat.which == impl->joystickId && e.jhat.hat == 0) {
                    inputState.joystickHat = e.jhat.value;
                    if (e.jhat.value == SDL_HAT_CENTERED) inputState.consumedJoystickHat = false;
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                // SDL does not guarantee key-up events while the window is
                // unfocused. Never carry movement, fire, or modifier state
                // into the next focus period.
                for (bool& down : inputState.keysDown) down = false;
                for (bool& consumed : inputState.consumedSc) consumed = false;
                for (bool& down : inputState.mouseButtons) down = false;
                for (bool& consumed : inputState.consumedMouse) consumed = false;
                for (bool& down : inputState.joystickButtons) down = false;
                for (bool& consumed : inputState.consumedJoystickButtons) consumed = false;
                std::fill(std::begin(inputState.joystickAxes), std::end(inputState.joystickAxes), 0.0f);
                inputState.consumedJoystickHat = false;
                inputState.joystickHat = 0;
                inputState.focusLost = true;
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                if (impl->resizeCb) {
                    int w = 0, h = 0;
                    SDL_GetWindowSizeInPixels(impl->window, &w, &h);
                    impl->resizeCb(w, h);
                }
                break;
        }
    }
    if (impl->joystick && SDL_JoystickConnected(impl->joystick)) {
        for (int axis = 0; axis < std::min(SDL_GetNumJoystickAxes(impl->joystick), JoystickInput::MaxAxes); ++axis)
            inputState.joystickAxes[axis] = JoystickInput::normalizeAxis(SDL_GetJoystickAxis(impl->joystick, axis));
        for (int button = 0; button < std::min(SDL_GetNumJoystickButtons(impl->joystick), JoystickInput::MaxButtons); ++button) {
            inputState.joystickButtons[button] = SDL_GetJoystickButton(impl->joystick, button);
            if (!inputState.joystickButtons[button]) inputState.consumedJoystickButtons[button] = false;
        }
        if (SDL_GetNumJoystickHats(impl->joystick) > 0) {
            inputState.joystickHat = SDL_GetJoystickHat(impl->joystick, 0);
            if (inputState.joystickHat == SDL_HAT_CENTERED) inputState.consumedJoystickHat = false;
        }
    }
    impl->frameCount++;
    return true;
}

void Platform::swapBuffers() { SDL_GL_SwapWindow(impl->window); }
bool Platform::isRunning() const { return running; }
bool Platform::hasJoystick() const {
    return impl && impl->joystick && SDL_JoystickConnected(impl->joystick);
}

int32_t Platform::width() const {
    int w; SDL_GetWindowSize(impl->window, &w, nullptr); return w;
}

int32_t Platform::height() const {
    int h; SDL_GetWindowSize(impl->window, nullptr, &h); return h;
}

int32_t Platform::drawableWidth() const {
    int w = 1;
    if (impl->window) SDL_GetWindowSizeInPixels(impl->window, &w, nullptr);
    return std::max(1, w);
}

int32_t Platform::drawableHeight() const {
    int h = 1;
    if (impl->window) SDL_GetWindowSizeInPixels(impl->window, nullptr, &h);
    return std::max(1, h);
}

float Platform::aspect() const { return (float)width() / (float)height(); }
double Platform::time() const { return SDL_GetTicks() / 1000.0; }
uint64_t Platform::frameCount() const { return impl->frameCount; }

void Platform::setTitle(const char* title) { SDL_SetWindowTitle(impl->window, title); }
bool Platform::setVideoMode(int32_t width, int32_t height, bool fullscreen, bool vsync) {
    if (!impl->window || width <= 0 || height <= 0) return false;
    if (!SDL_SetWindowSize(impl->window, width, height)) return false;
    if (!SDL_SetWindowFullscreen(impl->window, fullscreen)) return false;
    if (SDL_GL_SetSwapInterval(vsync ? 1 : 0) != 0) return false;
    if (impl->resizeCb)
        impl->resizeCb(drawableWidth(), drawableHeight());
    return true;
}
void Platform::showMouse(bool show) {
    mouseWanted = show;
    if (show && !softwareCursor) SDL_ShowCursor();
    else SDL_HideCursor();
}
void Platform::setSoftwareCursor(bool on) {
    if (on == softwareCursor) return;
    softwareCursor = on;
    showMouse(mouseWanted);
}
bool Platform::enableMouse() {
    if (!impl->window) return false;
    mouseEnabled = true;
    return true;
}
bool Platform::disableMouse() {
    if (!impl->window) return false;
    mouseEnabled = false;
    for (bool& down : inputState.mouseButtons) down = false;
    for (bool& consumed : inputState.consumedMouse) consumed = false;
    inputState.mouseDeltaX = 0;
    inputState.mouseDeltaY = 0;
    inputState.mouseWheel = 0;
    return true;
}
void Platform::setMousePos(int32_t x, int32_t y) {
    SDL_WarpMouseInWindow(impl->window, x, y);
    // The pointer is there now, before the warp's motion event arrives.
    inputState.mouseX = x;
    inputState.mouseY = y;
}
void Platform::setRelativeMouse(bool relative) { SDL_SetWindowRelativeMouseMode(impl->window, relative); }
bool Platform::isRelativeMouse() const { return impl->window && SDL_GetWindowRelativeMouseMode(impl->window); }

void* Platform::nativeWindow() { return impl->window; }
void* Platform::nativeGLContext() { return impl->glContext; }

void Platform::setResizeCallback(ResizeCallback cb) { impl->resizeCb = std::move(cb); }

void Platform::startTextInput() { SDL_StartTextInput(impl->window); }
void Platform::stopTextInput() { SDL_StopTextInput(impl->window); }
