#pragma once

#include <cstddef>
#include <string_view>

namespace V12 {

inline constexpr std::string_view GhostClassNames[] = {
    "AIObjective", "AudioEmitter", "BeaconObject", "BombProjectile",
    "Camera", "Debris", "ELFProjectile", "EnergyProjectile",
    "FireballAtmosphere", "FlareProjectile", "FlyingVehicle", "ForceFieldBare",
    "GameBase", "GrenadeProjectile", "HoverVehicle", "InteriorInstance",
    "Item", "Lightning", "LinearFlareProjectile", "LinearProjectile",
    "Marker", "MissionArea", "MissionMarker", "ParticleEmissionDummy",
    "PhysicalZone", "Player", "Precipitation", "Projectile", "RepairProjectile",
    "ScopeAlwaysShape", "SeekerProjectile", "ShapeBase", "ShockLanceProjectile",
    "Shockwave", "SimpleNetObject", "Sky", "SniperProjectile", "SpawnSphere",
    "Splash", "StaticShape", "StationFXPersonal", "StationFXVehicle", "Sun",
    "TSStatic", "TargetProjectile", "TerrainBlock", "TracerProjectile", "Trigger",
    "Turret", "VehicleBlocker", "WaterBlock", "WayPoint", "WheeledVehicle"
};

inline constexpr std::string_view DataBlockClassNames[] = {
    "AudioDescription", "AudioEnvironment", "AudioProfile", "AudioSampleEnvironment",
    "BombProjectileData", "CameraData", "CannedChatItem", "CommanderIconData",
    "DebrisData", "DecalData", "ELFProjectileData", "EffectProfile",
    "EnergyProjectileData", "ExplosionData", "FireballAtmosphereData", "FlareProjectileData",
    "FlyingVehicleData", "ForceFieldBareData", "GameBaseData", "GrenadeProjectileData",
    "HoverVehicleData", "ItemData", "JetEffectData", "LightningData",
    "LinearFlareProjectileData", "LinearProjectileData", "MissionMarkerData", "ParticleData",
    "ParticleEmissionDummyData", "ParticleEmitterData", "PlayerData", "PrecipitationData",
    "ProjectileData", "RepairProjectileData", "RunningLightData", "SeekerProjectileData",
    "SensorData", "ShapeBaseData", "ShapeBaseImageData", "ShockLanceProjectileData",
    "ShockwaveData", "SimDataBlock", "SniperProjectileData", "SplashData",
    "StaticShapeData", "StationFXPersonalData", "StationFXVehicleData", "TSShapeConstructor",
    "TargetProjectileData", "TracerProjectileData", "TriggerData", "TurretData",
    "TurretImageData", "WheeledVehicleData"
};

inline constexpr std::string_view EventClassNames[] = {
    "CRCChallengeEvent", "CRCChallengeResponseEvent", "FogChallengeEvent",
    "GhostAlwaysObjectEvent", "GhostingMessageEvent", "GravityEvent",
    "LightningStrikeEvent", "NetStringEvent", "PathManagerEvent", "RemoteCommandEvent",
    "RemoveClientTargetTypeEvent", "ResetClientTargetsEvent", "SensorGroupColorEvent",
    "SetMissionCRCEvent", "SetObjectActiveImageEvent", "SetSensorGroupEvent",
    "SetServerTargetEvent", "Sim2DAudioEvent", "Sim3DAudioEvent", "SimDataBlockEvent",
    "SimTargetAudioEvent", "SimVoiceStreamEvent", "SimpleMessageEvent", "TargetFreeEvent",
    "TargetInfoEvent", "TargetToEvent"
};

constexpr size_t GhostClassCount = sizeof(GhostClassNames) / sizeof(GhostClassNames[0]);
constexpr size_t DataBlockClassCount = sizeof(DataBlockClassNames) / sizeof(DataBlockClassNames[0]);
constexpr size_t EventClassCount = sizeof(EventClassNames) / sizeof(EventClassNames[0]);

const char* ghostClassName(size_t classId);
const char* dataBlockClassName(size_t classId);
const char* eventClassName(size_t classId);

} // namespace V12
