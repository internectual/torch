#pragma once
// Camera (game/camera.cc): fly and orbit observer cameras driven by the
// controlling client's moves; ghosted as the retail Camera class.
#include "sim/shape_base.h"

class Camera : public ShapeBase {
public:
    enum CameraMasks : uint32_t { MoveMask = ShapeBase::NextFreeMask };
    enum Mode { StationaryMode = 0, FreeRotateMode = 1, FlyMode = 2, OrbitObjectMode = 3, OrbitPointMode = 4 };
    static constexpr float MovementSpeed = 40.0f; // Camera::mMovementSpeed
    static constexpr float MaxPitch = 1.3962f;

    Camera();
    const char* netClassName() const override { return "Camera"; }
    void readFields() override;

    int mode = FlyMode;
    float rotX = 0.0f, rotZ = 0.0f; // mRot.x (pitch), mRot.z (yaw)
    float orbitPosition[3] = {0, 0, 0}; // mPosition
    std::string orbitObject;
    float minOrbitDist = 0.0f, maxOrbitDist = 0.0f, curOrbitDist = 0.0f;
    bool observingClientObject = false;

    void position(float out[3]) const { out[0] = transform[3]; out[1] = transform[7]; out[2] = transform[11]; }
    // Camera::setPosition(pos, rot): zRot * xRot at pos.
    void setPosition(const float pos[3], float pitch, float yaw);
    // Camera::setTransform: the matrix's rotation as pitch and yaw.
    void setTransform(const std::array<float, 16>& matrix);
    void setFlyMode();
    void setOrbitMode(const std::string& object, const float pos[3], const float axisAngle[4],
                      float minDist, float maxDist, float curDist, bool ownClientObject);

    void processMove(const ClientMoveIn* move) override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    bool writePacketData(GameConnection& connection, TorqueBitWriter& w) override;
};

void registerCameraNatives(class TorqueScript& ts);
