#pragma once
// The math and container utilities of the Tribes 2 AI navigation graph
// (engine ai/graphMath.h, graphBSP.h, tBinHeap.h, core/bitVector.h), and
// the Torque Point3F / Point2I arithmetic the graph code is written in.
//
// The arithmetic operators live in namespace NavMath (Point3F is a plain
// struct shared with the renderer); the graph sources bring them in with
// `using namespace NavMath;`.
#include "core/math.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace NavMath {

inline Point3F operator+(Point3F a, const Point3F& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Point3F operator-(Point3F a, const Point3F& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Point3F operator-(const Point3F& a) { return {-a.x, -a.y, -a.z}; }
inline Point3F operator*(const Point3F& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Point3F& operator+=(Point3F& a, const Point3F& b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline Point3F& operator-=(Point3F& a, const Point3F& b) { a.x -= b.x; a.y -= b.y; a.z -= b.z; return a; }
inline Point3F& operator*=(Point3F& a, float s) { a.x *= s; a.y *= s; a.z *= s; return a; }
inline bool operator==(const Point3F& a, const Point3F& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline float mDot(const Point3F& a, const Point3F& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Point3F mCross(const Point3F& a, const Point3F& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lenSquared(const Point3F& a) { return mDot(a, a); }
inline float len(const Point3F& a) { return std::sqrt(lenSquared(a)); }
// m_point3F_normalize: a zero vector becomes (0, 0, 1).
inline Point3F& normalize(Point3F& a) {
    const float squared = lenSquared(a);
    if (squared != 0.0f) a *= 1.0f / std::sqrt(squared);
    else a = {0, 0, 1};
    return a;
}
// m_point3F_normalize_f
inline Point3F& normalize(Point3F& a, float val) { return a *= val / len(a); }

inline Point2I operator+(Point2I a, const Point2I& b) { return {a.x + b.x, a.y + b.y}; }
inline Point2I operator-(Point2I a, const Point2I& b) { return {a.x - b.x, a.y - b.y}; }
inline Point2I& operator+=(Point2I& a, const Point2I& b) { a.x += b.x; a.y += b.y; return a; }
inline Point2I& operator-=(Point2I& a, const Point2I& b) { a.x -= b.x; a.y -= b.y; return a; }
inline Point2I& operator*=(Point2I& a, int32_t s) { a.x *= s; a.y *= s; return a; }

inline float len2D(float x, float y) { return std::sqrt(x * x + y * y); }

} // namespace NavMath

// math/mSphere.h
struct SphereF {
    Point3F center{0, 0, 0};
    float radius = 0;
    SphereF() = default;
    SphereF(const Point3F& c, float r) : center(c), radius(r) {}
    bool isContained(const Point3F& p) const {
        const float dx = center.x - p.x, dy = center.y - p.y, dz = center.z - p.z;
        return dx * dx + dy * dy + dz * dz <= radius * radius;
    }
};

namespace NavMath {

// PlaneF::distToPlane
inline float distToPlane(const PlaneF& p, const Point3F& cp) { return p.x * cp.x + p.y * cp.y + p.z * cp.z + p.d; }
// Box3F::collideLine (math/mBox.cc).
bool boxCollideLine(const Box3F& box, const Point3F& start, const Point3F& end, float* t = nullptr, Point3F* n = nullptr);
// Box3F(min, max, true)
inline Box3F makeBox(const Point3F& a, const Point3F& b) { Box3F box; box.min = a; box.max = b; return box; }

inline bool validArrayIndex(int32_t index, int32_t arrayLength) { return uint32_t(index) < uint32_t(arrayLength); }

// Where vertical line through point hits plane (whose normal.z != 0).
float solveForZ(const PlaneF& plane, const Point3F& point);
// Point at the intersection of three planes (computed in doubles).
bool intersectPlanes(const PlaneF& p, const PlaneF& q, const PlaneF& r, Point3F* pOut);

template <class T> T scaleBetween(T first, T last, float pct) { return first + (last - first) * pct; }
inline float scaleBetween(float first, float last, float pct) { return first + pct * (last - first); }
// Percent of the way of value between min and max, clamped to [0, 1].
inline float getPercentBetween(float value, float min, float max) {
    if (std::fabs(max - min) < 0.0001f) return 0.0f;
    else if (min < max) {
        if (value <= min) return 0.0f;
        else if (value >= max) return 1.0f;
    } else if (min > max) {
        if (value >= min) return 0.0f;
        else if (value <= max) return 1.0f;
    }
    return (value - min) / (max - min);
}
inline float mapValueLinear(float value, float dmin, float dmax, float rmin, float rmax) {
    return scaleBetween(rmin, rmax, getPercentBetween(value, dmin, dmax));
}

inline float mapValueQuadratic(float value, float dmin, float dmax, float rmin, float rmax) {
    const float pct = getPercentBetween(value, dmin, dmax);
    return scaleBetween(rmin, rmax, pct * pct);
}
inline float mapValueSqrt(float value, float dmin, float dmax, float rmin, float rmax) {
    return scaleBetween(rmin, rmax, std::sqrt(getPercentBetween(value, dmin, dmax)));
}

// Whether the two points are within the threshold distance (3D).
inline bool within(const Point3F& p1, const Point3F& p2, float thresh) {
    return lenSquared(p1 - p2) <= thresh * thresh;
}
inline bool within_2D(const Point3F& a, const Point3F& b, float d) {
    const float dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy <= d * d;
}

// Reverse elements in place.
template <class T> std::vector<T>& reverseVec(std::vector<T>& v) {
    std::reverse(v.begin(), v.end());
    return v;
}

} // namespace NavMath

// graphDefines.h
constexpr float LiberalBonkXY = 21.0f;

// Grid rectangles (graphMath.h GridArea over math/mRect.h RectI).
struct GridArea {
    Point2I point{0, 0};
    Point2I extent{0, 0};
    GridArea() = default;
    GridArea(const Point2I& pt, const Point2I& ext) : point(pt), extent(ext) {}
    GridArea(int32_t l, int32_t t, int32_t w, int32_t h) : point{l, t}, extent{w, h} {}

    int32_t getIndex(Point2I p) const {
        p.x -= point.x; p.y -= point.y;
        if (NavMath::validArrayIndex(p.x, extent.x))
            if (NavMath::validArrayIndex(p.y, extent.y))
                return p.y * extent.x + p.x;
        return -1;
    }
    Point2I getPos(int32_t index) const { return {point.x + index % extent.x, point.y + index / extent.x}; }
    bool start(Point2I& p) const;
    bool step(Point2I& p) const;

    bool isValidRect() const { return extent.x > 0 && extent.y > 0; }
    int32_t len_x() const { return extent.x; }
    int32_t len_y() const { return extent.y; }
    bool contains(const GridArea& r) const {
        return point.x <= r.point.x && point.y <= r.point.y && r.point.x + r.extent.x <= point.x + extent.x &&
               r.point.y + r.extent.y <= point.y + extent.y;
    }
    bool intersect(const GridArea& clip);
    bool overlaps(GridArea r) const { return r.intersect(*this); }
};

// A quad-tree traversal of a grid region.
class GridVisitor {
protected:
    bool mPreCheck;
    bool mPostCheck;
    bool recurse(GridArea rect, int32_t level);

public:
    const GridArea mArea;
    explicit GridVisitor(const GridArea& area);
    virtual ~GridVisitor() = default;
    virtual bool beforeDivide(const GridArea& R, int32_t level);
    virtual bool atLevelZero(const GridArea& R);
    virtual bool afterDivide(const GridArea& R, int32_t level, bool success);
    bool traverse();
};

// Closest distance / point on a segment.
class LineSegment {
    Point3F a{0, 0, 0}, b{0, 0, 0};
    Point3F soln{0, 0, 0};

public:
    LineSegment() = default;
    LineSegment(const Point3F& a_, const Point3F& b_) : a(a_), b(b_), soln(a_) {}
    void set(const Point3F& a_, const Point3F& b_) { soln = a = a_; b = b_; }
    float distance(const Point3F& p);
    bool botDistCheck(const Point3F& p, float d3, float d2);
    Point3F solution() const { return soln; }
    Point3F getEnd(bool which) const { return which ? b : a; }
};

class LineStepper {
    Point3F A, B;
    Point3F dir1;
    float total, soFar;
    float advance;
    Point3F solution;

public:
    LineStepper(const Point3F& a, const Point3F& b) { init(a, b); }
    const Point3F& getSolution() const { return solution; }
    float distSoFar() const { return soFar; }
    float totalDist() const { return total; }
    float remainingDist() const { return total - soFar; }
    void init(const Point3F& a, const Point3F& b);
    float getOutboundIntersection(const SphereF& s);
    const Point3F& advanceToSolution();
};

// core/bitVector.h
class BitVector {
    std::vector<uint8_t> mBits;
    uint32_t mSize = 0;

public:
    BitVector() = default;
    explicit BitVector(uint32_t sizeInBits) { setSize(sizeInBits); }
    void setSize(uint32_t sizeInBits) {
        mSize = sizeInBits;
        mBits.assign((sizeInBits + 7) >> 3, 0);
    }
    uint32_t getSize() const { return mSize; }
    uint32_t getByteSize() const { return (uint32_t)mBits.size(); }
    const uint8_t* getBits() const { return mBits.data(); }
    void clear() { std::fill(mBits.begin(), mBits.end(), 0); }
    void copy(const BitVector& from) { mBits = from.mBits; mSize = from.mSize; }
    void set(uint32_t i) { mBits[i >> 3] |= uint8_t(1u << (i & 7)); }
    void clear(uint32_t i) { mBits[i >> 3] &= uint8_t(~(1u << (i & 7))); }
    bool test(uint32_t i) const { return (mBits[i >> 3] & (1u << (i & 7))) != 0; }
};

// A 32-bit flag set (core/bitSet.h BitSet32).
struct BitSet32 {
    uint32_t bits = 0;
    BitSet32() = default;
    BitSet32(uint32_t b) : bits(b) {}
    operator uint32_t() const { return bits; }
    void set(uint32_t m) { bits |= m; }
    void set(uint32_t m, bool b) { bits = b ? (bits | m) : (bits & ~m); }
    void clear(uint32_t m) { bits &= ~m; }
    void clear() { bits = 0; }
    bool test(uint32_t m) const { return (bits & m) != 0; }
};

// ai/tBinHeap.h: an indexed binary heap over a pool (16-bit heap indices).
// T::operator< orders the heap: the head is the element no other is "less"
// than.
template <class T> class BinHeap {
protected:
    std::vector<T> mPool;
    std::vector<int16_t> mHeap;
    std::vector<int16_t> mBack;
    bool mIsHeapified = false;
    int32_t mHeapCount = 0;

    void keyChange(int32_t heapIndex) {
        int32_t i = heapIndex;
        const int32_t temp = mHeap[heapIndex];
        mIsHeapified = false;
        while (i > 0) {
            const int32_t parent = (i - 1) >> 1;
            if (mPool[mHeap[parent]] < mPool[temp]) {
                mIsHeapified = true;
                shiftDown(parent, i);
                i = parent;
            } else {
                break;
            }
        }
        mHeap[i] = (int16_t)temp;
        mBack[mHeap[i]] = (int16_t)i;
        if (!mIsHeapified) heapify(heapIndex);
    }
    void shiftDown(int32_t parent, int32_t child) {
        mHeap[child] = mHeap[parent];
        mBack[mHeap[child]] = (int16_t)child;
    }
    void shiftUp(int32_t parent, int32_t child) {
        mHeap[parent] = mHeap[child];
        mBack[mHeap[parent]] = (int16_t)parent;
    }

public:
    void changeKey(int32_t indexInArray) { keyChange(mBack[indexInArray]); }
    void clear() {
        mPool.clear();
        mHeap.clear();
        mBack.clear();
        mIsHeapified = false;
        mHeapCount = 0;
    }
    void insert(const T& elem) {
        const int32_t indexInArray = (int32_t)mPool.size();
        mPool.push_back(elem);
        mHeap.push_back(0);
        mBack.push_back((int16_t)mHeapCount);
        mHeap[mHeapCount++] = (int16_t)indexInArray;
        if (mIsHeapified) {
            int32_t i = mHeapCount - 1;
            const int32_t temp = mHeap[i];
            while (i > 0) {
                const int32_t parent = (i - 1) >> 1;
                if (mPool[mHeap[parent]] < elem) {
                    shiftDown(parent, i);
                    i = parent;
                } else {
                    break;
                }
            }
            mHeap[i] = (int16_t)temp;
            mBack[temp] = (int16_t)i;
        }
    }
    T* head() { return mHeapCount > 0 ? &mPool[mHeap[0]] : nullptr; }
    int32_t headIndex() const { return mHeapCount > 0 ? mHeap[0] : -1; }
    int32_t count() const { return mHeapCount; }
    int32_t size() const { return (int32_t)mPool.size(); }
    void buildHeap() {
        mIsHeapified = true;
        for (int32_t j = (mHeapCount >> 1) - 1; j >= 0; j--) heapify(j);
    }
    void heapify(int32_t parent) {
        int32_t largest = parent;
        const int32_t temp = mHeap[parent];
        while (true) {
            int32_t l, r;
            if ((l = (parent << 1) + 1) < mHeapCount) {
                if (mPool[temp] < mPool[mHeap[l]]) largest = l;
                if ((r = (parent + 1) << 1) < mHeapCount) {
                    if (largest == parent && mHeap[parent] != temp) {
                        if (mPool[temp] < mPool[mHeap[r]]) largest = r;
                    } else {
                        if (mPool[mHeap[largest]] < mPool[mHeap[r]]) largest = r;
                    }
                }
            }
            if (largest != parent) {
                shiftUp(parent, largest);
                parent = largest;
            } else {
                mHeap[parent] = (int16_t)temp;
                mBack[temp] = (int16_t)parent;
                break;
            }
        }
        mIsHeapified = true;
    }
    void removeHead() {
        if (mHeapCount < 1) return;
        if (!mIsHeapified) buildHeap();
        mBack[mHeap[0]] = -1;
        mBack[mHeap[0] = mHeap[--mHeapCount]] = 0;
        if (mHeapCount) heapify(0);
    }
    T& operator[](uint32_t index) { return mPool[index]; }
    void reserve(int32_t amount) {
        mPool.reserve(amount);
        mHeap.reserve(amount);
        mBack.reserve(amount);
    }
};

// ai/graphBSP.h: an axis-aligned BSP over objects with location().
// APPROXIMATION: dQsort is replaced by std::sort, which may order elements
// with equal coordinates differently (the tree then splits ties differently).
template <class T> class AxisAlignedBSP {
public:
    enum { Lower, Upper };
    struct BSPNode {
        uint16_t mAxis = 0;
        uint16_t mCount = 0;
        uint16_t mBranch[2] = {0, 0};
        T* mLeaf = nullptr;
        float mDivide = 0.0f;
    };

protected:
    std::vector<BSPNode> mTree;
    std::vector<T*> mList;

    static float axisOf(const T* t, int32_t axis) {
        const Point3F& loc = t->location();
        return axis == 0 ? loc.x : (axis == 1 ? loc.y : loc.z);
    }
    static float boxAxis(const Point3F& p, int32_t axis) { return axis == 0 ? p.x : (axis == 1 ? p.y : p.z); }
    static void setAxis(Point3F& p, int32_t axis, float v) { (axis == 0 ? p.x : (axis == 1 ? p.y : p.z)) = v; }
    static bool boxContainsPt(const Box3F& b, const Point3F& p) {
        return p.x >= b.min.x && p.x <= b.max.x && p.y >= b.min.y && p.y <= b.max.y && p.z >= b.min.z &&
               p.z <= b.max.z;
    }

    uint16_t partition(int32_t start, int32_t n) {
        const int32_t place = (int32_t)mTree.size();
        BSPNode assemble;
        if (n < 2) {
            assemble.mCount = 1;
            assemble.mLeaf = mList[start];
            mTree.push_back(assemble);
        } else {
            const int32_t mid1 = n >> 1, mid0 = mid1 - 1;
            float bestSeparation = -1;
            std::vector<T*> divisions[3];
            for (int32_t axis = 0; axis < 3; axis++) {
                divisions[axis].assign(mList.begin() + start, mList.begin() + start + n);
                std::sort(divisions[axis].begin(), divisions[axis].end(),
                          [axis](const T* a, const T* b) { return axisOf(a, axis) < axisOf(b, axis); });
                const float low = axisOf(divisions[axis][mid0], axis);
                const float high = axisOf(divisions[axis][mid1], axis);
                const float separation = high - low;
                if (separation > bestSeparation) {
                    bestSeparation = separation;
                    assemble.mCount = (uint16_t)n;
                    assemble.mAxis = (uint16_t)axis;
                    assemble.mDivide = low + separation * 0.5f;
                }
            }
            std::copy(divisions[assemble.mAxis].begin(), divisions[assemble.mAxis].end(), mList.begin() + start);
            mTree.push_back(assemble);
            const uint16_t lower = partition(start, mid1);
            mTree[place].mBranch[Lower] = lower;
            const uint16_t upper = partition(start + mid1, n - mid1);
            mTree[place].mBranch[Upper] = upper;
        }
        return (uint16_t)place;
    }

    void traverse(int32_t ind, const Box3F& box, std::vector<T*>& result) const {
        const BSPNode& node = mTree[ind];
        if (node.mCount == 1) {
            if (boxContainsPt(box, node.mLeaf->location())) result.push_back(node.mLeaf);
        } else {
            if (node.mDivide > boxAxis(box.max, node.mAxis))
                traverse(node.mBranch[Lower], box, result);
            else if (node.mDivide < boxAxis(box.min, node.mAxis))
                traverse(node.mBranch[Upper], box, result);
            else {
                Box3F splitUpper = box, splitLower = box;
                setAxis(splitUpper.min, node.mAxis, node.mDivide);
                setAxis(splitLower.max, node.mAxis, node.mDivide);
                traverse(node.mBranch[Lower], splitLower, result);
                traverse(node.mBranch[Upper], splitUpper, result);
            }
        }
    }

public:
    void makeTree(const std::vector<T*>& listIn) {
        mList = listIn;
        mTree.clear();
        if (!mList.empty()) {
            mTree.reserve(mList.size() * 3);
            partition(0, (int32_t)mList.size());
        }
        mList.clear();
    }
    // Pushes (without clearing) the objects inside the box.
    int32_t getIntersecting(std::vector<T*>& result, const Box3F& box) const {
        if (!mTree.empty()) traverse(0, box, result);
        return (int32_t)result.size();
    }
    void clear() {
        mTree.clear();
        mList.clear();
    }
};
