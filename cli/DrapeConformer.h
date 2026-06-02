#pragma once
#include <string>
#include <vector>
#include <unordered_map>

// ── Types reused from mppatcher (duplicated to keep Drape standalone) ─

struct Vec3 { float x, y, z;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator*(float f)       const { return {x*f,   y*f,   z*f};   }
    Vec3& operator+=(const Vec3& o)     { x+=o.x; y+=o.y; z+=o.z; return *this; }
    float dot(const Vec3& o)      const { return x*o.x + y*o.y + z*o.z; }
    float len2()                  const { return x*x + y*y + z*z; }
};

// Per-vertex displacement: vertex index → delta
using VertexDiffs = std::unordered_map<uint16_t, Vec3>;

// ── DrapeConformer options ────────────────────────────────────────────

struct DrapeOptions {
    // Distance thresholds (game units, approx mm in Skyrim scale)
    float d_tight = 2.0f;    // below → skin-tight (vanilla Automorph)
    float d_rigid = 20.0f;   // above → rigid island (translate-only)
    // KD-tree neighbours for rigid-body average displacement
    int   rigid_k = 8;
};

// ── Result per shape ─────────────────────────────────────────────────

struct ShapeResult {
    std::string name;
    enum class Zone { SkinTight, Loose, Rigid } dominantZone;
    int vertexCount   = 0;
    int tightVerts    = 0;
    int looseVerts    = 0;
    int rigidVerts    = 0;
    VertexDiffs diffs;           // final post-processed diffs
};

// ── DrapeConformer ───────────────────────────────────────────────────

class DrapeConformer {
public:
    explicit DrapeConformer(const DrapeOptions& opts = {});

    // Analyse distances between outfit and reference body NIF.
    // Returns per-shape zone distribution (for UI preview / detect-only mode).
    std::vector<ShapeResult> detect(const std::string& outfitNif,
                                    const std::string& refBodyNif) const;

    // Full conform: apply Automorph-style displacement filtered by cloth zone.
    // outfitNif    : path to the outfit .nif
    // refBodyNif   : path to the MPP reference body .nif (neutral pose)
    // refBodyDiffs : per-vertex displacements of the reference body for one slider
    // Returns post-processed diffs for every shape in the outfit.
    std::vector<ShapeResult> conform(const std::string& outfitNif,
                                     const std::string& refBodyNif,
                                     const VertexDiffs& refBodyDiffs) const;

private:
    DrapeOptions m_opts;

    // Compute per-vertex min-distance to the reference body mesh
    std::vector<float> computeDistances(const std::vector<Vec3>& outVerts,
                                        const std::vector<Vec3>& refVerts) const;

    // Flood-fill connected vertex components all with dist > d_rigid
    std::vector<std::vector<uint16_t>> rigidIslands(
        const std::vector<float>& dists,
        const std::vector<std::vector<int>>& adjacency) const;

    // Smooth blend factor:  0 = skin-tight, 1 = rigid
    float clothFactor(float dist) const;
};
