#include "script/torquescript.h"
#include "sim/net_object.h"
#include "sim/engine_classes.h"
#include "sim/net_string_table.h"
#include "script/script_engine.h"
#include "render/gui_renderer.h"
#include "core/console.h"
#include "core/engine.h"
#include "fs/path_policy.h"
#include <sys/stat.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>
#include <filesystem>

extern char** environ;

static void syncGuiField(const std::string& objName, const std::string& field, const VMValue& val);
#include <cctype>
#include <strings.h>

static std::string toLower(const std::string& s) {
    std::string r = s;
    for (auto& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

static bool sameName(const std::string& a, const std::string& b) {
    return a.size() == b.size() && strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
}

// TorqueScript names are case-insensitive: key function tables that way so
// lookups are hashed rather than scanned.
struct NameHash {
    size_t operator()(const std::string& s) const {
        size_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= (size_t)tolower(c); h *= 1099511628211ull; }
        return h;
    }
};
struct NameEqual {
    bool operator()(const std::string& a, const std::string& b) const { return sameName(a, b); }
};
using FunctionTable = std::unordered_map<std::string, TSFunc, NameHash, NameEqual>;

static VMValue* findField(ScriptObject* object, const std::string& name) {
    if (!object) return nullptr;
    for (auto& [field, value] : object->fields)
        if (sameName(field, name)) return &value;
    return nullptr;
}

static bool isCompilableExt(const std::string& p) {
    auto ext = p.size() > 3 ? p.substr(p.size() - 3) : std::string();
    return ext == ".cs" || ext == ".gui" || ext == ".mis";
}

static std::string normalizedScriptPath(const std::string& path) {
    return std::filesystem::path(path).lexically_normal().generic_string();
}

static bool readU32Bounded(const std::vector<uint8_t>& data, size_t& offset, uint32_t& value) {
    if (offset > data.size() || data.size() - offset < sizeof(uint32_t)) return false;
    value = (uint32_t)data[offset] | ((uint32_t)data[offset + 1] << 8) |
            ((uint32_t)data[offset + 2] << 16) | ((uint32_t)data[offset + 3] << 24);
    offset += sizeof(uint32_t);
    return true;
}

static bool readStringBounded(const std::vector<uint8_t>& data, size_t& offset,
                              std::string& value) {
    uint32_t length = 0;
    if (!readU32Bounded(data, offset, length) || length > data.size() - offset) return false;
    value.assign((const char*)data.data() + offset, length);
    offset += length;
    return true;
}

struct TorqueScript::Impl {
    TorqueScript* outer;
    std::unordered_map<std::string, std::function<VMValue(const std::vector<VMValue>&)>> natives;
    FunctionTable functions;
    std::unordered_map<std::string, FunctionTable> packageFunctions;
    std::vector<std::string> activePackages;
    std::vector<std::string> callPackages;
    std::vector<std::string> callNames; // executing script functions
    // A lower active package or the base table defines `name` below the
    // package `currentPackage` (the target of Parent:: from that package).
    bool packageParentExists(const std::string& name, const std::string& currentPackage) const {
        auto current = std::find_if(activePackages.rbegin(), activePackages.rend(),
            [&](const std::string& package) { return strcasecmp(package.c_str(), currentPackage.c_str()) == 0; });
        if (current != activePackages.rend())
            for (auto package = current + 1; package != activePackages.rend(); ++package) {
                auto it = packageFunctions.find(*package);
                if (it != packageFunctions.end() && it->second.count(name)) return true;
            }
        return functions.count(name) != 0;
    }
    std::string parsingPackage;
    std::unordered_map<std::string, VMValue> globals;
    // Lowercased name -> key in `globals` (names are case-insensitive).
    std::unordered_map<std::string, std::string> globalIndex;
    ScriptScheduler scheduler;
    TSLocals locals;
    bool initializing = false;
    bool errorOccurred = false;
    bool resolveParentNext = false;

    // Exec state
    const char* srcPtr{};
    const char* srcEnd{};
    int srcLine = 1;
    int srcCol = 1;
    std::vector<TSToken> tokens;
    size_t tokenPos = 0;
    std::string currentFile;
    std::string dbgSource; // last tokenized source (for parse-error context)
    bool running = true;
    bool returning = false;
    bool breaking = false;
    bool continuing = false;
    bool evaluating = true;
    VMValue returnValue;
    int loopDepth = 0;

    std::set<std::string> loadingFiles; // prevent circular includes
    std::vector<std::string> loadingStack;
    std::unordered_map<std::string, std::set<std::string>> fileDependencies;
    std::string lastVarName;
    std::string lastFieldObj;  // for %obj.field = value assignment write-back
    std::string lastFieldName;
    int execDepth = 0;
    int bodyDepth = 0; // script function bodies in flight (also from native calls)

    // GUI parent-child tracking (persists across expressions within a file)
    std::vector<ScriptObject*> guiParentStack;

    // Compile .cs to .dso using the turd compiler (Node.js)
    bool compileToDSO(const std::string& srcContent, const std::string& dsoFullPath) {
        // Look for the compiler script in the data directory
        std::string compilerScript = Console::instance().getStringVariable("compilerScript");
        if (compilerScript.empty()) {
            // Try default locations relative to CWD
            for (auto* p : {"./torque-dso.js", "../torque-dso.js", "data/torque-dso.js"}) {
                struct stat st;
                if (stat(p, &st) == 0) { compilerScript = p; break; }
            }
        }
        // Node.js path
        std::string nodeBin = Console::instance().getStringVariable("nodePath");
        if (nodeBin.empty()) nodeBin = "node";
        struct stat st;
        if (compilerScript.empty() || stat(compilerScript.c_str(), &st) != 0) {
            Console::instance().printf(LogLevel::Debug, "TS: turd compiler not found");
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
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(nodeBin.c_str()));
        argv.push_back(const_cast<char*>(compilerScript.c_str()));
        argv.push_back(const_cast<char*>(tmpPath.c_str()));
        argv.push_back(const_cast<char*>(dsoFullPath.c_str()));
        static std::string target = "Tribes2";
        argv.push_back(const_cast<char*>(target.c_str()));
        argv.push_back(nullptr);
        pid_t pid = 0;
        const int spawnResult = posix_spawnp(&pid, nodeBin.c_str(), nullptr, nullptr,
                                             argv.data(), environ);
        int status = 0;
        const int waitResult = spawnResult == 0 ? waitpid(pid, &status, 0) : -1;
        if (spawnResult != 0 || waitResult < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
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
        const std::string tempPath = dsoPath + ".tmp";
        FILE* f = fopen(tempPath.c_str(), "wb");
        if (!f) return;
        // Write header: version 0x54534F02 ("TSO\1" — Torque Source Object)
        uint32_t version = 0x54534F02;
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
        if (fclose(f) != 0 || rename(tempPath.c_str(), dsoPath.c_str()) != 0)
            unlink(tempPath.c_str());
    }

    void writeBackVar(VMValue val) {
         if (evaluating && !lastFieldObj.empty() && !lastFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
            if (obj) {
                ScriptEngine::instance().setObjectField(obj, lastFieldName, val);
                syncGuiField(lastFieldObj, lastFieldName, val);
            }
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
    VMValue parseDatablock();
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
    VMValue parseConcat();
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

    static bool sourceHasPackage(const std::string& source) {
        size_t pos = 0;
        while ((pos = source.find("package", pos)) != std::string::npos) {
            const bool left = pos == 0 || !std::isalnum((unsigned char)source[pos - 1]);
            const size_t end = pos + 7;
            const bool right = end == source.size() || !std::isalnum((unsigned char)source[end]);
            if (left && right) return true;
            pos = end;
        }
        return false;
    }

    void recordDependency(const std::string& path) {
        if (!loadingStack.empty() && loadingStack.back() != path)
            fileDependencies[loadingStack.back()].insert(path);
    }

    bool readDependencyManifest(const std::string& path, std::set<std::string>& deps) const {
        std::ifstream file(path + ".deps");
        if (!file) return false;
        std::string dep;
        int64_t stamp = 0;
        while (file >> dep >> stamp) {
            if (Engine::instance().fs().fileModifyTime(dep.c_str()) != stamp) return false;
            deps.insert(dep);
        }
        return file.eof();
    }

    void writeDependencyManifest(const std::string& path, const std::set<std::string>& deps) {
        const std::string temp = path + ".deps.tmp";
        std::ofstream file(temp, std::ios::trunc);
        if (!file) return;
        for (const auto& dep : deps)
            file << dep << '\t' << Engine::instance().fs().fileModifyTime(dep.c_str()) << '\n';
        file.close();
        if (!file) { unlink(temp.c_str()); return; }
        if (rename(temp.c_str(), (path + ".deps").c_str()) != 0) unlink(temp.c_str());
    }
};

TorqueScript::TorqueScript() : impl(new Impl) { impl->outer = this; }
TorqueScript::~TorqueScript() { delete impl; }

bool TorqueScript::writeCompileDependencyManifest(const std::string& dsoPath,
                                                  const std::string& sourcePath) {
    if (dsoPath.empty() || sourcePath.empty()) return false;
    std::set<std::string> dependencies{sourcePath};
    impl->writeDependencyManifest(dsoPath, dependencies);
    return std::filesystem::exists(dsoPath + ".deps");
}

void TorqueScript::unloadFile(const std::string& path) {
    for (auto it = impl->functions.begin(); it != impl->functions.end();) {
        if (it->second.filename == path) it = impl->functions.erase(it);
        else ++it;
    }
    for (auto package = impl->packageFunctions.begin(); package != impl->packageFunctions.end();) {
        auto& functions = package->second;
        for (auto it = functions.begin(); it != functions.end();) {
            if (it->second.filename == path) it = functions.erase(it);
            else ++it;
        }
        if (functions.empty()) {
            impl->activePackages.erase(
                std::remove(impl->activePackages.begin(), impl->activePackages.end(), package->first),
                impl->activePackages.end());
            package = impl->packageFunctions.erase(package);
        } else {
            ++package;
        }
    }
}

void TorqueScript::init() {}
void TorqueScript::shutdown() {
    impl->scheduler.clear();
    impl->loadingFiles.clear();
    impl->loadingStack.clear();
    impl->fileDependencies.clear();
    impl->functions.clear();
    impl->packageFunctions.clear();
    clearPackages();
    impl->callPackages.clear();
    impl->parsingPackage.clear();
    impl->globals.clear();
    impl->globalIndex.clear();
    impl->locals = TSLocals{};
    impl->guiParentStack.clear();
    impl->tokens.clear();
    impl->tokenPos = 0;
    impl->currentFile.clear();
    impl->execDepth = 0;
    impl->errorOccurred = false;
    impl->resolveParentNext = false;
}

// Stock TorqueScript flattens indexed globals: $pref::Player[2] and
// $pref::Player2 are THE SAME variable. Old exported prefs files use the
// flattened form, while scripts index with brackets — both must land on
// one canonical key or saved warriors silently vanish.
static std::string normalizeGlobalKey(const std::string& n) {
    if (n.empty() || n[0] != '$') return n;
    auto lb = n.find('[');
    if (lb == std::string::npos) return n;
    auto rb = n.rfind(']');
    if (rb != n.size() - 1 || rb < lb + 1) return n;
    std::string idx = n.substr(lb + 1, rb - lb - 1);
    if (idx.empty()) return n;
    // Canonicalize whitespace inside the index (e.g. "$Skin[0, name]" vs
    // "$Skin[0,name]"): writes assembled keys without whitespace while
    // reads preserved the source's comma-space, so every lookup of a
    // multi-field index missed during first-pass cascades.
    std::string stripped;
    stripped.reserve(idx.size());
    bool changed = false;
    for (char c : idx) {
        if (c == ' ' || c == '\t') { changed = true; continue; }
        stripped += c;
    }
    if (!changed) {
        for (char c : idx)
            if (!isdigit((unsigned char)c)) return n; // non-numeric index: keep verbatim
        return n.substr(0, lb) + idx;
    }
    return n.substr(0, lb) + "[" + stripped + "]";
}

void TorqueScript::setGlobal(const std::string& name, const VMValue& val) {
    const std::string key = normalizeGlobalKey(name);
    const std::string lower = toLower(key);
    auto indexed = impl->globalIndex.find(lower);
    std::string canonicalKey = key;
    if (indexed != impl->globalIndex.end()) {
        canonicalKey = indexed->second;
        impl->globals[canonicalKey] = val;
    } else {
        impl->globalIndex.emplace(lower, key);
        impl->globals[canonicalKey] = val;
    }
    // Keep the console registry on the same canonical key as TorqueScript's
    // case-insensitive global table; exporting prefs reads from this registry.
    Console::instance().setVariable(canonicalKey.c_str(), val.toString().c_str());
}

VMValue TorqueScript::getGlobal(const std::string& name) {
    std::string key = normalizeGlobalKey(name);
    auto indexed = impl->globalIndex.find(toLower(key));
    if (indexed != impl->globalIndex.end()) {
        auto stored = impl->globals.find(indexed->second);
        if (stored != impl->globals.end()) return stored->second;
    }
    auto* item = Console::instance().find(key.c_str());
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

const std::unordered_map<std::string, std::function<VMValue(const std::vector<VMValue>&)>>& TorqueScript::getNatives() const {
    return impl->natives;
}

// === TSLocals ===
void TSLocals::push() { scopes.push_back({}); }
void TSLocals::pop() { if (!scopes.empty()) scopes.pop_back(); }
void TSLocals::set(const std::string& name, const VMValue& val) {
    if (scopes.empty()) return;
    for (auto& [stored, value] : scopes.back()) {
        if (sameName(stored, name)) { value = val; return; }
    }
    scopes.back()[name] = val;
}
// Locals belong to the executing function's frame only; a name it never
// set is empty, not the caller's value.
VMValue TSLocals::get(const std::string& name) {
    if (scopes.empty()) return VMValue();
    for (const auto& [stored, value] : scopes.back())
        if (sameName(stored, name)) return value;
    return VMValue();
}

// === Tokenizer ===
void TorqueScript::Impl::tokenize(const std::string& source) {
    tokens.clear();
    tokenPos = 0;
    dbgSource = source;
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
            // scan.l: 0[xX]{HEXDIGIT}+ is an S32 (Sc_ScanHex: %x); FLOAT
            // allows an exponent.
            if (c == '0' && srcPtr + 2 < srcEnd && (srcPtr[1] == 'x' || srcPtr[1] == 'X') &&
                isxdigit((unsigned char)srcPtr[2])) {
                srcPtr += 2;
                while (srcPtr < srcEnd && isxdigit((unsigned char)*srcPtr)) srcPtr++;
                tok.text = std::string(start, srcPtr - start);
                tok.numVal = (double)(int32_t)(uint32_t)std::strtoul(tok.text.c_str() + 2, nullptr, 16);
                tok.pos.col = srcCol;
                srcCol += (int)(srcPtr - start);
                tokens.push_back(tok);
                continue;
            }
            // FLOAT is {INTEGER}\.{INTEGER}: "3511.text" is an id, then a field.
            while (srcPtr < srcEnd && *srcPtr >= '0' && *srcPtr <= '9') srcPtr++;
            if (srcPtr + 1 < srcEnd && *srcPtr == '.' && srcPtr[1] >= '0' && srcPtr[1] <= '9') {
                ++srcPtr;
                while (srcPtr < srcEnd && *srcPtr >= '0' && *srcPtr <= '9') srcPtr++;
            }
            if (srcPtr < srcEnd && (*srcPtr == 'e' || *srcPtr == 'E')) {
                const char* exp = srcPtr + 1;
                if (exp < srcEnd && (*exp == '+' || *exp == '-')) ++exp;
                if (exp < srcEnd && *exp >= '0' && *exp <= '9') {
                    srcPtr = exp;
                    while (srcPtr < srcEnd && *srcPtr >= '0' && *srcPtr <= '9') srcPtr++;
                }
            }
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
            tok.tagged = quote == '\'';
            tok.pos.col = srcCol;
            srcPtr++; srcCol++;
            std::string val;
            while (srcPtr < srcEnd) {
                if (*srcPtr == quote) { srcPtr++; srcCol++; break; }
                if (*srcPtr == '\\' && srcPtr + 1 < srcEnd) {
                    srcPtr++; srcCol++;
                    // scan.l collapseEscape: \xHH, \c0-\c9 colour codes
                    // (remapped around \t \n \r), \cr/\cp/\co, else charConv.
                    auto hexDigit = [](char h) -> int {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                        return -1;
                    };
                    static const char colourRemap[10] = {0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0xb, 0xc, 0xe};
                    if (*srcPtr == 'x' && srcPtr + 2 < srcEnd &&
                        hexDigit(srcPtr[1]) >= 0 && hexDigit(srcPtr[2]) >= 0) {
                        val += (char)(hexDigit(srcPtr[1]) * 16 + hexDigit(srcPtr[2]));
                        srcPtr += 2; srcCol += 2;
                    } else if (*srcPtr == 'c' && srcPtr + 1 < srcEnd &&
                               (srcPtr[1] == 'r' || srcPtr[1] == 'p' || srcPtr[1] == 'o' ||
                                (srcPtr[1] >= '0' && srcPtr[1] <= '9'))) {
                        const char code = srcPtr[1];
                        val += code == 'r' ? (char)15 : code == 'p' ? (char)16 : code == 'o' ? (char)17
                                                                          : colourRemap[code - '0'];
                        srcPtr++; srcCol++;
                    } else switch (*srcPtr) {
                        case 'n': val += '\n'; break;
                        case 't': val += '\t'; break;
                        case 'r': val += '\r'; break;
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
            else if (tok.text == "datablock") tok.type = TSTokenType::Datablock;
            else if (tok.text == "new") tok.type = TSTokenType::New;
            else if (tok.text == "parent") tok.type = TSTokenType::Parent;
            else if (tok.text == "this") tok.type = TSTokenType::This;
            else if (tok.text == "true") { tok.type = TSTokenType::True; tok.numVal = 1; }
            else if (tok.text == "false") { tok.type = TSTokenType::False; tok.numVal = 0; }
            else if (tok.text == "null") tok.type = TSTokenType::Null;
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
                if (match2('=', TSTokenType::PercentEq)) continue;
                tok = {TSTokenType::Percent, "%", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '=':
                if (match2('=', TSTokenType::EqEq)) continue;
                tok = {TSTokenType::Eq, "=", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '!':
                if (match3('$', '=', TSTokenType::StrNeq)) continue;
                if (match2('=', TSTokenType::Neq)) continue;
                tok = {TSTokenType::Not, "!", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '<':
                if (match3('<', '=', TSTokenType::ShlEq)) continue;
                if (match2('<', TSTokenType::Shl)) continue;
                if (match2('=', TSTokenType::Le)) continue;
                tok = {TSTokenType::Lt, "<", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '>':
                if (match3('>', '=', TSTokenType::ShrEq)) continue;
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
                if (match2('=', TSTokenType::BitAndEq)) continue;
                tok = {TSTokenType::BitwiseAnd, "&", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '|':
                if (match2('|', TSTokenType::Or)) continue;
                if (match2('=', TSTokenType::BitOrEq)) continue;
                tok = {TSTokenType::BitwiseOr, "|", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '?':
                tok = {TSTokenType::Question, "?", 0, tok.pos}; srcPtr++; srcCol++; break;
            case '^':
                if (match2('=', TSTokenType::BitXorEq)) continue;
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
    errorOccurred = true;
    running = false;
    Console::instance().printf(LogLevel::Error, "TS:%s(%d): %s",
        currentFile.c_str(), srcLine, msg.c_str());
    // Include the offending source so parse errors are actionable
    // (e.g. auto-generated command strings with misbalanced parens).
    if (!dbgSource.empty()) {
        const std::string& s = dbgSource;
        size_t lineStart = 0;
        for (int i = 1; i < srcLine; i++) {
            size_t nl = s.find('\n', lineStart);
            if (nl == std::string::npos) break;
            lineStart = nl + 1;
        }
        size_t lineEnd = s.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = s.size();
        std::string snippet = s.substr(lineStart, lineEnd - lineStart);
        if (snippet.size() > 160) snippet = snippet.substr(0, 160) + "...";
        Console::instance().printf(LogLevel::Error, "TS source: %s", snippet.c_str());
    }
}

// === Parser ===
VMValue TorqueScript::Impl::parseProgram() {
    VMValue result;
    int stmtCount = 0;
    const int maxStmts = 100000;
    while (peekToken().type != TSTokenType::Eof && running) {
        result = parseStatement();
        stmtCount++;
        if (stmtCount > maxStmts) {
            Console::instance().printf(LogLevel::Debug, "TS: parseProgram safety break after %d stmts in '%s'",
                stmtCount, currentFile.c_str());
            break;
        }
        if (returning || breaking || continuing) {
            break;
        }
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
        case TSTokenType::Datablock: return parseDatablock();
        case TSTokenType::LBrace: return parseBlock();
        case TSTokenType::Semicolon: nextToken(); return {};
        case TSTokenType::Eof: return {};
        default: return parseExpressionStatement();
    }
}

VMValue TorqueScript::Impl::parseDatablock() {
    nextToken(); // datablock
    TSToken classToken = nextToken();
    if (classToken.type != TSTokenType::Ident)
        return {};
    expect(TSTokenType::LParen);
    TSToken nameToken = nextToken();
    if (nameToken.type != TSTokenType::Ident && nameToken.type != TSTokenType::String)
        return {};
    expect(TSTokenType::RParen);
    std::string parentName;
    if (match(TSTokenType::Colon)) {
        TSToken parentToken = nextToken();
        if (parentToken.type != TSTokenType::Ident && parentToken.type != TSTokenType::String)
            return {};
        parentName = parentToken.text;
    }

    // OP_CREATE_OBJECT: a datablock declared again under its name is the
    // same datablock, its fields set anew (a different class is refused).
    auto& engine = ScriptEngine::instance();
    ScriptObject* existing = engine.findDataBlock(nameToken.text);
    const bool redeclared = existing && strcasecmp(existing->className.c_str(), classToken.text.c_str()) != 0;
    if (redeclared)
        Console::instance().printf(LogLevel::Error, "Cannot re-declare data block %s with a different class.",
                                   nameToken.text.c_str());
    auto* object = new ScriptObject;
    object->className = classToken.text;
    object->name = nameToken.text;
    if (!existing && !parentName.empty()) {
        if (auto* parent = engine.findObject(parentName.c_str()))
            object->fields = parent->fields;
    }

    if (match(TSTokenType::LBrace)) {
        while (peekToken().type != TSTokenType::RBrace &&
               peekToken().type != TSTokenType::Eof && running) {
            if (match(TSTokenType::Semicolon)) continue;
            TSToken field = nextToken();
            if (field.type != TSTokenType::Ident) {
                while (peekToken().type != TSTokenType::Semicolon &&
                       peekToken().type != TSTokenType::RBrace &&
                       peekToken().type != TSTokenType::Eof)
                    nextToken();
                match(TSTokenType::Semicolon);
                continue;
            }
            // Array fields: stateName[0] = ... (the same key form as new {}).
            if (match(TSTokenType::LBracket)) {
                VMValue index = parseExpression();
                while (match(TSTokenType::Comma))
                    index = VMValue(index.toString() + "," + parseExpression().toString());
                expect(TSTokenType::RBracket);
                field.text += "[" + index.toString() + "]";
            }
            if (!match(TSTokenType::Eq)) {
                while (peekToken().type != TSTokenType::Semicolon &&
                       peekToken().type != TSTokenType::RBrace &&
                       peekToken().type != TSTokenType::Eof)
                    nextToken();
                match(TSTokenType::Semicolon);
                continue;
            }
            object->fields[field.text] = parseExpression();
            match(TSTokenType::Semicolon);
        }
        match(TSTokenType::RBrace);
    }

    if (existing) {
        const bool reuse = !redeclared;
        if (reuse) {
            for (auto& [field, value] : object->fields) existing->fields[field] = value;
            engine.dataBlockModified(existing);
        }
        delete object;
        return reuse ? VMValue(existing->id) : VMValue(0);
    }
    // Datablocks take ids from the datablock range.
    engine.assignDatablockId(object);
    engine.addObject(object);
    engine.registerDataBlock(object);
    outer->setGlobal("$" + object->name, VMValue(object->name));
    outer->setGlobal(object->name, VMValue(object->name));
    return VMValue(object->id);
}

VMValue TorqueScript::Impl::parseBlock() {
    expect(TSTokenType::LBrace);
    VMValue result;
    while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof && running) {
        result = parseStatement();
        if (returning || breaking || continuing) {
            // Control flow belongs to the enclosing loop/function.  Do not
            // execute the remaining statements in a braced loop body, but do
            // consume it so the enclosing parser resumes after the brace.
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
    } else if (peekToken().type == TSTokenType::For || peekToken().type == TSTokenType::While ||
               peekToken().type == TSTokenType::Switch || peekToken().type == TSTokenType::SwitchStr) {
        // for/while/switch (...) statement: the header's semicolons belong to
        // the parenthesised group, not the statement.
        nextToken();
        expect(TSTokenType::LParen);
        int parenDepth = 1;
        while (parenDepth > 0 && peekToken().type != TSTokenType::Eof) {
            TSToken t = nextToken();
            if (t.type == TSTokenType::LParen) parenDepth++;
            if (t.type == TSTokenType::RParen) parenDepth--;
        }
        skipStatement();
    } else if (peekToken().type == TSTokenType::Do) {
        // do statement while (...);
        nextToken();
        skipStatement();
        if (peekToken().type == TSTokenType::While) {
            nextToken();
            expect(TSTokenType::LParen);
            int parenDepth = 1;
            while (parenDepth > 0 && peekToken().type != TSTokenType::Eof) {
                TSToken t = nextToken();
                if (t.type == TSTokenType::LParen) parenDepth++;
                if (t.type == TSTokenType::RParen) parenDepth--;
            }
            if (peekToken().type == TSTokenType::Semicolon) nextToken();
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
        // A simple statement ends at its own ';'. Braces, parentheses and
        // brackets inside it (an object body in `x = new C() { f = 1; };`,
        // call arguments) are part of the statement.
        int depth = 0;
        while (peekToken().type != TSTokenType::Eof) {
            const TSTokenType type = peekToken().type;
            if (depth == 0 && (type == TSTokenType::Semicolon || type == TSTokenType::RBrace ||
                               type == TSTokenType::Else))
                break;
            if (type == TSTokenType::LBrace || type == TSTokenType::LParen || type == TSTokenType::LBracket) depth++;
            if (type == TSTokenType::RBrace || type == TSTokenType::RParen || type == TSTokenType::RBracket) depth--;
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
    return result;
}

VMValue TorqueScript::Impl::parseFor() {
    expect(TSTokenType::For);
    expect(TSTokenType::LParen);

    // NOTE: no locals.push() here — TorqueScript has no block scoping; the
    // induction variable lives in the function scope and must survive the
    // loop (scripts read %i after `for` to blank the last shifted slot).

    // Init
    if (peekToken().type != TSTokenType::Semicolon) {
        parseExpression();
    }
    expect(TSTokenType::Semicolon);

    // Condition - save token range
    size_t condStart = tokenPos;
    size_t condEnd = tokenPos;
    int condParenDepth = 0;
    while (peekToken().type != TSTokenType::Eof) {
        if (peekToken().type == TSTokenType::Semicolon && condParenDepth == 0) break;
        if (peekToken().type == TSTokenType::LParen) condParenDepth++;
        if (peekToken().type == TSTokenType::RParen) condParenDepth--;
        nextToken();
    }
    condEnd = tokenPos;
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
    int iterCount = 0;
    const int maxIters = 10000000;
    while (running) {
        if (iterCount++ >= maxIters) {
            Console::instance().printf(LogLevel::Debug, "TS: parseWhile safety break after %d iters in '%s'",
                iterCount, currentFile.c_str());
            break;
        }
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
    return result;
}

VMValue TorqueScript::Impl::parseWhile() {
    expect(TSTokenType::While);
    expect(TSTokenType::LParen);

    // Save condition token range
    size_t condStart = tokenPos;
    int condParenDepth = 0;
    while (peekToken().type != TSTokenType::Eof) {
        if (peekToken().type == TSTokenType::RParen && condParenDepth == 0) break;
        if (peekToken().type == TSTokenType::LParen) condParenDepth++;
        if (peekToken().type == TSTokenType::RParen) condParenDepth--;
        nextToken();
    }
    expect(TSTokenType::RParen);
    size_t bodyStart = tokenPos; // body token range

    loopDepth++;
    VMValue result;
    int iterCount = 0;
    bool bodyExecuted = false;
    const int maxIters = 1000000;
    while (running) {
        // Evaluate condition
        {
            size_t saved = tokenPos;
            tokenPos = condStart;
            VMValue cond = parseExpression();
            tokenPos = saved;
        if (!cond.toBool()) break;
        }

        if (iterCount++ >= maxIters) {
            Console::instance().printf(LogLevel::Debug, "TS: parseWhile safety break after %d iters in '%s'",
                iterCount, currentFile.c_str());
            break;
        }

        tokenPos = bodyStart;
        result = parseStatement();
        bodyExecuted = true;
        if (returning) break;
        if (breaking) { breaking = false; break; }
        if (continuing) { continuing = false; }
    }
    if (!bodyExecuted) {
        tokenPos = bodyStart;
        skipStatement();
    }
    loopDepth--;
    return result;
}

VMValue TorqueScript::Impl::parseDo() {
    expect(TSTokenType::Do);
    loopDepth++;
    VMValue result;

    const size_t bodyStart = tokenPos;
    result = parseStatement();
    const size_t bodyEnd = tokenPos;
    if (returning) { loopDepth--; return result; }
    if (breaking) { breaking = false; loopDepth--; return result; }

    if (match(TSTokenType::While)) {
        expect(TSTokenType::LParen);
        const size_t condStart = tokenPos;
        int condParenDepth = 0;
        while (peekToken().type != TSTokenType::Eof) {
            if (peekToken().type == TSTokenType::RParen && condParenDepth == 0) break;
            if (peekToken().type == TSTokenType::LParen) condParenDepth++;
            if (peekToken().type == TSTokenType::RParen) condParenDepth--;
            nextToken();
        }
        expect(TSTokenType::RParen);
        match(TSTokenType::Semicolon);

        int iterCount = 0;
        const int maxIters = 10000000;
        while (running) {
            size_t saved = tokenPos;
            tokenPos = condStart;
            VMValue cond = parseExpression();
            tokenPos = saved;
            if (!cond.toBool()) break;
            if (iterCount++ >= maxIters) {
                Console::instance().printf(LogLevel::Debug, "TS: parseDo safety break after %d iters in '%s'",
                    iterCount, currentFile.c_str());
                break;
            }
            tokenPos = bodyStart;
            result = parseStatement();
            if (returning) break;
            if (breaking) { breaking = false; break; }
            if (continuing) { continuing = false; }
            tokenPos = bodyEnd;
        }
    }

    loopDepth--;
    return result;
}

VMValue TorqueScript::Impl::parseSwitch() {
    bool isStrSwitch = false;
    if (match(TSTokenType::SwitchStr)) {
        isStrSwitch = true;
    } else if (match(TSTokenType::Switch)) {
        isStrSwitch = false;
    } else {
        error("Expected 'switch' or 'switch$'");
    }
    expect(TSTokenType::LParen);
    VMValue val = parseExpression();
    expect(TSTokenType::RParen);
    expect(TSTokenType::LBrace);

    bool matched = false;
    bool switchBroken = false;
    while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
        if (match(TSTokenType::Case)) {
            VMValue caseVal = parseExpression();
            expect(TSTokenType::Colon);
            bool eq = isStrSwitch
                ? (strcasecmp(val.toString().c_str(), caseVal.toString().c_str()) == 0)
                : (val.toDouble() == caseVal.toDouble());
            if (eq) {
                matched = true;
                while (peekToken().type != TSTokenType::Case && peekToken().type != TSTokenType::Default &&
                       peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    VMValue r = parseStatement();
                    if (returning) break;
                    if (breaking) { switchBroken = true; break; }
                }
            } else {
                while (peekToken().type != TSTokenType::Case && peekToken().type != TSTokenType::Default &&
                       peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    skipStatement();
                }
            }
            breaking = false; // break inside switch exits the switch, not the enclosing loop
            if (switchBroken) {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof)
                    skipStatement();
                break;
            }
        } else if (match(TSTokenType::Default)) {
            expect(TSTokenType::Colon);
            if (!matched && !switchBroken) {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    VMValue r = parseStatement();
                    if (returning || breaking) { switchBroken = breaking; break; }
                }
            } else {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof) {
                    skipStatement();
                }
            }
            breaking = false;
            if (switchBroken) {
                while (peekToken().type != TSTokenType::RBrace && peekToken().type != TSTokenType::Eof)
                    skipStatement();
                break;
            }
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
    // :: is tokenized as TWO Colon tokens; check for consecutive colons
    if (peekToken().type == TSTokenType::Colon && peekToken(1).type == TSTokenType::Colon) {
        nextToken(); nextToken(); // consume both :
        TSToken methodTok = nextToken();
        fullName = nameTok.text + "::" + methodTok.text;
    }

    expect(TSTokenType::LParen);

    TSFunc func;
    while (peekToken().type != TSTokenType::RParen && peekToken().type != TSTokenType::Eof) {
        TSToken paramTok = nextToken();
        if (paramTok.type == TSTokenType::Dollar || paramTok.type == TSTokenType::Ident ||
            paramTok.type == TSTokenType::This || paramTok.type == TSTokenType::Parent) {
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
    if (parsingPackage.empty()) functions[fullName] = func;
    else packageFunctions[toLower(parsingPackage)][fullName] = func;

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
    const std::string previousPackage = parsingPackage;
    parsingPackage = toLower(packageName);
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
    if (depth != 0) {
        error("Unterminated package body");
        parsingPackage = previousPackage;
        return {};
    }
    if (peekToken().type == TSTokenType::RBrace) nextToken();
    match(TSTokenType::Semicolon);
    parsingPackage = previousPackage;

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
         peekToken().type == TSTokenType::SlashEq ||
         peekToken().type == TSTokenType::PercentEq ||
         peekToken().type == TSTokenType::BitAndEq ||
         peekToken().type == TSTokenType::BitOrEq ||
         peekToken().type == TSTokenType::BitXorEq ||
         peekToken().type == TSTokenType::ShlEq ||
         peekToken().type == TSTokenType::ShrEq) {
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

        if (lhs.type == VMValue::None && targetVar.empty() &&
            (targetFieldObj.empty() || targetFieldName.empty())) {
            error("Invalid assignment target");
            return rhs;
        }

        VMValue val = rhs;
        if (op.type == TSTokenType::PlusEq) { val = VMValue(lhs.toDouble() + rhs.toDouble()); }
        else if (op.type == TSTokenType::MinusEq) { val = VMValue(lhs.toDouble() - rhs.toDouble()); }
        else if (op.type == TSTokenType::StarEq) { val = VMValue(lhs.toDouble() * rhs.toDouble()); }
         else if (op.type == TSTokenType::SlashEq) { val = rhs.toDouble() != 0 ? VMValue(lhs.toDouble() / rhs.toDouble()) : VMValue(0); }
         else if (op.type == TSTokenType::PercentEq) { val = rhs.toInt() != 0 ? VMValue(lhs.toInt() % rhs.toInt()) : VMValue(0); }
         else if (op.type == TSTokenType::BitAndEq) { val = VMValue(lhs.toInt() & rhs.toInt()); }
         else if (op.type == TSTokenType::BitOrEq) { val = VMValue((double)(lhs.toInt() | rhs.toInt())); }
         else if (op.type == TSTokenType::BitXorEq) { val = VMValue(lhs.toInt() ^ rhs.toInt()); }
         else if (op.type == TSTokenType::ShlEq) { val = VMValue(lhs.toInt() << rhs.toInt()); }
         else if (op.type == TSTokenType::ShrEq) { val = VMValue(lhs.toInt() >> rhs.toInt()); }

        // Store back: object field, global, or local. A field of a missing
        // object is dropped, as in Torque.
        if (evaluating && !lastFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
            if (obj) {
                ScriptEngine::instance().setObjectField(obj, lastFieldName, val);
                syncGuiField(lastFieldObj, lastFieldName, val);
            }
         } else if (evaluating && !targetFieldObj.empty() && !targetFieldName.empty()) {
            // Fallback: use the saved field target (bracket+field may have lost it)
            auto* obj = ScriptEngine::instance().findObject(targetFieldObj.c_str());
            if (obj) {
                ScriptEngine::instance().setObjectField(obj, targetFieldName, val);
                syncGuiField(targetFieldObj, targetFieldName, val);
            }
         } else if (evaluating && !lastVarName.empty()) {
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
        const bool savedEvaluating = evaluating;
        const bool conditionTrue = savedEvaluating && cond.toBool();
        evaluating = conditionTrue;
        VMValue trueVal = parseExpression();
        expect(TSTokenType::Colon);
        evaluating = savedEvaluating && !cond.toBool();
        VMValue falseVal = parseTernary();
        evaluating = savedEvaluating;
        return cond.toBool() ? trueVal : falseVal;
    }
    return cond;
}

VMValue TorqueScript::Impl::parseLogicalOr() {
    VMValue lhs = parseLogicalAnd();
    // TS also accepts the word forms: "a or b" / "a and b"
    while (peekToken().type == TSTokenType::Or ||
           (peekToken().type == TSTokenType::Ident && peekToken().text == "or")) {
        nextToken();
        const bool evaluateRhs = evaluating && !lhs.toBool();
        const bool savedEvaluating = evaluating;
        evaluating = evaluateRhs;
        VMValue rhs = parseLogicalAnd();
        evaluating = savedEvaluating;
        lhs = VMValue(lhs.toBool() || (evaluateRhs && rhs.toBool()) ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseLogicalAnd() {
    VMValue lhs = parseBitwiseOr();
    while (peekToken().type == TSTokenType::And ||
           (peekToken().type == TSTokenType::Ident && peekToken().text == "and")) {
        nextToken();
        const bool evaluateRhs = evaluating && lhs.toBool();
        const bool savedEvaluating = evaluating;
        evaluating = evaluateRhs;
        VMValue rhs = parseBitwiseOr();
        evaluating = savedEvaluating;
        lhs = VMValue(lhs.toBool() && evaluateRhs && rhs.toBool() ? 1 : 0);
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
    while (peekToken().type == TSTokenType::EqEq || peekToken().type == TSTokenType::Neq) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseRelational();
        const bool eq = lhs.toDouble() == rhs.toDouble();
        lhs = VMValue((op == TSTokenType::EqEq) == eq ? 1 : 0);
    }
    return lhs;
}

VMValue TorqueScript::Impl::parseRelational() {
    VMValue lhs = parseConcat();
    while (peekToken().type == TSTokenType::Lt || peekToken().type == TSTokenType::Gt ||
           peekToken().type == TSTokenType::Le || peekToken().type == TSTokenType::Ge) {
        TSTokenType op = nextToken().type;
        VMValue rhs = parseConcat();
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

// gram.y: '@', the SPC/TAB/NL concatenations and $= / !$= share one level,
// below the shifts and above the relational operators.
VMValue TorqueScript::Impl::parseConcat() {
    VMValue lhs = parseShift();
    while (true) {
        const TSToken& t = peekToken();
        if (t.type == TSTokenType::At) {
            nextToken();
            VMValue rhs = parseShift();
            lhs = VMValue(lhs.toString() + rhs.toString());
            continue;
        }
        if (t.type == TSTokenType::StrEq || t.type == TSTokenType::StrNeq) {
            const TSTokenType op = nextToken().type;
            VMValue rhs = parseShift();
            const bool eq = strcasecmp(lhs.toString().c_str(), rhs.toString().c_str()) == 0;
            lhs = VMValue((op == TSTokenType::StrEq) == eq ? 1 : 0);
            continue;
        }
        if (t.type == TSTokenType::Ident) {
            std::string sep;
            if (t.text == "TAB") sep = "\t";
            else if (t.text == "SPC") sep = " ";
            else if (t.text == "NL") sep = "\n";
            if (!sep.empty()) {
                nextToken();
                VMValue rhs = parseShift();
                lhs = VMValue(lhs.toString() + sep + rhs.toString());
                continue;
            }
        }
        break;
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
        if (peekToken().type == TSTokenType::Plus) {
            nextToken();
            VMValue rhs = parseMultiplicative();
            // '+' is always numeric in TorqueScript ("1" + 1 is 2); '@'
            // concatenates.
            lhs = VMValue(lhs.toDouble() + rhs.toDouble());
            continue;
        }
        if (peekToken().type == TSTokenType::Minus) {
            nextToken();
            VMValue rhs = parseMultiplicative();
            lhs = VMValue(lhs.toDouble() - rhs.toDouble());
            continue;
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
        if (evaluating && !saveFieldObj.empty() && !saveFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(saveFieldObj.c_str());
                    if (obj) ScriptEngine::instance().setObjectField(obj, saveFieldName, newVal);
        } else if (evaluating && !saveVarName.empty()) {
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
        if (evaluating && !saveFieldObj.empty() && !saveFieldName.empty()) {
            auto* obj = ScriptEngine::instance().findObject(saveFieldObj.c_str());
                    if (obj) ScriptEngine::instance().setObjectField(obj, saveFieldName, newVal);
        } else if (evaluating && !saveVarName.empty()) {
            if (saveVarName[0] == '$') outer->setGlobal(saveVarName, newVal);
            else locals.set(saveVarName, newVal);
        }
        return newVal;
    }
    return parsePostfix();
}

// Runtime script writes to named GUI-control objects must reach the live
// control tree, not just the ScriptObject field map — e.g. commonDialogs.cs
// sets MBYesNoButtonYes.command right before the confirm dialog opens.
static void syncGuiField(const std::string& objName, const std::string& field, const VMValue& val) {
    if (objName.empty()) return;
    auto* object = ScriptEngine::instance().findObject(objName.c_str());
    if (!object) return;
    const std::string& className = object->className;
    if (className.find("Gui") != 0 && className.find("Shell") != 0 &&
        className.find("Hud") != 0 && className != "GameTSCtrl" &&
        className != "VirtualScrollCtrl" && className != "VirtualScrollContentCtrl") return;
    GuiControl* ctl = Engine::instance().guiRenderer().findControl(objName);
    if (!ctl) return;
    if (field == "command") {
        ctl->command = val.toString();
        // Click dispatch runs the prebuilt onClick closure, not the live
        // command string — rebuild it so runtime-assigned commands fire.
        if (!ctl->command.empty()) {
            std::string cmd = ctl->command;
            ctl->onClick = [cmd]() { Console::instance().execute(cmd.c_str()); };
        }
    }
    else if (field == "text") ctl->text = val.toString();
    else if (field == "visible") ctl->visible = val.toBool();
    else if (field == "active") ctl->active = val.toBool();
    else if (field == "bitmap") ctl->bitmap = val.toString();
    else if (field == "profile") ctl->profileName = val.toString();
    else if (field == "position") {
        float x = ctl->posX, y = ctl->posY;
        sscanf(val.toString().c_str(), "%f %f", &x, &y);
        ctl->posX = x;
        ctl->posY = y;
    }
    else if (field == "extent") {
        float w = ctl->extentX, h = ctl->extentY;
        sscanf(val.toString().c_str(), "%f %f", &w, &h);
        ctl->extentX = w;
        ctl->extentY = h;
        if (auto* ts = Engine::instance().script().ts()) {
            const std::string callback = ctl->name + "::onResize";
            if (ts->hasFunction(callback))
                ts->callFunction(callback, {VMValue(ctl->name), VMValue(w), VMValue(h)});
        }
    }
    else if (field == "value") {
        ctl->hudValue = (float)val.toDouble();
        ctl->hudValueSet = true;
        ctl->fields["value"] = val.toString();
    }
    ctl->fields[field] = val.toString();
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

            break;
        }
        if (peekToken().type == TSTokenType::LBracket) {
            std::string varBeforeIndex = lastVarName;
            std::string savedFieldObj = lastFieldObj;
            std::string savedFieldName = lastFieldName;
            nextToken();
            VMValue idx = parseExpression();
            lastFieldObj = savedFieldObj;
            lastFieldName = savedFieldName;
            std::string savedVar = varBeforeIndex;
            while (match(TSTokenType::Comma)) {
                std::string savedFieldObj2 = lastFieldObj;
                std::string savedFieldName2 = lastFieldName;
                // Bare identifiers in multi-part indices are string literals
                // (TS semantics: $Skin[0, name] indexes by the TEXT "name").
                // Routing them through parseExpression() let contaminated
                // locals frames resolve them to garbage/empty values.
                // A bare word is its text; "Game.teamCount[%t]" is not bare.
                VMValue idx2;
                if (peekToken().type == TSTokenType::Ident &&
                    (peekToken(1).type == TSTokenType::Comma || peekToken(1).type == TSTokenType::RBracket))
                    idx2 = VMValue(nextToken().text);
                else
                    idx2 = parseExpression();
                lastFieldObj = savedFieldObj2;
                lastFieldName = savedFieldName2;
                idx = VMValue(idx.toString() + "," + idx2.toString());
            }
            expect(TSTokenType::RBracket);
            if (!savedVar.empty()) {
                std::string arrayKey = savedVar + "[" + idx.toString() + "]";
                lastVarName = arrayKey;
                if (!lastFieldName.empty()) {
                    // obj.field[idx] — qualify the field name. The object may
                    // not exist; the access then reads "" and writes nothing.
                    lastFieldName = lastFieldName + "[" + idx.toString() + "]";
                } else {
                    // Pure indexed variable ($g[idx] / %v[idx]): drop any STALE
                    // field target left by an earlier obj.field access, or the
                    // upcoming assignment stores into that random object field
                    // instead of the global (broke doDeleteWarrior's shift).
                    lastFieldObj.clear();
                    lastFieldName.clear();
                }
                if (!lastFieldName.empty()) {
                    // obj.field[idx] — read from ScriptObject field
                    auto* sobj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
                    if (sobj) {
                        auto* field = findField(sobj, lastFieldName);
                        val = field ? *field : VMValue("");
                    } else {
                        val = VMValue("");
                    }
                } else if (savedVar[0] == '$') {
                    val = outer->getGlobal(arrayKey);
                } else {
                    val = locals.get(arrayKey);
                }
                if (peekToken().type == TSTokenType::PlusPlus) {
                    nextToken();
                    VMValue newVal(val.toDouble() + 1);
                    if (evaluating && !lastFieldObj.empty() && !lastFieldName.empty()) {
                        if (auto* object = ScriptEngine::instance().findObject(lastFieldObj.c_str()))
                            ScriptEngine::instance().setObjectField(object, lastFieldName, newVal);
                    } else if (evaluating && lastVarName[0] == '$') outer->setGlobal(arrayKey, newVal);
                    else if (evaluating) locals.set(arrayKey, newVal);
                    val = newVal;
                    break;
                }
                if (peekToken().type == TSTokenType::MinusMinus) {
                    nextToken();
                    VMValue newVal(val.toDouble() - 1);
                    if (evaluating && !lastFieldObj.empty() && !lastFieldName.empty()) {
                        if (auto* object = ScriptEngine::instance().findObject(lastFieldObj.c_str()))
                            ScriptEngine::instance().setObjectField(object, lastFieldName, newVal);
                    } else if (evaluating && lastVarName[0] == '$') outer->setGlobal(arrayKey, newVal);
                    else if (evaluating) locals.set(arrayKey, newVal);
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
                if (!evaluating) {
                    val = VMValue(0);
                    continue;
                }

                // Look up method: objClass::methodName
                std::string objName = val.toString();
                std::string methodName = member.text;
                bool called = false;
                // Method calls pass the object as the first argument (self)
                std::vector<VMValue> methodArgs;
                methodArgs.push_back(VMValue(objName));
                methodArgs.insert(methodArgs.end(), args.begin(), args.end());
                // Resolve a method against its native name before falling
                // back to an unqualified script helper.
                std::string fullName = methodName;
                auto* sobj = ScriptEngine::instance().findObject(objName.c_str());
                std::string className = sobj ? sobj->className : "";
                if (className.empty() && (objName == "moveMap" ||
                                           objName == "GlobalActionMap" ||
                                           objName == "observerMap"))
                    className = "ActionMap";
                std::string bareLower = fullName;
                for (auto& c : bareLower) c = (char)tolower((unsigned char)c);
                // Namespace::lookup along the object's linked namespaces:
                // at each level a script function, else the console method.
                std::vector<std::string> spaces = sobj
                    ? ScriptEngine::instance().objectNamespaces(sobj)
                    : std::vector<std::string>{objName};
                if (!sobj && !className.empty()) spaces.push_back(className);
                for (const std::string& space : spaces) {
                    if (called) break;
                    const std::string spaceFull = space + "::" + methodName;
                    if (outer->hasFunction(spaceFull)) {
                        val = outer->callFunction(spaceFull, methodArgs);
                        called = true;
                        break;
                    }
                    auto nit = natives.find(toLower(spaceFull));
                    if (nit != natives.end()) {
                        val = nit->second(methodArgs);
                        called = true;
                    }
                }
                if (!called) {
                    auto nit = natives.find(bareLower);
                    if (nit != natives.end()) { val = nit->second(methodArgs); called = true; }
                }
                if (!called) {
                    // Fall back to a bare script helper only after object and
                    // class dispatch have failed, and never back into the
                    // function making the call (hud.cs addLine forwards to
                    // $Hud[%tag].addLine).
                    const bool reentry = !callNames.empty() && sameName(callNames.back(), fullName);
                    if (!reentry && outer->hasFunction(fullName)) {
                        val = outer->callFunction(fullName, methodArgs);
                        called = true;
                    }
                }
                if (!called) {
                    // compiledEval.cc: an unresolved method warns and yields "".
                    const int line = tokenPos > 0 && tokenPos <= tokens.size()
                        ? tokens[tokenPos - 1].pos.line : 0;
                    if (!sobj && ScriptEngine::instance().missionObjects().empty())
                        Console::instance().printf(LogLevel::Warn,
                            "%s (%d): Unable to find object: '%s' attempting to call function '%s'",
                            currentFile.c_str(), line, objName.c_str(), methodName.c_str());
                    else if (sobj) {
                        std::string list;
                        for (const auto& space : spaces) list += (list.empty() ? "" : " -> ") + space;
                        Console::instance().printf(LogLevel::Warn, "%s (%d): Unknown command %s.",
                                                   currentFile.c_str(), line, methodName.c_str());
                        Console::instance().printf(LogLevel::Warn, "  Object %s(%d) %s",
                                                   sobj->name.c_str(), ScriptEngine::instance().objectId(sobj),
                                                   list.c_str());
                    }
                    val = VMValue("");
                }
            } else {
                // Field access
                lastFieldObj = val.toString();
                lastFieldName = member.text;
                auto* obj = ScriptEngine::instance().findObject(lastFieldObj.c_str());
                std::string live;
                auto* scene = obj ? dynamic_cast<SceneObject*>(obj->engine.get()) : nullptr;
                if (scene && scene->liveField(lastFieldName, live)) {
                    val = VMValue(live);
                } else if (obj) {
                    auto* field = findField(obj, lastFieldName);
                    if (field) val = *field;
                    else val = VMValue("");
                } else {
                    val = VMValue("");
                }
            }
            continue;
        }
        if (peekToken().type == TSTokenType::PlusPlus) {
            nextToken();
            if (evaluating) writeBackVar(VMValue(val.toDouble() + 1));
            val = VMValue(val.toDouble() + 1);
            break;
        }
        if (peekToken().type == TSTokenType::MinusMinus) {
            nextToken();
            if (evaluating) writeBackVar(VMValue(val.toDouble() - 1));
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
            // OP_TAG_TO_STR: a 'literal' is its tag, "\x01<id>".
            return tok.tagged ? VMValue(NetStrings::literal(tok.text)) : VMValue(tok.text);

        case TSTokenType::True:
            return VMValue(1);

        case TSTokenType::False:
            return VMValue(0);

        case TSTokenType::Null:
            return VMValue("");

        case TSTokenType::Dollar: {
            TSToken nameTok = nextToken();
            lastVarName = "$" + nameTok.text;
            // A new variable is not the target of an earlier obj.field in
            // the same expression; a stale target made "$a[%i]" read a field.
            lastFieldObj.clear();
            lastFieldName.clear();

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
                if (!evaluating) return VMValue(0);

                // A script definition replaces a console function of the
                // same name (Namespace::addFunction), so scripts come first.
                if (outer->hasFunction(lastVarName)) {
                    return outer->callFunction(lastVarName, args);
                }
                auto& engine = ScriptEngine::instance();
                VMValue vmResult;
                if (engine.vm() && engine.vm()->callScriptFunction(lastVarName.c_str(), args, vmResult))
                    return vmResult;
                // Keep the complete spelling for namespaced natives.  Looking
                // up only nameTok made $Foo::bar() resolve as $Foo().
                auto nit = natives.find(toLower(lastVarName));
                if (nit != natives.end()) {
                    return nit->second(args);
                }

                Console::instance().printf(LogLevel::Warn, "TS: unknown function '%s'", lastVarName.c_str());
                return VMValue(0);
            }

            return outer->getGlobal(lastVarName);
        }

        case TSTokenType::Percent: {
            TSToken nameTok = nextToken();
            lastVarName = nameTok.text;
            lastFieldObj.clear();
            lastFieldName.clear();
            // Function call: %name(args) - treat as function call
            if (peekToken().type == TSTokenType::LParen) {
                nextToken();
                std::vector<VMValue> args;
                parseArgumentList(args);
                expect(TSTokenType::RParen);
                if (!evaluating) return VMValue(0);

                if (outer->hasFunction(nameTok.text)) return outer->callFunction(nameTok.text, args);

                auto& engine = ScriptEngine::instance();
                VMValue vmResult;
                if (engine.vm() && engine.vm()->callScriptFunction(nameTok.text.c_str(), args, vmResult))
                    return vmResult;

                auto nit = natives.find(toLower(nameTok.text));
                if (nit != natives.end()) return nit->second(args);

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
                if (!evaluating) return VMValue(0);
                // exec uses the same mounted filesystem and precedence as every
                // other script load. Hard-coding modPath/base here made an
                // include change behavior when the active mode changed.
                if (fn == "exec" && !args.empty()) {
                    std::string execPath = args[0].toString();
                    const std::string normalized = normalizedScriptPath(execPath);
                    // Saved preferences are optional per-user state. Retail
                    // scripts sometimes exec a prefs/*.cs file without the
                    // optional flag even though the file is created only
                    // after the user saves that configuration.
                    const bool preferenceFile = normalized.size() >= 6 &&
                        strncasecmp(normalized.c_str(), "prefs/", 6) == 0;
                    const bool startupOverride = strcasecmp(normalized.c_str(), "autoexec.cs") == 0 ||
                        strcasecmp(normalized.c_str(), "autojournal.cs") == 0;
                    const bool optional = preferenceFile || startupOverride ||
                        (args.size() > 1 && args[1].toBool());
                    if (!TorchPath::isSafeLogicalPath(normalized.c_str())) {
                        Console::instance().printf(LogLevel::Error,
                            "TS: rejecting unsafe exec path '%s'", execPath.c_str());
                        return VMValue(0);
                    }
                    if (loadingFiles.count(normalized)) {
                        Console::instance().printf(LogLevel::Debug,
                            "TS: skipping circular exec '%s'", normalized.c_str());
                        return VMValue(1);
                    }
                    if (Engine::instance().fs().fileExists(normalized.c_str()))
                        return outer->executeFile(normalized);
                    const std::string function = callNames.empty()
                        ? std::string() : " [" + callNames.back() + "]";
                    const std::string location = "TS:" +
                        (currentFile.empty() ? std::string("<runtime>") : currentFile) +
                        (function.empty() ? ":" + std::to_string(srcLine) : function);
                    Console::instance().printf(optional ? LogLevel::Debug : LogLevel::Error,
                        "%s: %s exec file not found: %s", location.c_str(),
                        optional ? "optional" : "required", execPath.c_str());
                    return VMValue(optional ? 1 : 0);
                }
                // Script functions (packages included) replace console
                // functions of the same name; Parent:: from a package reaches
                // the native through callFunction.
                if (outer->hasFunction(fn)) return outer->callFunction(fn, args);
                auto& engine = ScriptEngine::instance();
                VMValue vmResult;
                if (engine.vm() && engine.vm()->callScriptFunction(fn.c_str(), args, vmResult))
                    return vmResult;
                auto nit = natives.find(toLower(fn));
                if (nit != natives.end()) return nit->second(args);
                // Check console commands
                auto* item = Console::instance().find(fn.c_str());
                if (item && item->type == Console::ConsoleItem::Command) {
                    // Two-pass marshalling: c_str() during insertion dangles
                    // earlier entries when argStorage reallocates.
                    std::vector<std::string> argStorage;
                    argStorage.reserve(args.size());
                    for (auto& a : args) argStorage.push_back(a.toString());
                    std::vector<const char*> argv;
                    argv.reserve(args.size() + 1);
                    argv.push_back(fn.c_str());
                    for (auto& s : argStorage) argv.push_back(s.c_str());
                    item->cmd((int32_t)argv.size(), argv.data());
                    return VMValue(1);
                }
                Console::instance().printf(LogLevel::Warn, "TS: unknown function '%s'", fn.c_str());
                return VMValue(0);
            };

            // Check for namespace::method syntax (:: is two Colon tokens)
            if (peekToken().type == TSTokenType::Colon && peekToken(1).type == TSTokenType::Colon) {
                nextToken(); // consume colon #1
                nextToken(); // consume colon #2 — was missing, so methodName
                             // became ':' and every Class::method(args)
                             // call derailed into the no-paren return
                std::string methodName = nextToken().text;
                std::string fullName = name + "::" + methodName;

                if (peekToken().type == TSTokenType::LParen) {
                    nextToken();
                    std::vector<VMValue> args;
                    parseArgumentList(args);
                    expect(TSTokenType::RParen);

                    // Parent:: first reaches the definition the current package
                    // function overrides (a lower package, then the base
                    // definition); failing that, the next namespace linked
                    // above the current function's (Namespace::mParent).
                    if (name == "Parent") {
                        if (!evaluating) return VMValue(0);
                        const std::string current = callNames.empty() ? std::string() : callNames.back();
                        const std::string currentPackage = callPackages.empty() ? std::string() : callPackages.back();
                        const size_t sep = current.rfind("::");
                        const std::string space = sep == std::string::npos ? std::string() : current.substr(0, sep);
                        const std::string sameName = space.empty() ? methodName : space + "::" + methodName;
                        if (!currentPackage.empty() && packageParentExists(sameName, currentPackage)) {
                            resolveParentNext = true;
                            return outer->callFunction(sameName, args);
                        }
                        if (space.empty()) {
                            auto nit = natives.find(toLower(methodName));
                            if (nit != natives.end()) return nit->second(args);
                            Console::instance().printf(LogLevel::Warn, "TS: no parent for '%s'", methodName.c_str());
                            return VMValue("");
                        }
                        ScriptObject* self = args.empty() ? nullptr
                            : ScriptEngine::instance().findObject(args[0].toString().c_str());
                        std::vector<std::string> spaces = self
                            ? ScriptEngine::instance().objectNamespaces(self)
                            : EngineClasses::chain(space);
                        bool past = false;
                        for (const auto& candidate : spaces) {
                            if (!past) { past = strcasecmp(candidate.c_str(), space.c_str()) == 0; continue; }
                            const std::string full = candidate + "::" + methodName;
                            if (outer->hasFunction(full)) return outer->callFunction(full, args);
                            auto nit = natives.find(toLower(full));
                            if (nit != natives.end()) return nit->second(args);
                        }
                        // Torch's flat console methods stand for the engine
                        // classes' own.
                        auto nit = natives.find(toLower(methodName));
                        if (nit != natives.end()) return nit->second(args);
                        return VMValue("");
                    }
                    if (outer->hasFunction(fullName)) return outer->callFunction(fullName, args);
                    auto nativeIt = natives.find(toLower(fullName));
                    if (nativeIt != natives.end()) return nativeIt->second(args);
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

            // Variable reference: check locals then globals
            {
                lastVarName = name;
                lastFieldObj.clear();
                lastFieldName.clear();
                VMValue lv = locals.get(name);
                if (lv.type != VMValue::None) return lv;
                if (auto indexed = globalIndex.find(toLower(name)); indexed != globalIndex.end()) {
                    auto stored = globals.find(indexed->second);
                    if (stored != globals.end()) return stored->second;
                }
                // Undefined $ or % variable returns 0; bare name returns itself as string.
                if (!name.empty() && name[0] != '$' && name[0] != '%')
                    return VMValue(name);
                return VMValue(0);
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
            // new ObjectType(name, ...) { fields }; gram.y class_name_expr is
            // also '(' expr ')' (new (%data.projectileType)()).
            TSToken className;
            if (match(TSTokenType::LParen)) {
                className = TSToken{TSTokenType::Ident, parseExpression().toString(), 0, tok.pos};
                expect(TSTokenType::RParen);
            } else {
                className = nextToken();
            }
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
                                       obj->className.find("Hud") == 0 || obj->className == "GameTSCtrl" ||
                                       obj->className == "VirtualScrollCtrl" || obj->className == "VirtualScrollContentCtrl")) {
                static uint32_t guiCounter = 0;
                obj->name = "_unnamed" + std::to_string(guiCounter++);
            }

            // Track parent-child for GUI controls (Gui*, Shell*, GameTSCtrl)
            bool isGuiControl = (obj->className.find("Gui") == 0) ||
                                (obj->className.find("Shell") == 0) ||
                                obj->className == "GameTSCtrl" ||
                                obj->className.find("Hud") == 0 ||
                                obj->className == "VirtualScrollCtrl" ||
                                obj->className == "VirtualScrollContentCtrl";
            // Any SimSet (Path, AIObjectiveQ, ...) holds the objects declared in it.
            bool isContainer = isGuiControl || EngineClasses::isA(obj->className, "SimSet");

            if (peekToken().type == TSTokenType::LBrace) {
                nextToken();
                // Record this object as parent for nested new expressions
                if (isContainer) guiParentStack.push_back(obj);
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
                    std::string fieldKey = fieldName.text;
                    if (match(TSTokenType::LBracket)) {
                        VMValue index = parseExpression();
                        while (match(TSTokenType::Comma)) {
                            VMValue nextIndex = parseExpression();
                            index = VMValue(index.toString() + "," + nextIndex.toString());
                        }
                        expect(TSTokenType::RBracket);
                        fieldKey += "[" + index.toString() + "]";
                    }
                    if (peekToken().type == TSTokenType::Eq) {
                        nextToken();
                        VMValue fieldVal = parseExpression();
                        ScriptEngine::instance().setObjectField(obj, fieldKey, fieldVal);
                    } else {
                        // Could be a method call or other expression
                        // Re-tokenize? No, just consume until semicolon
                        while (peekToken().type != TSTokenType::Semicolon && peekToken().type != TSTokenType::Eof
                               && peekToken().type != TSTokenType::RBrace)
                            nextToken();
                    }
                    while (peekToken().type == TSTokenType::Semicolon) nextToken();
                }
                if (isContainer && !guiParentStack.empty()) guiParentStack.pop_back();
                if (peekToken().type == TSTokenType::RBrace) nextToken();
            }

            // OP_ADD_OBJECT: a nested object joins the enclosing SimGroup/SimSet
            // (GUI controls link to their parent control); a top-level one
            // joins the group named by $instantGroup.
            auto& engine = ScriptEngine::instance();
            engine.addObject(obj);
            ScriptObject* enclosing = guiParentStack.empty() ? nullptr : guiParentStack.back();
            if (enclosing == obj) enclosing = nullptr;
            if (enclosing && engine.isSimSet(enclosing)) {
                engine.addToSet(enclosing, obj);
                // GuiControl is a SimGroup; the GUI renderer also follows the
                // declared parent link.
                if (EngineClasses::isA(enclosing->className, "GuiControl") ||
                    !EngineClasses::isEngineClass(enclosing->className))
                    obj->internals["parent"] = VMValue(engine.nameOrId(engine.objectKey(enclosing)));
            } else if (enclosing) {
                obj->internals["parent"] = VMValue(engine.nameOrId(engine.objectKey(enclosing)));
            } else if (ScriptObject* instant = engine.findObject(outer->getGlobal("$instantGroup").toString().c_str());
                       instant && engine.isSimGroup(instant)) {
                engine.addToSet(instant, obj);
            }

            // Set globals so script can reference the object by name (both $name and bare name)
            if (!obj->name.empty()) {
                outer->setGlobal("$" + obj->name, VMValue(obj->name));
                globals[obj->name] = VMValue(obj->name);
                globalIndex.emplace(toLower(obj->name), obj->name);
            }
            engine.objectAdded(obj);
            // new returns the SimObject id.
            return VMValue(engine.objectId(obj));
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
// Prefs files are EXPORT-ONLY artifacts: they must always run as fresh
// source and must never be DSO-cached — a cached snapshot would freeze
// warrior aliases (and everything else $pref::*) at cache time.
static bool isPrefsScript(const std::string& path) {
    std::string low;
    low.reserve(path.size());
    for (char c : path) low += (char)tolower((unsigned char)c);
    return low.find("clientprefs.cs") != std::string::npos ||
           low.find("serverprefs.cs") != std::string::npos;
}

// CodeBlock::exec: an exec'd file runs in a frame of its own (setFrame -1);
// eval and console lines (Con::evaluate, setFrame 0) run in the current
// frame, and get one when no function is executing.
namespace {
struct ExecFrame {
    TSLocals& locals;
    bool pushed = false;
    ExecFrame(TSLocals& locals, const std::string& filename) : locals(locals) {
        const bool evaluate = filename.empty() || filename == "eval" || filename == "console";
        if (!evaluate || locals.depth() == 0) { locals.push(); pushed = true; }
    }
    ~ExecFrame() { if (pushed) locals.pop(); }
};
}

VMValue TorqueScript::executeNested(const std::string& source, const std::string& path) {
    ExecFrame frame(impl->locals, path);
    Console::instance().printf(LogLevel::Debug, "TS: nested enter '%s'", path.c_str());
    // Save outer state
    struct StateGuard {
        Impl* impl;
        std::string savedFile;
        std::vector<TSToken> savedTokens;
        size_t savedPos;
        bool savedRunning;
        VMValue savedReturnValue;
        std::string savedLastVarName;
        std::string savedLastFieldObj;
        std::string savedLastFieldName;
        int savedDepth;
        int savedSrcLine;
         bool savedErrorOccurred;
         bool savedResolveParentNext;
         bool restored = false;
        ~StateGuard() {
            if (restored) return;
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
            impl->errorOccurred = savedErrorOccurred;
            impl->resolveParentNext = savedResolveParentNext;
        }
    };
    StateGuard sg{
        impl,
        std::move(impl->currentFile),
        std::move(impl->tokens),
        impl->tokenPos,
        impl->running,
        impl->returnValue,
        std::move(impl->lastVarName),
        std::move(impl->lastFieldObj),
        std::move(impl->lastFieldName),
        impl->execDepth,
        impl->srcLine,
        impl->errorOccurred,
        impl->resolveParentNext
    };

    // Try loading DSO cache before parsing source (.cs, .gui, .mis)
    // Only cache .cs/.mis files — .gui files have no functions
    if (false && isCompilableExt(path) && !isPrefsScript(path) && path.substr(path.size()-3) != ".gui") {
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
                    if (version == 0x54534F02) {
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
                                        // DSO argNames don't include implicit 'this' for namespaced functions
                                        stub.params = fn.argNames;
                                        if (!fn.ns.empty() && (stub.params.empty() || stub.params[0] != "this"))
                                            stub.params.insert(stub.params.begin(), "this");
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
                            sg.restored = true;
                            impl->execDepth = sg.savedDepth;
                            impl->srcLine = sg.savedSrcLine;
                            impl->currentFile = std::move(sg.savedFile);
                            impl->tokens = std::move(sg.savedTokens);
                            impl->tokenPos = sg.savedPos;
                            impl->running = sg.savedRunning;
                            impl->returning = false;
                            impl->breaking = false;
                            impl->continuing = false;
                            impl->returnValue = sg.savedReturnValue;
                            impl->lastVarName = std::move(sg.savedLastVarName);
                            impl->lastFieldObj = std::move(sg.savedLastFieldObj);
                            impl->lastFieldName = std::move(sg.savedLastFieldName);
                            // NOTE: no locals.pop() here — executeNested never
                            // pushed a locals frame; popping one corrupted the
                            // shared locals stack for every later call.
                            return dsoResult;
                        }
                    }
                }
            }
        }
    }

    // Execute inner
    impl->execDepth = sg.savedDepth + 1;
    impl->currentFile = path;
    impl->running = true;
    impl->returning = false;
    impl->breaking = false;
    impl->continuing = false;
    impl->lastVarName.clear();
    impl->lastFieldObj.clear();
    impl->lastFieldName.clear();
    impl->errorOccurred = false;
    impl->resolveParentNext = false;
    impl->tokenize(source);
    VMValue result = impl->parseProgram();
    if (impl->returning) {
        result = impl->returnValue;
        // Don't propagate returning/breaking/continuing to outer context
    }

    // Write source-cache DSO for .cs/.mis files only (.gui files have no functions to cache)
    if (false && isCompilableExt(path) && !isPrefsScript(path) && path.find('/') != std::string::npos && path.substr(path.size()-3) != ".gui") {
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (!outDir.empty()) {
            std::string dsoRelPath = modPath + "/" + path + ".dso";
            std::string dsoFullPath = outDir + "/" + dsoRelPath;
            impl->writeDSOCache(dsoFullPath, path, source);
        }
    }

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
        if (!gui.activeKeyCapture() && Engine::instance().toggleConsoleKeyEdge()) {
            if (gui.isDialogActive("ConsoleDlg")) gui.popDialog("ConsoleDlg");
            else gui.pushDialog("ConsoleDlg");
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
        // A bare identifier that is the whole argument is a string literal
        // (schedule(100, 0, checkGGIntroDone) passes "checkGGIntroDone"); one
        // inside an expression is a constant operand (isObject(a/b) divides).
        auto isBareIdent = [&]() {
            return peekToken().type == TSTokenType::Ident &&
                   (peekToken(1).type == TSTokenType::Comma || peekToken(1).type == TSTokenType::RParen);
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
    // Reentrancy guard: if an execution is already in flight (e.g. a click
    // handler or schedule callback firing mid-parse), delegating to the
    // state-guarded nested executor prevents tokenize()/parseProgram() from
    // clobbering the live token stream — which corrupted native-call args
    // (garbage strlen/getSubStr inputs) and silently truncated evaluation.
    if (impl->execDepth > 0 || impl->bodyDepth > 0)
        return executeNested(source, filename.empty() ? "console" : filename);
    // Track depth for THIS execution too, so any reentrant execute() while
    // this parse is in flight is routed through the guarded nested path.
    ExecFrame frame(impl->locals, filename);
    impl->execDepth++;
    impl->currentFile = filename;
    impl->running = true;
    impl->returning = false;
    impl->breaking = false;
    impl->continuing = false;
    impl->lastVarName.clear();
    impl->errorOccurred = false;
    impl->resolveParentNext = false;
    impl->tokenize(source);
    VMValue r = impl->parseProgram();
    impl->execDepth--;
    return r;
}

VMValue TorqueScript::executeFile(const std::string& path) {
    const std::string normalizedPath = normalizedScriptPath(path);
    if (!TorchPath::isSafeLogicalPath(normalizedPath.c_str())) {
        Console::instance().printf(LogLevel::Error, "TS: rejecting unsafe script path '%s'", path.c_str());
        return {};
    }
    const std::string& scriptPath = normalizedPath;
    impl->recordDependency(scriptPath);
    // Prevent circular includes
    if (impl->loadingFiles.count(scriptPath)) {
        Console::instance().printf(LogLevel::Debug, "TS: skipping already-loading file '%s'", scriptPath.c_str());
        return {};
    }
    impl->loadingFiles.insert(scriptPath);
    impl->loadingStack.push_back(scriptPath);
    struct LoadGuard {
        Impl* impl;
        std::string path;
        ~LoadGuard() {
            impl->loadingFiles.erase(path);
            if (!impl->loadingStack.empty() && impl->loadingStack.back() == path)
                impl->loadingStack.pop_back();
            else
                impl->loadingStack.erase(std::remove(impl->loadingStack.begin(), impl->loadingStack.end(), path),
                                         impl->loadingStack.end());
            if (!impl->loadingStack.empty()) {
                auto& parent = impl->fileDependencies[impl->loadingStack.back()];
                parent.insert(path);
                const auto it = impl->fileDependencies.find(path);
                if (it != impl->fileDependencies.end())
                    parent.insert(it->second.begin(), it->second.end());
            }
        }
    } loadGuard{impl, scriptPath};
    // A failed reload must not retain dependencies from an earlier version.
    impl->fileDependencies[scriptPath].clear();
    const std::string dsoPath = scriptPath + ".dso";
    // Replace all definitions and DSO code owned by this logical source. This
    // prevents old package entries and function pointers surviving a reload.
    unloadFile(scriptPath);
    if (auto* engine = ScriptEngine::exists() ? &ScriptEngine::instance() : nullptr;
        engine && engine->vm())
        engine->vm()->unloadScript(dsoPath.c_str());

    // For modpath .cs/.mis files, try .cs.dso first (.gui files have no functions)
    // Root-level files (no '/') like console_start.cs skip DSO
    if (isCompilableExt(scriptPath) && !isPrefsScript(scriptPath) && scriptPath.find('/') != std::string::npos && scriptPath.substr(scriptPath.size()-3) != ".gui") {
        const std::string outDir = Console::instance().getStringVariable("outputDir", "");
        const std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string configuredDsoPath;
        if (!outDir.empty())
            TorchPath::safeOutputPath(outDir.c_str(), (modPath + "/" + dsoPath).c_str(), configuredDsoPath);
        const int64_t sourceTime = Engine::instance().fs().fileModifyTime(scriptPath.c_str());
        std::vector<uint8_t> dsoData;
        if (!configuredDsoPath.empty()) {
            std::ifstream f(configuredDsoPath, std::ios::binary);
            if (f) dsoData = std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), {});
        }
        // An archive DSO is a valid cache only when the source itself came
        // from an archive. A loose source must not be paired with an older
        // archive cache just because its output cache was invalidated.
        if (dsoData.empty() && sourceTime <= 0) {
            dsoData = Engine::instance().fs().read(dsoPath.c_str());
        }
        // A cache from an older loose source must never shadow the source.
        // Archive-backed assets have no meaningful host mtime and retain the
        // normal cache behavior.
        std::set<std::string> cachedDependencies;
        if (!dsoData.empty() && !configuredDsoPath.empty()) {
            std::error_code timeError;
            const auto cacheStamp = std::filesystem::last_write_time(configuredDsoPath, timeError);
            // Archive files have no host mtime. Do not let an output cache
            // from an old loose install shadow a script selected from an
            // archive (or from a different mode).
            if (sourceTime <= 0 || timeError) {
                dsoData.clear();
            } else {
                const auto systemStamp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                    cacheStamp - std::filesystem::file_time_type::clock::now() +
                    std::chrono::system_clock::now());
                const auto cacheSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                    systemStamp.time_since_epoch()).count();
                if (cacheSeconds < sourceTime) dsoData.clear();
            }
            if (!dsoData.empty() && !impl->readDependencyManifest(configuredDsoPath, cachedDependencies))
                dsoData.clear();
        }
        // Torch's source cache (v0x54534F02) holds only function bodies, not
        // the file's top-level statements; running it for a repeated exec
        // skipped datablocks and objects. exec runs the whole file, as a
        // compiled Torque DSO does, so the source is executed instead.
        if (dsoData.size() >= 4) {
            size_t versionOffset = 0;
            uint32_t cacheVersion = 0;
            if (readU32Bounded(dsoData, versionOffset, cacheVersion) && cacheVersion == 0x54534F02)
                dsoData.clear();
        }
        if (!dsoData.empty()) {
            // Check for custom source DSO cache (v0x54534F02)
            bool sourceCache = false;
            if (dsoData.size() >= 4) {
                size_t offset = 0;
                uint32_t version = 0;
                readU32Bounded(dsoData, offset, version);
                if (version == 0x54534F02) {
                    sourceCache = true;
                    // Source-cache files do not encode package ownership. They
                    // must be rebuilt from source so package order remains exact.
                    std::vector<uint8_t> sourceData;
                    if (Engine::instance().fs().readFile(scriptPath.c_str(), sourceData) &&
                        Impl::sourceHasPackage(std::string((const char*)sourceData.data(), sourceData.size())))
                        dsoData.clear();
                    if (dsoData.empty()) sourceCache = true;
                    Console::instance().printf(LogLevel::Debug, "TS: loading source DSO cache '%s'", dsoPath.c_str());
                    uint32_t funcCount = 0;
                    bool valid = readU32Bounded(dsoData, offset, funcCount) && funcCount <= 100000;
                    std::vector<std::string> addedFunctions;
                    for (uint32_t fi = 0; fi < funcCount; fi++) {
                        std::string fnName;
                        uint32_t paramCount = 0;
                        if (!readStringBounded(dsoData, offset, fnName) ||
                            !readU32Bounded(dsoData, offset, paramCount) || paramCount > 10000) {
                            valid = false;
                            break;
                        }
                        std::vector<std::string> params;
                        for (uint32_t pi = 0; pi < paramCount; pi++) {
                            std::string param;
                            if (!readStringBounded(dsoData, offset, param)) { valid = false; break; }
                            params.push_back(std::move(param));
                        }
                        std::string body;
                        if (!valid || !readStringBounded(dsoData, offset, body)) { valid = false; break; }
                        if (impl->functions.find(fnName) == impl->functions.end()) {
                            TSFunc fn;
                            fn.params = params;
                            fn.body = body;
                            fn.filename = scriptPath;
                            impl->functions[fnName] = fn;
                            addedFunctions.push_back(fnName);
                        }
                    }
                    if (valid && offset == dsoData.size()) {
                        impl->fileDependencies[scriptPath] = std::move(cachedDependencies);
                        impl->loadingFiles.erase(scriptPath);
                        return VMValue(1);
                    }
                    for (const auto& name : addedFunctions) impl->functions.erase(name);
                    Console::instance().printf(LogLevel::Warn,
                        "TS: rejecting truncated DSO cache '%s'", dsoPath.c_str());
                }
            }
            // A recognized source-cache marker is never a native DSO. If its
            // payload is malformed, discard it and execute the source.
            if (!sourceCache) {
                Console::instance().printf(LogLevel::Debug, "TS: loading DSO '%s' (%zu bytes)", dsoPath.c_str(), dsoData.size());
                auto& engine = ScriptEngine::instance();
                if (engine.vm() && engine.vm()->loadScript(dsoData.data(), dsoData.size(), dsoPath.c_str())) {
                Console::instance().printf(LogLevel::Debug, "TS: DSO loaded successfully: %s", dsoPath.c_str());
                for (auto& dso : engine.vm()->loadedScripts()) {
                    for (auto& fn : dso->functions) {
                        std::string fullName = fn.ns.empty() ? fn.name : fn.ns + "::" + fn.name;
                            if (!fn.package.empty()) {
                                impl->packageFunctions[toLower(fn.package)][fullName] = TSFunc{};
                                auto& stub = impl->packageFunctions[toLower(fn.package)][fullName];
                                stub.params = fn.argNames;
                                stub.filename = scriptPath;
                                stub.isDSO = true;
                                stub.dsoFunc = &fn;
                            } else if (impl->functions.find(fullName) == impl->functions.end()) {
                            TSFunc stub;
                            stub.params = fn.argNames;
                             stub.filename = scriptPath;
                            stub.isDSO = true;
                            stub.dsoFunc = &fn;
                            impl->functions[fullName] = stub;
                        }
                    }
                }
                    impl->loadingFiles.erase(scriptPath);
                    return VMValue(1);
                }
            }
        }
    }

    // Fall back to loading source
    std::vector<uint8_t> data;
    if (!Engine::instance().fs().readFile(scriptPath.c_str(), data)) {
        Console::instance().printf(LogLevel::Error, "TS: cannot open script file '%s'", scriptPath.c_str());
        impl->loadingFiles.erase(scriptPath);
        return {};
    }

    Console::instance().printf(LogLevel::Debug, "TS: executing '%s' (%zu bytes)", scriptPath.c_str(), data.size());
    std::string source((const char*)data.data(), data.size());
    VMValue result = execute(source, scriptPath);

    if (impl->errorOccurred) {
        return result;
    }

    // After successful execution, write DSO cache to outputDir/modPath
    if (isCompilableExt(scriptPath) && !isPrefsScript(scriptPath) && scriptPath.find('/') != std::string::npos && scriptPath.substr(scriptPath.size()-3) != ".gui") {
        std::string modPath = Console::instance().getStringVariable("modPath", "base");
        std::string outDir = Console::instance().getStringVariable("outputDir", "");
        if (!outDir.empty()) {
            std::string dsoRelPath = modPath + "/" + scriptPath + ".dso";
            std::string dsoFullPath;
            if (!TorchPath::safeOutputPath(outDir.c_str(), dsoRelPath.c_str(), dsoFullPath)) {
                Console::instance().printf(LogLevel::Warn, "TS: rejected unsafe DSO output path '%s'", dsoRelPath.c_str());
                impl->loadingFiles.erase(scriptPath);
                return result;
            }
            // Create parent dir
            std::error_code outputError;
            std::filesystem::create_directories(std::filesystem::path(dsoFullPath).parent_path(), outputError);
            if (outputError) {
                Console::instance().printf(LogLevel::Warn, "TS: cannot create DSO directory for '%s'", dsoFullPath.c_str());
                impl->loadingFiles.erase(scriptPath);
                return result;
            }
            Console::instance().printf(LogLevel::Debug, "TS: writing DSO cache '%s' (%zu funcs)", dsoFullPath.c_str(), impl->functions.size());
            if (!Impl::sourceHasPackage(source)) {
                impl->writeDSOCache(dsoFullPath, scriptPath, source);
                impl->writeDependencyManifest(dsoFullPath, impl->fileDependencies[scriptPath]);
            }
        }
    }

    return result;
}

bool TorqueScript::hasFunction(const std::string& name) const {
    for (auto package = impl->activePackages.rbegin(); package != impl->activePackages.rend(); ++package) {
        auto it = impl->packageFunctions.find(*package);
        if (it == impl->packageFunctions.end()) continue;
        if (it->second.count(name)) return true;
    }
    return impl->functions.count(name) != 0;
}

bool TorqueScript::isFunction(const std::string& name) const {
    return hasFunction(name) || impl->natives.count(toLower(name)) != 0;
}

bool TorqueScript::isPackage(const std::string& name) const {
    return impl->packageFunctions.count(toLower(name)) != 0;
}

bool TorqueScript::activatePackage(const std::string& name) {
    if (name.empty() || isActivePackage(name)) return !name.empty();
    const std::string package = toLower(name);
    if (impl->packageFunctions.find(package) == impl->packageFunctions.end()) return false;
    impl->activePackages.push_back(package);
    return true;
}

bool TorqueScript::deactivatePackage(const std::string& name) {
    for (auto it = impl->activePackages.begin(); it != impl->activePackages.end(); ++it) {
        if (sameName(*it, name)) {
            impl->activePackages.erase(it);
            return true;
        }
    }
    return false;
}

void TorqueScript::clearPackages() {
    impl->activePackages.clear();
    impl->resolveParentNext = false;
}

bool TorqueScript::isActivePackage(const std::string& name) const {
    return std::any_of(impl->activePackages.begin(), impl->activePackages.end(),
        [&](const std::string& active) { return sameName(active, name); });
}

const std::string& TorqueScript::dbgFile() const { return impl->currentFile; }
int TorqueScript::dbgLine() const { return impl->srcLine; }
std::string TorqueScript::currentFunction() const {
    return impl->callNames.empty() ? std::string() : impl->callNames.back();
}

VMValue TorqueScript::callFunction(const std::string& name, const std::vector<VMValue>& args) {
    static thread_local int callDepth = 0;
    if (callDepth >= 128) {
        Console::instance().printf(LogLevel::Error,
            "TS: call depth limit reached in '%s'", name.c_str());
        return {};
    }
    ++callDepth;
    struct CallDepthGuard {
        int& depth;
        ~CallDepthGuard() { --depth; }
    } guard{callDepth};
    // Consume Parent:: at the call boundary. A native/missing fallback must
    // not leave the next unrelated function call in parent-dispatch mode.
    const bool parentCall = impl->resolveParentNext;
    impl->resolveParentNext = false;
    const TSFunc* selected = nullptr;
    std::string selectedPackage;
    if (parentCall) {
        const std::string currentPackage = impl->callPackages.empty()
            ? std::string() : impl->callPackages.back();
        auto current = std::find_if(impl->activePackages.rbegin(), impl->activePackages.rend(),
            [&](const std::string& package) { return sameName(package, currentPackage); });
        auto begin = impl->activePackages.rend();
        if (!currentPackage.empty() && current != impl->activePackages.rend()) begin = current + 1;
        for (auto package = begin; package != impl->activePackages.rend() && !selected; ++package) {
            auto packageIt = impl->packageFunctions.find(*package);
            if (packageIt == impl->packageFunctions.end()) continue;
            auto function = packageIt->second.find(name);
            if (function != packageIt->second.end()) {
                selected = &function->second;
                selectedPackage = *package;
            }
        }
    } else {
        for (auto package = impl->activePackages.rbegin(); package != impl->activePackages.rend(); ++package) {
            auto packageIt = impl->packageFunctions.find(*package);
            if (packageIt == impl->packageFunctions.end()) continue;
            auto function = packageIt->second.find(name);
            if (function != packageIt->second.end()) {
                selected = &function->second;
                selectedPackage = *package;
                break;
            }
        }
    }
    auto it = impl->functions.find(name);
    if (!selected && it == impl->functions.end()) {
        std::string nativeName = name;
        for (char& c : nativeName)
            c = (char)tolower((unsigned char)c);
        auto native = impl->natives.find(nativeName);
        if (native != impl->natives.end())
            return native->second(args);
        const std::string caller = impl->callNames.empty()
            ? std::string() : " [" + impl->callNames.back() + "]";
        Console::instance().printf(LogLevel::Warn,
            "TS:%s%s: function not found: '%s'",
            impl->currentFile.empty() ? "<runtime>" : impl->currentFile.c_str(),
            caller.c_str(), name.c_str());
        return {};
    }


    const TSFunc& func = selected ? *selected : it->second;

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

    // A body running from native code (schedule, callbacks) is an
    // execution in flight: a nested execute()/eval() must take the
    // state-guarded path instead of replacing this body's tokens.
    impl->bodyDepth++;
    // Set up locals
    impl->locals.push();
    impl->callPackages.push_back(selectedPackage);
    impl->callNames.push_back(name);
    // TorqueScript exposes the actual call arguments through %argc and
    // %argv[index].  Missing declared parameters are empty, which still
    // converts to zero in numeric contexts.
    impl->locals.set("argc", VMValue((int32_t)args.size()));
    for (size_t i = 0; i < args.size(); i++)
        impl->locals.set("argv[" + std::to_string(i) + "]", args[i]);
    for (size_t i = 0; i < func.params.size(); i++) {
        VMValue val = (i < args.size()) ? args[i] : VMValue("");
        impl->locals.set(func.params[i], val);
    }

    impl->returning = false;
    impl->returnValue = VMValue();
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
        Console::instance().printf(LogLevel::Debug, "TS: calling DSO function '%s' (%zu args)", name.c_str(), args.size());
        auto& engine = ScriptEngine::instance();
        // Find the DSO file containing this function
        for (auto* dso : engine.vm()->loadedScripts()) {
            auto fit = dso->funcMap.find(name);
            if (fit != dso->funcMap.end()) {
                result = engine.vm()->execute(dso, fit->second->startIp, args);
                break;
            }
            const size_t separator = name.rfind("::");
            if (separator == std::string::npos) continue;
            const std::string namespaceName = name.substr(0, separator);
            const std::string functionName = name.substr(separator + 2);
            for (auto& dsoFunction : dso->functions) {
                if (dsoFunction.ns == namespaceName && dsoFunction.name == functionName) {
                    result = engine.vm()->execute(dso, dsoFunction.startIp, args);
                    break;
                }
            }
            if (result.type != VMValue::None) break;
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
    impl->bodyDepth--;
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
    impl->callPackages.pop_back();
    if (!impl->callNames.empty()) impl->callNames.pop_back();
    return result;
}

int TorqueScript::scheduleEvent(double now, double delay, const std::string& object,
                                const std::string& command, const std::vector<VMValue>& args,
                                bool onObject) {
    std::vector<std::string> values;
    for (const auto& arg : args) values.push_back(arg.toString());
    // Sim::postEvent holds the object itself: the event follows it, not
    // whichever object later takes its name.
    std::string target = object;
    if (ScriptEngine::exists())
        if (ScriptObject* sobj = ScriptEngine::instance().findObject(object.c_str()))
            target = ScriptEngine::instance().objectKey(sobj);
    return impl->scheduler.schedule(now, delay, target, command, std::move(values), onObject);
}

bool TorqueScript::callObjectMethod(const std::string& object, const std::string& method,
                                    const std::vector<VMValue>& args, VMValue* result) {
    std::vector<VMValue> methodArgs;
    methodArgs.reserve(args.size() + 1);
    methodArgs.emplace_back(object);
    methodArgs.insert(methodArgs.end(), args.begin(), args.end());
    std::vector<std::string> spaces;
    if (ScriptEngine::exists()) {
        if (ScriptObject* sobj = ScriptEngine::instance().findObject(object.c_str()))
            spaces = ScriptEngine::instance().objectNamespaces(sobj);
        else
            for (const auto& mission : ScriptEngine::instance().missionObjects()) {
                char* end = nullptr;
                const long id = std::strtol(object.c_str(), &end, 10);
                if (mission.name == object || (end && *end == '\0' && id > 0 && mission.id == id)) {
                    spaces = EngineClasses::chain(mission.className);
                    break;
                }
            }
    }
    for (const auto& space : spaces) {
        const std::string full = space + "::" + method;
        if (hasFunction(full)) {
            VMValue value = callFunction(full, methodArgs);
            if (result) *result = value;
            return true;
        }
        auto nit = impl->natives.find(toLower(full));
        if (nit != impl->natives.end()) {
            VMValue value = nit->second(methodArgs);
            if (result) *result = value;
            return true;
        }
    }
    auto nit = impl->natives.find(toLower(method));
    if (nit != impl->natives.end() && !spaces.empty()) {
        VMValue value = nit->second(methodArgs);
        if (result) *result = value;
        return true;
    }
    return false;
}
bool TorqueScript::cancelEvent(int id) { return impl->scheduler.cancel(id); }
size_t TorqueScript::cancelEventsForObject(const std::string& object) {
    return impl->scheduler.cancelForObject(object);
}
bool TorqueScript::isEventPending(int id) const { return impl->scheduler.pending(id); }
size_t TorqueScript::processScheduledEvents(double now) {
    return impl->scheduler.advance(now, [this](const ScriptScheduler::Event& event) {
        std::vector<VMValue> args;
        for (const auto& value : event.args) args.emplace_back(value);
        if (event.onObject) {
            // Deleting the object cancelled its events; a stale handle runs nothing.
            callObjectMethod(event.object, event.command, args);
            return;
        }
        // SimConsoleEvent: Con::execute resolves script functions and
        // natives alike, with the scheduled arguments.
        if (isFunction(event.command)) callFunction(event.command, args);
        else execute(event.command, "schedule");
    });
}
void TorqueScript::clearScheduledEvents() { impl->scheduler.clear(); }

bool TorqueScript::dispatchPrefixedFunction(const std::string& prefix,
                                             const std::vector<std::string>& words) {
    if (words.empty()) return false;
    std::string wanted = prefix + words.front();
    std::string lower = wanted;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    std::vector<VMValue> values;
    values.reserve(words.size() - 1);
    for (size_t i = 1; i < words.size(); ++i) values.emplace_back(words[i]);
    // HUD/server command handlers may be native functions as well as script
    // functions.  Keep the same case-insensitive lookup for both paths.
    if (impl->natives.find(lower) != impl->natives.end()) {
        callFunction(wanted, values);
        return true;
    }
    if (hasFunction(wanted)) {
        callFunction(wanted, values);
        return true;
    }
    return false;
}

bool TorqueScript::dispatchClientCommand(const std::vector<std::string>& args) {
    return dispatchPrefixedFunction("clientCmd", args);
}

bool TorqueScript::dispatchServerCommand(const std::vector<std::string>& args) {
    return dispatchPrefixedFunction("serverCmd", args);
}

bool TorqueScript::dispatchMissionCallback(const std::string& name,
                                           const std::vector<VMValue>& args) {
    std::string lower = name;
    for (char& c : lower) c = (char)tolower((unsigned char)c);
    if (!hasFunction(name) && impl->natives.find(lower) == impl->natives.end()) return false;
    callFunction(name, args);
    return true;
}
