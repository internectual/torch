// Round trip of the 'shapes' datablock writers through Torch's reader.
#include "sim/datablock_pack.h"
#include "tests/datablock_pack_harness.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

namespace {

using Fields = std::map<std::string, std::string>;
using Refs = std::map<std::string, uint32_t>;

int failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

PackRoundTrip roundTrip(const char* className, const Fields& fields, const Refs& refs = {}) {
    PackRoundTrip r = packRoundTrip(className, fields, refs);
    if (!r.exact()) std::fprintf(stderr, "  (%s, %zu fields)\n", className, fields.size());
    CHECK(r.exact());
    return r;
}

// Torch's reader keeps a reference as its raw 11-bit ranged value, which
// is the datablock id minus DataBlockObjectIdFirst (3).
uint32_t wire(uint32_t id) { return id - DataBlockPack::ObjectIdFirst; }

// Datablock ids for the names the test fields reference.
Refs refIds() {
    Refs refs;
    uint32_t next = 100;
    for (const char* name : {
             "CMDPlayerIcon", "playerDebris", "ArmorJetSound", "HumanArmorJetEmitter",
             "HumanArmorJetEffect", "LightMaleFootprint", "LightPuffEmitter",
             "LiftoffDustEmitter", "PlayerSplash", "PlayerFoamDropletsEmitter",
             "PlayerFoamEmitter", "PlayerBubbleEmitter", "LFootLightSoftSound",
             "RFootLightSoftSound", "ImpactLightSoftSound", "SkiAllSoftSound",
             "ImpactLightWaterEasySound", "ExitingWaterLightSound", "DiscProjectile",
             "DiscSwitchSound", "DiscLoopSound", "DiscFireSound", "DiscReloadSound",
             "DiscDryFireSound", "ShapeDebris", "VehicleExplosion", "ContrailEmitter",
             "FlyerJetEmitter", "ScoutFlyerThrustSound", "ScoutFlyerEngineSound",
             "SoftImpactSound", "HardImpactSound", "VehicleExitWaterMediumSound",
             "VehicleImpactWaterSoftSound", "VehicleImpactWaterMediumSound",
             "VehicleWakeMediumSplashSound", "VehicleLiftoffDustEmitter", "LightDamageSmoke",
             "HeavyDamageSmoke", "DamageBubbles", "VehicleFoamDropletsEmitter",
             "VehicleFoamEmitter", "CMDFlyingScoutIcon", "TireEmitter", "ScoutSqueelSound",
             "ScoutEngineSound", "ScoutThrustSound", "GravSoftImpactSound",
             "VehicleExitWaterSoftSound", "VehicleWakeSoftSplashSound", "WildcatJetEmitter",
             "CMDHoverScoutIcon", "LargeGroundVehicleExplosion", "MPBThrustSound",
             "MPBEngineSound", "AssaultVehicleSkid", "VehicleImpactWaterHardSound",
             "TurretExplosion", "CMDTurretIcon", "TurretDebris", "PlasmaBarrelBolt",
             "PBLSwitchSound", "PBLFireSound", "ShapeExplosion", "CMDStationIcon",
             "StationDebris"})
        refs[name] = next++;
    return refs;
}

// ShapeBaseData fields shared by the realistic sets below.
void addLightMaleShape(Fields& f) {
    f["emap"] = "1";
    f["shapeFile"] = "light_male.dts";
    f["cameraMaxDist"] = "3";
    f["computeCRC"] = "1";
    f["canObserve"] = "1";
    f["cmdCategory"] = "Clients";
    f["cmdIcon"] = "CMDPlayerIcon";
    f["cmdMiniIconName"] = "commander/MiniIcons/com_player_grey";
    f["hudImageNameFriendly[0]"] = "gui/hud_playertriangle";
    f["hudImageNameEnemy[0]"] = "gui/hud_playertriangle_enemy";
    f["hudRenderModulated[0]"] = "1";
    f["hudImageNameFriendly[1]"] = "commander/MiniIcons/com_flag_grey";
    f["hudImageNameEnemy[1]"] = "commander/MiniIcons/com_flag_grey";
    f["hudRenderModulated[1]"] = "1";
    f["hudRenderAlways[1]"] = "1";
    f["hudRenderCenter[1]"] = "1";
    f["hudRenderDistance[1]"] = "1";
    f["hudImageNameFriendly[2]"] = "commander/MiniIcons/com_flag_grey";
    f["hudImageNameEnemy[2]"] = "commander/MiniIcons/com_flag_grey";
    f["hudRenderModulated[2]"] = "1";
    f["hudRenderAlways[2]"] = "1";
    f["hudRenderCenter[2]"] = "1";
    f["hudRenderDistance[2]"] = "1";
    f["cameraDefaultFov"] = "90.0";
    f["cameraMinFov"] = "5.0";
    f["cameraMaxFov"] = "120.0";
    f["debrisShapeName"] = "debris_player.dts";
    f["debris"] = "playerDebris";
    f["mass"] = "90";
    f["drag"] = "0.134";
    f["density"] = "19";
    f["maxEnergy"] = "60";
    f["shieldEffectScale"] = "0.7 0.7 1.0";
}

// player.cs: LightMaleHumanArmor (script expressions evaluated).
Fields lightMaleHumanArmor() {
    Fields f;
    addLightMaleShape(f);
    f["minLookAngle"] = "-1.5";
    f["maxLookAngle"] = "1.5";
    f["maxFreelookAngle"] = "3.0";
    f["jetForce"] = "3355.2";
    f["underwaterJetForce"] = "3780";
    f["underwaterVertJetFactor"] = "1.5";
    f["jetEnergyDrain"] = "0.90";
    f["underwaterJetEnergyDrain"] = "0.7";
    f["minJetEnergy"] = "3";
    f["maxJetHorizontalPercentage"] = "0.6";
    f["runForce"] = "4968";
    f["runEnergyDrain"] = "0";
    f["minRunEnergy"] = "0";
    f["maxForwardSpeed"] = "15";
    f["maxBackwardSpeed"] = "13";
    f["maxSideSpeed"] = "13";
    f["maxUnderwaterForwardSpeed"] = "13";
    f["maxUnderwaterBackwardSpeed"] = "11";
    f["maxUnderwaterSideSpeed"] = "11";
    f["jumpForce"] = "750.6";
    f["jumpEnergyDrain"] = "0";
    f["minJumpEnergy"] = "0";
    f["jumpDelay"] = "0";
    f["recoverDelay"] = "8";
    f["recoverRunForceScale"] = "1.4";
    f["minImpactSpeed"] = "45";
    f["jetSound"] = "ArmorJetSound";
    f["wetJetSound"] = "ArmorJetSound";
    f["jetEmitter"] = "HumanArmorJetEmitter";
    f["jetEffect"] = "HumanArmorJetEffect";
    f["boundingBox"] = "1.2 1.2 2.3";
    f["decalData"] = "LightMaleFootprint";
    f["decalOffset"] = "0.25";
    f["footPuffEmitter"] = "LightPuffEmitter";
    f["footPuffNumParts"] = "15";
    f["footPuffRadius"] = "0.25";
    f["dustEmitter"] = "LiftoffDustEmitter";
    f["splash"] = "PlayerSplash";
    f["splashVelocity"] = "4.0";
    f["splashAngle"] = "67.0";
    f["splashFreqMod"] = "300.0";
    f["splashVelEpsilon"] = "0.60";
    f["bubbleEmitTime"] = "0.4";
    f["splashEmitter[0]"] = "PlayerFoamDropletsEmitter";
    f["splashEmitter[1]"] = "PlayerFoamEmitter";
    f["splashEmitter[2]"] = "PlayerBubbleEmitter";
    f["mediumSplashSoundVelocity"] = "10.0";
    f["hardSplashSoundVelocity"] = "20.0";
    f["exitSplashSoundVelocity"] = "5.0";
    f["runSurfaceAngle"] = "70";
    f["jumpSurfaceAngle"] = "80";
    f["minJumpSpeed"] = "20";
    f["maxJumpSpeed"] = "30";
    f["noFrictionOnSki"] = "1";
    f["horizMaxSpeed"] = "500";
    f["horizResistSpeed"] = "48.74";
    f["horizResistFactor"] = "0";
    f["maxJetForwardSpeed"] = "30";
    f["upMaxSpeed"] = "52";
    f["upResistSpeed"] = "20.89";
    f["upResistFactor"] = "0.35";
    f["heatDecayPerSec"] = "0.25";
    f["heatIncreasePerSec"] = "0.333333";
    f["footstepSplashHeight"] = "0.35";
    f["LFootSoftSound"] = "LFootLightSoftSound";
    f["RFootSoftSound"] = "RFootLightSoftSound";
    f["impactSoftSound"] = "ImpactLightSoftSound";
    f["skiSoftSound"] = "SkiAllSoftSound";
    f["impactWaterEasy"] = "ImpactLightWaterEasySound";
    f["exitingWater"] = "ExitingWaterLightSound";
    f["groundImpactMinSpeed"] = "14.0";
    f["groundImpactShakeFreq"] = "3.0 3.0 3.0";
    f["groundImpactShakeAmp"] = "1.0 1.0 1.0";
    f["groundImpactShakeDuration"] = "0.6";
    f["groundImpactShakeFalloff"] = "10.0";
    return f;
}

// weapons/disc.cs: DiscImage.
Fields discImage() {
    Fields f;
    f["shapeFile"] = "weapon_disc.dts";
    f["offset"] = "0 0 0";
    f["emap"] = "1";
    f["projectile"] = "DiscProjectile";
    f["stateName[0]"] = "Preactivate";
    f["stateTransitionOnLoaded[0]"] = "Activate";
    f["stateTransitionOnNoAmmo[0]"] = "NoAmmo";
    f["stateName[1]"] = "Activate";
    f["stateTransitionOnTimeout[1]"] = "Ready";
    f["stateTimeoutValue[1]"] = "0.5";
    f["stateSequence[1]"] = "Activated";
    f["stateSound[1]"] = "DiscSwitchSound";
    f["stateName[2]"] = "Ready";
    f["stateTransitionOnNoAmmo[2]"] = "NoAmmo";
    f["stateTransitionOnTriggerDown[2]"] = "Fire";
    f["stateSequence[2]"] = "DiscSpin";
    f["stateSound[2]"] = "DiscLoopSound";
    f["stateName[3]"] = "Fire";
    f["stateTransitionOnTimeout[3]"] = "Reload";
    f["stateTimeoutValue[3]"] = "1.25";
    f["stateFire[3]"] = "1";
    f["stateRecoil[3]"] = "LightRecoil";
    f["stateAllowImageChange[3]"] = "0";
    f["stateSequence[3]"] = "Fire";
    f["stateScript[3]"] = "onFire";
    f["stateSound[3]"] = "DiscFireSound";
    f["stateName[4]"] = "Reload";
    f["stateTransitionOnNoAmmo[4]"] = "NoAmmo";
    f["stateTransitionOnTimeout[4]"] = "Ready";
    f["stateTimeoutValue[4]"] = "0.5";
    f["stateAllowImageChange[4]"] = "0";
    f["stateSequence[4]"] = "Reload";
    f["stateSound[4]"] = "DiscReloadSound";
    f["stateName[5]"] = "NoAmmo";
    f["stateTransitionOnAmmo[5]"] = "Reload";
    f["stateSequence[5]"] = "NoAmmo";
    f["stateTransitionOnTriggerDown[5]"] = "DryFire";
    f["stateName[6]"] = "DryFire";
    f["stateSound[6]"] = "DiscDryFireSound";
    f["stateTimeoutValue[6]"] = "1.0";
    f["stateTransitionOnTimeout[6]"] = "NoAmmo";
    return f;
}

// VehicleData fields shared by the three vehicles (per-vehicle values set after).
void addVehicleCommon(Fields& f) {
    f["computeCRC"] = "1";
    f["debris"] = "ShapeDebris";
    f["renderWhenDestroyed"] = "0";
    f["explosion"] = "VehicleExplosion";
    f["softImpactSound"] = "SoftImpactSound";
    f["hardImpactSound"] = "HardImpactSound";
    f["exitingWater"] = "VehicleExitWaterMediumSound";
    f["impactWaterEasy"] = "VehicleImpactWaterSoftSound";
    f["impactWaterMedium"] = "VehicleImpactWaterMediumSound";
    f["impactWaterHard"] = "VehicleImpactWaterMediumSound";
    f["waterWakeSound"] = "VehicleWakeMediumSplashSound";
    f["dustEmitter"] = "VehicleLiftoffDustEmitter";
    f["damageEmitter[0]"] = "LightDamageSmoke";
    f["damageEmitter[1]"] = "HeavyDamageSmoke";
    f["damageEmitter[2]"] = "DamageBubbles";
    f["damageLevelTolerance[0]"] = "0.3";
    f["damageLevelTolerance[1]"] = "0.7";
    f["splashEmitter[0]"] = "VehicleFoamDropletsEmitter";
    f["splashEmitter[1]"] = "VehicleFoamEmitter";
    f["cmdCategory"] = "Tactical";
    f["observeParameters"] = "1 10 10";
}

// vehicles/vehicle_shrike.cs: ScoutFlyer.
Fields scoutFlyer() {
    Fields f;
    addVehicleCommon(f);
    f["shapeFile"] = "vehicle_air_scout.dts";
    f["debrisShapeName"] = "vehicle_air_scout_debris.dts";
    f["drag"] = "0.15";
    f["density"] = "1.0";
    f["cameraMaxDist"] = "15";
    f["cameraOffset"] = "2.5";
    f["cameraLag"] = "0.9";
    f["maxEnergy"] = "280";
    f["minDrag"] = "30";
    f["rotationalDrag"] = "900";
    f["maxAutoSpeed"] = "15";
    f["autoAngularForce"] = "400";
    f["autoLinearForce"] = "300";
    f["autoInputDamping"] = "0.95";
    f["maxSteeringAngle"] = "5"; // script spelling: a dynamic field in the engine
    f["horizontalSurfaceForce"] = "6";
    f["verticalSurfaceForce"] = "4";
    f["maneuveringForce"] = "3000";
    f["steeringForce"] = "1200";
    f["steeringRollForce"] = "400";
    f["rollForce"] = "4";
    f["hoverHeight"] = "5";
    f["createHoverHeight"] = "3";
    f["maxForwardSpeed"] = "100";
    f["jetForce"] = "2000";
    f["minJetEnergy"] = "28";
    f["jetEnergyDrain"] = "2.8";
    f["vertThrustMultiple"] = "4.0";
    f["mass"] = "150";
    f["bodyFriction"] = "0";
    f["bodyRestitution"] = "0.5";
    f["minRollSpeed"] = "0";
    f["softImpactSpeed"] = "14";
    f["hardImpactSpeed"] = "25";
    f["minImpactSpeed"] = "10";
    f["collDamageThresholdVel"] = "23.0";
    f["collDamageMultiplier"] = "0.02";
    f["minTrailSpeed"] = "15";
    f["trailEmitter"] = "ContrailEmitter";
    f["forwardJetEmitter"] = "FlyerJetEmitter";
    f["downJetEmitter"] = "FlyerJetEmitter";
    f["jetSound"] = "ScoutFlyerThrustSound";
    f["engineSound"] = "ScoutFlyerEngineSound";
    f["softSplashSoundVelocity"] = "10.0";
    f["mediumSplashSoundVelocity"] = "15.0";
    f["hardSplashSoundVelocity"] = "20.0";
    f["exitSplashSoundVelocity"] = "10.0";
    f["triggerDustHeight"] = "4.0";
    f["dustHeight"] = "1.0";
    f["damageEmitterOffset[0]"] = "0.0 -3.0 0.0 ";
    f["numDmgEmitterAreas"] = "1";
    f["cmdIcon"] = "CMDFlyingScoutIcon";
    f["cmdMiniIconName"] = "commander/MiniIcons/com_scout_grey";
    f["sensorRadius"] = "200";
    f["sensorColor"] = "255 194 9";
    f["shieldEffectScale"] = "0.937 1.125 0.60";
    return f;
}

// vehicles/vehicle_wildcat.cs: ScoutVehicle.
Fields scoutVehicle() {
    Fields f;
    addVehicleCommon(f);
    f["floatingGravMag"] = "3.5";
    f["shapeFile"] = "vehicle_grav_scout.dts";
    f["debrisShapeName"] = "vehicle_grav_scout_debris.dts";
    f["drag"] = "0.0";
    f["density"] = "0.9";
    f["cameraMaxDist"] = "5.0";
    f["cameraOffset"] = "0.7";
    f["cameraLag"] = "0.5";
    f["maxEnergy"] = "150";
    f["minJetEnergy"] = "15";
    f["jetEnergyDrain"] = "1.3";
    f["mass"] = "400";
    f["bodyFriction"] = "0.1";
    f["bodyRestitution"] = "0.5";
    f["softImpactSpeed"] = "20";
    f["hardImpactSpeed"] = "28";
    f["minImpactSpeed"] = "29";
    f["collDamageThresholdVel"] = "23";
    f["collDamageMultiplier"] = "0.030";
    f["dragForce"] = "0.555556";
    f["vertFactor"] = "0.0";
    f["floatingThrustFactor"] = "0.35";
    f["mainThrustForce"] = "35";
    f["reverseThrustForce"] = "10";
    f["strafeThrustForce"] = "8";
    f["turboFactor"] = "1.80";
    f["brakingForce"] = "25";
    f["brakingActivationSpeed"] = "4";
    f["stabLenMin"] = "2.25";
    f["stabLenMax"] = "3.75";
    f["stabSpringConstant"] = "30";
    f["stabDampingConstant"] = "16";
    f["gyroDrag"] = "16";
    f["normalForce"] = "30";
    f["restorativeForce"] = "20";
    f["steeringForce"] = "30";
    f["rollForce"] = "15";
    f["pitchForce"] = "7";
    f["triggerDustHeight"] = "2.5";
    f["dustHeight"] = "1.0";
    f["dustTrailEmitter"] = "TireEmitter";
    f["dustTrailOffset"] = "0.0 -1.0 0.5";
    f["triggerTrailHeight"] = "3.6";
    f["dustTrailFreqMod"] = "15.0";
    f["jetSound"] = "ScoutSqueelSound";
    f["engineSound"] = "ScoutEngineSound";
    f["floatSound"] = "ScoutThrustSound";
    f["softImpactSound"] = "GravSoftImpactSound";
    f["exitingWater"] = "VehicleExitWaterSoftSound";
    f["waterWakeSound"] = "VehicleWakeSoftSplashSound";
    f["damageEmitterOffset[0]"] = "0.0 -1.5 0.5 ";
    f["numDmgEmitterAreas"] = "1";
    f["forwardJetEmitter"] = "WildcatJetEmitter";
    f["cmdIcon"] = "CMDHoverScoutIcon";
    f["cmdMiniIconName"] = "commander/MiniIcons/com_landscout_grey";
    f["sensorRadius"] = "40";
    f["shieldEffectScale"] = "0.9375 1.125 0.6";
    return f;
}

// vehicles/vehicle_mpb.cs: MobileBaseVehicle.
Fields mobileBaseVehicle() {
    Fields f;
    addVehicleCommon(f);
    f["shapeFile"] = "vehicle_land_mpbase.dts";
    f["debrisShapeName"] = "vehicle_land_mpbase_debris.dts";
    f["drag"] = "0.0";
    f["density"] = "20.0";
    f["cameraMaxDist"] = "20";
    f["cameraOffset"] = "6";
    f["cameraLag"] = "1.5";
    f["explosion"] = "LargeGroundVehicleExplosion";
    f["maxSteeringAngle"] = "0.3";
    f["mass"] = "2000";
    f["bodyFriction"] = "0.8";
    f["bodyRestitution"] = "0.5";
    f["minRollSpeed"] = "3";
    f["gyroForce"] = "400";
    f["gyroDamping"] = "0.3";
    f["stabilizerForce"] = "10";
    f["minDrag"] = "10";
    f["softImpactSpeed"] = "15";
    f["hardImpactSpeed"] = "25";
    f["minImpactSpeed"] = "12";
    f["collDamageThresholdVel"] = "18";
    f["collDamageMultiplier"] = "0.070";
    f["engineTorque"] = "5215";
    f["breakTorque"] = "5215";
    f["maxWheelSpeed"] = "20";
    f["springForce"] = "8000";
    f["springDamping"] = "1300";
    f["antiSwayForce"] = "6000";
    f["staticLoadScale"] = "2";
    f["tireRadius"] = "1.6";
    f["tireFriction"] = "10.0";
    f["tireRestitution"] = "0.5";
    f["tireLateralForce"] = "3000";
    f["tireLateralDamping"] = "400";
    f["tireLateralRelaxation"] = "1";
    f["tireLongitudinalForce"] = "12000";
    f["tireLongitudinalDamping"] = "600";
    f["tireLongitudinalRelaxation"] = "1";
    f["tireEmitter"] = "TireEmitter";
    f["maxEnergy"] = "600";
    f["jetForce"] = "4000";
    f["minJetEnergy"] = "60";
    f["jetEnergyDrain"] = "2.75";
    f["jetSound"] = "MPBThrustSound";
    f["engineSound"] = "MPBEngineSound";
    f["squeelSound"] = "AssaultVehicleSkid";
    f["softImpactSound"] = "GravSoftImpactSound";
    f["softSplashSoundVelocity"] = "5.0";
    f["mediumSplashSoundVelocity"] = "8.0";
    f["hardSplashSoundVelocity"] = "12.0";
    f["exitSplashSoundVelocity"] = "8.0";
    f["impactWaterHard"] = "VehicleImpactWaterHardSound";
    f["damageEmitterOffset[0]"] = "3.0 0.5 0.0 ";
    f["damageEmitterOffset[1]"] = "-3.0 0.5 0.0 ";
    f["numDmgEmitterAreas"] = "2";
    return f;
}

// turret.cs: TurretBaseLarge.
Fields turretBaseLarge() {
    Fields f;
    f["shapeFile"] = "turret_base_large.dts";
    f["mass"] = "1.0";
    f["explosion"] = "TurretExplosion";
    f["emap"] = "1";
    f["thetaMin"] = "15";
    f["thetaMax"] = "140";
    f["maxEnergy"] = "150";
    f["canControl"] = "1";
    f["cmdCategory"] = "Tactical";
    f["cmdIcon"] = "CMDTurretIcon";
    f["cmdMiniIconName"] = "commander/MiniIcons/com_turretbase_grey";
    f["sensorRadius"] = "80";
    f["sensorColor"] = "0 212 45";
    f["firstPersonOnly"] = "1";
    f["debrisShapeName"] = "debris_generic.dts";
    f["debris"] = "TurretDebris";
    return f;
}

// turrets/plasmabarrellarge.cs: PlasmaBarrelLarge.
Fields plasmaBarrelLarge() {
    Fields f;
    f["shapeFile"] = "turret_fusion_large.dts";
    f["projectile"] = "PlasmaBarrelBolt";
    f["usesEnergy"] = "1";
    f["minEnergy"] = "10";
    f["emap"] = "1";
    f["activationMS"] = "700";
    f["deactivateDelayMS"] = "1500";
    f["thinkTimeMS"] = "140";
    f["degPerSecTheta"] = "300";
    f["degPerSecPhi"] = "500";
    f["attackRadius"] = "150";
    f["stateName[0]"] = "Activate";
    f["stateTransitionOnNotLoaded[0]"] = "Dead";
    f["stateTransitionOnLoaded[0]"] = "ActivateReady";
    f["stateName[1]"] = "ActivateReady";
    f["stateSequence[1]"] = "Activate";
    f["stateSound[1]"] = "PBLSwitchSound";
    f["stateTimeoutValue[1]"] = "1";
    f["stateTransitionOnTimeout[1]"] = "Ready";
    f["stateTransitionOnNotLoaded[1]"] = "Deactivate";
    f["stateTransitionOnNoAmmo[1]"] = "NoAmmo";
    f["stateName[2]"] = "Ready";
    f["stateTransitionOnNotLoaded[2]"] = "Deactivate";
    f["stateTransitionOnTriggerDown[2]"] = "Fire";
    f["stateTransitionOnNoAmmo[2]"] = "NoAmmo";
    f["stateName[3]"] = "Fire";
    f["stateTransitionOnTimeout[3]"] = "Reload";
    f["stateTimeoutValue[3]"] = "0.3";
    f["stateFire[3]"] = "1";
    f["stateRecoil[3]"] = "LightRecoil";
    f["stateSequence[3]"] = "Fire";
    f["stateSound[3]"] = "PBLFireSound";
    f["stateName[4]"] = "Reload";
    f["stateTimeoutValue[4]"] = "0.80";
    f["stateSequence[4]"] = "Reload";
    f["stateTransitionOnTimeout[4]"] = "Ready";
    f["stateTransitionOnNotLoaded[4]"] = "Deactivate";
    f["stateTransitionOnNoAmmo[4]"] = "NoAmmo";
    f["stateName[5]"] = "Deactivate";
    f["stateSequence[5]"] = "Activate";
    f["stateDirection[5]"] = "0";
    f["stateTimeoutValue[5]"] = "1";
    f["stateTransitionOnLoaded[5]"] = "ActivateReady";
    f["stateTransitionOnTimeout[5]"] = "Dead";
    f["stateName[6]"] = "Dead";
    f["stateTransitionOnLoaded[6]"] = "ActivateReady";
    f["stateName[7]"] = "NoAmmo";
    f["stateTransitionOnAmmo[7]"] = "Reload";
    f["stateSequence[7]"] = "NoAmmo";
    return f;
}

// station.cs: StationInventory.
Fields stationInventory() {
    Fields f;
    f["shapeFile"] = "station_inv_human.dts";
    f["explosion"] = "ShapeExplosion";
    f["noIndividualDamage"] = "1";
    f["dynamicType"] = "8192";
    f["maxEnergy"] = "50";
    f["cmdCategory"] = "Support";
    f["cmdIcon"] = "CMDStationIcon";
    f["cmdMiniIconName"] = "commander/MiniIcons/com_inventory_grey";
    f["debrisShapeName"] = "debris_generic.dts";
    f["debris"] = "StationDebris";
    return f;
}

} // namespace

int main() {
    DataBlockPack::registerAll();
    const Refs refs = refIds();

    // (a) Every class with no fields: engine constructor defaults.
    for (const char* className : {
             "ShapeBaseData", "ShapeBaseImageData", "PlayerData", "FlyingVehicleData",
             "HoverVehicleData", "WheeledVehicleData", "StaticShapeData", "TurretData",
             "TurretImageData", "ItemData", "CameraData", "MissionMarkerData",
             "StationFXPersonalData", "StationFXVehicleData"})
        roundTrip(className, {});

    {   // Defaults decode as the engine's.
        auto r = roundTrip("PlayerData", {});
        CHECK(r.decoded.isPlayerData);
        CHECK(near(r.decoded.shapeMass, 9.0f));
        CHECK(near(r.decoded.shapeMaxEnergy, 60.0f));
        CHECK(near(r.decoded.playerPhysics.maxForwardSpeed, 10.0f));
        CHECK(near(r.decoded.playerBoxSize[2], 2.3f));
        auto item = roundTrip("ItemData", {});
        CHECK(near(item.decoded.shapeDensity, 2.0f));
        CHECK(near(item.decoded.shapeDrag, 0.5f));
        CHECK(near(item.decoded.itemGravityMod, 1.0f));
        CHECK(near(item.decoded.itemMaxVelocity, -1.0f));
        auto turret = roundTrip("TurretData", {});
        CHECK(near(turret.decoded.turretThetaMin, 45.0f));
        CHECK(near(turret.decoded.turretThetaMax, 135.0f));
        auto image = roundTrip("ShapeBaseImageData", {});
        CHECK(image.decoded.imageCloakable);
        CHECK(image.decoded.imageStates.empty());
    }

    // (b) Realistic retail datablocks.
    {   // ShapeBaseData / CameraData / MissionMarkerData
        Fields f;
        addLightMaleShape(f);
        auto r = roundTrip("ShapeBaseData", f, refs);
        CHECK(r.decoded.shapeFile == "light_male.dts");
        CHECK(r.decoded.debrisShape == "debris_player.dts");
        CHECK(near(r.decoded.shapeMass, 90.0f));
        CHECK(near(r.decoded.shapeDrag, 0.134f));
        CHECK(near(r.decoded.cameraMaxDist, 3.0f));
        CHECK(r.decoded.shapeEmap);
        roundTrip("CameraData", {{"mode", "observerStatic"}, {"firstPersonOnly", "1"}}, refs);
        auto marker = roundTrip("MissionMarkerData", {{"catagory", "Misc"}, {"shapeFile", "octahedron.dts"}}, refs);
        CHECK(marker.decoded.shapeFile == "octahedron.dts");
    }
    {   // PlayerData: LightMaleHumanArmor
        auto r = roundTrip("PlayerData", lightMaleHumanArmor(), refs);
        const auto& d = r.decoded;
        CHECK(d.isPlayerData);
        CHECK(near(d.shapeMass, 90.0f));
        CHECK(near(d.playerMinLookAngle, -1.5f));
        CHECK(near(d.playerPhysics.jetForce, 3355.2f));
        CHECK(near(d.playerPhysics.maxForwardSpeed, 15.0f));
        CHECK(near(d.playerPhysics.upResistFactor, 0.35f));
        CHECK(d.playerPhysics.jumpDelay == 0);
        CHECK(d.playerJetEmitterRef == wire(refs.at("HumanArmorJetEmitter")));
        CHECK(d.playerSounds.size() == 32);
        CHECK(d.playerSounds[0] == wire(refs.at("ArmorJetSound")));
        CHECK(d.playerSounds[1] == wire(refs.at("ArmorJetSound")));
        CHECK(d.playerSounds[2] == wire(refs.at("LFootLightSoftSound")));
        CHECK(d.playerSounds[3] == wire(refs.at("RFootLightSoftSound")));
        CHECK(d.playerSounds[20] == wire(refs.at("ImpactLightSoftSound")));
        CHECK(d.playerSounds[24] == wire(refs.at("ImpactLightWaterEasySound")));
        CHECK(d.playerSounds[27] == wire(refs.at("ExitingWaterLightSound")));
        CHECK(d.playerSounds[28] == wire(refs.at("SkiAllSoftSound")));
        CHECK(near(d.playerBoxSize[0], 1.2f));
        CHECK(d.playerFootPuffEmitter == wire(refs.at("LightPuffEmitter")));
        CHECK(d.playerFootPuffNumParts == 15);
        CHECK(d.playerDecalData == wire(refs.at("LightMaleFootprint")));
        CHECK(near(d.playerDecalOffset, 0.25f));
        CHECK(d.playerDustEmitter == wire(refs.at("LiftoffDustEmitter")));
        CHECK(near(d.playerFootSplashHeight, 0.35f));
    }
    {   // ShapeBaseImageData: DiscImage
        auto r = roundTrip("ShapeBaseImageData", discImage(), refs);
        const auto& d = r.decoded;
        CHECK(d.shapeFile == "weapon_disc.dts");
        CHECK(d.hasMountPoint && d.mountPoint == 0);
        CHECK(d.imageStates.size() == 31);
        if (d.imageStates.size() == 31) {
            const auto& s = d.imageStates;
            CHECK(s[0].valid && s[6].valid && !s[7].valid);
            CHECK(s[0].transitionOnLoaded == 1);    // Activate
            CHECK(s[0].transitionOnNoAmmo == 5);    // NoAmmo
            CHECK(s[0].transitionOnTimeout == -1);
            CHECK(s[1].transitionOnTimeout == 2);   // Ready
            CHECK(near(s[1].timeoutValue, 0.5f));
            CHECK(s[1].sound == (int)wire(refs.at("DiscSwitchSound")));
            CHECK(s[2].transitionOnTriggerDown == 3);
            CHECK(s[3].fire && !s[2].fire);
            CHECK(near(s[3].timeoutValue, 1.25f));
            CHECK(s[3].waitForTimeout && s[3].direction && s[3].scaleAnimation);
            CHECK(s[5].transitionOnAmmo == 4);
            CHECK(s[6].transitionOnTimeout == 5);
        }
        // A non-identity offset/rotation goes through writeAffineTransform.
        Fields rotated = discImage();
        rotated["offset"] = "0.1 0.2 -0.3";
        rotated["rotation"] = "0 0 1 90";
        rotated["isSeeker"] = "1";
        rotated["seekRadius"] = "300";
        rotated["lightType"] = "WeaponFireLight";
        rotated["lightColor"] = "0.8 0.6 0.2";
        rotated["stateEmitter[3]"] = "LightDamageSmoke";
        rotated["stateEmitterTime[3]"] = "0.2";
        rotated["stateEnergyDrain[3]"] = "5";
        rotated["stateSpinThread[2]"] = "FullSpeed";
        roundTrip("ShapeBaseImageData", rotated, refs);
        rotated["rotation"] = "1 0 0 180";
        roundTrip("ShapeBaseImageData", rotated, refs);
    }
    {   // FlyingVehicleData: ScoutFlyer
        auto r = roundTrip("FlyingVehicleData", scoutFlyer(), refs);
        const auto& d = r.decoded;
        CHECK(d.isFlyingVehicleData);
        CHECK(d.shapeFile == "vehicle_air_scout.dts");
        CHECK(near(d.shapeMass, 150.0f));
        CHECK(near(d.vehicleManeuveringForce, 3000.0f));
        CHECK(near(d.vehicleMinTrailSpeed, 15.0f));
        CHECK(d.vehicleJetSound == wire(refs.at("ScoutFlyerThrustSound")));
        CHECK(d.vehicleJetEmitters.size() == 4);
        if (d.vehicleJetEmitters.size() == 4) {
            CHECK(d.vehicleJetEmitters[0] == wire(refs.at("FlyerJetEmitter")));
            CHECK(d.vehicleJetEmitters[1] == 0);
            CHECK(d.vehicleJetEmitters[2] == wire(refs.at("FlyerJetEmitter")));
            CHECK(d.vehicleJetEmitters[3] == wire(refs.at("ContrailEmitter")));
        }
    }
    {   // HoverVehicleData: ScoutVehicle
        auto r = roundTrip("HoverVehicleData", scoutVehicle(), refs);
        CHECK(r.decoded.isHoverVehicleData);
        CHECK(near(r.decoded.shapeMass, 400.0f));
        CHECK(r.decoded.vehicleJetEmitters.size() == 3);
        if (r.decoded.vehicleJetEmitters.size() == 3)
            CHECK(r.decoded.vehicleJetEmitters[0] == wire(refs.at("WildcatJetEmitter")));
    }
    {   // WheeledVehicleData: MobileBaseVehicle
        auto r = roundTrip("WheeledVehicleData", mobileBaseVehicle(), refs);
        CHECK(r.decoded.shapeFile == "vehicle_land_mpbase.dts");
        CHECK(near(r.decoded.shapeDensity, 20.0f));
        CHECK(near(r.decoded.shapeMaxEnergy, 600.0f));
    }
    {   // StaticShapeData: StationInventory
        auto r = roundTrip("StaticShapeData", stationInventory(), refs);
        CHECK(r.decoded.shapeFile == "station_inv_human.dts");
        CHECK(near(r.decoded.shapeMaxEnergy, 50.0f));
    }
    {   // TurretData: TurretBaseLarge
        auto r = roundTrip("TurretData", turretBaseLarge(), refs);
        CHECK(r.decoded.hasTurretTheta);
        CHECK(near(r.decoded.turretThetaMin, 15.0f));
        CHECK(near(r.decoded.turretThetaMax, 140.0f));
        CHECK(r.decoded.shapeFile == "turret_base_large.dts");
    }
    {   // TurretImageData: PlasmaBarrelLarge
        auto r = roundTrip("TurretImageData", plasmaBarrelLarge(), refs);
        const auto& d = r.decoded;
        CHECK(d.shapeFile == "turret_fusion_large.dts");
        CHECK(d.imageStates.size() == 31);
        if (d.imageStates.size() == 31) {
            CHECK(d.imageStates[0].transitionOnNotLoaded == 6); // Dead
            CHECK(d.imageStates[0].transitionOnLoaded == 1);    // ActivateReady
            CHECK(!d.imageStates[5].direction);
            CHECK(d.imageStates[7].valid && !d.imageStates[8].valid);
        }
    }
    {   // ItemData: weapons/disc.cs Disc, and item.cs RepairKit-style light
        auto r = roundTrip("ItemData", {{"shapeFile", "weapon_disc.dts"}, {"mass", "1"},
                                        {"elasticity", "0.2"}, {"friction", "0.6"},
                                        {"pickupRadius", "2"}, {"pickUpName", "a spinfusor"},
                                        {"emap", "1"}}, refs);
        CHECK(r.decoded.shapeFile == "weapon_disc.dts");
        CHECK(near(r.decoded.itemFriction, 0.6f));
        CHECK(near(r.decoded.itemElasticity, 0.2f));
        CHECK(near(r.decoded.shapeDensity, 2.0f));
        auto lit = roundTrip("ItemData", {{"shapeFile", "repair_kit.dts"}, {"sticky", "1"},
                                          {"gravityMod", "0.5"}, {"maxVelocity", "30"},
                                          {"lightType", "PulsingLight"}, {"lightColor", "0.2 0.8 0.2 1"},
                                          {"lightTime", "1200"}, {"lightRadius", "4"}}, refs);
        CHECK(lit.decoded.itemSticky);
        CHECK(near(lit.decoded.itemGravityMod, 0.5f));
        CHECK(near(lit.decoded.itemMaxVelocity, 30.0f));
        CHECK(lit.decoded.shapeLightType == 2);
        CHECK(lit.decoded.shapeLightTimeMS == 1200);
        CHECK(near(lit.decoded.shapeLightRadius, 4.0f));
    }
    {   // StationFXPersonalData: station.cs PersonalInvFX
        roundTrip("StationFXPersonalData", {{"delay", "0"}, {"fadeDelay", "0.5"}, {"lifetime", "1.2"},
                                            {"height", "2.5"}, {"numArcSegments", "10.0"},
                                            {"numDegrees", "180.0"}, {"trailFadeTime", "0.5"},
                                            {"leftRadius", "1.85"}, {"rightRadius", "1.85"},
                                            {"leftNodeName", "FX1"}, {"rightNodeName", "FX2"},
                                            {"texture[0]", "special/stationLight"}});
    }
    {   // StationFXVehicleData: servervehiclehud.cs VehicleInvFX
        roundTrip("StationFXVehicleData", {
            {"lifetime", "6.0"}, {"glowTopHeight", "1.5"}, {"glowBottomHeight", "0.1"},
            {"glowTopRadius", "12.5"}, {"glowBottomRadius", "12.0"}, {"numGlowSegments", "26"},
            {"glowFadeTime", "3.25"}, {"armLightDelay", "2.3"}, {"armLightLifetime", "3.0"},
            {"armLightFadeTime", "1.5"}, {"numArcSegments", "10.0"}, {"sphereColor", "0.1 0.1 0.5"},
            {"spherePhiSegments", "13"}, {"sphereThetaSegments", "8"}, {"sphereRadius", "12.0"},
            {"sphereScale", "1.05 1.05 0.85"}, {"glowNodeName", "GLOWFX"},
            {"leftNodeName[0]", "LFX1"}, {"leftNodeName[1]", "LFX2"}, {"leftNodeName[2]", "LFX3"},
            {"leftNodeName[3]", "LFX4"}, {"rightNodeName[0]", "RFX1"}, {"rightNodeName[1]", "RFX2"},
            {"rightNodeName[2]", "RFX3"}, {"rightNodeName[3]", "RFX4"},
            {"texture[0]", "special/stationGlow"}, {"texture[1]", "special/stationLight2"}});
    }

    if (failures) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    std::printf("datablock pack shapes: all round trips exact\n");
    return 0;
}
