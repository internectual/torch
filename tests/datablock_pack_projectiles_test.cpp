// Round trip of the 'projectiles' datablock writers through Torch's reader.
#include "sim/datablock_pack.h"
#include "tests/datablock_pack_harness.h"

int main() {
    DataBlockPack::registerAll();
    return 0;
}
