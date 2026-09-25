#include "script/conversion_parity.h"

#include <cassert>

int main() {
    using namespace ScriptConversionParity;
    assert(decToBin(0) == "0");
    assert(decToBin(5) == "101");
    assert(decToBin(-1) == "11111111111111111111111111111111");
    assert(binToDec("101") == 5);
    assert(binToDec("11111111111111111111111111111111") == -1);
    assert(binToDec("") == 0);
    assert(binToDec("102") == 0);
    assert(binToDec("111111111111111111111111111111111") == 0);
    return 0;
}
