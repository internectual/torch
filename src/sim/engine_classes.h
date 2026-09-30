#pragma once
// Engine console classes and their console parent class, from the
// IMPLEMENT_CONOBJECT/IMPLEMENT_CO_* declarations of the Tribes 2 engine
// source (first base that is itself a console class), and the retail-only
// classes from the retail binary. A method call on an
// object resolves along this chain (Namespace linking in consoleObject.cc).
#include <string>
#include <strings.h>
#include <unordered_map>
#include <vector>

namespace EngineClasses {

struct CaseHash {
    size_t operator()(const std::string& s) const {
        size_t h = 1469598103934665603ull;
        for (unsigned char c : s) { h ^= (size_t)(c | 0x20); h *= 1099511628211ull; }
        return h;
    }
};
struct CaseEqual {
    bool operator()(const std::string& a, const std::string& b) const {
        return a.size() == b.size() && strncasecmp(a.c_str(), b.c_str(), a.size()) == 0;
    }
};

inline const std::unordered_map<std::string, std::string, CaseHash, CaseEqual>& parents() {
    static const std::unordered_map<std::string, std::string, CaseHash, CaseEqual> table = {
        {"AIConnection", "GameConnection"},
        {"AIObjective", "MissionMarker"},
        {"AIObjectiveQ", "SimSet"},
        {"AITask", "SimObject"},
        {"ActionMap", "SimObject"},
        {"AudioDescription", "SimDataBlock"},
        {"AudioEmitter", "SceneObject"},
        {"AudioEnvironment", "SimDataBlock"},
        {"AudioProfile", "SimDataBlock"},
        {"AudioSampleEnvironment", "SimDataBlock"},
        {"BanList", "SimObject"},
        {"BombProjectile", "GrenadeProjectile"},
        {"BombProjectileData", "GrenadeProjectileData"},
        {"BombSight", "SceneObject"},
        {"Camera", "ShapeBase"},
        {"CameraData", "ShapeBaseData"},
        {"ChannelVector", "MessageVector"},
        {"ClientTarget", "SimObject"},
        {"CommanderIconData", "SimDataBlock"},
        {"CompTest", "GuiScrollContentCtrl"},
        {"CreatorTree", "GuiArrayCtrl"},
        {"DbgFileView", "GuiArrayCtrl"},
        {"Debris", "GameBase"},
        {"DebrisData", "GameBaseData"},
        {"DebugView", "GuiTextCtrl"},
        {"DecalData", "SimDataBlock"},
        {"DecalManager", "SceneObject"},
        {"ELFProjectile", "Projectile"},
        {"ELFProjectileData", "ProjectileData"},
        {"EditManager", "GuiControl"},
        {"EditTSCtrl", "GuiTSCtrl"},
        {"EditorButtonCtrl", "GuiButtonCtrl"},
        {"EditorCheckBoxCtrl", "GuiCheckBoxCtrl"},
        {"EnergyProjectile", "GrenadeProjectile"},
        {"EnergyProjectileData", "GrenadeProjectileData"},
        {"Explosion", "GameBase"},
        {"ExplosionData", "GameBaseData"},
        {"FileObject", "SimObject"},
        {"FireballAtmosphere", "GameBase"},
        {"FireballAtmosphereData", "GameBaseData"},
        {"FlareProjectile", "GrenadeProjectile"},
        {"FlareProjectileData", "GrenadeProjectileData"},
        {"FloorPlan", "SimObject"},
        {"FlyingVehicle", "Vehicle"},
        {"FlyingVehicleData", "VehicleData"},
        {"ForceFieldBare", "GameBase"},
        {"ForceFieldBareData", "GameBaseData"},
        {"GameBase", "SceneObject"},
        {"GameBaseData", "SimDataBlock"},
        {"GameConnection", "NetConnection"},
        {"GameTSCtrl", "GuiTSCtrl"},
        {"GrenadeProjectile", "Projectile"},
        {"GrenadeProjectileData", "ProjectileData"},
        {"GroundPlan", "SimObject"},
        {"GuiArrayCtrl", "GuiControl"},
        {"GuiAviBitmapCtrl", "GuiControl"},
        {"GuiBackgroundCtrl", "GuiControl"},
        {"GuiBitmapBorderCtrl", "GuiControl"},
        {"GuiBitmapCtrl", "GuiControl"},
        {"GuiBubbleTextCtrl", "GuiTextCtrl"},
        {"GuiButtonCtrl", "GuiControl"},
        {"GuiCanvas", "GuiControl"},
        {"GuiChannelVectorCtrl", "GuiMessageVectorCtrl"},
        {"GuiChatMenuTreeCtrl", "GuiTreeViewCtrl"},
        {"GuiCheckBoxCtrl", "GuiTextCtrl"},
        {"GuiChunkedBitmapCtrl", "GuiControl"},
        {"GuiConsole", "GuiArrayCtrl"},
        {"GuiConsoleEditCtrl", "GuiTextEditCtrl"},
        {"GuiConsoleTextCtrl", "GuiControl"},
        {"GuiControl", "SimGroup"},
        {"GuiControlListPopUp", "GuiPopUpMenuCtrl"},
        {"GuiControlProfile", "SimObject"},
        {"GuiCursor", "SimObject"},
        {"GuiEditCtrl", "GuiControl"},
        {"GuiFilterCtrl", "GuiControl"},
        {"GuiFrameSetCtrl", "GuiControl"},
        {"GuiInputCtrl", "GuiControl"},
        {"GuiInspector", "GuiControl"},
        {"GuiMLTextCtrl", "GuiControl"},
        {"GuiMLTextEditCtrl", "GuiMLTextCtrl"},
        {"GuiMessageVectorCtrl", "GuiControl"},
        {"GuiMouseEventCtrl", "GuiControl"},
        {"GuiNoMouseCtrl", "GuiControl"},
        {"GuiPopUpMenuCtrl", "GuiTextCtrl"},
        {"GuiProgressCtrl", "GuiControl"},
        {"GuiRadioCtrl", "GuiTextCtrl"},
        {"GuiScrollContentCtrl", "GuiControl"},
        {"GuiScrollCtrl", "GuiControl"},
        {"GuiServerBrowser", "ShellFancyArray"},
        {"GuiShapeNameHud", "GuiControl"},
        {"GuiSliderCtrl", "GuiControl"},
        {"GuiTSCtrl", "GuiControl"},
        {"GuiTargetManagerListCtrl", "GuiTextListCtrl"},
        {"GuiTerrPreviewCtrl", "GuiControl"},
        {"GuiTextCtrl", "GuiControl"},
        {"GuiTextEditCtrl", "GuiTextCtrl"},
        {"GuiTextEditSliderCtrl", "GuiTextEditCtrl"},
        {"GuiTextListCtrl", "GuiArrayCtrl"},
        {"GuiTreeView", "GuiArrayCtrl"},
        {"GuiTreeViewCtrl", "GuiArrayCtrl"},
        {"GuiVoteCtrl", "GuiControl"},
        {"GuiWindowCtrl", "GuiTextCtrl"},
        {"HTTPObject", "TCPObject"},
        {"HoverVehicle", "Vehicle"},
        {"HoverVehicleData", "VehicleData"},
        {"HudBarBaseCtrl", "HudBitmapFrameCtrl"}, // retail (RTTI); V12: HudCtrl
        {"HudBitmapCtrl", "HudCtrl"},
        {"HudBitmapFrameCtrl", "HudBitmapCtrl"},
        {"HudClockCtrl", "HudBitmapFrameCtrl"},
        {"HudCompassCtrl", "HudCtrl"},
        {"HudCrosshairCtrl", "HudBitmapCtrl"},
        {"HudCtrl", "GuiControl"},
        {"HudDamageCtrl", "HudBarBaseCtrl"},
        {"HudEnergyCtrl", "HudBarBaseCtrl"},
        {"HudHeat", "HudBarBaseCtrl"},
        {"HudZoom", "HudBitmapCtrl"},
        {"InteriorInstance", "SceneObject"},
        {"Item", "ShapeBase"},
        {"ItemData", "ShapeBaseData"},
        {"Lightning", "GameBase"},
        {"LightningData", "GameBaseData"},
        {"LinearFlareProjectile", "LinearProjectile"},
        {"LinearFlareProjectileData", "LinearProjectileData"},
        {"LinearProjectile", "Projectile"},
        {"LinearProjectileData", "ProjectileData"},
        {"Marker", "SceneObject"},
        {"MaterialPropertyMap", "SimObject"},
        {"MessageVector", "SimObject"},
        {"MissionArea", "NetObject"},
        {"MissionAreaEditor", "GuiBitmapCtrl"},
        {"MissionMarker", "ShapeBase"},
        {"MissionMarkerData", "ShapeBaseData"},
        {"NavigationGraph", "SimObject"},
        {"NetConnection", "SimGroup"},
        {"NetObject", "SimObject"},
        {"ParticleData", "SimDataBlock"},
        {"ParticleEmissionDummy", "GameBase"},
        {"ParticleEmissionDummyData", "GameBaseData"},
        {"ParticleEmitterData", "GameBaseData"},
        {"Path", "SimGroup"},
        {"PhysicalZone", "SceneObject"},
        {"Player", "ShapeBase"},
        {"PlayerData", "ShapeBaseData"},
        {"Precipitation", "GameBase"},
        {"PrecipitationData", "GameBaseData"},
        {"Projectile", "GameBase"},
        {"ProjectileData", "GameBaseData"},
        {"RepairProjectile", "Projectile"},
        {"RepairProjectileData", "ProjectileData"},
        {"SceneObject", "NetObject"},
        {"ScopeAlwaysShape", "StaticShape"},
        {"ScriptObject", "SimObject"},
        {"SeekerProjectile", "Projectile"},
        {"SeekerProjectileData", "ProjectileData"},
        {"SensorData", "SimDataBlock"},
        {"ShapeBase", "GameBase"},
        {"ShapeBaseData", "GameBaseData"},
        {"ShapeBaseImageData", "GameBaseData"},
        {"ShapeNameHud", "GuiControl"},
        {"ShellFancyArray", "GuiControl"},
        {"ShellFancyArrayScrollCtrl", "GuiControl"},
        {"ShellFancyTextList", "ShellFancyArray"},
        {"ShellScrollCtrl", "GuiScrollCtrl"},
        {"ShellTextEditCtrl", "GuiTextEditCtrl"},
        {"ShockLanceProjectile", "Projectile"},
        {"ShockLanceProjectileData", "ProjectileData"},
        {"Shockwave", "GameBase"},
        {"ShockwaveData", "GameBaseData"},
        {"ShowTSCtrl", "GuiTSCtrl"},
        {"SimDataBlock", "SimObject"},
        {"SimGroup", "SimSet"},
        {"SimSet", "SimObject"},
        {"SimpleNetObject", "NetObject"},
        {"Sky", "SceneObject"},
        {"SniperProjectile", "Projectile"},
        {"SniperProjectileData", "ProjectileData"},
        {"SpawnSphere", "MissionMarker"},
        {"Splash", "GameBase"},
        {"SplashData", "GameBaseData"},
        {"StaticShape", "ShapeBase"},
        {"StaticShapeData", "ShapeBaseData"},
        {"StationFXPersonal", "GameBase"},
        {"StationFXPersonalData", "GameBaseData"},
        {"StationFXVehicle", "GameBase"},
        {"StationFXVehicleData", "GameBaseData"},
        {"Sun", "NetObject"},
        {"TCPObject", "SimObject"},
        {"TSShapeConstructor", "SimDataBlock"},
        {"TSStatic", "SceneObject"},
        {"TargetProjectile", "Projectile"},
        {"TargetProjectileData", "ProjectileData"},
        {"Terraformer", "SimObject"},
        {"TerrainBlock", "SceneObject"},
        {"TerrainEditor", "EditTSCtrl"},
        {"TracerProjectile", "LinearProjectile"},
        {"TracerProjectileData", "LinearProjectileData"},
        {"Trigger", "GameBase"},
        {"TriggerData", "GameBaseData"},
        {"Turret", "StaticShape"},
        {"TurretData", "StaticShapeData"},
        {"TurretImageData", "ShapeBaseImageData"},
        {"Vehicle", "ShapeBase"},
        {"VehicleBlocker", "SceneObject"},
        {"VehicleData", "ShapeBaseData"},
        {"VirtualScrollContentCtrl", "GuiScrollContentCtrl"},
        {"VirtualScrollCtrl", "ShellScrollCtrl"},
        {"WaterBlock", "SceneObject"},
        {"WayPoint", "MissionMarker"},
        {"WheeledVehicle", "Vehicle"},
        {"WheeledVehicleData", "VehicleData"},
        {"WorldEditor", "EditTSCtrl"},
        // Retail-only console classes: their first console ancestor from the
        // retail binary's RTTI (egcs __tf<Class> -> __rtti_si(node, name, base)).
        {"AIStepEngage", "SimObject"},
        {"AIStepIdlePatrol", "SimObject"},
        {"AIStepJet", "SimObject"},
        {"CannedChatItem", "SimDataBlock"},
        {"EffectProfile", "SimDataBlock"},
        {"GuiBorderButtonCtrl", "GuiButtonBaseCtrl"},
        {"GuiButtonBaseCtrl", "GuiControl"},
        {"GuiCommanderMapButton", "GuiButtonCtrl"},
        {"GuiCommanderMapCheckbox", "GuiCheckBoxCtrl"},
        {"GuiCommanderMapPopupMenu", "GuiPopUpMenuCtrl"},
        {"GuiCommanderTV", "GameTSCtrl"},
        {"GuiDTSView", "GuiTSCtrl"},
        {"GuiDashBoardCtrl", "GuiControl"},
        {"GuiEmailBrowser", "ShellFancyArray"},
        {"GuiFadeinBitmapCtrl", "GuiBitmapCtrl"},
        {"GuiLoginPasswordCtrl", "ShellTextEditCtrl"},
        {"GuiMenuBar", "GuiControl"},
        {"HudBombSight", "HudCtrl"},
        {"HudCapacitor", "HudBarBaseCtrl"},
        {"HudChat", "HudBitmapFrameCtrl"},
        {"HudClock", "HudBitmapFrameCtrl"},
        {"HudCommandMsg", "HudBitmapFrameCtrl"},
        {"HudCompass", "HudCtrl"},
        {"HudCrosshair", "HudBitmapCtrl"},
        {"HudDamage", "HudBarBaseCtrl"},
        {"HudEnergy", "HudBarBaseCtrl"},
        {"HudFancyCtrl", "GuiControl"},
        {"HudHorzCtrl", "GuiControl"},
        {"HudInventory", "HudWeaponInvBase"},
        {"HudNavDisplay", "HudBitmapCtrl"},
        {"HudPulsingBitmap", "HudBitmapCtrl"},
        {"HudScoreCtrl", "HudCtrl"},
        {"HudVehicleWeapon", "HudWeaponInvBase"},
        {"HudWeaponInvBase", "HudCtrl"},
        {"HudWeapons", "HudWeaponInvBase"},
        {"SecureHTTPObject", "HTTPObject"},
        {"ShellAdCtrl", "GuiControl"},
        {"ShellChatMemberList", "GuiTextListCtrl"},
        {"ShellDlgFrame", "GuiTextCtrl"},
        {"ShellFieldCtrl", "GuiControl"},
        {"ShellPaneCtrl", "GuiTextCtrl"},
        {"ShellTabFrame", "GuiControl"},
        {"ShellTabGroupCtrl", "GuiControl"},
        {"ShellWindowCtrl", "GuiTextCtrl"},
    };
    return table;
}

inline bool isEngineClass(const std::string& name) {
    return name == "SimObject" || parents().count(name) != 0;
}

// The class and its ancestors, most derived first, ending at SimObject.
// The table is fixed, so each class's chain is built once.
inline const std::vector<std::string>& chain(const std::string& name) {
    static std::unordered_map<std::string, std::vector<std::string>, CaseHash, CaseEqual> cache;
    auto cached = cache.find(name);
    if (cached != cache.end()) return cached->second;
    std::vector<std::string> out;
    std::string current = name;
    for (int guard = 0; !current.empty() && guard < 32; ++guard) {
        out.push_back(current);
        auto it = parents().find(current);
        if (it == parents().end()) break;
        current = it->second;
    }
    if (out.empty() || !CaseEqual{}(out.back(), "SimObject")) out.push_back("SimObject");
    return cache.emplace(name, std::move(out)).first->second;
}

inline bool isA(const std::string& name, const std::string& base) {
    for (const auto& c : chain(name)) if (CaseEqual{}(c, base)) return true;
    return false;
}

} // namespace EngineClasses
