#include "sim/camera.h"
#include "sim/game_connection.h"
#include "script/script_engine.h"
#include "script/torquescript.h"
#include "core/console.h"
#include <cmath>
#include <cstdio>

namespace {
constexpr float TickSec = 0.032f;
constexpr float Pi = 3.14159265358979323846f;

// $Camera::movementSpeed (Camera::mMovementSpeed).
float movementSpeed() {
    auto* ts = ScriptEngine::instance().ts();
    const VMValue value = ts ? ts->getGlobal("$Camera::movementSpeed") : VMValue();
    const std::string text = value.toString();
    return text.empty() ? Camera::MovementSpeed : (float)std::atof(text.c_str());
}

// The column j of a row-major MatrixF.
void column(const std::array<float, 16>& m, int j, float out[3]) {
    out[0] = m[j]; out[1] = m[4 + j]; out[2] = m[8 + j];
}

// "x y z ax ay az angle" (TypeMatrixPosition + rotation, angle in radians).
std::array<float, 16> parseTransform(const std::string& text) {
    float x = 0, y = 0, z = 0, ax = 0, ay = 0, az = 1, angle = 0;
    std::sscanf(text.c_str(), "%f %f %f %f %f %f %f", &x, &y, &z, &ax, &ay, &az, &angle);
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 0) { ax /= len; ay /= len; az /= len; } else { ax = 0; ay = 0; az = 1; }
    const float s = std::sin(angle * 0.5f);
    const float qx = ax * s, qy = ay * s, qz = az * s, qw = std::cos(angle * 0.5f);
    const float xx = qx * qx, yy = qy * qy, zz = qz * qz, xy = qx * qy, xz = qx * qz,
                yz = qy * qz, wx = qw * qx, wy = qw * qy, wz = qw * qz;
    return {1 - 2 * (yy + zz), 2 * (xy + wz), 2 * (xz - wy), x,
            2 * (xy - wz), 1 - 2 * (xx + zz), 2 * (yz + wx), y,
            2 * (xz + wy), 2 * (yz - wx), 1 - 2 * (xx + yy), z,
            0, 0, 0, 1};
}

// MatrixF -> "x y z ax ay az angle" (AngAxisF from the rotation).
std::string formatTransform(const std::array<float, 16>& m) {
    const float trace = m[0] + m[5] + m[10];
    float angle = std::acos(std::fmax(-1.0f, std::fmin(1.0f, (trace - 1.0f) * 0.5f)));
    float ax = m[6] - m[9], ay = m[8] - m[2], az = m[1] - m[4];
    const float len = std::sqrt(ax * ax + ay * ay + az * az);
    if (len > 1e-6f) { ax /= len; ay /= len; az /= len; } else { ax = 1; ay = 0; az = 0; angle = 0; }
    char buffer[200];
    std::snprintf(buffer, sizeof(buffer), "%g %g %g %g %g %g %g", m[3], m[7], m[11], ax, ay, az, angle);
    return buffer;
}
} // namespace

// The client camera starts at (0, 0, 100), in fly mode.
Camera::Camera() {
    ghostable = true;
    transform[11] = 100.0f;
}

void Camera::readFields() {
    ShapeBase::readFields();
    if (Fields::present(script, "position") || Fields::present(script, "rotation")) setTransform(transform);
    else transform[11] = 100.0f;
}

void Camera::setPosition(const float pos[3], float pitch, float yaw) {
    const float cx = std::cos(pitch), sx = std::sin(pitch);
    const float cz = std::cos(yaw), sz = std::sin(yaw);
    // zRot * xRot (MatrixF::set(EulerF)).
    transform = {cz, sz * cx, sz * sx, pos[0],
                 -sz, cz * cx, cz * sx, pos[1],
                 0, -sx, cx, pos[2],
                 0, 0, 0, 1};
    rotX = pitch;
    rotZ = yaw;
}

void Camera::setTransform(const std::array<float, 16>& matrix) {
    float vec[3];
    column(matrix, 1, vec);
    const float pos[3] = {matrix[3], matrix[7], matrix[11]};
    setPosition(pos, -std::atan2(vec[2], std::sqrt(vec[0] * vec[0] + vec[1] * vec[1])),
                -std::atan2(-vec[0], vec[1]));
}

void Camera::setFlyMode() {
    mode = FlyMode;
    orbitObject.clear();
}

void Camera::setOrbitMode(const std::string& object, const float pos[3], const float axisAngle[4],
                          float minDist, float maxDist, float curDist, bool ownClientObject) {
    observingClientObject = ownClientObject;
    orbitObject = object;
    SceneObject* target = object.empty() ? nullptr : EngineObjects::get<SceneObject>(object);
    if (target) {
        // The object's world box centre; its position until world boxes land.
        orbitPosition[0] = target->transform[3];
        orbitPosition[1] = target->transform[7];
        orbitPosition[2] = target->transform[11];
        mode = OrbitObjectMode;
    } else {
        orbitObject.clear();
        mode = OrbitPointMode;
        for (int i = 0; i < 3; ++i) orbitPosition[i] = pos[i];
    }
    // setPosition(mPosition, dir): the rotation's forward as the angles.
    char text[160];
    std::snprintf(text, sizeof(text), "0 0 0 %g %g %g %g", axisAngle[0], axisAngle[1], axisAngle[2], axisAngle[3]);
    const auto rotation = parseTransform(text);
    float dir[3];
    column(rotation, 1, dir);
    setPosition(orbitPosition, -std::atan2(dir[2], std::sqrt(dir[0] * dir[0] + dir[1] * dir[1])),
                -std::atan2(-dir[0], dir[1]));
    minOrbitDist = minDist;
    maxOrbitDist = maxDist;
    curOrbitDist = curDist;
}

void Camera::processMove(const ClientMoveIn* move) {
    ShapeBase::processMove(move);
    if (!move) return;
    constexpr float AngleUnit = 2.0f * Pi / 65536.0f;
    rotX += (int16_t)move->pitch * AngleUnit;
    if (rotX > MaxPitch) rotX = MaxPitch;
    else if (rotX < -MaxPitch) rotX = -MaxPitch;
    rotZ += (int16_t)move->yaw * AngleUnit;
    if (rotZ > Pi) rotZ -= 2.0f * Pi;
    if (mode == OrbitObjectMode || mode == OrbitPointMode) {
        if (mode == OrbitObjectMode)
            if (auto* target = EngineObjects::get<SceneObject>(orbitObject)) {
                orbitPosition[0] = target->transform[3];
                orbitPosition[1] = target->transform[7];
                orbitPosition[2] = target->transform[11];
            }
        setPosition(orbitPosition, rotX, rotZ);
        // validateEyePoint(1): back along the view by the orbit range (the
        // collision cast against the scene is not in yet).
        float dir[3];
        column(transform, 1, dir);
        const float back = maxOrbitDist - minOrbitDist;
        transform[3] -= dir[0] * back;
        transform[7] -= dir[1] * back;
        transform[11] -= dir[2] * back;
    } else {
        const bool faster = move->trigger[0] || move->trigger[1];
        const float scale = movementSpeed() * (faster ? 2.0f : 1.0f) * TickSec;
        const float mx = (move->x - 16) / 16.0f, my = (move->y - 16) / 16.0f, mz = (move->z - 16) / 16.0f;
        float pos[3], vx[3], vy[3], vz[3];
        position(pos);
        column(transform, 0, vx);
        column(transform, 1, vy);
        column(transform, 2, vz);
        for (int i = 0; i < 3; ++i) pos[i] += (vx[i] * mx + vy[i] * my + vz[i] * mz) * scale;
        setPosition(pos, rotX, rotZ);
    }
    setMaskBits(MoveMask);
}

uint32_t Camera::packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) {
    ShapeBase::packUpdate(connection, mask, w);
    // The rest goes with the control object's packet data to its client.
    const bool controlledHere = !controllingClient.empty() && connection.script &&
                                controllingClient == ScriptEngine::instance().objectKey(connection.script);
    if (w.writeFlag(controlledHere && !(mask & InitialUpdateMask))) return 0;
    if (w.writeFlag(mask & MoveMask)) {
        w.writeF32(transform[3]);
        w.writeF32(transform[7]);
        w.writeF32(transform[11]);
        w.writeF32(rotX);
        w.writeF32(rotZ);
    }
    return 0;
}

bool Camera::writePacketData(GameConnection& connection, TorqueBitWriter& w) {
    bool ret = ShapeBase::writePacketData(connection, w);
    w.writePoint({transform[3], transform[7], transform[11]});
    w.writeF32(rotX);
    w.writeF32(rotZ);
    int writeMode = mode;
    float writePos[3] = {orbitPosition[0], orbitPosition[1], orbitPosition[2]};
    int ghostIndex = -1;
    if (mode == OrbitObjectMode) {
        ghostIndex = orbitObject.empty() ? -1 : connection.ghostIndex(orbitObject);
        if (ghostIndex == -1) {
            writeMode = OrbitPointMode;
            ret = false;
        }
    }
    w.writeRangedU32((uint32_t)writeMode, StationaryMode, OrbitPointMode);
    if (writeMode == OrbitObjectMode || writeMode == OrbitPointMode) {
        w.writeF32(minOrbitDist);
        w.writeF32(maxOrbitDist);
        w.writeF32(curOrbitDist);
        if (writeMode == OrbitObjectMode) {
            w.writeFlag(observingClientObject);
            w.writeInt(ghostIndex, 10);
        } else {
            // The retail client reads it against the packet's previous
            // compression point.
            w.writeCompressedPoint({writePos[0], writePos[1], writePos[2]});
        }
    }
    w.setCompressionPoint({transform[3], transform[7], transform[11]});
    return ret;
}

void registerCameraNatives(TorqueScript& ts) {
    EngineObjects::registerClass("Camera", [] { return std::make_shared<Camera>(); });
    using Args = std::vector<VMValue>;
    auto camera = [](const Args& args) -> Camera* {
        return args.empty() ? nullptr : EngineObjects::get<Camera>(args[0].toString());
    };
    // ShapeBase::setControlDirty: the controlling client resends its state.
    auto controlDirty = [](Camera& c) {
        if (auto* con = c.controllingClient.empty() ? nullptr : EngineObjects::get<GameConnection>(c.controllingClient))
            con->setControlObjectDirty();
    };
    ts.registerNative("Camera::setFlyMode", [camera, controlDirty](const Args& args) -> VMValue {
        if (auto* c = camera(args)) { controlDirty(*c); c->setFlyMode(); c->setMaskBits(Camera::MoveMask); }
        return VMValue("");
    });
    ts.registerNative("Camera::setOrbitMode", [camera, controlDirty](const Args& args) -> VMValue {
        auto* c = camera(args);
        if (!c || args.size() < 6) return VMValue("");
        const std::string object = args[1].toString();
        if (!ScriptEngine::instance().findObject(object.c_str())) {
            Console::instance().printf(LogLevel::Warn, "Cannot orbit non-existing object.");
            c->setFlyMode();
            return VMValue("");
        }
        float pos[3] = {0, 0, 0}, aa[4] = {0, 0, 1, 0};
        std::sscanf(args[2].toString().c_str(), "%f %f %f %f %f %f %f",
                    &pos[0], &pos[1], &pos[2], &aa[0], &aa[1], &aa[2], &aa[3]);
        controlDirty(*c);
        c->setOrbitMode(object, pos, aa, args[3].toFloat(), args[4].toFloat(), args[5].toFloat(),
                        args.size() > 6 && args[6].toBool());
        c->setMaskBits(Camera::MoveMask);
        return VMValue("");
    });
    ts.registerNative("Camera::getPosition", [camera](const Args& args) -> VMValue {
        auto* c = camera(args);
        if (!c) return VMValue("");
        char buffer[100];
        std::snprintf(buffer, sizeof(buffer), "%f %f %f", c->transform[3], c->transform[7], c->transform[11]);
        return VMValue(buffer);
    });
    ts.registerNative("Camera::setTransform", [camera](const Args& args) -> VMValue {
        auto* c = camera(args);
        if (c && args.size() > 1) {
            c->setTransform(parseTransform(args[1].toString()));
            c->setMaskBits(Camera::MoveMask);
        }
        return VMValue("");
    });
    ts.registerNative("Camera::getTransform", [camera](const Args& args) -> VMValue {
        auto* c = camera(args);
        return VMValue(c ? formatTransform(c->transform) : std::string());
    });
}
