#include "sim/net_string_table.h"
#include <unordered_map>
#include <vector>

namespace NetStrings {

namespace {
struct Entry { std::string text; uint32_t refs = 0; };
struct Table {
    std::vector<Entry> entries = std::vector<Entry>(MaxStrings);
    std::unordered_map<std::string, uint32_t> byText;
    std::unordered_map<std::string, uint32_t> literals;
};
Table& table() { static Table t; return t; }
} // namespace

uint32_t add(const std::string& text) {
    auto& t = table();
    if (auto it = t.byText.find(text); it != t.byText.end()) {
        ++t.entries[it->second].refs;
        return it->second;
    }
    for (uint32_t id = 1; id < MaxStrings; ++id) {
        if (t.entries[id].refs) continue;
        t.entries[id] = {text, 1};
        t.byText[text] = id;
        return id;
    }
    return 0;
}

void remove(uint32_t id) {
    auto& t = table();
    if (id == 0 || id >= MaxStrings || !t.entries[id].refs) return;
    if (--t.entries[id].refs) return;
    t.byText.erase(t.entries[id].text);
    t.entries[id].text.clear();
}

const std::string* lookup(uint32_t id) {
    auto& t = table();
    if (id == 0 || id >= MaxStrings || !t.entries[id].refs) return nullptr;
    return &t.entries[id].text;
}

std::string literal(const std::string& text) {
    auto& t = table();
    if (auto it = t.literals.find(text); it != t.literals.end() && lookup(it->second)) return tag(it->second);
    const uint32_t id = add(text);
    t.literals[text] = id;
    return tag(id);
}

std::string expand(uint32_t id, const std::vector<std::string>& args) {
    std::string out = tag(id) + " ";
    const std::string* text = lookup(id);
    if (!text) return out;
    for (size_t i = 0; i < text->size(); ++i) {
        char c = (*text)[i];
        if (c == '%') {
            if (++i >= text->size()) break;
            c = (*text)[i];
            if (c >= '1' && c <= '9') {
                const size_t index = (size_t)(c - '1');
                if (index >= args.size()) continue;
                std::string copy = args[index];
                if (isTag(copy)) {
                    const size_t space = copy.find(' ');
                    copy = space == std::string::npos ? std::string() : copy.substr(space + 1);
                }
                out += copy;
                continue;
            }
        }
        out += c;
    }
    return out;
}

} // namespace NetStrings
