#pragma once
#include <cstdint>
#include <functional>
#include <string>

// SDL3 scancode constants (USB HID usage table)
constexpr int SCANCODE_GRAVE   = 53;  // ~ key
constexpr int SCANCODE_M       = 16;  // M key (SDL3: 16, was 50 in SDL2)
constexpr int SCANCODE_PAUSE   = 72;  // Pause/Break (SDL3: 72, was 110 in SDL2)
constexpr int SCANCODE_RETURN  = 40;
constexpr int SCANCODE_ESCAPE  = 41;
constexpr int SCANCODE_BACKSPACE = 42;
constexpr int SCANCODE_SPACE   = 44;
constexpr int SCANCODE_TAB     = 43;
constexpr int SCANCODE_UP      = 82;
constexpr int SCANCODE_DOWN    = 81;
constexpr int SCANCODE_F1      = 58;
constexpr int SCANCODE_R       = 21;
constexpr int SCANCODE_W       = 26;
constexpr int SCANCODE_A       = 4;
constexpr int SCANCODE_S       = 22;
constexpr int SCANCODE_D       = 7;
constexpr int SCANCODE_P       = 19;
constexpr int SCANCODE_E       = 8;
constexpr int SCANCODE_Q       = 20;
constexpr int SCANCODE_F2      = 59;
constexpr int SCANCODE_LBRACKET = 47;
constexpr int SCANCODE_RBRACKET = 48;
constexpr int SCANCODE_PERIOD  = 54;
constexpr int SCANCODE_PAGEUP  = 75;
constexpr int SCANCODE_PAGEDOWN = 78;
constexpr int SCANCODE_HOME    = 74;
constexpr int SCANCODE_END     = 77;
constexpr int SCANCODE_LEFT    = 80;
constexpr int SCANCODE_RIGHT   = 79;
constexpr int SCANCODE_DELETE  = 76;

struct PlatformConfig {
    std::string title = "Tribes 2";
    int32_t width = 1024;
    int32_t height = 768;
    bool fullscreen = false;
    bool vsync = true;
    int32_t msaaSamples = 0;
};

struct InputState {
    bool keysDown[512]{};
    bool mouseButtons[8]{};
    int32_t mouseX{}, mouseY{}, mouseDeltaX{}, mouseDeltaY{};
    int32_t mouseWheel{};
    std::string textInput; // consumed text this frame (SDL_TEXT_INPUT)
};

class Platform {
public:
    Platform();
    ~Platform();

    bool init(const PlatformConfig& config);
    void shutdown();
    bool processEvents();
    void swapBuffers();
    bool isRunning() const;

    InputState& input() { return inputState; }

    int32_t width() const;
    int32_t height() const;
    float aspect() const;
    double time() const;
    uint64_t frameCount() const;

    void setTitle(const char* title);
    void showMouse(bool show);
    void setMousePos(int32_t x, int32_t y);
    void setRelativeMouse(bool relative);

    void startTextInput();
    void stopTextInput();

    void* nativeWindow();
    void* nativeGLContext();

    using ResizeCallback = std::function<void(int32_t w, int32_t h)>;
    void setResizeCallback(ResizeCallback cb);

private:
    struct Impl;
    Impl* impl;
    InputState inputState;
    bool running = false;
};
