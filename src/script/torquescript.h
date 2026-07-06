#pragma once
#include "script/script_engine.h"
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
    PlusEq, MinusEq, StarEq, SlashEq,
    Hash, New, If, Else, For, While, Do, Switch, Case, Default,
    Return, Break, Continue, Function, Package, Parent, This,
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
    VMValue callFunction(const std::string& name, const std::vector<VMValue>& args);

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
    struct Impl;
    Impl* impl;
};
