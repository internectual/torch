#include "net/torque_bit_writer.h"
#include "game/demo.h"
#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }

int main() {
    TorqueBitWriter w;
    w.writeFlag(true);
    w.writeInt(1234, 11);
    w.writeSignedInt(-77, 9);
    w.writeRangedU32(3, 0, 4);
    w.writeFloat(0.5f, 7);
    w.writeSignedFloat(-0.25f, 9);
    w.writeF32(3.75f);
    w.setCompressionPoint({100, 200, 50});
    w.writeCompressedPoint({101.5f, 198.0f, 50.25f});
    w.writeCompressedPoint({100 + 3000, 200, 50});
    w.writeCompressedPoint({1e6f, 0, 0});
    w.setStringBuffer(true);
    w.writeString("MsgClientJoin");
    w.writeString("MsgClientJoinTeam");
    w.writeString("x");

    const auto& bytes = w.data();
    BitStream r(bytes.data(), bytes.size());
    assert(r.readFlag());
    assert(r.readInt(11) == 1234);
    assert(r.readSignedInt(9) == -77);
    assert(r.readRangedU32(0, 4) == 3);
    assert(near(r.readFloat(7), 0.5f, 0.01f));
    assert(near(r.readSignedFloat(9), -0.25f, 0.01f));
    assert(r.readF32() == 3.75f);
    const Vec3 cp{100, 200, 50};
    Vec3 a = r.readCompressedPoint(cp);
    assert(near(a.x, 101.5f, 0.011f) && near(a.y, 198.0f, 0.011f) && near(a.z, 50.25f, 0.011f));
    Vec3 b = r.readCompressedPoint(cp);
    assert(near(b.x, 3100.0f, 0.011f));
    Vec3 c = r.readCompressedPoint(cp);
    assert(c.x == 1e6f);
    r.setStringBufferEnabled(true);
    assert(r.readString() == "MsgClientJoin");
    assert(r.readString() == "MsgClientJoinTeam");
    assert(r.readString() == "x");
    assert(!r.isError());
    return 0;
}
