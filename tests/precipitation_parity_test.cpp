#include "game/precipitation_parity.h"

#include <cassert>
#include <cmath>
#include <limits>

int main() {
    assert(precipitationDelta(-1.0f) == 0.0f);
    assert(precipitationDelta(0.0f) == 0.0f);
    assert(precipitationDelta(std::numeric_limits<float>::quiet_NaN()) == 0.0f);
    assert(precipitationDelta(0.02f) == 0.02f);
    assert(precipitationDelta(1.0f) == 0.05f);
    const Point3F adjusted = precipitationWindAdjustedVelocity(
        {2.0f, -5.0f, 3.0f}, {1.0f, 0.5f, 2.0f}, {-4.0f, 1.5f, 6.0f});
    assert(adjusted.x == -4.0f && adjusted.y == -4.0f && adjusted.z == 6.0f);
    const Point3F sanitizedVelocity = precipitationWindAdjustedVelocity(
        {2.0f, std::numeric_limits<float>::quiet_NaN(), 3.0f},
        {1.0f, 0.5f, 2.0f},
        {std::numeric_limits<float>::infinity(), 1.5f, 6.0f});
    assert(sanitizedVelocity.x == 0.0f && sanitizedVelocity.y == 1.0f &&
           sanitizedVelocity.z == 6.0f);
    const Point3F moved = precipitationAdvancedPosition(
        {10.0f, 20.0f, 30.0f}, {-4.0f, -8.0f, 6.0f}, 0.25f);
    assert(moved.x == 9.0f && moved.y == 18.0f && moved.z == 31.5f);
    const Point3F sanitizedPosition = precipitationAdvancedPosition(
        {10.0f, std::numeric_limits<float>::quiet_NaN(), 30.0f},
        {-4.0f, -8.0f, std::numeric_limits<float>::infinity()},
        0.25f);
    assert(std::isfinite(sanitizedPosition.x) &&
           std::isfinite(sanitizedPosition.y) &&
           std::isfinite(sanitizedPosition.z));
    assert(std::fabs(sanitizedPosition.x - 9.0f) < 0.00001f);
    assert(std::fabs(sanitizedPosition.y + 2.0f) < 0.00001f);
    assert(std::fabs(sanitizedPosition.z - 30.0f) < 0.00001f);
    assert(precipitationRecenterCoordinate(5.0f, 1005.0f, 100.0f) == 1005.0f);
    assert(precipitationRecenterCoordinate(1005.0f, 5.0f, 100.0f) == 5.0f);
    assert(precipitationRecenterCoordinate(5.0f, 1005.0f, 0.0f) == 5.0f);
    return 0;
}
