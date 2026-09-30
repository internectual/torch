// ai/graphMath.cc, math/mBox.cc Box3F::collideLine.
#include "ai/graph_math.h"

using namespace NavMath;

namespace NavMath {

bool boxCollideLine(const Box3F& box, const Point3F& start, const Point3F& end, float* t, Point3F* n) {
    float st, et, fst = 0, fet = 1;
    const float bmin[3] = {box.min.x, box.min.y, box.min.z};
    const float bmax[3] = {box.max.x, box.max.y, box.max.z};
    const float si[3] = {start.x, start.y, start.z};
    const float ei[3] = {end.x, end.y, end.z};
    const Point3F na[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Point3F finalNormal{0, 0, 0};
    for (int i = 0; i < 3; i++) {
        Point3F normal = na[i];
        if (si[i] < ei[i]) {
            if (si[i] > bmax[i] || ei[i] < bmin[i]) return false;
            const float di = ei[i] - si[i];
            st = (si[i] < bmin[i]) ? (bmin[i] - si[i]) / di : 0;
            et = (ei[i] > bmax[i]) ? (bmax[i] - si[i]) / di : 1;
            normal = -normal;
        } else {
            if (ei[i] > bmax[i] || si[i] < bmin[i]) return false;
            const float di = ei[i] - si[i];
            st = (si[i] > bmax[i]) ? (bmax[i] - si[i]) / di : 0;
            et = (ei[i] < bmin[i]) ? (bmin[i] - si[i]) / di : 1;
        }
        if (st > fst) {
            fst = st;
            finalNormal = normal;
        }
        if (et < fet) fet = et;
        if (fet < fst) return false;
    }
    if (t) *t = fst;
    if (n) *n = finalNormal;
    return true;
}

float solveForZ(const PlaneF& plane, const Point3F& point) {
    return (-plane.d - plane.x * point.x - plane.y * point.y) / plane.z;
}

// Lifted from tools/morian/CSGBrush.cc; doubles for insurance.
bool intersectPlanes(const PlaneF& p, const PlaneF& q, const PlaneF& r, Point3F* pOut) {
    const double p1x = p.x, p1y = p.y, p1z = p.z;
    const double p2x = q.x, p2y = q.y, p2z = q.z;
    const double p3x = r.x, p3y = r.y, p3z = r.z;
    const double d1 = p.d, d2 = q.d, d3 = r.d;
    const double bc = (p2y * p3z) - (p3y * p2z);
    const double ac = (p2x * p3z) - (p3x * p2z);
    const double ab = (p2x * p3y) - (p3x * p2y);
    const double det = (p1x * bc) - (p1y * ac) + (p1z * ab);
    // Parallel planes
    if (std::fabs(det) < 1e-5) return false;
    const double dc = (d2 * p3z) - (d3 * p2z);
    const double db = (d2 * p3y) - (d3 * p2y);
    const double ad = (d3 * p2x) - (d2 * p3x);
    const double detInv = 1.0 / det;
    pOut->x = (float)(((p1y * dc) - (d1 * bc) - (p1z * db)) * detInv);
    pOut->y = (float)(((d1 * ac) - (p1x * dc) - (p1z * ad)) * detInv);
    pOut->z = (float)(((p1y * ad) + (p1x * db) - (d1 * ab)) * detInv);
    return true;
}

} // namespace NavMath

// ---------------------------------------------------------------------------
// GridArea

// for (area.start(step); ...; area.step(step))
bool GridArea::start(Point2I& p) const {
    if (isValidRect()) {
        p = point;
        return true;
    }
    return false;
}

bool GridArea::step(Point2I& p) const {
    if (++p.x >= point.x + extent.x) {
        p.x = point.x;
        if (++p.y >= point.y + extent.y) return false;
    }
    return true;
}

// RectI::intersect
bool GridArea::intersect(const GridArea& clip) {
    const int32_t bx = std::min(point.x + extent.x - 1, clip.point.x + clip.extent.x - 1);
    const int32_t by = std::min(point.y + extent.y - 1, clip.point.y + clip.extent.y - 1);
    point.x = std::max(point.x, clip.point.x);
    point.y = std::max(point.y, clip.point.y);
    extent.x = bx - point.x + 1;
    extent.y = by - point.y + 1;
    return isValidRect();
}

// ---------------------------------------------------------------------------
// GridVisitor

GridVisitor::GridVisitor(const GridArea& area) : mArea(area.point, area.extent) { mPostCheck = mPreCheck = true; }

// The default Before and After callbacks just remove themselves.
bool GridVisitor::beforeDivide(const GridArea&, int32_t) { return !(mPreCheck = false); }
bool GridVisitor::atLevelZero(const GridArea&) { return true; }
bool GridVisitor::afterDivide(const GridArea&, int32_t, bool) { return !(mPostCheck = false); }

// R is a box of width 2^L, aligned on a like boundary.
bool GridVisitor::recurse(GridArea R, int32_t L) {
    bool success = true;
    if (!R.overlaps(mArea)) {
        success = false;
    } else if (L == 0) {
        success = atLevelZero(R);
    } else if (mPreCheck && mArea.contains(R) && !beforeDivide(R, L)) {
        success = false;
    } else {
        const int32_t half = 1 << (L - 1);
        for (int32_t y = half; y >= 0; y -= half)
            for (int32_t x = half; x >= 0; x -= half)
                if (!recurse(GridArea(R.point.x + x, R.point.y + y, half, half), L - 1)) success = false;
        if (mPostCheck && mArea.contains(R) && !afterDivide(R, L, success)) success = false;
    }
    return success;
}

bool GridVisitor::traverse() {
    int32_t level = 1;
    GridArea powerTwoRect(-1, -1, 2, 2);
    // The power-of-two-sized rect that encloses the grid.
    while (!powerTwoRect.contains(mArea)) {
        if (++level < 31) {
            powerTwoRect.point *= 2;
            powerTwoRect.extent *= 2;
        } else {
            return false;
        }
    }
    return recurse(powerTwoRect, level);
}

// ---------------------------------------------------------------------------
// LineSegment: distance from the segment (from the endpoints when the
// projection falls outside); leaves the closest point in soln.

float LineSegment::distance(const Point3F& p) {
    Point3F vec1 = b - a, vec2 = p - a;
    const float l = len(vec1);
    float dist = len(vec2);
    soln = a;
    if (l > 0.001f) {
        const float dot = mDot(vec1 *= (1 / l), vec2);
        if (dot >= 0) {
            if (dot <= l) {
                float sideSquared = (dist * dist) - (dot * dot);
                if (sideSquared <= 0) dist = sideSquared = 0;
                else dist = std::sqrt(sideSquared);
                soln += (vec1 * dot);
            } else {
                soln = b;
                dist = len(b - p);
            }
        }
    }
    return dist;
}

// 3D check, then a 2D check against the closest point.
bool LineSegment::botDistCheck(const Point3F& p, float dist3, float dist2) {
    const float d3 = distance(p);
    if (d3 < dist3) {
        const float dx = soln.x - p.x, dy = soln.y - p.y;
        const float d2 = dx * dx + dy * dy;
        dist2 *= dist2;
        return d2 < dist2;
    }
    return false;
}

// ---------------------------------------------------------------------------
// LineStepper

void LineStepper::init(const Point3F& a, const Point3F& b) {
    solution = A = a;
    B = b;
    total = len(dir1 = B - A);
    soFar = advance = 0.0f;
    if (total > 0.00001f) {
        dir1 *= (1 / total);
    } else {
        total = 0.0f;
        dir1 = {0, 0, 0};
    }
}

// The intersection with the sphere on the way out (the second of two when
// starting outside).
float LineStepper::getOutboundIntersection(const SphereF& S) {
    const float project = mDot(S.center - A, dir1);
    const Point3F nearPoint = A + (dir1 * project);
    if (!S.isContained(nearPoint)) return -1.0f;
    const float length = std::sqrt(S.radius * S.radius - lenSquared(nearPoint - S.center));
    solution = nearPoint + dir1 * length;
    return (advance = length + project);
}

const Point3F& LineStepper::advanceToSolution() {
    if (advance != 0.0f) {
        soFar += advance;
        A = solution;
        advance = 0.0f;
    }
    return solution;
}
