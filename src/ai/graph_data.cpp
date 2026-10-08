// ai/graphData.cc (the run-time reads and queries) and the run-time parts of
// ai/graphVolume.cc.
#include "ai/graph_data.h"
#include "core/console.h"

using namespace NavMath;

NavGraphGlobals gNavGlobs;

//    4  6  7        How the numbers 0 to 7 translate into
//    2  N  5        the eight surrounding grid locations.
//    0  1  3
const Point2I TerrainGraphInfo::gridOffs[8] = {
    {-1, -1},
    {0, -1}, {-1, 0},
    {1, -1}, {-1, 1},
    {1, 0}, {0, 1},
    {1, 1}};

//-------------------------------------------------------------------------------------

bool GraphEdgeInfo::read(GraphStream& s) {
    for (int i = 0; i < 2; i++) {
        uint32_t mask;
        if (!s.read(&mask)) return false;
        to[i].flags = mask;
        if (!(s.read(&to[i].res) && s.read(&to[i].dest))) return false;
    }
    return s.read(&segPoints[0]) && s.read(&segPoints[1]) && s.read(&segNormal);
}

bool IndoorNodeInfo::read(GraphStream& s) {
    uint32_t mask;
    if (!s.read(&mask)) return false;
    flags = mask;
    return s.read(&unused) && s.read(&antecedent) && s.read(&pos);
}

bool OutdoorNodeInfo::read(GraphStream& s) {
    return s.read(&level) && s.read(&flags) && s.read(&height) && s.read(&x) && s.read(&y);
}

//-------------------------------------------------------------------------------------

void SpawnLocations::reset() {
    clear();
    mSpheres.clear();
    mRes0 = 0;
}

bool SpawnLocations::read(GraphStream& s) {
    if (s.read(&mRes0))
        if (readVector2(s, *this)) return readVector1(s, mSpheres, 29);
    return false;
}

bool SpawnLocations::Sphere::read(GraphStream& s) {
    return s.read(&mRes0) && s.read(&mSphere.center) && s.read(&mSphere.radius) && s.read(&mInside) &&
           s.read(&mCount) && s.read(&mOffset);
}

// A random location from the pre-computed data: the closest sphere of the
// wanted kind.
int32_t SpawnLocations::getRandom(const SphereF& sphere, bool inside, uint32_t rnd) {
    const Sphere* closest = nullptr;
    float minDist = 1e22f, d;
    for (const Sphere& S : mSpheres)
        if (S.mInside == inside && S.mCount > 0)
            if ((d = lenSquared(S.mSphere.center - sphere.center)) < minDist) {
                closest = &S;
                minDist = d;
            }
    if (closest) {
        const uint32_t count = (uint32_t)closest->mCount;
        return closest->mOffset + (int32_t)((rnd & 0x7FFFFF) % count);
    }
    return -1;
}

void SpawnLocations::printInfo() const {
    Console::instance().printf(LogLevel::Info, "--- %d Spheres were generated", (int)mSpheres.size());
    for (size_t i = 0; i < mSpheres.size(); i++) {
        const Sphere& S = mSpheres[i];
        const Point3F& P = S.mSphere.center;
        Console::instance().printf(LogLevel::Info, "%d : %s at center (%f, %f, %f) - generated %d", (int)i + 1,
                                   S.mInside ? "Inside" : "Outside", P.x, P.y, P.z, S.mCount);
    }
}

//-------------------------------------------------------------------------------------

bool GraphBridgeData::read(GraphStream& s) {
    return s.read(&nodes[0]) && s.read(&nodes[1]) && s.read(&jetClear) && s.read(&howTo);
}

bool BridgeDataList::read(GraphStream& s) {
    mSaveTotal = 0;
    if (readVector1(s, *this, 6)) {
        mSaveTotal = (int32_t)size();
        // What accumEdgeCounts (sizing makeRunTimeNodes' edge pool) tallies.
        mReplaced[0] = mReplaced[1] = 0;
        for (const GraphBridgeData& b : *this)
            if (b.isReplacement()) mReplaced[b.isUnreachable() != 0]++;
        return true;
    }
    return false;
}

//-------------------------------------------------------------------------------------
// Volumes

bool GraphVolumeList::read(GraphStream& s) { return readVector1(s, *this, 8) && readVector2(s, mPlanes); }

bool GraphVolumeList::closestPoint(int32_t ind, const Point3F& point, Point3F& soln) const {
    std::vector<Point3F> corners;
    bool foundIt = false;
    const PlaneF floor = floorPlane(ind);
    if (intersectWalls(ind, floor, corners)) {
        const size_t numCorners = corners.size();
        float minDist = 1e13f;
        corners.push_back(corners[0]);
        for (size_t i = 0; i < numCorners; i++) {
            LineSegment segment(corners[i], corners[i + 1]);
            const float D = segment.distance(point);
            if (D < minDist) {
                foundIt = true;
                minDist = D;
                soln = segment.solution();
                soln.z = point.z;
            }
        }
    }
    return foundIt;
}

bool GraphVolumeList::intersectWalls(int32_t i, const PlaneF& with, std::vector<Point3F>& list) const {
    bool noneWereParallel = true;
    const PlaneF* planes = planeArray(i);
    const int32_t N = planeCount(i) - 2;
    for (int32_t w = 0; w < N; w++) {
        Point3F point;
        if (intersectPlanes(with, planes[w], planes[(w + 1) % N], &point))
            list.push_back(point);
        else
            // Parallel planes occasionally happen; the corners only feed
            // bounding boxes, so continue but return false.
            noneWereParallel = false;
    }
    return noneWereParallel;
}

int32_t GraphVolumeList::getCorners(int32_t ind, std::vector<Point3F>& points, bool justFloor) const {
    points.clear();
    intersectWalls(ind, floorPlane(ind), points);
    if (!justFloor) intersectWalls(ind, abovePlane(ind), points);
    return (int32_t)points.size();
}

// Minimum over the walls of the maximum distance of the floor corners in
// from that wall.
float GraphVolumeList::getMinExt(int32_t ind, std::vector<Point3F>& ptBuffer) const {
    ptBuffer.clear();
    if (intersectWalls(ind, floorPlane(ind), ptBuffer)) {
        const PlaneF* planes = planeArray(ind);
        const int32_t N = planeCount(ind) - 2;
        float minD = 1e9f;
        for (int32_t w = 0; w < N; w++) {
            float maxD = -10, d;
            for (const Point3F& p : ptBuffer)
                if ((d = -distToPlane(planes[w], p)) > maxD) maxD = d;
            if (maxD < minD) minD = maxD;
        }
        return minD;
    }
    return 0.5f;
}

//-------------------------------------------------------------------------------------
// LOS Hash Table

uint32_t LOSHashTable::value(int32_t i1, int32_t i2) const {
    if (i1 == i2) return FullLOS;
    uint16_t from, to, seg;
    if (i1 < i2) {       // Lookup goes UP
        from = (uint16_t)i1;
        seg = (uint16_t)((to = (uint16_t)i2) >> SegShift);
    } else {
        from = (uint16_t)i2;
        seg = (uint16_t)((to = (uint16_t)i1) >> SegShift);
    }
    Key key;
    key.mNode = from;
    key.mSeg = seg;
    const uint32_t hash = calcHash(key);
    // The bucket "walk" (the U16 table math as the engine does it).
    if (int32_t bucketSize = int32_t(mTable[hash + 1]) - int32_t(mTable[hash])) {
        const Segment* s = &mSegments[mTable[hash]];
        while (--bucketSize >= 0) {
            if (s->mKey.compare() == key.compare()) return s->value(to & SegMask);
            s++;
        }
    }
    return Hidden;
}

uint32_t LOSHashTable::calcHash(Key key) const {
    const uint32_t n = key.mNode;
    const uint32_t s = key.mSeg;
    // (n << 8 + (s & 7)) as the engine parses it: a shift by 8 + (s & 7).
    const uint32_t val = (n << (8 + (s & 7))) ^ (n + (n >> 1)) ^ ((s << 2) | (s & 1));
    return val % mTabSz;
}

// A size relatively prime to the first few primes.
uint32_t LOSHashTable::calcTabSize() {
    static const uint32_t primes[12] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37};
    for (uint32_t nSegs = (uint32_t)mSegments.size();; nSegs++) {
        bool done = true;
        for (uint32_t p : primes)
            if (!(nSegs % p)) done = false;
        if (done) {
            mTabSz = nSegs;
            break;
        }
    }
    return mTabSz;
}

// APPROXIMATION: dQsort is replaced by std::sort; segments with equal hashes
// may land in a different order within their bucket, which lookups ignore.
void LOSHashTable::sortByHashVal() {
    std::sort(mSegments.begin(), mSegments.end(),
              [this](const Segment& a, const Segment& b) { return calcHash(a.mKey) < calcHash(b.mKey); });
}

// Given the segments sorted by hash value, make mTable (bucket starts).
uint32_t LOSHashTable::makeTheTable() {
    uint32_t deepest = 0, depth;
    // Flag as uninitialized; the extra entry is for the bucket size math.
    mTable.assign(mTabSz + 1, IndexType(-1));
    for (uint32_t seg = 0; seg < mSegments.size(); seg++) {
        const uint32_t hash = calcHash(mSegments[seg].mKey);
        if (mTable[hash] == IndexType(-1))
            mTable[hash] = (IndexType)seg;
        else if ((depth = seg - mTable[hash]) > deepest)
            deepest = depth;
    }
    // Fill in empty elements so the bucket size math works.
    IndexType fillValue = (IndexType)mSegments.size();
    uint32_t numFilled = 0;
    for (uint32_t H = mTabSz; H; H--) {
        if (mTable[H] == IndexType(-1)) {
            mTable[H] = fillValue;
        } else {
            fillValue = mTable[H];
            numFilled++;
        }
    }
    Console::instance().printf(LogLevel::Info, "Table size = %d,    Segments = %d,    Filled = %d,  Deepest = %d",
                               (int)mTabSz, (int)mSegments.size(), (int)numFilled, (int)deepest);
    return (mTabSz + 1) * (uint32_t)sizeof(IndexType);
}

bool LOSHashTable::Segment::read(GraphStream& s) {
    return s.read(&mKey.mNode) && s.read(&mKey.mSeg) && s.bytes(mLOS, SegAlloc);
}

void LOSHashTable::clear() {
    mNumNodes = 0;
    mTable.clear();
    mSegments.clear();
}

bool LOSHashTable::read(GraphStream& s) {
    if (s.read(&mNumNodes))
        if (readVector1(s, mSegments, 4 + SegAlloc))
            if (s.read(&mRes0) && s.read(&mRes1)) {
                // Sorted on load since the hash function was still being tweaked.
                calcTabSize();
                sortByHashVal();
                makeTheTable();
                return true;
            }
    return false;
}

//-------------------------------------------------------------------------------------
// Terrain graph info

bool TerrainGraphInfo::read(GraphStream& s) {
    int32_t version;
    haveGraph = false;
    if (!s.read(&version) || version != smVersion) return false;
    const bool ok = s.read(&nodeCount) && s.read(&originWorld) && s.read(&originGrid) &&
                    s.read(&gridDimensions) && s.read(&gridTopRight) && readVector2(s, navigableFlags) &&
                    readVector2(s, neighborFlags) && readVector2(s, shadowHeights) && readVector2(s, roamRadii) &&
                    readVector1(s, consolidated, 8);
    if (ok) doFinalDataSetup();
    return haveGraph = ok;
}

// The index of the grid location if it's in our region, else -1 (no check
// for obstruction).
int32_t TerrainGraphInfo::posToIndex(Point2I p) const {
    int32_t idx = -1;
    if (haveGraph) {
        p -= originGrid;
        if (validArrayIndex(p.x, gridDimensions.x))
            if (validArrayIndex(p.y, gridDimensions.y)) idx = p.y * gridDimensions.x + p.x;
    }
    return idx;
}

// A straight mapping of the grid location to world space; returns whether
// it's a valid node.
bool TerrainGraphInfo::posToLoc(Point3F& loc, const Point2I& p) const {
    const int32_t index = posToIndex(p);
    const bool isValid = (index >= 0 && !obstructed(index));
    loc.x = float(p.x << gNavGlobs.mSquareShift);
    loc.y = float(p.y << gNavGlobs.mSquareShift);
    loc.z = 0.0f;
    loc += originWorld;
    return isValid;
}

// World coordinate to the graph grid index.
int32_t TerrainGraphInfo::locToIndex(Point3F loc) const {
    loc += gNavGlobs.mHalfSquare;
    loc -= originWorld;
    loc *= gNavGlobs.mInverseWidth;
    return posToIndex({(int32_t)std::floor(loc.x), (int32_t)std::floor(loc.y)});
}

Point3F* TerrainGraphInfo::indexToLoc(Point3F& loc, int32_t index) const {
    const Point2I pos = indexToPos(index);
    loc.x = float(pos.x << gNavGlobs.mSquareShift);
    loc.y = float(pos.y << gNavGlobs.mSquareShift);
    loc.z = 0.0f;
    loc += originWorld;
    return &loc;
}

bool TerrainGraphInfo::obstructed(const Point3F& loc) const {
    const int32_t index = locToIndex(loc);
    return (index < 0) || obstructed(index);
}

Point2I TerrainGraphInfo::indexToPos(int32_t index) const {
    return {originGrid.x + index % gridDimensions.x, originGrid.y + index / gridDimensions.x};
}

int32_t TerrainGraphInfo::locToIndexAndSphere(SphereF& sphereOut, const Point3F& loc) const {
    if (!roamRadii.empty()) {
        const int32_t idx = locToIndex(loc);
        if (validArrayIndex(idx, (int32_t)roamRadii.size()) && !obstructed(idx)) {
            indexToLoc(sphereOut.center, idx);
            sphereOut.radius = fixedToFloat(roamRadii[(size_t)idx]);
            if (sphereOut.isContained(loc)) return idx;
        }
    }
    return -1;
}

// Where to 'inbound' when out of bounds: a location on the graph border.
void TerrainGraphInfo::setSideSegs() {
    const Point2I corners[4] = {
        {originGrid.x + 0, originGrid.y + 0},
        {originGrid.x + gridDimensions.x - 1, originGrid.y + 0},
        {originGrid.x + gridDimensions.x - 1, originGrid.y + gridDimensions.y - 1},
        {originGrid.x + 0, originGrid.y + gridDimensions.y - 1}};
    bool Ok[4];
    int32_t numOk = 0;
    Point3F loc1, loc2;
    for (int32_t p1 = 0; p1 < 4; p1++) {
        const int32_t p2 = (p1 + 1) % 4;
        Ok[p1] = posToLoc(loc1, corners[p1]);
        Ok[p2] = posToLoc(loc2, corners[p2]);
        numOk += Ok[p1];
        boundarySegs[p1] = LineSegment(loc1, loc2);
    }
    if (numOk < 4) Console::instance().printf(LogLevel::Info, "Graph Warning!  Only %d corners of grid are navigable!", numOk);
}

void TerrainGraphInfo::doFinalDataSetup() {
    gridArea.point = originGrid;
    gridArea.extent = gridDimensions;
    // read() sets haveGraph only after this, so posToLoc finds no valid
    // corner here and the engine always reports "Only 0 corners".
    setSideSegs();
    // Offsets for finding neighboring squares when working with indices.
    for (int32_t dir = 0; dir < 8; dir++) {
        const Point2I off = gridOffs[dir];
        indOffs[dir] = off.y * gridDimensions.x + off.x;
    }
}

Point3F TerrainGraphInfo::whereToInbound(const Point3F& loc) {
    int32_t solution = -1;
    float bestDist = 1e9f;
    for (int32_t i = 0; i < 4; i++) {
        const float d = boundarySegs[i].distance(loc);
        if (d < bestDist) bestDist = d, solution = i;
    }
    return boundarySegs[solution].solution();
}

// Whether you can go straight across terrain between the points: the share
// of the way the roam spheres allow. `to` becomes where that stops.
float TerrainGraphInfo::checkOpenTerrain(const Point3F& from, Point3F& to) const {
    SphereF sphere;
    int32_t prevSphere = locToIndexAndSphere(sphere, from);
    if (prevSphere < 0) {
        to = from;
        return 0.0f;
    }
    LineStepper linearPath(from, to);
    while (true) {
        // Outbound intersection on the sphere (we tell linearPath when to step).
        const float dist = linearPath.getOutboundIntersection(sphere);
        if (dist > linearPath.remainingDist()) return 1.0f;
        // No intersection: only from rounding.
        if (dist < 0.0f) break;
        const int32_t nextSphere = locToIndexAndSphere(sphere, linearPath.advanceToSolution());
        if (nextSphere >= 0 && nextSphere != prevSphere)
            prevSphere = nextSphere;
        else
            break;
    }
    to = linearPath.getSolution();
    return linearPath.distSoFar() / linearPath.totalDist();
}
