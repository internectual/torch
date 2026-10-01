#pragma once
// Server weather objects: Lightning (game/fx/lightning.cc: the storm ghost,
// its strikes and the LightningStrikeEvent) and Precipitation
// (game/fx/precipitation.cc: the precipitation ghost and its storms).
class TorqueScript;

void registerLightningNatives(TorqueScript& ts);
void registerPrecipitationNatives(TorqueScript& ts);
