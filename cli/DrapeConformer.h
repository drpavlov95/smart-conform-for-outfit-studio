#pragma once
#include <string>
#include <vector>
#include <array>
#include <cstdint>
#include <unordered_map>

// ── Lightweight vector (keeps the public API nifly-free) ───────────────

struct Vec3 { float x, y, z;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float f)       const { return {x*f,   y*f,   z*f};   }
    Vec3& operator+=(const Vec3& o)     { x+=o.x; y+=o.y; z+=o.z; return *this; }
    float dot(const Vec3& o)      const { return x*o.x + y*o.y + z*o.z; }
    float len2()                  const { return x*x + y*y + z*z; }
};

// Per-vertex displacement: vertex index → delta
using VertexDiffs = std::unordered_map<uint16_t, Vec3>;
using TriList     = std::vector<std::array<uint16_t,3>>;

// ── Options ────────────────────────────────────────────────────────────
//
// Cloth-aware falloff is a pure function of distance to the *reference body
// surface*:
//     w(d) = 1                              for d <= tightDistance
//     w(d) = 1 - smoothstep(t), t=(d-tD)/(lD-tD)   for tight < d < loose
//     w(d) = 0                              for d >= looseDistance
//
// Rigidity is NOT inferred from distance (a long skirt hem is far but loose).
// It is an explicit per-shape choice handled by the caller (see filterDiffs
// `rigid` flag), never a measurement.

struct DrapeOptions {
    float tightDistance = 2.0f;   // game units; full movement at/below this
    float looseDistance = 20.0f;  // zero movement at/above this
};

// ── Result per shape (preview / reporting) ─────────────────────────────

struct ShapeResult {
    std::string name;
    enum class Zone { SkinTight, Loose, Far } dominantZone = Zone::SkinTight;
    int vertexCount = 0;
    int tightVerts  = 0;   // w == 1   (d <= tight)
    int looseVerts  = 0;   // 0 < w < 1
    int farVerts    = 0;   // w == 0   (d >= loose)
    VertexDiffs diffs;     // post-processed diffs (conform path only)
};

// ── DrapeConformer ─────────────────────────────────────────────────────

class DrapeConformer {
public:
    explicit DrapeConformer(const DrapeOptions& opts = {});

    // Core: per-outfit-vertex weight in [0,1] measured against the reference
    // body *surface* using a BVH (point-to-triangle distance, capped at
    // looseDistance). Returns one weight per outfit vertex.
    std::vector<float> buildWeights(const std::vector<Vec3>& outVerts,
                                    const std::vector<Vec3>& bodyVerts,
                                    const TriList&           bodyTris) const;

    // Preview: bucket the weights into tight / loose / far counts.
    ShapeResult classify(const std::vector<float>& weights,
                         const std::string& shapeName) const;

    // Loose mode: scale each diff by its vertex weight; drop ~zero entries.
    VertexDiffs applyLoose(const VertexDiffs& diffs,
                           const std::vector<float>& weights) const;

    // Rigid mode: replace per-vertex deform with one uniform translation
    // (mean of the existing deltas) applied to EVERY vertex of the shape so
    // the accessory moves as a single rigid body without deforming.
    VertexDiffs applyRigid(const VertexDiffs& diffs, int shapeVertCount) const;

    // Disk-filter convenience (algo.cpp): loose by default, rigid if flagged.
    VertexDiffs filterDiffs(const std::vector<Vec3>& outVerts,
                            const std::vector<Vec3>& bodyVerts,
                            const TriList&           bodyTris,
                            const VertexDiffs&       existing,
                            bool                     rigid = false) const;

    // Detect zones for one pre-loaded shape (no NIF I/O).
    ShapeResult detectShape(const std::vector<Vec3>& outVerts,
                            const std::vector<Vec3>& bodyVerts,
                            const TriList&           bodyTris,
                            const std::string&       shapeName = "") const;

    // ── File-based convenience (CLI harness) ───────────────────────────
    std::vector<ShapeResult> detect(const std::string& outfitNif,
                                    const std::string& refBodyNif) const;
    std::vector<ShapeResult> conform(const std::string& outfitNif,
                                     const std::string& refBodyNif,
                                     const VertexDiffs& refBodyDiffs) const;

private:
    DrapeOptions m_opts;
    float weightForDistance(float d) const;   // smoothstep falloff
};
