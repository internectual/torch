// Round trip of the 'misc' datablock writers through Torch's reader.
#include "sim/datablock_pack.h"
#include "tests/datablock_pack_harness.h"
#include <cmath>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

// Classes whose packData writes nothing: packRoundTrip hands the reader a
// null buffer for an empty write (V12BitStream then fails), so read these
// back from a non-null empty buffer instead.
PackRoundTrip emptyRoundTrip(const std::string& className,
                             const std::map<std::string, std::string>& fields) {
    ScriptObject object;
    object.className = className;
    for (const auto& [key, value] : fields) object.fields[key] = VMValue(value);
    TorqueBitWriter w;
    w.setStringBuffer(true);
    DataBlockPack::Context context(&object, w, [](const std::string&) { return 0u; });
    const DataBlockPack::PackFn* pack = DataBlockPack::find(className);
    PackRoundTrip result;
    if (!pack) return result;
    (*pack)(context);
    result.writtenBits = w.bitPosition();
    static const uint8_t none[1] = {0};
    V12BitStream stream(none, 0);
    stream.setStringBuffer(true);
    const int index = DataBlockPack::classIndex(className);
    if (index < 0) return result;
    result.readOk = V12::readDataBlockPayload(stream, (size_t)index + 128, &result.decoded) && !stream.failed();
    result.consumedBits = stream.position();
    return result;
}

bool writesNothing(const std::string& className) {
    return className == "SensorData" || className == "GameBaseData" || className == "SimDataBlock";
}

PackRoundTrip exactRoundTrip(const std::string& className,
                             const std::map<std::string, std::string>& fields,
                             const std::map<std::string, uint32_t>& refs = {}) {
    PackRoundTrip r = writesNothing(className) ? emptyRoundTrip(className, fields)
                                               : packRoundTrip(className, fields, refs);
    if (writesNothing(className)) check(r.writtenBits == 0, (className + " writes no bits").c_str());
    check(r.exact(), (className + (fields.empty() ? " defaults" : " retail fields")).c_str());
    return r;
}

} // namespace

int main() {
    DataBlockPack::registerAll();

    // Every class with no fields (engine constructor defaults).
    for (const char* cls : {"AudioDescription", "AudioProfile", "AudioEnvironment",
                            "AudioSampleEnvironment", "SensorData", "TriggerData",
                            "ForceFieldBareData", "CommanderIconData", "CannedChatItem",
                            "GameBaseData", "SimDataBlock", "TSShapeConstructor"})
        exactRoundTrip(cls, {});

    {   // Defaults decode as the constructor's.
        auto r = packRoundTrip("AudioDescription", {});
        check(near(r.decoded.audioVolume, 1.0f) && !r.decoded.audioLooping && !r.decoded.audioIs3D,
              "AudioDescription default decode");
        auto f = packRoundTrip("ForceFieldBareData", {});
        check(f.decoded.forceFieldFadeMS == 1000 && near(f.decoded.forceFieldBaseTranslucency, 0.65f) &&
              f.decoded.forceFieldNumFrames == 5 && f.decoded.forceFieldFramesPerSec == 10,
              "ForceFieldBareData default decode");
    }

    {   // serveraudio.cs AudioClose3d ($EffectAudioType = 3).
        auto r = exactRoundTrip("AudioDescription", {{"volume", "1.0"}, {"isLooping", "false"},
            {"is3D", "true"}, {"minDistance", "10.0"}, {"MaxDistance", "60.0"}, {"type", "3"},
            {"environmentLevel", "1.0"}});
        check(r.decoded.audioIs3D && near(r.decoded.audioMinDistance, 10.0f) &&
              near(r.decoded.audioMaxDistance, 60.0f) && !r.decoded.audioLooping,
              "AudioClose3d decode");
    }
    {   // serveraudio.cs ProjectileLooping3d, plus loop gaps and a cone.
        auto r = exactRoundTrip("AudioDescription", {{"volume", "0.5"}, {"isLooping", "true"},
            {"is3D", "true"}, {"minDistance", "5.0"}, {"MaxDistance", "20.0"}, {"type", "3"},
            {"environmentLevel", "1.0"}, {"loopCount", "4"}, {"minLoopGap", "100"},
            {"maxLoopGap", "250"}, {"coneInsideAngle", "90"}, {"coneOutsideAngle", "180"},
            {"coneOutsideVolume", "0.5"}, {"coneVector", "1 0 0"}});
        check(r.decoded.audioLooping && r.decoded.audioLoopCount == 4 &&
              r.decoded.audioMinLoopGapMs == 100 && r.decoded.audioMaxLoopGapMs == 250 &&
              near(r.decoded.audioVolume, 31.0f / 63.0f) && near(r.decoded.audioMaxDistance, 20.0f),
              "ProjectileLooping3d decode");
    }
    {   // weapons/disc.cs DiscFireSound.
        auto r = exactRoundTrip("AudioProfile", {{"filename", "fx/weapons/spinfusor_fire.wav"},
            {"description", "AudioDefault3d"}, {"preload", "true"}, {"effect", "DiscFireEffect"}},
            {{"AudioDefault3d", 40}, {"DiscFireEffect", 41}});
        // Torch's reader keeps datablock refs as the wire value (id - 3), the
        // same numbering SimDataBlockEvent's object id uses.
        check(r.decoded.audioDescriptionRef == 40 - 3 && r.decoded.audioEffectRef == 41 - 3 &&
              r.decoded.audioEnvironmentRef == 0 &&
              r.decoded.audioFilename == "fx/weapons/spinfusor_fire.wav", "DiscFireSound decode");
    }
    {   // An AudioProfile with a sample environment and no ".wav" suffix.
        auto r = exactRoundTrip("AudioProfile", {{"fileName", "fx/misc/thunder"},
            {"description", "ThunderDescription"}, {"environment", "SomeSampleEnv"}},
            {{"ThunderDescription", 2050}, {"SomeSampleEnv", 5}});
        check(r.decoded.audioDescriptionRef == 2050 - 3 &&
              r.decoded.audioEffectRef == 0 && r.decoded.audioEnvironmentRef == 5 - 3 &&
              r.decoded.audioFilename == "fx/misc/thunder.wav",
              "AudioProfile environment decode");
    }
    // environmentals.cs: Underwater / BigRoom.
    exactRoundTrip("AudioEnvironment", {{"useRoom", "true"}, {"room", "UNDERWATER"}, {"effectVolume", "0.6"}});
    exactRoundTrip("AudioEnvironment", {{"useRoom", "true"}, {"room", "AUDITORIUM"}, {"effectVolume", "0.4"}});
    // Custom (non-preset) environment.
    exactRoundTrip("AudioEnvironment", {{"useRoom", "false"}, {"roomHF", "-500"}, {"reflections", "-2000"},
        {"reverb", "200"}, {"roomRolloffFactor", "1.5"}, {"decayTime", "2.9"}, {"decayHFRatio", "0.8"},
        {"reflectionsDelay", "0.02"}, {"reverbDelay", "0.03"}, {"roomVolume", "-1000"},
        {"effectVolume", "0.5"}, {"damping", "0.5"}, {"environmentSize", "7.5"},
        {"environmentDiffusion", "1.0"}, {"airAbsorption", "-5"}, {"flags", "63"}});
    // No retail AudioSampleEnvironment exists; typical EAX occlusion values.
    exactRoundTrip("AudioSampleEnvironment", {{"direct", "-100"}, {"directHF", "-2000"}, {"room", "0"},
        {"roomHF", "-300"}, {"obstruction", "0.5"}, {"obstructionLFRatio", "0.25"},
        {"occlusion", "0.75"}, {"occlusionLFRatio", "0.25"}, {"occlusionRoomRatio", "1.5"},
        {"roomRolloff", "0"}, {"airAbsorption", "1"}, {"outsideVolumeHF", "0"}, {"flags", "7"}});
    // deployables.cs DeployMotionSensorObj (SensorData packs nothing).
    exactRoundTrip("SensorData", {{"detects", "true"}, {"detectsUsingLOS", "true"},
        {"detectsActiveJammed", "false"}, {"detectsPassiveJammed", "true"}, {"detectsCloaked", "true"},
        {"detectionPings", "false"}, {"detectMinVelocity", "2"}, {"detectRadius", "60"}});
    // flagTrigger.
    exactRoundTrip("TriggerData", {{"tickPeriodMS", "10"}});
    {   // forcefield.cs defaultForceFieldBare.
        auto r = exactRoundTrip("ForceFieldBareData", {{"fadeMS", "1000"}, {"baseTranslucency", "0.30"},
            {"powerOffTranslucency", "0.0"}, {"teamPermiable", "false"}, {"otherPermiable", "false"},
            {"color", "0.0 0.55 0.99"}, {"powerOffColor", "0.0 0.0 0.0"},
            {"targetNameTag", "Force Field"}, {"targetTypeTag", "ForceField"},
            {"texture[0]", "skins/forcef1"}, {"texture[1]", "skins/forcef2"},
            {"texture[2]", "skins/forcef3"}, {"texture[3]", "skins/forcef4"},
            {"texture[4]", "skins/forcef5"}, {"framesPerSec", "10"}, {"numFrames", "5"},
            {"scrollSpeed", "15"}, {"umapping", "1.0"}, {"vmapping", "0.15"}});
        const auto& d = r.decoded;
        check(d.hasForceField && d.forceFieldFadeMS == 1000 && near(d.forceFieldBaseTranslucency, 0.30f) &&
              near(d.forceFieldScrollSpeed, 15.0f) && near(d.forceFieldVMapping, 0.15f) &&
              near(d.forceFieldColor[1], 140.0f / 255.0f) && near(d.forceFieldColor[3], 1.0f) &&
              d.forceFieldTextures.size() == 5 && d.forceFieldTextures[0] == "skins/forcef1" &&
              d.forceFieldTextures[4] == "skins/forcef5", "defaultForceFieldBare decode");
    }
    // commanderprofiles.cs CMDPlayerIcon.
    exactRoundTrip("CommanderIconData", {{"baseImage", "static com_player_grey_24x false true"},
        {"selectImage", "static com_player_grey_24x_glow true true"},
        {"hilightImage", "static com_player_grey_24x_glow true true"}});
    // CannedChatItem (retail chat menu entries are built at runtime; typical fields).
    exactRoundTrip("CannedChatItem", {{"name", "ChatSelfAttack"}, {"text", "I will attack."},
        {"audioFile", "slf.att.attack"}, {"animation", "ATTACK"}, {"teamOnly", "true"}});
    // SimDataBlock damage profile / GameBaseData.
    exactRoundTrip("SimDataBlock", {{"shieldDamageScale[1]", "1.75"}, {"shieldDamageScale[2]", "1.75"}});
    exactRoundTrip("GameBaseData", {{"catagory", "Misc"}, {"className", "Foo"}});
    {   // heavy_male.cs HeavyMaleDts (first sequences) plus a sparse index.
        auto r = exactRoundTrip("TSShapeConstructor", {{"baseShape", "heavy_male.dts"},
            {"sequence0", "heavy_male_root.dsq root"}, {"sequence1", "heavy_male_forward.dsq run"},
            {"sequence2", "heavy_male_back.dsq back"}, {"sequence3", "heavy_male_side.dsq side"},
            {"sequence4", "heavy_male_lookde.dsq look"}, {"sequence5", "heavy_male_head.dsq head"},
            {"sequence6", "heavy_male_fall.dsq fall"}, {"sequence7", "heavy_male_jet.dsq jet"},
            {"sequence8", "heavy_male_land.dsq land"}, {"sequence9", "heavy_male_jump.dsq jump"},
            {"sequence[35]", "heavy_male_looknw.dsq looknw"}});
        const auto& d = r.decoded;
        check(d.constructorShape == "heavy_male.dts" && d.constructorSequences.size() == 11 &&
              d.constructorSequences[0] == "heavy_male_root.dsq root" &&
              d.constructorSequences[9] == "heavy_male_jump.dsq jump" &&
              d.constructorSequences[10] == "heavy_male_looknw.dsq looknw", "HeavyMaleDts decode");
    }

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("datablock_pack_misc_test: ok\n");
    return 0;
}
