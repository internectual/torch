#include "core/console.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cstring>

struct Console::Impl {
    std::unordered_map<std::string, ConsoleItem> items;
    std::vector<std::string> log;
    FILE* logFile = nullptr;
};

Console::Console() : impl(new Impl) {}
Console::~Console() { delete impl; }

Console& Console::instance() {
    static Console c;
    return c;
}

void Console::printf(LogLevel level, const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    const char* prefix = "";
    switch (level) {
        case LogLevel::Debug: prefix = "[DEBUG] "; break;
        case LogLevel::Info:  prefix = "[INFO] "; break;
        case LogLevel::Warn:  prefix = "[WARN] "; break;
        case LogLevel::Error: prefix = "[ERROR] "; break;
    }

    std::string msg = prefix + std::string(buf);
    impl->log.push_back(msg);
    fputs(msg.c_str(), stdout);
    fputc('\n', stdout);
    fflush(stdout);

    if (impl->logFile) {
        fprintf(impl->logFile, "%s\n", msg.c_str());
        fflush(impl->logFile);
    }
}

void Console::println(const char* str) {
    printf(LogLevel::Info, "%s", str);
}

void Console::setVariable(const char* name, const char* value) {
    impl->items[name] = { ConsoleItem::Variable, name, value, nullptr, "" };
}

void Console::setVariable(const char* name, int32_t value) {
    setVariable(name, std::to_string(value).c_str());
}

void Console::setVariable(const char* name, float value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", value);
    setVariable(name, buf);
}

void Console::addCommand(const char* name, ConsoleCommand cmd, const char* usage) {
    impl->items[name] = { ConsoleItem::Command, name, "", std::move(cmd), usage };
}

const char* Console::getStringVariable(const char* name, const char* def) const {
    auto it = impl->items.find(name);
    if (it != impl->items.end() && it->second.type == ConsoleItem::Variable)
        return it->second.value.c_str();
    return def;
}

int32_t Console::getIntVariable(const char* name, int32_t def) const {
    auto v = getStringVariable(name, nullptr);
    if (v) return atoi(v);
    return def;
}

float Console::getFloatVariable(const char* name, float def) const {
    auto v = getStringVariable(name, nullptr);
    if (v) return atof(v);
    return def;
}

bool Console::getBoolVariable(const char* name, bool def) const {
    auto v = getStringVariable(name, nullptr);
    if (v) return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
    return def;
}

Console::ConsoleItem* Console::find(const char* name) {
    auto it = impl->items.find(name);
    if (it != impl->items.end()) return &it->second;
    return nullptr;
}

void Console::list(const char* pattern) {
    for (auto& [name, item] : impl->items) {
        if (!pattern || name.find(pattern) != std::string::npos) {
            if (item.type == ConsoleItem::Variable)
                printf(LogLevel::Info, "  %s = %s", name.c_str(), item.value.c_str());
            else
                printf(LogLevel::Info, "  %s() - %s", name.c_str(), item.usage.c_str());
        }
    }
}

void Console::setLogFile(const char* path) {
    if (impl->logFile) fclose(impl->logFile);
    impl->logFile = fopen(path, "w");
}

void Console::execute(const char* script) {
    printf(LogLevel::Debug, "Exec: %s", script);
    // Parse single command: "command(args)" or "var = value"
    const char* s = script;
    while (*s == ' ' || *s == '\t') s++;

    std::string cmd;
    while (*s && *s != '(' && *s != '=' && *s != ' ' && *s != '\t')
        cmd += *s++;

    if (*s == '=') {
        // Variable assignment
        s++;
        while (*s == ' ') s++;
        setVariable(cmd.c_str(), s);
        return;
    }

    auto it = impl->items.find(cmd);
    if (it != impl->items.end() && it->second.type == ConsoleItem::Command) {
        std::vector<const char*> argv;
        std::vector<std::string> args;
        // Parse parenthesized args
        if (*s == '(') {
            s++;
            std::string cur;
            int depth = 1;
            while (*s && depth > 0) {
                if (*s == ',' && depth == 1) {
                    args.push_back(cur);
                    cur.clear();
                } else if (*s == '(') depth++;
                else if (*s == ')') depth--;
                if (depth > 0) cur += *s;
                s++;
            }
            if (!cur.empty()) args.push_back(cur);
        }
        argv.push_back(cmd.c_str());
        for (auto& a : args) argv.push_back(a.c_str());
        it->second.cmd((int32_t)argv.size(), argv.data());
    } else if (it != impl->items.end() && it->second.type == ConsoleItem::Variable) {
        printf(LogLevel::Info, "%s", it->second.value.c_str());
    } else {
        printf(LogLevel::Warn, "Unknown command: %s", cmd.c_str());
    }
}

void Console::executeFile(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        printf(LogLevel::Warn, "Cannot open script: %s", path);
        return;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (line[0] && line[0] != '/' && line[0] != '#')
            execute(line);
    }
    fclose(f);
}

void Console::forEach(ItemCallback cb) const {
    for (auto& [name, item] : impl->items) {
        cb(name.c_str(), item);
    }
}

void Console::processEvents() {
    // Input processing for interactive console
}

const std::vector<std::string>& Console::getLog() const {
    return impl->log;
}
