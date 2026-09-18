#include "game/mission_parser.h"

#include <cassert>

int main() {
    const auto objects = parseMisFile(R"(
        new AudioEmitter(MapWind) {
            position = "10 20 30";
            fileName = "fx/wind.wav";
            volume = "0.65";
            is3D = "1";
            isLooping = "1";
            minDistance = "4";
            maxDistance = "80";
        };
    )");
    assert(objects.size() == 1);
    assert(objects[0].className == "AudioEmitter");
    assert(getProp(objects[0].props, "filename") == "fx/wind.wav");
    assert(getProp(objects[0].props, "is3D") == "1");
    assert(getProp(objects[0].props, "isLooping") == "1");
    assert(getProp(objects[0].props, "minDistance") == "4");
    assert(getProp(objects[0].props, "maxDistance") == "80");

    const auto environment = parseMisFile(R"(
        new Sun() {
            direction = "0.57735 0.57735 -0.57735";
            color = "0.6 0.6 0.6 1";
            ambient = "0.2 0.2 0.2 1";
        };
         new Sky(Sky) {
             cloudHeightPer[0] = "0.349971";
             cloudSpeed1 = "0.0001";
             fogDistance = "300";
             fogVolume1 = "500 0 300";
             fogVolume2 = "0 0 0";
             fogColor = "0.85 0.38 0.1 1";
            materialList = "sky_lush_starrynight.dml";
        };
        new Precipitation(Precipitation) {
            position = "-7 5 0";
            dataBlock = "Snow";
            color1 = "1 1 1 1";
            minVelocity = "0.25";
            maxVelocity = "1.5";
            maxNumDrops = "2000";
            maxRadius = "125";
            offsetSpeed = "0.25";
        };
         new WaterBlock() {
            position = "-232 -184 13.106";
            scale = "352 320 100";
            liquidType = "CrustyLava";
             surfaceTexture = "LiquidTiles/Lava";
             surfaceOpacity = "1";
             envMapTexture = "desert/skies/d_n_move1";
             envMapIntensity = "0.4";
             shoreDepth = "6";
             shoreTexture = "LiquidTiles/shore";
        };
        new Lightning() {
            dataBlock = "DefaultStorm";
            strikesPerMinute = "9";
            strikeWidth = "5";
        };
    )");
    assert(environment.size() == 5);
    assert(getProp(environment[2].props, "maxnumdrops") == "2000");
    assert(getProp(environment[2].props, "maxradius") == "125");
    assert(getProp(environment[2].props, "minvelocity") == "0.25");
    assert(getProp(environment[1].props, "fogvolume1") == "500 0 300");
    assert(getProp(environment[3].props, "liquidtype") == "CrustyLava");
    assert(getProp(environment[3].props, "surfacetexture") == "LiquidTiles/Lava");
    assert(getProp(environment[3].props, "envmaptexture") == "desert/skies/d_n_move1");
    assert(getProp(environment[3].props, "envmapintensity") == "0.4");
    assert(getProp(environment[3].props, "shoredepth") == "6");
    assert(getProp(environment[4].props, "strikesperminute") == "9");

    const auto multipleWater = parseMisFile(R"(
        new WaterBlock(Ocean) {
            position = "-688 -1008 0";
            scale = "768 1152 71.4642";
            liquidType = "RiverWater";
            waveMagnitude = "1";
            surfaceOpacity = "0.8";
        };
        new WaterBlock(Lava) {
            position = "-224 -264 93.1568";
            scale = "768 608 27.0092";
            liquidType = "Lava";
            surfaceOpacity = "1";
        };
    )");
    assert(multipleWater.size() == 2);
    assert(getProp(multipleWater[0].props, "liquidtype") == "RiverWater");
    assert(getProp(multipleWater[1].props, "liquidtype") == "Lava");
    assert(getProp(multipleWater[0].props, "surfaceopacity") == "0.8");

    const auto lexical = parseMisFile(R"(
        // new Fake() { ignored = 1; };
        new	AudioEmitter(Ambience) {
            fileName = "audio//wind.wav";
        };
        datablock	ItemData(AmmoData) {
            shapeFile = "shapes/ammo.dts";
        };
    )");
    assert(lexical.size() == 2);
    assert(getProp(lexical[0].props, "filename") == "audio//wind.wav");
    assert(getProp(lexical[1].props, "shapefile") == "shapes/ammo.dts");

    const auto authored = parseMisFile(R"(
        new WayPoint() {
            position = "-4.16021 869.706 341.214";
            nameTag = "Goal";
            dataBlock = "WayPointMarker";
            name = "Goal";
            team = "1";
        };
        new ForceFieldBare() {
            position = "-420.839 433.798 126.019";
            rotation = "0 0 1 90";
            scale = "0.772095 6.3679 5.62782";
            dataBlock = "defaultTeamSlowFieldBare";
        };
        new SpawnSphere() {
            position = "1 2 3";
            radius = "60";
        };
    )");
    assert(authored.size() == 3);
    assert(authored[0].className == "WayPoint");
    assert(getProp(authored[0].props, "name") == "Goal");
    assert(getProp(authored[0].props, "datablock") == "WayPointMarker");
    assert(authored[1].className == "ForceFieldBare");
    assert(getProp(authored[1].props, "scale") == "0.772095 6.3679 5.62782");
    assert(getProp(authored[2].props, "radius") == "60");

    const auto grouped = parseMisFile(R"(
        new SimGroup(Teams) {
            new SimGroup(Team2) {
                new Item(Team2Flag) { dataBlock = "Flag"; };
            };
        };
    )");
    assert(grouped.size() == 3);
    assert(grouped[2].parentName == "Team2");
    assert(grouped[2].teamId == 2);

    const auto parity = parseMisFile(R"(
        new TSStatic(Wall) { shapeName = "props/wall.dts"; position = "1 2 3"; scale = "2 4 6"; };
        new StaticShape(Door) { datablock = "DoorData"; position = "4 5 6"; };
        new MissionMarker(Objective) { position = "7 8 9"; };
        new Trigger(Zone) { position = "10 11 12"; scale = "8 6 4"; };
        new PhysicalZone(Gravity) { velocityMod = "0.5"; gravityMod = "0.25"; appliedForce = "1 2 3"; active = "0"; };
    )");
    assert(parity.size() == 5);
    assert(getProp(parity[0].props, "shapename") == "props/wall.dts");
    assert(getProp(parity[1].props, "datablock") == "DoorData");
    assert(getProp(parity[3].props, "scale") == "8 6 4");
    assert(getProp(parity[4].props, "velocitymod") == "0.5");
    assert(getProp(parity[4].props, "appliedforce") == "1 2 3");

    const auto marker = authoredMissionMarker(parity[2]);
    assert(marker.position.x == 7.0f && marker.position.y == 8.0f && marker.position.z == 9.0f);
    assert(marker.rotation.z == 1.0f && marker.scale.x == 1.0f);
    assert(marker.label == "Objective" && marker.teamId == 0);

    const auto authoredObjective = parseMisFile(R"(
        new SimGroup(Team2) {
            new AIObjective(AIOAttackObject) {
                position = "7 8 9";
                description = "Attack the GeneratorLarge";
                mode = "Destroy";
                targetObject = "Generator3";
                targetObjectId = "3234";
                weightLevel1 = "3100";
                offense = "1";
                defense = "0";
                locked = "0";
                isInvalid = "0";
            };
        };
        new NavigationGraph(NavGraph) {
            GraphFile = "Training2.nav";
            customArea = "-10 -20 30 40";
            conjoinAngleDev = "65";
            cullDensity = "0.1";
        };
    )");
    assert(authoredObjective.size() == 3);
    const auto objective = authoredMissionObjective(authoredObjective[2]);
    assert(objective.marker.teamId == 2);
    assert(objective.description == "Attack the GeneratorLarge");
    assert(objective.targetObject == "Generator3" && objective.targetObjectId == 3234);
    assert(objective.weight[0] == 3100.0f && objective.offense && !objective.defense);
    const auto graph = authoredNavigationGraph(authoredObjective[1]);
    assert(graph.graphFile == "Training2.nav" && graph.customArea.x == -10.0f);
    assert(graph.customAreaWidth == 30.0f && graph.customAreaHeight == 40.0f);

    const auto objectFlags = parseMisFile(R"(
        new Marker(EditorMarker) { hidden = "1"; };
        new TSStatic(Glass) { visible = "0"; disableCollision = "1"; sequence = "open"; };
        new StaticShape(NoCollision) { collisionType = "None"; };
    )");
    assert(objectFlags.size() == 3);
    assert(!authoredVisible(objectFlags[0]));
    assert(!authoredVisible(objectFlags[1]) &&
           !authoredCollidable(objectFlags[1], true));
    assert(!authoredCollidable(objectFlags[2], true));
    assert(authoredSequence(objectFlags[1]) == "open");
    assert(authoredMissionMarker(objectFlags[0]).label == "EditorMarker");

    const auto spawns = parseMisFile(R"(
        new SpawnSphere(Team2Late) { position = "30 0 0"; team = "2"; radius = "20"; };
        new SpawnSphere(Alpha) { position = "10 0 0"; radius = "10"; };
        new SpawnSphere(Team1Early) { position = "20 0 0"; team = "1"; scale = "2 3 4"; };
    )");
    const MisObject* teamSpawn = selectAuthoredSpawn(spawns, 1);
    assert(teamSpawn && teamSpawn->objName == "Team1Early");
    assert(authoredMissionMarker(*teamSpawn).scale.z == 4.0f);
    const MisObject* neutralSpawn = selectAuthoredSpawn(spawns, 3);
    assert(neutralSpawn && neutralSpawn->objName == "Alpha");

    // interiorTest is an interior-only preview mission. It deliberately has
    // no TerrainBlock, so loading it must not manufacture a terrain failure.
    auto interiorPreview = parseMisFile(R"(
        new SimGroup(MissionGroup) {
            new MissionArea(MissionArea) { area = "-896 -488 1520 1152"; };
            new InteriorInstance(TestObject) { interiorFile = $TestObjectFileName; };
        };
    )");
    assert(findObject(interiorPreview, "TerrainBlock") == nullptr);
    assert(findObject(interiorPreview, "InteriorInstance") != nullptr);
    return 0;
}
