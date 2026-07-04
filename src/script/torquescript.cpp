#include "script/torquescript.h"
#include "core/console.h"
#include "core/engine.h"
#include <sys/stat.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <sstream>
#include <fstream>
#include <set>
#include <cctype>

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static bool isCompilableExt(const std::string& p) {
    auto ext = p.size() > 3 ? p.substr(p.size() - 3) : std::string();
    return ext == ".cs" || ext == ".gui" || ext == ".mis";
}

struct TorqueScript::Impl {
    TorqueScript* outer;
    std::unordered_map<std::string, std::function<VMValue(const std::vector<VMValue>&)>> natives;
    std::unordered_map<std::string, TSFunc> functions;
    std::unordered_map<std::string, VMValue> globals;
    TSLocals locals;
    bool initializing = false;

    // Exec state
    const char* srcPtr{};
    const char* srcEnd{};
    int srcLine = 1;
    int srcCol = 1;
    std::vector<TSToken> tokens;
    size_t tokenPos = 0;
    std::string currentFile;
    bool running = true;
    bool returning = false;
    bool breaking = false;
    bool continuing = false;
    VMValue returnValue;
    int loopDepth = 0;

    std::set<std::string> loadingFiles; // prevent circular includes
    std::string lastVarName;
    std::string lastFieldObj;  // for %obj.field = value assignment write-back
    std::string lastFieldName;
    int execDepth = 0;

    // GUI parent-child tracking (persists across expressions within a file)
    std::vector<ScriptObject*> guiParentStack;

    // Compile .cs to .dso using the turd compiler (Node.js)
    bool compileToDSO(const std::string& srcContent, const std::string& dsoFullPath) {
        const char* compilerScript = "/home/methodown/torch/torque-dso.js";
        struct stat st;
        if (stat(compilerScript, &st) != 0) {
            Console::instance().printf(LogLevel::Debug, "TS: turd compiler not found at %s", compilerScript);
            return false;
        }
        // Create parent directory (and intermediate dirs)
        auto mkdirP = [](const std::string& p) {
            size_t pos = 0;
            while ((pos = p.find('/', pos + 1)) != std::string::npos) {
                std::string sub = p.substr(0, pos);
                struct stat st; if (stat(sub.c_str(), &st) != 0) mkdir(sub.c_str(), 0755);
            }
            struct stat st; if (stat(p.c_str(), &st) != 0) mkdir(p.c_str(), 0755);
        };
        auto sl = dsoFullPath.rfind('/');
        if (sl != std::string::npos) mkdirP(dsoFullPath.substr(0, sl));
        // Write source to temp file, compile, clean up
        std::string tmpPath = dsoFullPath + ".src.tmp";
        {
            FILE* f = fopen(tmpPath.c_str(), "w");
            if (!f) return false;
            fwrite(srcContent.data(), 1, srcContent.size(), f);
            fclose(f);
        }
        std::string cmd = "/home/linuxbrew/.linuxbrew/bin/node " + std::string(compilerScript) + " " + tmpPath + " " + dsoFullPath + " 2>/dev/null";
        int ret = system(cmd.c_str());
        if (ret != 0) {
            unlink(tmpPath.c_str());
            return false;
        }
        unlink(tmpPath.c_str());
        Console::instance().printf(LogLevel::Debug, "TS: compiled DSO: %s", dsoFullPath.c_str());
        return true;
    }

    // Write a minimal DSO cache file so subsequent loads find DSO first
    void writeDSOCache(const std::string& dsoPath, const std::string& srcPath, const std::string& source) {
        // We need to capture function declarations from the parsed source.
        // But since we already parsed it, we have all function info in `functions`.
        // For now, write a minimal DSO that registers function stubs.
        // The function bodies won't be executable bytecode, but the DSO loader will
        // register them. The actual callFunction falls back to source for these stubs.
        //
        // In the future, an external compiler (like turd) should produce real DSO bytecode.
        // For now, we write a custom "source DSO" marker that the loader recognizes.
        FILE* f = fopen(dsoPath.c_str(), "wb");
        if (!f) return;
        // Write header: version 0x54534F01 ("TSO\1" — Torque Source Object)
        uint32_t version = 0x54534F01;
        fwrite(&version, 4, 1, f);
        // Store function count
        uint32_t funcCount = 0;
        for (auto& [name, fn] : functions) {
            if (!fn.isDSO && fn.filename == srcPath) funcCount++; // count only functions from this file
        }
        fwrite(&funcCount, 4, 1, f);
        // Store each function name and param count
        for (auto& [name, fn] : functions) {
            if (fn.isDSO || fn.filename != srcPath) continue;
            uint32_t nameLen = (uint32_t)name.size();
            fwrite(&nameLen, 4, 1, f);
            fwrite(name.c_str(), 1, nameLen, f);
            uint32_t paramCount = (uint32_t)fn.params.size();
            fwrite(&paramCount, 4, 1, f);
            for (auto& p : fn.params) {
                uint32_t plen = (uint32_t)p.size();
                fwrite(&plen, 4, 1, f);
                fwrite(p.c_str(), 1, plen, f);
            }
            uint32_t bodyLen = (uint32_t)fn.body.size();
            fwrite(&bodyLen, 4, 1, f);
            fwrite(fn.body.c_str(), 1, bodyLen, f);
        }
        fclose(f);
    }

    void writeBackVar(VMValue val) {
        if (!lastFieldObj.empty() && !lastFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
            if (obj) obj->fields[lastFieldName] = val;
        } else if (!lastVarName.empty()) {
            if (lastVarName[0] == '$') {
                outer->setGlobal(lastVarName, val);
            } else {
                locals.set(lastVarName, val);
            }
        }
    }

    void tokenize(const std::string& source);
    TSToken nextToken();
    TSToken peekToken(size_t ahead = 0);
    void expect(TSTokenType type);
    bool match(TSTokenType type);

    VMValue parseProgram();
    VMValue parseStatement();
    VMValue parseBlock();
    VMValue parseIf();
    VMValue parseFor();
    VMValue parseWhile();
    VMValue parseDo();
    VMValue parseSwitch();
    VMValue parseReturn();
    VMValue parseBreak();
    VMValue parseContinue();
    VMValue parseFunctionDecl();
    VMValue parsePackage();
    VMValue parseExpressionStatement();

    VMValue parseExpression();
    VMValue parseAssignment();
    VMValue parseTernary();
    VMValue parseLogicalOr();
    VMValue parseLogicalAnd();
    VMValue parseBitwiseOr();
    VMValue parseBitwiseXor();
    VMValue parseBitwiseAnd();
    VMValue parseEquality();
    VMValue parseRelational();
    VMValue parseShift();
    VMValue parseAdditive();
    VMValue parseMultiplicative();
    VMValue parseUnary();
    VMValue parsePostfix();
    VMValue parsePrimary();

    void parseArgumentList(std::vector<VMValue>& args);
    std::string parseStringLiteral();
    void skipStatement();

    void error(const std::string& msg);
};

TorqueScript::TorqueScript() : impl(new Impl) { impl->outer = this; }
TorqueScript::~TorqueScript() { delete impl; }

void TorqueScript::init() {}
void TorqueScript::shutdown() {}

void TorqueScript::setGlobal(const std::string& name, const VMValue& val) {
    impl->globals[name] = val;
    Console::instance().setVariable(name.c_str(), val.toString().c_str());
}

VMValue TorqueScript::getGlobal(const std::string& name) {
    auto it = impl->globals.find(name);
    if (it != impl->globals.end()) return it->second;
    auto* item = Console::instance().find(name.c_str());
    if (item && item->type == Console::ConsoleItem::Variable)
        return VMValue(item->value.c_str());
    return VMValue("");  // undefined variables are empty string in TorqueScript
}

void TorqueScript::registerNative(const std::string& name,
    std::function<VMValue(const std::vector<VMValue>&)> fn) {
    std::string lower = name;
    for (auto& c : lower) c = (char)tolower((unsigned char)c);
    impl->natives[lower] = std::move(fn);
}

// === TSLocals ===
void TSLocals::push() { scopes.push_back({}); }
void TSLocals::pop() { if (!scopes.empty()) scopes.pop_back(); }
void TSLocals::set(const std::string& name, const VMValue& val) {
    if (!scopes.empty()) scopes.back()[name] = val;
}
VMValue TSLocals::get(const std::string& name) {
    for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
        auto f = it->find(name);
        if (f != it->end()) return f->second;
    }
    return VMValue(0);
}

// === Tokenizer ===
void TorqueScript::Impl::tokenize(const std::string& source) {
    tokens.clear();
    tokenPos = 0;
    srcPtr = source.c_str();
    srcEnd = srcPtr + source.size();
    srcLine = 1;
    srcCol = 1;

    while (srcPtr < srcEnd) {
        TSToken tok;
        tok.pos.ptr = srcPtr;
        tok.pos.line = srcLine;
        tok.pos.col = srcCol;

        char c = *srcPtr;

        // Skip whitespace
        if (c == ' ' || c == '\t' || c == '\r') {
            srcPtr++; srcCol++; continue;
        }
        if (c == '\n') {
            srcPtr++; srcLine++; srcCol = 1; continue;
        }

        // Comments
        if (c == '/' && srcPtr + 1 < srcEnd) {
            if (*(srcPtr + 1) == '/') {
                while (srcPtr < srcEnd && *srcPtr != '\n') srcPtr++;
                continue;
            }
            if (*(srcPtr + 1) == '*') {
                srcPtr += 2;
                while (srcPtr < srcEnd) {
                    if (*srcPtr == '*' && srcPtr + 1 < srcEnd && *(srcPtr + 1) == '/') {
                        srcPtr += 2; break;
                    }
                    if (*srcPtr == '\n') { srcLine++; srcCol = 1; }
                    srcPtr++;
                }
                continue;
            }
        }

        // Numbers
        if (c >= '0' && c <= '9') {
            tok.type = TSTokenType::Number;
            const char* start = srcPtr;
            while (srcPtr < srcEnd && ((*srcPtr >= '0' && *srcPtr <= '9') || *srcPtr == '.')) srcPtr++;
            tok.text = std::string(start, srcPtr - start);
            tok.numVal = atof(tok.text.c_str());
            tok.pos.col = srcCol;
            srcCol += (int)(srcPtr - start);
            tokens.push_back(tok);
            continue;
        }

        // Strings
        if (c == '"' || c == '\'') {
            char quote = c;
            tok.type = TSTokenType::String;
            tok.pos.col = srcCol;
            srcPtr++; srcCol++;
            std::string val;
            while (srcPtr < srcEnd) {
                if (*srcPtr == quote) { srcPtr++; srcCol++; break; }
                if (*srcPtr == '\\' && srcPtr + 1 < srcEnd) {
                    srcPtr++; srcCol++;
                    switch (*srcPtr) {
                        case 'n': val += '\n'; break;
                        case 't': val += '\t'; break;
                        case 'r': val += '\r'; break;
                        case '"': val += '"'; break;
                        case '\'': val += '\''; break;
                        case '\\': val += '\\'; break;
                        default: val += *srcPtr; break;
                    }
                } else {
                    val += *srcPtr;
                }
                srcPtr++; srcCol++;
            }
            tok.text = val;
            tokens.push_back(tok);
            continue;
        }

        // Identifiers and keywords
        if (isalpha(c) || c == '_') {
            const char* start = srcPtr;
            while (srcPtr < srcEnd && (isalnum(*srcPtr) || *srcPtr == '_')) srcPtr++;
            tok.text = std::string(start, srcPtr - start);
            tok.pos.col = srcCol;
            srcCol += (int)tok.text.size();

            if (tok.text == "if") tok.type = TSTokenType::If;
            else if (tok.text == "else") tok.type = TSTokenType::Else;
            else if (tok.text == "for") tok.type = TSTokenType::For;
            else if (tok.text == "while") tok.type = TSTokenType::While;
            else if (tok.text == "do") tok.type = TSTokenType::Do;
            else if (tok.text == "switch") {
                tok.type = TSTokenType::Switch;
                // Check for switch$ (string switch)
                if (srcPtr < srcEnd && *srcPtr == '$') {
                    tok.type = TSTokenType::SwitchStr;
                    tok.text += '$';
                    srcPtr++; srcCol++;
                }
            }
            else if (tok.text == "case") tok.type = TSTokenType::Case;
            else if (tok.text == "default") tok.type = TSTokenType::Default;
            else if (tok.text == "return") tok.type = TSTokenType::Return;
            else if (tok.text == "break") tok.type = TSTokenType::Break;
            else if (tok.text == "continue") tok.type = TSTokenType::Continue;
            else if (tok.text == "function") tok.type = TSTokenType::Function;
            else if (tok.text == "package") tok.type = TSTokenType::Package;
            else if (tok.text == "new") tok.type = TSTokenType::New;
            else if (tok.text == "parent") tok.type = TSTokenType::Parent;
            else if (tok.text == "this") tok.type = TSTokenType::This;
            else if (tok.text == "true") { tok.type = TSTokenType::True; tok.numVal = 1; }
            else if (tok.text == "false") { tok.type = TSTokenType::False; tok.numVal = 0; }
            else tok.type = TSTokenType::Ident;

            tokens.push_back(tok);
            continue;
        }

        // Multi-char operators
        auto match2 = [&](char second, TSTokenType type) {
            if (srcPtr + 1 < srcEnd && *(srcPtr + 1) == second) {
                tok.type = type;
                tok.text = std::string(srcPtr, 2);
                srcPtr += 2; srcCol += 2;
                tokens.push_back(tok);
                return true;
            }
            return false;
        };
        auto match3 = [&](char c2, char c3, TSTokenType type) {
            if (srcPtr + 2 < srcEnd && *(srcPtr + 1) == c2 && *(srcPtr + 2) == c3) {
                tok.type = type;
                tok.text = std::string(srcPtr, 3);
                srcPtr += 3; srcCol += 3;
                tokens.push_back(tok);
                return true;
            }
            return false;
        };

        tok.pos.col = srcCol;

        switch (c) {
            case '(': tok = {TSTokenType::LParen, "(", 0, tok.pos}; srcPtr++; srcCol++; break;
            case ')': tok = {TSTokenType::RParen, ")", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '{': tok = {TSTokenType::LBrace, "{", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '}': tok = {TSTokenType::RBrace, "}", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '[': tok = {TSTokenType::LBracket, "[", 0, tok.pos}; srcPtr++; srcCol++; break;
            case ']': tok = {TSTokenType::RBracket, "]", 0, tok.pos}; srcPtr++; srcCol++; break;
            case ';': tok = {TSTokenType::Semicolon, ";", 0, tok.pos}; srcPtr++; srcCol++; break;
            case ',': tok = {TSTokenType::Comma, ",", 0, tok.pos}; srcPtr++; srcCol++; break;
            case ':': tok = {TSTokenType::Colon, ":", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '.': tok = {TSTokenType::Dot, ".", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '~': tok = {TSTokenType::Tilde, "~", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '#': tok = {TSTokenType::Hash, "#", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '@': tok = {TSTokenType::At, "@", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '$':
                if (match2('=', TSTokenType::StrEq)) continue;
                if (match2('+', TSTokenType::At)) continue;
                tok = {TSTokenType::Dollar, "$", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '%':
                tok = {TSTokenType::Percent, "%", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '=':
                if (match2('=', TSTokenType::EqEq)) continue;
                tok = {TSTokenType::Eq, "=", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '!':
                if (match3('$', '=', TSTokenType::StrNeq)) continue;
                if (match2('=', TSTokenType::Neq)) continue;
                tok = {TSTokenType::Not, "!", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '<':
                if (match2('<', TSTokenType::Shl)) continue;
                if (match2('=', TSTokenType::Le)) continue;
                tok = {TSTokenType::Lt, "<", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '>':
                if (match2('>', TSTokenType::Shr)) continue;
                if (match2('=', TSTokenType::Ge)) continue;
                tok = {TSTokenType::Gt, ">", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '+':
                if (match2('+', TSTokenType::PlusPlus)) continue;
                if (match2('=', TSTokenType::PlusEq)) continue;
                tok = {TSTokenType::Plus, "+", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '-':
                if (match2('-', TSTokenType::MinusMinus)) continue;
                if (match2('=', TSTokenType::MinusEq)) continue;
                tok = {TSTokenType::Minus, "-", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '*':
                if (match2('=', TSTokenType::StarEq)) continue;
                tok = {TSTokenType::Star, "*", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '/':
                if (match2('=', TSTokenType::SlashEq)) continue;
                tok = {TSTokenType::Slash, "/", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '&':
                if (match2('&', TSTokenType::And)) continue;
                tok = {TSTokenType::BitwiseAnd, "&", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '|':
                if (match2('|', TSTokenType::Or)) continue;
                tok = {TSTokenType::BitwiseOr, "|", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '?':
                tok = {TSTokenType::Question, "?", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '^':
                tok = {TSTokenType::BitwiseXor, "^", 0, tok.pos}; srcPtr++; srcCol++; break;

            default:
                error(std::string("Unexpected character: ") + c);
                srcPtr++; srcCol++; continue;
        }
        tokens.push_back(tok);
    }

    tokens.push_back({TSTokenType::Eof, "", 0, {srcPtr, srcLine, srcCol}});
}

TSToken TorqueScript::Impl::nextToken() {
    if (tokenPos < tokens.size()) {
        TSToken t = tokens[tokenPos++];
        srcLine = t.pos.line;
        return t;
    }
    return {TSTokenType::Eof, "", 0, {}};
}

TSToken TorqueScript::Impl::peekToken(size_t ahead) {
    size_t idx = tokenPos + ahead;
    if (idx < tokens.size()) return tokens[idx];
    return {TSTokenType::Eof, "", 0, {}};
}

void TorqueScript::Impl::expect(TSTokenType type) {
    TSToken t = nextToken();
    if (t.type != type) {
        char buf[256];
        snprintf(buf, sizeof(buf), "Expected token type %d, got '%s'", (int)type, t.text.c_str());
        error(buf);
    }
}

bool TorqueScript::Impl::match(TSTokenType type) {
    if (peekToken().type == type) { nextToken(); return true; }
    return false;
}

void TorqueScript::Impl::error(const std::string& msg) {
    Console::instance().printf(LogLevel::Error, "TS:%s(%d): %s",
        currentFile.c_str(), srcLine, msg.c_str());
}

// === Parser ===
VMValue TorqueScript::Impl::parseProgram() {
    VMValue result;
    int stmtCount = 0;
    while (peekToken().type != TSTokenType::Eof && running) {
        result = parseStatement();
        stmtCount++;
        if (returning || breaking || continuing) break;
    }
    Console::instance().printf(LogLevel::Debug, "TS: parseProgram done (%d stmts, returning=%d, breaking=%d, continuing=%d, eof=%d)", 
        stmtCount, returning, breaking, continuing, peekToken().type == TSTokenType::Eof);
    return result;
}

VMValue TorqueScript::Impl::parseStatement() {
    if (!running) return {};

    TSToken tok = peekToken();
    switch (tok.type) {
        case TSTokenType::If: return parseIf();
        case TSTokenType::For: return parseFor();
        case TSTokenType::While: return parseWhile();
        case TSTokenType::Do: return parseDo();
        case TSTokenType::Switch:
        case TSTokenType::SwitchStr: return parseSwitch();
        case TSTokenType::Return: return parseReturn();
        case TSTokenType::Break: return parseBreak();
        case TSTokenType::Continue: return parseContinue();
        case TSTokenType::Function: return parseFunctionDecl();
        case TSTokenType::Package: return parsePackage();
        case TSTokenType::LBrace: return parseBlock();
        case TSTokenType::Semicolon: nextToken(); return {};
        case TSTokenType::Eof: return {};
        default: return parseExpressionStatement();
    }
}

VMValue TorqueScript::Impl::parseBlock() {
    expect(TSTokenType::LBrace);
    VMValue result;
    while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof && running) {
        result = parseStatement();
        if (returning) {
            int depth = 1;
            while (depth > 0 && peekToken().type != TSTokenType::Eof) {
                TSToken t = nextToken();
                if (t.type == TSTokenType::LBrace) depth++;
                if (t.type == TSTokenType::RBrace) depth--;
            }
            return result;
        }
    }
    expect(TSTokenType::RBrace);
    return result;
}

void TorqueScript::Impl::skipStatement() {
    if (peekToken().type == TSTokenType::LBrace) {
        int braceDepth = 1;
        nextToken(); // consume '{'
        while (braceDepth > 0 && peekToken().type != TSTokenType::Eof) {
            TSToken t = nextToken();
            if (t.type == TSTokenType::LBrace) braceDepth++;
            if (t.type == TSTokenType::RBrace) braceDepth--;
        }
    } else if (peekToken().type == TSTokenType::If) {
        // else if (...) ... - skip the whole if/else if/else chain
        nextToken(); // consume 'if'
        expect(TSTokenType::LParen);
        // Skip condition (balance parens)
        int parenDepth = 1;
        while (parenDepth > 0 && peekToken().type != TSTokenType::Eof) {
            TSToken t = nextToken();
            if (t.type == TSTokenType::LParen) parenDepth++;
            if (t.type == TSTokenType::RParen) parenDepth--;
        }
        // Skip body
        skipStatement();
        // Skip trailing else
        if (peekToken().type == TSTokenType::Else) {
            nextToken();
            skipStatement();
        }
    } else {
        while (peekToken().type != TSTokenType::Semicolon && peekToken().type != TSTokenType::Eof
               && peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Else) {
            nextToken();
        }
        if (peekToken().type == TSTokenType::Semicolon) nextToken();
    }
}

VMValue TorqueScript::Impl::parseIf() {
    expect(TSTokenType::If);
    expect(TSTokenType::LParen);
    VMValue cond = parseExpression();
    expect(TSTokenType::RParen);

    locals.push();
    VMValue result;
    if (cond.toBool()) {
        result = parseStatement();
        // Skip else branch (don't execute it)
        if (peekToken().type == TSTokenType::Else) {
            nextToken();
            skipStatement();
        }
    } else {
        skipStatement();
        if (peekToken().type == TSTokenType::Else) {
            nextToken();
            result = parseStatement();
        }
    }
    locals.pop();
    return result;
}

VMValue TorqueScript::Impl::parseFor() {
    expect(TSTokenType::For);
    expect(TSTokenType::LParen);

    locals.push();

    // Init
    if (peekToken().type != TSTokenType::Semicolon) {
        parseExpression();
    }
    expect(TSTokenType::Semicolon);

    // Condition - save token range
    size_t condStart = tokenPos;
    size_t condEnd = tokenPos;
    if (peekToken().type != TSTokenType::Semicolon) {
        parseExpression();
        condEnd = tokenPos;
    }
    expect(TSTokenType::Semicolon);

    // Advance - save token range (respect paren nesting)
    size_t advStart = tokenPos;
    size_t advEnd = tokenPos;
    int parenDepth = 0;
    while (peekToken().type != TSTokenType::Eof) {
        if (peekToken().type == TSTokenType::RParen && parenDepth == 0) break;
        if (peekToken().type == TSTokenType::LParen) parenDepth++;
        if (peekToken().type == TSTokenType::RParen) parenDepth--;
        nextToken();
    }
    advEnd = tokenPos;
    expect(TSTokenType::RParen);

    loopDepth++;
    VMValue result;
    VMValue cond(true);
    // Save body start for re-execution and track end for skipping
    size_t bodyStart = tokenPos;
    size_t bodyEnd = tokenPos;
    while (running) {
        // Evaluate condition
        if (condStart < condEnd) {
            size_t saved = tokenPos;
            tokenPos = condStart;
            cond = parseExpression();
            tokenPos = saved;
        }
        if (!cond.toBool()) break;

        // Execute body from saved position
        tokenPos = bodyStart;
        result = parseStatement();
        bodyEnd = tokenPos;  // record where the body ends
        if (returning) break;
        if (breaking) { breaking = false; break; }
        if (continuing) { continuing = false; }

        // Execute advance
        if (advStart < advEnd) {
            size_t saved = tokenPos;
            tokenPos = advStart;
            parseExpression();
            tokenPos = saved;
        }
    }

    // Advance past the body one final time
    if (bodyEnd > bodyStart) {
        tokenPos = bodyEnd;
    } else {
        // Body was never executed (condition false on first check)
        // Skip past it without executing
        tokenPos = bodyStart;
        skipStatement();
    }

    loopDepth--;
    locals.pop();
    return result;
}

VMValue TorqueScript::Impl::parseWhile() {
    expect(TSTokenType::While);
    expect(TSTokenType::LParen);

    // Save condition token range
    size_t condStart = tokenPos;
    parseExpression();
    expect(TSTokenType::RParen);

    loopDepth++;
    VMValue result;
    while (running) {
        // Evaluate condition
        {
            size_t saved = tokenPos;
            tokenPos = condStart;
            VMValue cond = parseExpression();
            tokenPos = saved;
            if (!cond.toBool()) break;
        }

        result = parseStatement();
        if (returning) break;
        if (breaking) { breaking = false; break; }
        if (continuing) { continuing = false; }
    }
    loopDepth--;
    return result;
}

VMValue TorqueScript::Impl::parseDo() {
    expect(TSTokenType::Do);
    loopDepth++;
    VMValue result;

    size_t condStart = tokenPos;
    size_t condEnd = tokenPos;
    bool hasWhile = false;

    do {
        result = parseStatement();
        if (returning) break;
        if (breaking) { breaking = false; break; }
        if (continuing) { continuing = false; }

        if (match(TSTokenType::While)) {
            hasWhile = true;
            expect(TSTokenType::LParen);
            condStart = tokenPos;
            parseExpression();
            condEnd = tokenPos;
            expect(TSTokenType::RParen);
        } else {
            break;
        }
    } while (hasWhile && (condStart < condEnd));

    // Re-evaluate condition for proper looping
    if (hasWhile) {
        while (true) {
            size_t saved = tokenPos;
            tokenPos = condStart;
            VMValue cond = parseExpression();
            tokenPos = saved;
            if (!cond.toBool()) break;

            result = parseStatement();
            if (returning) break;
            if (breaking) { breaking = false; break; }
            if (continuing) { continuing = false; }
        }
    }

    loopDepth--;
    return result;
}

VMValue TorqueScript::Impl::parseSwitch() {
    if (!match(TSTokenType::Switch) && !match(TSTokenType::SwitchStr))
        error("Expected 'switch' or 'switch$'");
    expect(TSTokenType::LParen);
    VMValue val = parseExpression();
    expect(TSTokenType::RParen);
    expect(TSTokenType::LBrace);

    bool matched = false;
    while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
        if (match(TSTokenType::Case)) {
            VMValue caseVal = parseExpression();
            expect(TSTokenType::Colon);
            if (val.toString() == caseVal.toString()) {
                matched = true;
                while (peekToken().type != TSTokenType::Case && peekToken().type != TSTokenType::Default &&
                       peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    VMValue r = parseStatement();
                    if (returning || breaking) { matched = false; break; }
                }
            } else {
                while (peekToken().type != TSTokenType::Case && peekToken().type != TSTokenType::Default &&
                       peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    skipStatement();
                }
            }
            breaking = false; // break inside switch exits the switch, not the enclosing loop
        } else if (match(TSTokenType::Default)) {
            expect(TSTokenType::Colon);
            if (!matched) {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    VMValue r = parseStatement();
                    if (returning || breaking) break;
                }
            } else {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    skipStatement();
                }
            }
            breaking = false;
        } else {
            break;
        }
    }
    breaking = false;
    expect(TSTokenType::RBrace);
    return {};
}

VMValue TorqueScript::Impl::parseReturn() {
    expect(TSTokenType::Return);
    returning = true;
    if (peekToken().type != TSTokenType::Semicolon && peekToken().type != TSTokenType::RBrace &&
        peekToken().type != TSTokenType::Eof) {
        returnValue = parseExpression();
    }
    match(TSTokenType::Semicolon);
    return returnValue;
}

VMValue TorqueScript::Impl::parseBreak() {
    expect(TSTokenType::Break);
    match(TSTokenType::Semicolon);
    breaking = true;
    return {};
}

VMValue TorqueScript::Impl::parseContinue() {
    expect(TSTokenType::Continue);
    match(TSTokenType::Semicolon);
    continuing = true;
    return {};
}

VMValue TorqueScript::Impl::parseFunctionDecl() {
    expect(TSTokenType::Function);
    TSToken nameTok = nextToken();
    if (nameTok.type != TSTokenType::Ident && nameTok.type != TSTokenType::Dollar) {
        error("Expected function name");
        return {};
    }

    // Build full name (namespace::name or name)
    std::string fullName = nameTok.text;
    if (peekToken().type == TSTokenType::Colon && peekToken(1).type == TSTokenType::Colon) {
        nextToken(); nextToken();
        TSToken methodTok = nextToken();
        fullName = nameTok.text + "::" + methodTok.text;
    }

    expect(TSTokenType::LParen);

    TSFunc func;
    while (peekToken().type != TSTokenType::RParen && peekToken().type != TSTokenType::Eof) {
        TSToken paramTok = nextToken();
        if (paramTok.type == TSTokenType::Dollar || paramTok.type == TSTokenType::Ident) {
            func.params.push_back(paramTok.text);
        }
        match(TSTokenType::Comma);
    }
    expect(TSTokenType::RParen);

    // Save function body as source text
    if (peekToken().type == TSTokenType::LBrace) {
        // Find matching close brace
        const char* bodyStart = peekToken().pos.ptr;
        int depth = 0;
        size_t savePos = tokenPos;
        do {
            TSToken t = nextToken();
            if (t.type == TSTokenType::LBrace) depth++;
            if (t.type == TSTokenType::RBrace) depth--;
            if (depth == 0) break;
        } while (peekToken().type != TSTokenType::Eof);
        const char* bodyEnd = tokens[tokenPos - 1].pos.ptr + 1;
        func.body = std::string(bodyStart, bodyEnd - bodyStart);
        tokenPos = savePos; // Reset to re-parse body later
    }

    func.filename = currentFile;
    functions[fullName] = func;

    Console::instance().printf(LogLevel::Debug, "TS: defined function '%s' (%zu params)%s",
        fullName.c_str(), func.params.size(), func.body.empty() ? "" : " [ext]");

    // Skip body tokens
    if (peekToken().type == TSTokenType::LBrace) {
        int depth = 0;
        do {
            TSToken t = nextToken();
            if (t.type == TSTokenType::LBrace) depth++;
            if (t.type == TSTokenType::RBrace) depth--;
            if (depth == 0) break;
        } while (peekToken().type != TSTokenType::Eof);
    }

    return {};
}

VMValue TorqueScript::Impl::parsePackage() {
    expect(TSTokenType::Package);
    std::string packageName = nextToken().text;
    expect(TSTokenType::LBrace);

    // Parse function declarations inside the package
    int depth = 1;
    while (depth > 0 && peekToken().type != TSTokenType::Eof) {
        if (peekToken().type == TSTokenType::Function) {
            parseFunctionDecl();
        } else if (peekToken().type == TSTokenType::LBrace) {
            depth++;
            nextToken();
        } else if (peekToken().type == TSTokenType::RBrace) {
            depth--;
            if (depth > 0) nextToken();
        } else {
            nextToken();
        }
    }
    if (peekToken().type == TSTokenType::RBrace) nextToken();
    match(TSTokenType::Semicolon);

    return {};
}

VMValue TorqueScript::Impl::parseExpressionStatement() {
    VMValue result = parseExpression();
    match(TSTokenType::Semicolon);
    return result;
}

// === Expression parsing ===
VMValue TorqueScript::Impl::parseExpression() {
    return parseAssignment();
}

VMValue TorqueScript::Impl::parseAssignment() {
    lastFieldObj.clear();
    lastFieldName.clear();
    VMValue lhs = parseTernary();
    if (peekToken().type == TSTokenType::Eq ||
        peekToken().type == TSTokenType::PlusEq ||
        peekToken().type == TSTokenType::MinusEq ||
        peekToken().type == TSTokenType::StarEq ||
        peekToken().type == TSTokenType::SlashEq) {
        TSToken op = nextToken();
        // Save target before rhs parsing (which may overwrite lastVarName/lastField*)
        std::string targetVar = lastVarName;
        std::string targetFieldObj = lastFieldObj;
        std::string targetFieldName = lastFieldName;
        VMValue rhs = parseAssignment();
        // Restore target
        lastVarName = targetVar;
        lastFieldObj = targetFieldObj;
        lastFieldName = targetFieldName;

        if (lhs.type == VMValue::None) {
            error("Invalid assignment target");
            return rhs;
        }

        VMValue val = rhs;
        if (op.type == TSTokenType::PlusEq) { val = VMValue(lhs.toDouble() + rhs.toDouble()); }
        else if (op.type == TSTokenType::MinusEq) { val = VMValue(lhs.toDouble() - rhs.toDouble()); }
        else if (op.type == TSTokenType::StarEq) { val = VMValue(lhs.toDouble() * rhs.toDouble()); }
        else if (op.type == TSTokenType::SlashEq) { val = rhs.toDouble() != 0 ? VMValue(lhs.toDouble() / rhs.toDouble()) : VMValue(0); }

        // Store back: object field, global, or local
        if (!lastFieldObj.empty() && !lastFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
            if (obj) obj->fields[lastFieldName] = val;
        } else if (!lastVarName.empty()) {
            if (lastVarName[0] == '$') {
                outer->setGlobal(lastVarName, val);
            } else {
                locals.set(lastVarName, val);
            }
        }
        return val;
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseTernary() {
    VMValue cond = parseLogicalOr();
    if (match(TSTokenType::Question)) {
        VMValue trueVal = parseExpression();
        expect(TSTokenType::Colon);
        VMValue falseVal = parseTernary();
        return cond.toBool() ? trueVal : falseVal;
    }
    return cond;
}

VMValue TorqueScript::Impl::parseLogicalOr() {
    VMValue lhs = parseLogicalAnd();
    while (peekToken().type == TSTokenType::Or) {
        nextToken();
        VMValue rhs = parseLogicalAnd();
        lhs = VMValue(lhs.toBool() || rhs.toBool() ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseLogicalAnd() {
    VMValue lhs = parseBitwiseOr();
    while (peekToken().type == TSTokenType::And) {
        nextToken();
        VMValue rhs = parseBitwiseOr();
        lhs = VMValue(lhs.toBool() && rhs.toBool() ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseBitwiseOr() {
    VMValue lhs = parseBitwiseXor();
    while (peekToken().type == TSTokenType::BitwiseOr) {
        nextToken();
        VMValue rhs = parseBitwiseXor();
        lhs = VMValue(lhs.toInt() | rhs.toInt());
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseBitwiseXor() {
    VMValue lhs = parseBitwiseAnd();
    while (peekToken().type == TSTokenType::BitwiseXor) {
        nextToken();
        VMValue rhs = parseBitwiseAnd();
        lhs = VMValue(lhs.toInt() ^ rhs.toInt());
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseBitwiseAnd() {
    VMValue lhs = parseEquality();
    while (peekToken().type == TSTokenType::BitwiseAnd) {
        nextToken();
        VMValue rhs = parseEquality();
        lhs = VMValue(lhs.toInt() & rhs.toInt());
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseEquality() {
    VMValue lhs = parseRelational();
    while (peekToken().type == TSTokenType::EqEq || peekToken().type == TSTokenType::Neq ||
           peekToken().type == TSTokenType::StrEq || peekToken().type == TSTokenType::StrNeq) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseRelational();
        bool eq;
        if (op == TSTokenType::StrEq || op == TSTokenType::StrNeq) {
            eq = lhs.toString() == rhs.toString();
        } else {
            eq = (lhs.type == VMValue::String || rhs.type == VMValue::String)
                ? (lhs.toString() == rhs.toString())
                : (lhs.toDouble() == rhs.toDouble());
        }
        lhs = VMValue((op == TSTokenType::EqEq || op == TSTokenType::StrEq) == eq ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseRelational() {
    VMValue lhs = parseShift();
    while (peekToken().type == TSTokenType::Lt || peekToken().type == TSTokenType::Gt ||
           peekToken().type == TSTokenType::Le || peekToken().type == TSTokenType::Ge) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseShift();
        double a = lhs.toDouble(), b = rhs.toDouble();
        bool r = false;
        switch (op) {
            case TSTokenType::Lt: r = a < b; break;
            case TSTokenType::Gt: r = a > b; break;
            case TSTokenType::Le: r = a <= b; break;
            case TSTokenType::Ge: r = a >= b; break;
            default: break;
        }
        lhs = VMValue(r ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseShift() {
    VMValue lhs = parseAdditive();
    while (peekToken().type == TSTokenType::Shl || peekToken().type == TSTokenType::Shr) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseAdditive();
        if (op == TSTokenType::Shl)
            lhs = VMValue(lhs.toInt() << rhs.toInt());
        else
            lhs = VMValue(lhs.toInt() >> rhs.toInt());
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseAdditive() {
    VMValue lhs = parseMultiplicative();
    while (true) {
        std::string sep;
        if (peekToken().type == TSTokenType::At) {
            nextToken();
            VMValue rhs = parseMultiplicative();
            lhs = VMValue(lhs.toString() + rhs.toString());
            continue;
        }
        if (peekToken().type == TSTokenType::Plus) {
            nextToken();
            VMValue rhs = parseMultiplicative();
            if (lhs.type == VMValue::String || rhs.type == VMValue::String)
                lhs = VMValue(lhs.toString() + rhs.toString());
            else
                lhs = VMValue(lhs.toDouble() + rhs.toDouble());
            continue;
        }
        if (peekToken().type == TSTokenType::Minus) {
            nextToken();
            VMValue rhs = parseMultiplicative();
            lhs = VMValue(lhs.toDouble() - rhs.toDouble());
            continue;
        }
        if (peekToken().type == TSTokenType::Ident) {
            std::string txt = peekToken().text;
            if (txt == "TAB") { sep = "\t"; }
            else if (txt == "SPC") { sep = " "; }
            else if (txt == "NL") { sep = "\n"; }
            if (!sep.empty()) {
                nextToken();
                VMValue rhs = parseMultiplicative();
                lhs = VMValue(lhs.toString() + sep + rhs.toString());
                continue;
            }
        }
        break;
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseMultiplicative() {
    VMValue lhs = parseUnary();
    while (peekToken().type == TSTokenType::Star || peekToken().type == TSTokenType::Slash ||
           peekToken().type == TSTokenType::Percent) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseUnary();
        switch (op) {
            case TSTokenType::Star: lhs = VMValue(lhs.toDouble() * rhs.toDouble()); break;
            case TSTokenType::Slash: lhs = rhs.toDouble() != 0 ? VMValue(lhs.toDouble() / rhs.toDouble()) : VMValue(0); break;
            case TSTokenType::Percent: lhs = rhs.toInt() != 0 ? VMValue(lhs.toInt() % rhs.toInt()) : VMValue(0); break;
            default: break;
        }
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseUnary() {
    if (peekToken().type == TSTokenType::Minus || peekToken().type == TSTokenType::Not ||
        peekToken().type == TSTokenType::Tilde || peekToken().type == TSTokenType::Plus) {
        TSTokenType op = nextToken().type;
        VMValue val = parseUnary();
        if (op == TSTokenType::Minus) return VMValue(-val.toDouble());
        if (op == TSTokenType::Not) return VMValue(!val.toBool() ? 1 : 0);
        if (op == TSTokenType::Tilde) return VMValue(~val.toInt());
        return val;
    }
    if (peekToken().type == TSTokenType::PlusPlus) {
        nextToken();
        VMValue val = parseUnary();
        // Save original variable info before parseUnary potentially overwrites it
        std::string saveFieldObj = lastFieldObj;
        std::string saveFieldName = lastFieldName;
        std::string saveVarName = lastVarName;
        VMValue newVal(val.toDouble() + 1);
        if (!saveFieldObj.empty() && !saveFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(saveFieldObj.c_str());
            if (obj) obj->fields[saveFieldName] = newVal;
        } else if (!saveVarName.empty()) {
            if (saveVarName[0] == '$') outer->setGlobal(saveVarName, newVal);
            else locals.set(saveVarName, newVal);
        }
        return newVal;
    }
    if (peekToken().type == TSTokenType::MinusMinus) {
        nextToken();
        VMValue val = parseUnary();
        std::string saveFieldObj = lastFieldObj;
        std::string saveFieldName = lastFieldName;
        std::string saveVarName = lastVarName;
        VMValue newVal(val.toDouble() - 1);
        if (!saveFieldObj.empty() && !saveFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(saveFieldObj.c_str());
            if (obj) obj->fields[saveFieldName] = newVal;
        } else if (!saveVarName.empty()) {
            if (saveVarName[0] == '$') outer->setGlobal(saveVarName, newVal);
            else locals.set(saveVarName, newVal);
        }
        return newVal;
    }
    return parsePostfix();
}

VMValue TorqueScript::Impl::parsePostfix() {
    VMValue val = parsePrimary();

    while (true) {
        if (peekToken().type == TSTokenType::LParen) {
            // Function call
            nextToken();
            std::vector<VMValue> args;
            parseArgumentList(args);
            expect(TSTokenType::RParen);

            // The primary should have given us the function name
            // We stored it in the last value's string
            // For now, we handle this differently - the function name is in val.str
            // But VMValue might not have a function name. Let's handle via the expression flow.

            // Actually, function calls are handled in parsePrimary() where
            // we detect if the next token is LParen.
            // This postfix handling won't normally reach here for function calls.

            // Object method call: obj.method(args)
            // This is handled when the primary is an Ident followed by Dot
            break;
        }
        if (peekToken().type == TSTokenType::LBracket) {
            std::string varBeforeIndex = lastVarName; // save BEFORE parseExpression overwrites it
            nextToken();
            VMValue idx = parseExpression();
            std::string savedVar = varBeforeIndex; // use the saved variable name
            // Handle 2D arrays: arr[i, j] → key "i,j"
            while (match(TSTokenType::Comma)) {
                VMValue idx2 = parseExpression();
                idx = VMValue(idx.toString() + "," + idx2.toString());
            }
            expect(TSTokenType::RBracket);
            if (!savedVar.empty()) {
                std::string arrayKey = savedVar + "[" + idx.toString() + "]";
                lastVarName = arrayKey; // update for assignment write-back
                if (savedVar[0] == '$') {
                    val = outer->getGlobal(arrayKey);
                } else {
                    val = locals.get(arrayKey);
                }
                if (peekToken().type == TSTokenType::PlusPlus) {
                    nextToken();
                    VMValue newVal(val.toDouble() + 1);
                    if (lastVarName[0] == '$') outer->setGlobal(arrayKey, newVal);
                    else locals.set(arrayKey, newVal);
                    val = newVal;
                    break;
                }
                if (peekToken().type == TSTokenType::MinusMinus) {
                    nextToken();
                    VMValue newVal(val.toDouble() - 1);
                    if (lastVarName[0] == '$') outer->setGlobal(arrayKey, newVal);
                    else locals.set(arrayKey, newVal);
                    val = newVal;
                    break;
                }
            } else {
                val = VMValue(0);
            }
            continue;
        }
        if (peekToken().type == TSTokenType::Dot) {
            // Member access
            nextToken();
            TSToken member = nextToken();
            if (peekToken().type == TSTokenType::LParen) {
                // Method call
                nextToken();
                std::vector<VMValue> args;
                parseArgumentList(args);
                expect(TSTokenType::RParen);

                // Look up method: objClass::methodName
                std::string objName = val.toString();
                std::string methodName = member.text;
                bool called = false;
                // Method calls pass the object as the first argument (self)
                std::vector<VMValue> methodArgs;
                methodArgs.push_back(VMValue(objName));
                methodArgs.insert(methodArgs.end(), args.begin(), args.end());
                // Try the bare function name first
                std::string fullName = methodName;
                auto fit = functions.find(fullName);
                if (fit != functions.end()) { val = outer->callFunction(fullName, methodArgs); called = true; }
                if (!called) {
                    std::string lower = fullName;
                    for (auto& c : lower) c = (char)tolower((unsigned char)c);
                    auto nit = natives.find(lower);
                    if (nit != natives.end()) { val = nit->second(methodArgs); called = true; }
                }
                if (!called) {
                    // Try namespace-qualified: ClassName::method
                    std::string nsFull = objName + "::" + methodName;
                    auto nsFit = functions.find(nsFull);
                    if (nsFit != functions.end()) { val = outer->callFunction(nsFull, methodArgs); called = true; }
                }
                if (!called) {
                    std::string nsFull = objName + "::" + methodName;
                    std::string nsLower = nsFull;
                    for (auto& c : nsLower) c = (char)tolower((unsigned char)c);
                    auto nsNit = natives.find(nsLower);
                    if (nsNit != natives.end()) { val = nsNit->second(methodArgs); called = true; }
                }
            } else {
                // Field access
                lastFieldObj = val.toString();
                lastFieldName = member.text;
                auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
                if (obj) {
                    auto fit = obj->fields.find(lastFieldName);
                    if (fit != obj->fields.end()) val = fit->second;
                    else val = VMValue(0);
                } else {
                    val = VMValue(0);
                }
            }
            continue;
        }
        if (peekToken().type == TSTokenType::PlusPlus) {
            nextToken();
            writeBackVar(VMValue(val.toDouble() + 1));
            val = VMValue(val.toDouble() + 1);
            break;
        }
        if (peekToken().type == TSTokenType::MinusMinus) {
            nextToken();
            writeBackVar(VMValue(val.toDouble() - 1));
            val = VMValue(val.toDouble() - 1);
            break;
        }
        break; // No postfix operator matched
    }
    return val;
}

VMValue TorqueScript::Impl::parsePrimary() {
    TSToken tok = nextToken();

    switch (tok.type) {
        case TSTokenType::Number:
            return VMValue(tok.numVal);

        case TSTokenType::String:
            return VMValue(tok.text);

        case TSTokenType::True:
            return VMValue(1);

        case TSTokenType::False:
            return VMValue(0);

        case TSTokenType::Null:
            return VMValue("");

        case TSTokenType::Dollar: {
            TSToken nameTok = nextToken();
            lastVarName = "$" + nameTok.text;

            // Handle namespace::variable syntax ($Host::TimeLimit)
            while (peekToken().type == TSTokenType::Colon && peekToken(1).type == TSTokenType::Colon) {
                nextToken(); nextToken();
                lastVarName += "::" + nextToken().text;
            }

            // Check for function call: name(args)
            if (peekToken().type == TSTokenType::LParen) {
                nextToken();
                std::vector<VMValue> args;
                parseArgumentList(args);
                expect(TSTokenType::RParen);

                // Try native first
                auto nit = natives.find(toLower(nameTok.text));
                if (nit != natives.end()) {
                    return nit->second(args);
                }

                // Try DSO function (via ScriptEngine VM)
                auto& engine = ScriptEngine::instance();
                if (engine.vm()) {
                    VMValue vmResult = engine.vm()->callFunction(nameTok.text.c_str(), args);
                    if (vmResult.type != VMValue::None) return vmResult;
                }

                // Try TS function
                auto fit = functions.find(nameTok.text);
                if (fit != functions.end()) {
                    return outer->callFunction(nameTok.text, args);
                }

                Console::instance().printf(LogLevel::Warn, "TS: unknown function '%s'", nameTok.text.c_str());
                return VMValue(0);
            }

            return outer->getGlobal(lastVarName);
        }

        case TSTokenType::Percent: {
            TSToken nameTok = nextToken();
            lastVarName = nameTok.text;
            // Function call: %name(args) - treat as function call
            if (peekToken().type == TSTokenType::LParen) {
                nextToken();
                std::vector<VMValue> args;
                parseArgumentList(args);
                expect(TSTokenType::RParen);

                auto nit = natives.find(toLower(nameTok.text));
                if (nit != natives.end()) return nit->second(args);

                auto& engine = ScriptEngine::instance();
                if (engine.vm()) {
                    VMValue vmResult = engine.vm()->callFunction(nameTok.text.c_str(), args);
                    if (vmResult.type != VMValue::None) return vmResult;
                }

                auto fit = functions.find(nameTok.text);
                if (fit != functions.end()) return outer->callFunction(nameTok.text, args);

                Console::instance().printf(LogLevel::Warn, "TS: unknown function '%s'", nameTok.text.c_str());
                return VMValue(0);
            }

            return locals.get(lastVarName);
        }

        case TSTokenType::Ident:
        case TSTokenType::Parent: {
            std::string name = tok.text;
            if (tok.type == TSTokenType::Parent) name = "Parent";

            // Helper to look up and call a function by name with args
            auto lookupAndCall = [&](const std::string& fn, std::vector<VMValue>& args) -> VMValue {
                // exec: use nested execution with modpath overlay
                if (fn == "exec" && !args.empty()) {
                    std::string execPath = args[0].toString();
                    std::string modPath = Console::instance().getStringVariable("modPath", "base");
                    std::vector<uint8_t> data;
                    // Try modPath first, then base as fallback
                    if (modPath != "base") {
                        data = Engine::instance().fs().read((modPath + "/" + execPath).c_str());
                    }
                    if (data.empty()) {
                        data = Engine::instance().fs().read(("base/" + execPath).c_str());
                    }
                    // Fall back to direct path
                    if (data.empty()) {
                        data = Engine::instance().fs().read(execPath.c_str());
                    }
                    if (!data.empty()) {
                        std::string src((const char*)data.data(), data.size());
                        return outer->executeNested(src, execPath);
                    }
                    return VMValue(0);
                }
                // Try natives
                auto nit = natives.find(toLower(fn));
                if (nit != natives.end()) return nit->second(args);
                // Try DSO VM
                auto& engine = ScriptEngine::instance();
                if (engine.vm()) {
                    VMValue vmResult = engine.vm()->callFunction(fn.c_str(), args);
                    if (vmResult.type != VMValue::None) return vmResult;
                }
                // Try TS functions
                auto fit = functions.find(fn);
                if (fit != functions.end()) return outer->callFunction(fn, args);
                // Check console commands
                auto* item = Console::instance().find(fn.c_str());
                if (item && item->type == Console::ConsoleItem::Command) {
                    std::vector<const char*> argv;
                    argv.push_back(fn.c_str());
                    for (auto& a : args) {
                        static std::vector<std::string> argStorage;
                        argStorage.push_back(a.toString());
                        argv.push_back(argStorage.back().c_str());
                    }
                    item->cmd((int32_t)argv.size(), argv.data());
                    return VMValue(1);
                }
                Console::instance().printf(LogLevel::Warn, "TS: unknown function '%s'", fn.c_str());
                return VMValue(0);
            };

            // Check for namespace::method syntax
            if (peekToken().type == TSTokenType::Colon && peekToken(1).type == TSTokenType::Colon) {
                nextToken(); nextToken();
                std::string methodName = nextToken().text;
                std::string fullName = name + "::" + methodName;

                if (peekToken().type == TSTokenType::LParen) {
                    nextToken();
                    std::vector<VMValue> args;
                    parseArgumentList(args);
                    expect(TSTokenType::RParen);

                    auto fit = functions.find(fullName);
                    if (fit != functions.end()) {
                        return outer->callFunction(fullName, args);
                    }
                    // Fall back to unnamespaced function (Parent:: behavior)
                    return lookupAndCall(methodName, args);
                }
                return VMValue(0);
            }

            // Function call: name(args)
            if (peekToken().type == TSTokenType::LParen) {
                nextToken();
                std::vector<VMValue> args;
                parseArgumentList(args);
                expect(TSTokenType::RParen);

                return lookupAndCall(name, args);
            }

            // Variable reference: check globals if locals returns default (0 or empty)
            {
                VMValue lv = locals.get(name);
                bool notFound = (lv.type == VMValue::Int && lv.toInt() == 0);
                if (notFound) {
                    auto git = globals.find(name);
                    if (git != globals.end()) return git->second;
                }
                return lv;
            }
        }

        case TSTokenType::LParen: {
            VMValue val = parseExpression();
            expect(TSTokenType::RParen);
            return val;
        }

        case TSTokenType::LBrace:
            return parseBlock();

        case TSTokenType::New: {
            // new ObjectType(name, ...) { fields }
            TSToken className = nextToken();
            expect(TSTokenType::LParen);
            std::vector<VMValue> args;
            // Parse arguments: treat bare identifiers as string literals
            while (peekToken().type != TSTokenType::RParen && peekToken().type != TSTokenType::Eof) {
                if (peekToken().type == TSTokenType::Ident) {
                    // Bare identifier → string literal
                    args.push_back(VMValue(nextToken().text));
                } else {
                    args.push_back(parseExpression());
                }
                if (peekToken().type == TSTokenType::Comma) nextToken();
            }
            expect(TSTokenType::RParen);

            auto* obj = new ScriptObject;
            obj->className = className.text;
            if (!args.empty()) obj->name = args[0].toString();
            // Auto-generate name for unnamed controls
            if (obj->name.empty() && (obj->className.find("Gui") == 0 || obj->className.find("Shell") == 0 ||
                                       obj->className.find("Hud") == 0 || obj->className == "GameTSCtrl")) {
                static uint32_t guiCounter = 0;
                obj->name = "_unnamed" + std::to_string(guiCounter++);
            }

            // Track parent-child for GUI controls (Gui*, Shell*, GameTSCtrl)
            bool isGuiControl = (obj->className.find("Gui") == 0) ||
                                (obj->className.find("Shell") == 0) ||
                                obj->className == "GameTSCtrl" ||
                                obj->className.find("Hud") == 0;

            if (peekToken().type == TSTokenType::LBrace) {
                nextToken();
                // Record this object as parent for nested new expressions
                if (isGuiControl) guiParentStack.push_back(obj);
                int depth = 1;
                while (depth > 0 && peekToken().type != TSTokenType::Eof) {
                    TSToken tok = peekToken();
                    if (tok.type == TSTokenType::RBrace) {
                        nextToken();
                        if (--depth == 0) break;
                        continue;
                    }
                    if (tok.type == TSTokenType::LBrace) {
                        nextToken();
                        depth++;
                        continue;
                    }
                    if (tok.type == TSTokenType::Semicolon) { nextToken(); continue; }
                    if (tok.type == TSTokenType::New) {
                        parseExpression();
                        continue;
                    }
                    if (tok.type == TSTokenType::Function) {
                        parseFunctionDecl();
                        continue;
                    }
                    // Field assignment or expression
                    TSToken fieldName = nextToken();
                    if (peekToken().type == TSTokenType::Eq) {
                        nextToken();
                        VMValue fieldVal = parseExpression();
                        obj->fields[fieldName.text] = fieldVal;
                    } else {
                        // Could be a method call or other expression
                        // Re-tokenize? No, just consume until semicolon
                        while (peekToken().type != TSTokenType::Semicolon && peekToken().type != TSTokenType::Eof
                               && peekToken().type != TSTokenType::RBrace)
                            nextToken();
                    }
                    while (peekToken().type == TSTokenType::Semicolon) nextToken();
                }
                if (isGuiControl && !guiParentStack.empty()) guiParentStack.pop_back();
                if (peekToken().type == TSTokenType::RBrace) nextToken();
            }

            // Link parent-child for GUI controls
            if (isGuiControl && !guiParentStack.empty()) {
                ScriptObject* parent = guiParentStack.back();
                if (parent != obj) {
                    obj->internals["parent"] = VMValue(parent->name);
                }
            }

            ScriptEngine::instance().objects[obj->name] = obj;
            // Set globals so script can reference the object by name (both $name and bare name)
            if (!obj->name.empty()) {
                outer->setGlobal("$" + obj->name, VMValue(obj->name));
                globals[obj->name] = VMValue(obj->name);
            }
            return VMValue(obj->name);
        }

        default:
            if (tok.type == TSTokenType::RBrace && running) {
                // If we hit a closing brace unexpectedly, it means the parser
                // token position is misaligned. Skip it silently.
                return VMValue(0);
            }
            // case/default/colon tokens may leak if a switch body was misaligned
            if (tok.type == TSTokenType::Case || tok.type == TSTokenType::Default ||
                tok.type == TSTokenType::Colon) {
                return VMValue(0);
            }
            error(std::string("Unexpected token: ") + tok.text);
            return VMValue(0);
    }
}

// === Nested exec ===
VMValue TorqueScript::executeNested(const std::string& source, const std::string& path) {
    Console::instance().printf(LogLevel::Debug, "TS: nested enter '%s'", path.c_str());
    // Save outer state
    std::string savedFile = std::move(impl->currentFile);
    std::vector<TSToken> savedTokens = std::move(impl->tokens);
    size_t savedPos = impl->tokenPos;
    bool savedRunning = impl->running;
    VMValue savedReturnValue = impl->returnValue;
    std::string savedLastVarName = std::move(impl->lastVarName);
    std::string savedLastFieldObj = std::move(impl->lastFieldObj);
    std::string savedLastFieldName = std::move(impl->lastFieldName);
    int savedDepth = impl->execDepth;
    int savedSrcLine = impl->srcLine;

    // Try loading DSO cache before parsing source (.cs, .gui, .mis)
    if (isCompilableExt(path)) {
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (!outDir.empty()) {
            std::string dsoPath = outDir + "/" + modPath + "/" + path + ".dso";
            struct stat st;
            if (stat(dsoPath.c_str(), &st) == 0) {
                std::ifstream f(dsoPath, std::ios::binary);
                if (f) {
                    std::vector<uint8_t> dsoData((std::istreambuf_iterator<char>(f)), {});
                    uint32_t version = *(const uint32_t*)dsoData.data();
                    if (version == 0x54534F01) {
                        Console::instance().printf(LogLevel::Debug, "TS: loading DSO cache '%s'", path.c_str());
                        const uint8_t* p = dsoData.data() + 4;
                        uint32_t funcCount = *(const uint32_t*)p; p += 4;
                        for (uint32_t fi = 0; fi < funcCount; fi++) {
                            auto r32 = [&]() -> uint32_t { uint32_t v = *(const uint32_t*)p; p += 4; return v; };
                            auto rstr = [&]() -> std::string {
                                uint32_t len = r32();
                                std::string s((const char*)p, len); p += len;
                                return s;
                            };
                            std::string fnName = rstr();
                            uint32_t paramCount = r32();
                            std::vector<std::string> params;
                            for (uint32_t pi = 0; pi < paramCount; pi++) params.push_back(rstr());
                            std::string body = rstr();
                            if (impl->functions.find(fnName) == impl->functions.end()) {
                                TSFunc fn;
                                fn.params = params;
                                fn.body = body;
                                fn.filename = path;
                                impl->functions[fnName] = fn;
                            }
                        }
                    } else {
                        // Try native DSO format (turd compiler output, etc.)
                        Console::instance().printf(LogLevel::Debug, "TS: loading native DSO '%s'", path.c_str());
                        auto& engine = ScriptEngine::instance();
                        if (!engine.vm() || !engine.vm()->loadScript(dsoData.data(), dsoData.size(), dsoPath.c_str())) {
                            Console::instance().printf(LogLevel::Warn, "TS: failed to load native DSO '%s', falling back to source", path.c_str());
                            f.close();
                            // Continue to source execution below
                        } else {
                            // Register DSO functions as TS functions
                            for (auto& dso : engine.vm()->loadedScripts()) {
                                for (auto& fn : dso->functions) {
                                    std::string fullName = fn.ns.empty() ? fn.name : fn.ns + "::" + fn.name;
                                    if (impl->functions.find(fullName) == impl->functions.end()) {
                                        TSFunc stub;
                                        stub.params = fn.argNames;
                                        stub.filename = fn.filename;
                                        stub.isDSO = true;
                                        stub.dsoFunc = &fn;
                                        impl->functions[fullName] = stub;
                                    }
                                }
                            }
                            // DSO loaded successfully — skip source execution
                            f.close();
                            VMValue dsoResult;
                            impl->execDepth = savedDepth;
                            impl->srcLine = savedSrcLine;
                            impl->currentFile = std::move(savedFile);
                            impl->tokens = std::move(savedTokens);
                            impl->tokenPos = savedPos;
                            impl->running = savedRunning;
                            impl->returning = false;
                            impl->breaking = false;
                            impl->continuing = false;
                            impl->returnValue = savedReturnValue;
                            impl->lastVarName = std::move(savedLastVarName);
                            impl->lastFieldObj = std::move(savedLastFieldObj);
                            impl->lastFieldName = std::move(savedLastFieldName);
                            impl->locals.pop();
                            return dsoResult;
                        }
                    }
                }
            }
        }
    }

    // Execute inner
    impl->execDepth = savedDepth + 1;
    impl->currentFile = path;
    impl->running = true;
    impl->returning = false;
    impl->breaking = false;
    impl->continuing = false;
    impl->lastVarName.clear();
    impl->lastFieldObj.clear();
    impl->lastFieldName.clear();
    impl->tokenize(source);
    VMValue result = impl->parseProgram();
    if (impl->returning) {
        result = impl->returnValue;
        // Don't propagate returning/breaking/continuing to outer context
    }

    // Write source-cache DSO for fast loading on next run
    if (isCompilableExt(path) && path.find('/') != std::string::npos) {
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (!outDir.empty()) {
            std::string dsoRelPath = modPath + "/" + path + ".dso";
            std::string dsoFullPath = outDir + "/" + dsoRelPath;
            impl->writeDSOCache(dsoFullPath, path, source);
        }
    }

    // Restore outer state (discard inner returning/breaking/continuing)
    impl->execDepth = savedDepth;
    impl->srcLine = savedSrcLine;
    impl->currentFile = std::move(savedFile);
    impl->tokens = std::move(savedTokens);
    impl->tokenPos = savedPos;
    impl->running = savedRunning;
    impl->returning = false;
    impl->breaking = false;
    impl->continuing = false;
    impl->returnValue = savedReturnValue;
    impl->lastVarName = std::move(savedLastVarName);
    impl->lastFieldObj = std::move(savedLastFieldObj);
    impl->lastFieldName = std::move(savedLastFieldName);

    std::string exitPath = path;  // copy before any potential invalidation
    Console::instance().printf(LogLevel::Debug, "TS: nested exit-pre '%s'", exitPath.c_str());
    Console::instance().printf(LogLevel::Debug, "TS: nested exit '%s' (execDepth=%d)", exitPath.c_str(), impl->execDepth);
    // Process events + render so the window stays responsive during loading
    if (impl->execDepth <= 1) {
        auto& plat = Engine::instance().platform();
        auto& ren = Engine::instance().renderer();
        auto& gui = Engine::instance().guiRenderer();
        plat.processEvents();
        // Check for ~ or Pause during loading
        {
            static bool prevTilde = false;
            bool tildeDown = plat.input().keysDown[53]; // SCANCODE_GRAVE
            if (tildeDown && !prevTilde) {
                if (gui.isDialogActive("ConsoleDlg")) gui.popDialog("ConsoleDlg");
                else gui.pushDialog("ConsoleDlg");
            }
            prevTilde = tildeDown;
        }
        {
            static bool prevPause = false;
            bool pauseDown = plat.input().keysDown[72]; // SCANCODE_PAUSE
            if (pauseDown && !prevPause) Engine::instance().toggleOverlay();
            prevPause = pauseDown;
        }
        ren.beginFrame({0.15f, 0.15f, 0.2f, 1.0f});
        gui.render();
        ren.endFrame();
        plat.swapBuffers();
    }
    return result;
}

void TorqueScript::Impl::parseArgumentList(std::vector<VMValue>& args) {
    if (peekToken().type != TSTokenType::RParen) {
        // Bare identifiers (not followed by '(' or '[' or '::') are string literals in TorqueScript
        // e.g. schedule(100, 0, checkGGIntroDone) passes "checkGGIntroDone" as a string
        auto isBareIdent = [&]() {
            return peekToken().type == TSTokenType::Ident &&
                   peekToken(1).type != TSTokenType::LParen &&
                   peekToken(1).type != TSTokenType::LBracket &&
                   peekToken(1).type != TSTokenType::Colon &&
                   peekToken(1).type != TSTokenType::Dot;
        };
        if (isBareIdent()) {
            args.push_back(VMValue(nextToken().text));
        } else {
            args.push_back(parseExpression());
        }
        while (match(TSTokenType::Comma)) {
            if (isBareIdent()) {
                args.push_back(VMValue(nextToken().text));
            } else {
                args.push_back(parseExpression());
            }
        }
    }
}

// === Execution ===
VMValue TorqueScript::execute(const std::string& source, const std::string& filename) {
    impl->currentFile = filename;
    impl->running = true;
    impl->returning = false;
    impl->breaking = false;
    impl->continuing = false;
    impl->lastVarName.clear();
    impl->tokenize(source);
    return impl->parseProgram();
}

VMValue TorqueScript::executeFile(const std::string& path) {
    // Prevent circular includes
    if (impl->loadingFiles.count(path)) {
        Console::instance().printf(LogLevel::Debug, "TS: skipping already-loading file '%s'", path.c_str());
        return {};
    }
    impl->loadingFiles.insert(path);

    // For modpath .cs/.gui/.mis files, try .cs.dso first
    // Root-level files (no '/') like console_start.cs skip DSO
    if (isCompilableExt(path) && path.find('/') != std::string::npos) {
        std::string dsoPath = path + ".dso";
        auto dsoData = Engine::instance().fs().read(dsoPath.c_str());
        if (dsoData.empty()) {
            std::ifstream f(dsoPath, std::ios::binary);
            if (f) dsoData = std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
        }
        if (!dsoData.empty()) {
            // Check for custom source DSO cache (v0x54534F01)
            if (dsoData.size() >= 8) {
                uint32_t version = *(const uint32_t*)dsoData.data();
                if (version == 0x54534F01) {
                    Console::instance().printf(LogLevel::Debug, "TS: loading source DSO cache '%s'", dsoPath.c_str());
                    const uint8_t* p = dsoData.data() + 4;
                    uint32_t funcCount = *(const uint32_t*)p; p += 4;
                    for (uint32_t fi = 0; fi < funcCount; fi++) {
                        auto r32 = [&]() -> uint32_t { uint32_t v = *(const uint32_t*)p; p += 4; return v; };
                        auto rstr = [&]() -> std::string {
                            uint32_t len = r32();
                            std::string s((const char*)p, len); p += len;
                            return s;
                        };
                        std::string fnName = rstr();
                        uint32_t paramCount = r32();
                        std::vector<std::string> params;
                        for (uint32_t pi = 0; pi < paramCount; pi++) params.push_back(rstr());
                        std::string body = rstr();
                        if (impl->functions.find(fnName) == impl->functions.end()) {
                            TSFunc fn;
                            fn.params = params;
                            fn.body = body;
                            fn.filename = path;
                            impl->functions[fnName] = fn;
                        }
                    }
                    impl->loadingFiles.erase(path);
                    return VMValue(1);
                }
            }
            // Try native DSO format
            Console::instance().printf(LogLevel::Debug, "TS: loading DSO '%s' (%zu bytes)", dsoPath.c_str(), dsoData.size());
            auto& engine = ScriptEngine::instance();
            if (engine.vm() && engine.vm()->loadScript(dsoData.data(), dsoData.size(), dsoPath.c_str())) {
                Console::instance().printf(LogLevel::Debug, "TS: DSO loaded successfully: %s", dsoPath.c_str());
                for (auto& dso : engine.vm()->loadedScripts()) {
                    for (auto& fn : dso->functions) {
                        std::string fullName = fn.ns.empty() ? fn.name : fn.ns + "::" + fn.name;
                        if (impl->functions.find(fullName) == impl->functions.end()) {
                            TSFunc stub;
                            stub.params = fn.argNames;
                            stub.filename = fn.filename;
                            stub.isDSO = true;
                            stub.dsoFunc = &fn;
                            impl->functions[fullName] = stub;
                        }
                    }
                }
                impl->loadingFiles.erase(path);
                return VMValue(1);
            }
        }
    }

    // Fall back to loading source
    auto data = Engine::instance().fs().read(path.c_str());
    if (data.empty()) {
        // Try opening as regular file
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            Console::instance().printf(LogLevel::Warn, "TS: cannot open script file '%s'", path.c_str());
            impl->loadingFiles.erase(path);
            return {};
        }
        data = std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
    }

    Console::instance().printf(LogLevel::Debug, "TS: executing '%s' (%zu bytes)", path.c_str(), data.size());
    std::string source((const char*)data.data(), data.size());
    VMValue result = execute(source, path);

    // After successful execution, write DSO cache to outputDir/modPath
    if (isCompilableExt(path) && path.find('/') != std::string::npos) {
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (!outDir.empty()) {
            std::string dsoRelPath = modPath + "/" + path + ".dso";
            std::string dsoFullPath = outDir + "/" + dsoRelPath;
            // Create parent dir
            auto slash = dsoFullPath.rfind('/');
            if (slash != std::string::npos) {
                std::string dir = dsoFullPath.substr(0, slash);
                struct stat st; if (stat(dir.c_str(), &st) != 0) mkdir(dir.c_str(), 0755);
            }
            Console::instance().printf(LogLevel::Debug, "TS: writing DSO cache '%s' (%zu funcs)", dsoFullPath.c_str(), impl->functions.size());
            impl->writeDSOCache(dsoFullPath, path, source);
        }
    }

    impl->loadingFiles.erase(path);
    return result;
}

VMValue TorqueScript::callFunction(const std::string& name, const std::vector<VMValue>& args) {
    auto it = impl->functions.find(name);
    if (it == impl->functions.end()) {
        Console::instance().printf(LogLevel::Warn, "TS: function not found: '%s'", name.c_str());
        return {};
    }

    TSFunc& func = it->second;

    // Save outer parsing state (a function call must not destroy the caller's token stream)
    std::vector<TSToken> savedTokens = std::move(impl->tokens);
    size_t savedPos = impl->tokenPos;
    bool savedRunning = impl->running;
    bool savedReturning = impl->returning;
    bool savedBreaking = impl->breaking;
    bool savedContinuing = impl->continuing;
    VMValue savedReturnValue = impl->returnValue;
    std::string savedFile = std::move(impl->currentFile);
    std::string savedLastVarName = std::move(impl->lastVarName);
    std::string savedFieldObj = std::move(impl->lastFieldObj);
    std::string savedFieldName = std::move(impl->lastFieldName);
    int savedSrcLine = impl->srcLine;

    // Set up locals
    impl->locals.push();
    for (size_t i = 0; i < func.params.size(); i++) {
        VMValue val = (i < args.size()) ? args[i] : VMValue(0);
        impl->locals.set(func.params[i], val);
    }

    impl->returning = false;
    impl->running = true;
    impl->breaking = false;
    impl->continuing = false;
    impl->lastVarName.clear();
    impl->lastFieldObj.clear();
    impl->lastFieldName.clear();

    // Execute function body
    VMValue result;
    if (func.isDSO && func.dsoFunc) {
        // DSO-compiled function: execute via DSO VM
        Console::instance().printf(LogLevel::Debug, "TS: calling DSO function '%s'", name.c_str());
        auto& engine = ScriptEngine::instance();
        // Find the DSO file containing this function
        for (auto* dso : engine.vm()->loadedScripts()) {
            if (dso->funcMap.count(name)) {
                result = engine.vm()->execute(dso, func.dsoFunc->startIp, args);
                break;
            }
        }
    } else if (!func.body.empty()) {
        impl->currentFile = func.filename;
        Console::instance().printf(LogLevel::Debug, "TS: calling function '%s' (%s, %zu bytes body)", name.c_str(), func.filename.c_str(), func.body.size());
        impl->tokenize(func.body);
        result = impl->parseProgram();
        Console::instance().printf(LogLevel::Debug, "TS: function '%s' returned", name.c_str());
    }

    if (impl->returning) {
        result = impl->returnValue;
    }

    // Restore outer state, discard inner control flow flags
    impl->tokens = std::move(savedTokens);
    impl->tokenPos = savedPos;
    impl->running = savedRunning;
    impl->returning = savedReturning;
    impl->breaking = savedBreaking;
    impl->continuing = savedContinuing;
    impl->returnValue = savedReturnValue;
    impl->currentFile = std::move(savedFile);
    impl->lastVarName = std::move(savedLastVarName);
    impl->lastFieldObj = std::move(savedFieldObj);
    impl->lastFieldName = std::move(savedFieldName);
    impl->srcLine = savedSrcLine;

    impl->locals.pop();
    return result;
}
