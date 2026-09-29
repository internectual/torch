#pragma once
// Projectiles, server side: game/projectile.cc and linearProjectile.cc (V12)
// and the retail proj*.cc classes as the retail Linux server runs them
// (decompiled from the retail binary where V12 has no source): onAdd from
// the persist fields, the 32 ms processTick, collision through the server
// container (sim/containers.h), the script callbacks, and packUpdate in the
// layout the Tribes 2 client reads (src/game/demo.cpp read*ProjectileData).
#include "sim/game_base.h"
#include "sim/containers.h"
#include <string>
#include <vector>

class ShapeBase;

// Projectile (projectile.cc): initial conditions, the source object and its
// inherited ("excess") velocity, the tick counter.
class ProjectileObject : public GameBase {
public:
    enum ProjectileConstants { SourceIdTimeoutTicks = 7, DeleteWaitTicks = 13, ExcessVelDirBits = 7 };

    explicit ProjectileObject(const char* netClass = "Projectile");
    const char* netClassName() const override { return netClass; }
    // Projectile::onAdd (server half), then the class's onAdd.
    void readFields() override;
    // Projectile::processTick (mCurrTick, mSourceIdTimeoutTicks), then the class's.
    void processTick() override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;

    Point3F initialPosition{0, 0, 0};
    Point3F initialDirection{0, 0, 1};
    int sourceObjectId = -1, vehicleObjectId = -1, sourceSlot = -1;
    std::string sourceKey, vehicleKey; // registry keys; empty when none
    uint32_t currTick = 0;
    uint32_t sourceIdTimeoutTicks = 0;
    Point3F excessDir{0, 0, 1}, excessDirDumb{0, 0, 1};
    uint32_t excessVel = 0;
    bool hidden = false;
    float fadeValue = 1.0f;

    Point3F getPosition() const { return {transform[3], transform[7], transform[11]}; }
    // setTransform(MatrixF(true) with the position).
    void setPosition(const Point3F& p);
    virtual Point3F getVelocity() const { return {0, 0, 0}; }
    ShapeBase* source() const;
    ShapeBase* vehicle() const;
    ScriptObject* sourceScript() const;
    const std::string& key() const { return selfKey; }

protected:
    virtual void onAddServer() {}
    virtual void tick() {}
    // Projectile::onCollision: %data.onCollision(%proj, %hit, %fade, "pos", "normal").
    void onCollision(const Point3F& p, const Point3F& n, ScriptObject* hit);
    // %data.onExplode(%proj, "pos", "fade").
    void scriptOnExplode(const Point3F& p);
    // The object is still registered (a callback may have deleted it).
    bool alive() const;
    void deleteSelf();
    // disableCollision on the source and vehicle while the source timeout runs.
    std::vector<ScriptObject*> timeoutExempt() const;
    int ghostOf(GameConnection& connection, const std::string& key) const;
    float data(const char* field, float fallback) const { return dataFloat(field, fallback); }
    int dataS32(const char* field, int fallback) const { return (int)dataFloat(field, (float)fallback); }

    std::string selfKey;
    int selfId = 0;
    bool added = false;

private:
    const char* netClass;
};

// LinearProjectile (linearProjectile.cc), TracerProjectile and
// LinearFlareProjectile (render-only subclasses): the flight is forecast in
// onAdd as up to two segments against the static world and water; the tick
// only checks dynamic objects along the way.
class LinearProjectileObject : public ProjectileObject {
public:
    enum LPConstants { InitialDirectionBits = 14, MaxLivingTicks = 511 }; // retail writes 14-bit normals
    enum LPUpdateMasks : uint32_t { ExplosionMask = GameBase::NextFreeMask };
    explicit LinearProjectileObject(const char* netClass = "LinearProjectile") : ProjectileObject(netClass) {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    Point3F getVelocity() const override { return deriveExactVelocity(currTick); }

    struct Segment {
        Point3F start{0, 0, 0}, end{0, 0, 0}, segmentVel{0, 0, 0};
        bool cutShort = false;
        uint32_t endTypeMask = 0;
        Point3F endNormal{0, 0, 1};
        uint32_t msStart = 0, msEnd = 0;
        std::string hitObj;
    };
    Segment segments[2];
    int numSegments = 1;
    uint32_t deleteTick = 0;
    bool wetStart = false, hitWater = false, endedWithDecal = false;
    Point3F explosionPosition{0, 0, 0}, explosionNormal{0, 0, 1};

protected:
    void onAddServer() override;
    void tick() override;

private:
    void createSegments();
    Point3F deriveExactPosition(uint32_t tick) const;
    Point3F deriveExactVelocity(uint32_t tick) const;
    void explode(const Point3F& p, const Point3F& n, bool dynamicObject);
    float dryVelocity = 5, wetVelocity = 5;
    int lifetimeMS = 1024;
    bool explodeOnDeath = false, explodeOnWaterImpact = false;
    float reflectOnWaterImpactAngle = 0;
};

// GrenadeProjectile (projGrenade.cc, retail): ballistic flight under
// gravity x gravityMod, bouncing until armed; FlareProjectile shares it.
class GrenadeProjectileObject : public ProjectileObject {
public:
    enum GPUpdateMasks : uint32_t { BounceMask = GameBase::NextFreeMask, ExplosionMask = GameBase::NextFreeMask << 1 };
    explicit GrenadeProjectileObject(const char* netClass = "GrenadeProjectile") : ProjectileObject(netClass) {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    Point3F getVelocity() const override { return velocity; }

    Point3F velocity{0, 0, 0};
    Point3F explosionPosition{0, 0, 0}, explosionNormal{0, 0, 1};
    int64_t deleteTick = -1;
    uint32_t armTick = 0;
    int lockCount = 0; // FlareProjectile: missiles decoyed by it

protected:
    void onAddServer() override;
    void tick() override;
    // GrenadeProjectile::computeNewState: gravity, and drag underwater.
    void computeNewState(Point3F& newPosition);
    void explode(const Point3F& p, const Point3F& n);
    float elasticity = 0.999f, friction = 0.3f, drag = 0, density = 1, gravityMod = 1;
};

// EnergyProjectile (projEnergy.cc, retail): a grenade that mirrors off the
// world until armed and explodes on anything dynamic.
class EnergyProjectileObject : public GrenadeProjectileObject {
public:
    EnergyProjectileObject() : GrenadeProjectileObject("EnergyProjectile") {}

protected:
    void tick() override;
};

// BombProjectile (projBomb.cc, retail): falls and explodes on first contact.
class BombProjectileObject : public GrenadeProjectileObject {
public:
    BombProjectileObject() : GrenadeProjectileObject("BombProjectile") {}

protected:
    void tick() override;
};

// SeekerProjectile (projSeeker.cc, retail): a missile steering toward an
// object or a point, decoyed by flares, avoiding terrain.
class SeekerProjectileObject : public ProjectileObject {
public:
    enum SPUpdateMasks : uint32_t { TargetMask = GameBase::NextFreeMask, ExplosionMask = GameBase::NextFreeMask << 1 };
    enum TargetMode { ObjectTarget = 0, PositionTarget = 1, NoTarget = 2 };
    SeekerProjectileObject() : ProjectileObject("SeekerProjectile") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    Point3F getVelocity() const override { return velocity; }

    void setObjectTarget(ScriptObject* target);
    void setPositionTarget(const Point3F& p);
    void setNoTarget();
    int targetObjectId() const;

    Point3F velocity{0, 0, 0};
    int mode = NoTarget;
    std::string targetKey, originalTargetKey;
    Point3F targetPosition{0, 0, 0};
    uint32_t deleteTick = 0, lifetimeTicks = 0, projectedExcessVel = 0;
    bool hitWater = false;

protected:
    void onAddServer() override;
    void tick() override;

private:
    bool getTarget(Point3F& out);
    bool steer(const Point3F& pos, const Point3F& vel, const Point3F& target, Point3F& out);
    bool collide(const Point3F& a, const Point3F& b, bool dynamic, SimContainer::RayInfo& hit);
    void explode(const Point3F& p, const Point3F& n);
    void clearTarget();
};

// SniperProjectile (projSniper.cc) and TargetProjectile (projTarget.cc):
// a beam from the muzzle to the first thing within maxRifleRange.
class BeamProjectileObject : public ProjectileObject {
public:
    explicit BeamProjectileObject(const char* netClass) : ProjectileObject(netClass) {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    Point3F endPoint{0, 0, 0};
    bool truncated = false, beamHitWater = false;
    float energyPercentage = 0.0f;
    std::string hitKey;

protected:
    bool sniper = false;
    void cast(bool callCollision);
};

class SniperProjectileObject : public BeamProjectileObject {
public:
    SniperProjectileObject() : BeamProjectileObject("SniperProjectile") { sniper = true; }
    int lifetimeTicks = 0;

protected:
    void onAddServer() override;
    void tick() override;
};

class TargetProjectileObject : public BeamProjectileObject {
public:
    enum TPUpdateMasks : uint32_t { BeamMask = GameBase::NextFreeMask };
    TargetProjectileObject() : BeamProjectileObject("TargetProjectile") {}

protected:
    void onAddServer() override;
    void tick() override;
};

// ShockLanceProjectile (projShockLance.cc, retail): a bolt to the struck target.
class ShockLanceProjectileObject : public ProjectileObject {
public:
    ShockLanceProjectileObject() : ProjectileObject("ShockLanceProjectile") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    std::string targetKey;
    Point3F start{0, 0, 0}, end{0, 0, 0};
    bool hitObject = false;
    int lifetimeTicks = 0;

protected:
    void onAddServer() override;
    void tick() override;
};

// ELFProjectile (projELF.cc, retail): a beam from the source's muzzle that
// latches onto a damageable object, with zapTarget / unzapTarget callbacks.
class ELFProjectileObject : public ProjectileObject {
public:
    enum EPUpdateMasks : uint32_t { TargetMask = GameBase::NextFreeMask };
    ELFProjectileObject() : ProjectileObject("ELFProjectile") {}
    ~ELFProjectileObject() override;
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    std::string targetKey;

protected:
    void onAddServer() override { update(); }
    void tick() override { update(); }

private:
    void update();
    void zap(ScriptObject* target);
    void unzap();
    std::string dataKey;
};

// RepairProjectile (projRepair.cc, retail): a beam between the source and
// the object it repairs; the scripts create and delete it.
class RepairProjectileObject : public ProjectileObject {
public:
    RepairProjectileObject() : ProjectileObject("RepairProjectile") {}
    uint32_t packUpdate(GameConnection& connection, uint32_t mask, TorqueBitWriter& w) override;
    std::string targetKey;

protected:
    void onAddServer() override;
};

void registerProjectileNatives(class TorqueScript& ts);
