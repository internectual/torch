#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace TextLayout {

inline bool isInlineWhitespace(char value) {
    return std::isspace(static_cast<unsigned char>(value)) && value != '\n';
}

// Wrap at whitespace without splitting words unless a single word is wider
// than the available width. Newlines are always hard breaks.
inline std::vector<std::string> wrapLines(
    std::string_view text, float maxWidth,
    const std::function<float(std::string_view)>& measure) {
    std::vector<std::string> lines;
    // GUI extents can briefly be invalid while a control is being rebuilt;
    // do not let NaN bypass the width check and produce a spurious line.
    if (!std::isfinite(maxWidth) || maxWidth <= 0.0f) return lines;

    size_t begin = 0;
    while (begin <= text.size()) {
        const size_t hardEnd = text.find('\n', begin);
        const size_t end = hardEnd == std::string_view::npos ? text.size() : hardEnd;
        std::string current;
        size_t word = begin;
        while (word < end) {
            while (word < end && isInlineWhitespace(text[word])) ++word;
            if (word >= end) break;
            size_t wordEnd = word;
            while (wordEnd < end && !isInlineWhitespace(text[wordEnd])) ++wordEnd;
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
        // Whitespace-only lines are still authored hard breaks.  Do not render
        // their spaces, but preserve the empty line between surrounding text.
        if (!current.empty() || (hardEnd == begin) ||
            (current.empty() && hardEnd != std::string_view::npos && hardEnd > begin))
            lines.push_back(current);
        if (hardEnd == std::string_view::npos) {
            // A final newline is a hard break with an empty line after it.
            if (begin == text.size() && begin > 0 && text[begin - 1] == '\n')
                lines.emplace_back();
            break;
        }
        begin = hardEnd + 1;
    }
    return lines;
}

} // namespace TextLayout
