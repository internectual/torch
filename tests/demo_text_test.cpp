#include "game/demo_text.h"

#include <cassert>

int main() {
    assert(formatDemoRemoteText("%1/%2/%10", {"one", "two", "three"}) == "one/two/");
    assert(formatDemoRemoteText("%1/%10", {"one", "two", "three", "four", "five",
                                             "six", "seven", "eight", "nine", "ten"}) ==
           "one/ten");
    assert(formatDemoRemoteText("hello %99\n", {"one"}) == "hello ");
    return 0;
}
