#pragma once
// RemoteCommandEvent::process: each tagged argument, from the last back to
// the first, has %1-%9 replaced by the arguments after it
// (NetStringTable::expandString). A '%' before any other character is
// dropped and the character kept; a missing argument expands to nothing.
// Torch carries tags as their text, so the result stays plain text.
#include <string>
#include <vector>

namespace RemoteCommand {

inline std::string expand(const std::string& text, const std::vector<std::string>& args, size_t first) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (++i >= text.size()) break;
            c = text[i];
            if (c >= '1' && c <= '9') {
                const size_t index = first + (size_t)(c - '1');
                if (index < args.size()) out += args[index];
                continue;
            }
        }
        out += c;
    }
    return out;
}

inline void expandTagged(std::vector<std::string>& args, const std::vector<bool>& tagged) {
    for (size_t i = args.size(); i-- > 0;)
        if (i < tagged.size() && tagged[i]) args[i] = expand(args[i], args, i + 1);
}

} // namespace RemoteCommand
