#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TextLayout {

// Wrap at whitespace without splitting words unless a single word is wider
// than the available width. Newlines are always hard breaks.
inline std::vector<std::string> wrapLines(
    std::string_view text, float maxWidth,
    const std::function<float(std::string_view)>& measure) {
    std::vector<std::string> lines;
    if (maxWidth <= 0.0f) return lines;

    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t hardEnd = text.find('\n', begin);
        const size_t end = hardEnd == std::string_view::npos ? text.size() : hardEnd;
        std::string current;
        size_t word = begin;
        while (word < end) {
            while (word < end && text[word] == ' ') ++word;
            if (word >= end) break;
            size_t wordEnd = word;
            while (wordEnd < end && text[wordEnd] != ' ') ++wordEnd;
            const std::string candidate = current.empty()
                ? std::string(text.substr(word, wordEnd - word))
                : current + " " + std::string(text.substr(word, wordEnd - word));
            if (!current.empty() && measure(candidate) > maxWidth) {
                lines.push_back(current);
                current.assign(text.substr(word, wordEnd - word));
            } else {
                current = candidate;
            }
            // A long word remains on its own line; callers can shrink or clip it.
            word = wordEnd;
        }
        if (!current.empty() || (hardEnd == begin)) lines.push_back(current);
        if (hardEnd == std::string_view::npos) break;
        begin = hardEnd + 1;
    }
    return lines;
}

} // namespace TextLayout
