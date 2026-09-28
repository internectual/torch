#include "net/remote_command.h"
#include <cassert>

int main() {
    using RemoteCommand::expandTagged;
    // clientCmdChatMessage(%sender, %voice, %pitch, '\c3%1: %2', name, text)
    std::vector<std::string> args{"ChatMessage", "7", "Male1", "1", "%1: %2", "Rigorr", "Escort player %1."};
    expandTagged(args, {true, false, false, false, true, false, false});
    assert(args[4] == "Rigorr: Escort player %1.");
    // A tagged argument expands before the ones in front of it use it.
    std::vector<std::string> nested{"Cmd", "<%1>", "[%1]", "x"};
    expandTagged(nested, {false, true, true, false});
    assert(nested[2] == "[x]" && nested[1] == "<[x]>");
    // Missing arguments vanish; %% and %x keep the second character.
    std::vector<std::string> odd{"%3|%%|%x|%"};
    expandTagged(odd, {true});
    assert(odd[0] == "|%|x|");
    return 0;
}
