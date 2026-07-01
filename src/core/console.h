#pragma once
#include <cstdint>
#include <string>
#include <functional>

class Console;
class Platform;
class Engine;

enum class LogLevel { Debug, Info, Warn, Error };

class Console {
public:
    Console();
    ~Console();

    static Console& instance();

    void printf(LogLevel level, const char* fmt, ...);
    void println(const char* str);

    void execute(const char* script);
    void executeFile(const char* path);

    bool getBoolVariable(const char* name, bool def = false) const;
    int32_t getIntVariable(const char* name, int32_t def = 0) const;
    float getFloatVariable(const char* name, float def = 0.0f) const;
    const char* getStringVariable(const char* name, const char* def = "") const;

    void setVariable(const char* name, const char* value);
    void setVariable(const char* name, int32_t value);
    void setVariable(const char* name, float value);

    using ConsoleCommand = std::function<void(int32_t argc, const char* const* argv)>;
    void addCommand(const char* name, ConsoleCommand cmd, const char* usage = "");

    struct ConsoleItem {
        enum Type { Variable, Command } type;
        std::string name;
        std::string value;
        ConsoleCommand cmd;
        std::string usage;

        ConsoleItem() = default;
        ConsoleItem(Type t, const char* n, const char* v, ConsoleCommand c, const char* u)
            : type(t), name(n ? n : ""), value(v ? v : ""), cmd(std::move(c)), usage(u ? u : "") {}
    };

    ConsoleItem* find(const char* name);
    void list(const char* pattern = nullptr);
    using ItemCallback = std::function<void(const char* name, const ConsoleItem&)>;
    void forEach(ItemCallback cb) const;

    void setLogFile(const char* path);
    void processEvents();

    const std::vector<std::string>& getLog() const;

private:
    struct Impl;
    Impl* impl;
};
