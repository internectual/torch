#pragma once
// ProjectileData::calculateAim (projectile.cc and the retail proj*.cc):
// the direction to fire a datablock's projectile from a source to hit a
// moving target, and the flight times; false when it cannot reach. The
// engine's polynomial solvers (math/mSolver.cc) it needs.
#include "core/math.h"
#include <cstdint>

struct ScriptObject;

namespace ProjectileAim {

uint32_t solveQuadratic(float a, float b, float c, float* x);
uint32_t solveCubic(float a, float b, float c, float d, float* x);
uint32_t solveQuartic(float a, float b, float c, float d, float e, float* x);

// Dispatches on the datablock's class (LinearProjectileData, Grenade...,
// Energy..., Seeker..., Sniper..., Target..., ELF..., ShockLance...,
// Repair...); any other class cannot be aimed.
bool calculateAim(ScriptObject* data, const Point3F& targetPos, const Point3F& targetVel,
                  const Point3F& sourcePos, const Point3F& sourceVel, Point3F* outputVectorMin,
                  float* outputMinTime, Point3F* outputVectorMax, float* outputMaxTime);

} // namespace ProjectileAim
