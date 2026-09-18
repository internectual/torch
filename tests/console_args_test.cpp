#include "core/console_args.h"

#include <cassert>

int main() {
    const auto args = splitConsoleArguments(" 42, \"alpha,beta\", ,last,");
    assert(args.size() == 5);
    assert(args[0] == "42");
    assert(args[1] == "alpha,beta");
    assert(args[2].empty());
    assert(args[3] == "last");
    assert(args[4].empty());

    const auto nested = splitConsoleArguments("foo(1,2), \"quoted\"");
    assert(nested.size() == 2);
    assert(nested[0] == "foo(1,2)");
    assert(nested[1] == "quoted");
    return 0;
}
