#pragma once
// sim/netStringTable.cc: tagged strings. A tag is StringTagPrefixByte (1)
// followed by the decimal id; ids are 1..1023, reference counted and shared
// by text.
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace NetStrings {

constexpr char TagPrefix = '\x01';
constexpr uint32_t MaxStrings = 1024;

// NetStringTable::addString: one more reference to `text` (id 0 when full).
uint32_t add(const std::string& text);
void remove(uint32_t id);
// The text of a live id, or nullptr.
const std::string* lookup(uint32_t id);
// OP_TAG_TO_STR: a 'literal' converts once; evaluating it again does not
// take another reference.
std::string literal(const std::string& text);

inline bool isTag(const std::string& value) { return !value.empty() && value[0] == TagPrefix; }
inline uint32_t tagId(const std::string& value) {
    return (uint32_t)std::strtoul(value.c_str() + (isTag(value) ? 1 : 0), nullptr, 10);
}
inline std::string tag(uint32_t id) { return std::string(1, TagPrefix) + std::to_string(id); }

// NetStringTable::expandString: "\x01<id> " then the text with %1-%9
// replaced by args (tags in args contribute their text after the space).
std::string expand(uint32_t id, const std::vector<std::string>& args);

} // namespace NetStrings
