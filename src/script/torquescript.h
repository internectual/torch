#pragma once
#include "script/script_engine.h"
#include "script/script_scheduler.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <functional>
#include <cstdint>

struct TSPosition {
    const char* ptr{};
    int line = 1;
    int col = 1;
};

enum class TSTokenType {
    Eof, Ident, Number, String, Semicolon, Comma, Colon, Dot,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Plus, Minus, Star, Slash, Percent, At, Dollar, Tilde,
    Eq, EqEq, Neq, Lt, Gt, Le, Ge, StrEq, StrNeq,
    And, Or, Not, BitwiseAnd, BitwiseOr, BitwiseXor,
    Question, Shl, Shr,
    PlusPlus, MinusMinus,
    PlusEq, MinusEq, StarEq, SlashEq, BitOrEq,
    Hash, New, If, Else, For, While, Do, Switch, Case, Default,
    Return, Break, Continue, Function, Package, Datablock, Parent, This,
    True, False, Null, SwitchStr
};

struct TSToken {
    TSTokenType type;
    std::string text;
    double numVal{};
    TSPosition pos;
};

class TSLocals {
public:
    void push();
    void pop();
    void set(const std::string& name, const VMValue& val);
    VMValue get(const std::string& name);
private:
    std::vector<std::unordered_map<std::string, VMValue>> scopes;
};

struct DSOFunction;
class TSFunc {
public:
    std::vector<std::string> params;
    std::string body;
    std::string filename;
    bool isDSO = false;
    DSOFunction* dsoFunc = nullptr;
};

class TorqueScript {
public:
    TorqueScript();
    ~TorqueScript();

    void init();
    void shutdown();

    VMValue execute(const std::string& source, const std::string& filename = "");
    VMValue executeFile(const std::string& path);
    bool hasFunction(const std::string& name) const;
    const std::string& dbgFile() const;
    int dbgLine() const;
    VMValue callFunction(const std::string& name, const std::vector<VMValue>& args);
    int scheduleEvent(double now, double delay, const std::string& object,
                      const std::string& command, const std::vector<VMValue>& args);
    bool cancelEvent(int id);
    size_t cancelEventsForObject(const std::string& object);
    bool isEventPending(int id) const;
    size_t processScheduledEvents(double now);
    void clearScheduledEvents();
    void registerMessageCallback(const std::string& messageType,
                                 const std::string& functionName);
    void dispatchMessageCallback(const std::string& messageType,
                                 const std::vector<VMValue>& args);
    bool dispatchClientCommand(const std::vector<std::string>& args);
    bool dispatchServerCommand(const std::vector<std::string>& args);
    bool dispatchMissionCallback(const std::string& name,
                                 const std::vector<VMValue>& args = {});

    void setGlobal(const std::string& name, const VMValue& val);
    VMValue getGlobal(const std::string& name);

    void registerNative(const std::string& name,
        std::function<VMValue(const std::vector<VMValue>&)> fn);

    VMValue callFunctionImpl(const std::string& name, const std::vector<VMValue>& args);

    // Nested exec: save outer state, execute source, restore outer state, return result
    VMValue executeNested(const std::string& source, const std::string& path);

    using NativeFunc = std::function<VMValue(const std::vector<VMValue>&)>;
    const std::unordered_map<std::string, NativeFunc>& getNatives() const;

private:
    bool dispatchPrefixedFunction(const std::string& prefix,
                                  const std::vector<std::string>& words);
    struct Impl;
    Impl* impl;
};
