#include "DrapeConformer.h"
#include "NifFile.hpp"
#include "AABBTree.h"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace nifly;

// ── NIF helpers ────────────────────────────────────────────────────────

static Vec3 toVec3(const Vector3& v) { return {v.x, v.y, v.z}; }

static std::vector<Vec3> loadVerts(NifFile& nif, NiShape* shape) {
    std::vector<Vector3> nv;
    nif.GetVertsForShape(shape, nv);
    std::vector<Vec3> out;
    out.reserve(nv.size());
    for (auto& v : nv) out.push_back(toVec3(v));
    return out;
}

static TriList loadTris(NiShape* shape) {
    std::vector<Triangle> t;
    shape->GetTriangles(t);
    TriList out;
    out.reserve(t.size());
    for (auto& x : t) out.push_back({x.p1, x.p2, x.p3});
    return out;
}

static NiShape* largestShape(NifFile& nif) {
    NiShape* best = nullptr;
    int maxV = 0;
    for (auto* s : nif.GetShapes()) {
        if (!s) continue;
        std::vector<Vector3> v;
        nif.GetVertsForShape(s, v);
        if ((int)v.size() > maxV) { maxV = (int)v.size(); best = s; }
    }
    return best;
}

// ── Inverse-distance-weighted displacement from k nearest ref verts ────
//    Used only by the generative CLI conform() path.

static Vec3 idwDiff(const Vec3& pt,
                    const std::vector<Vec3>& refVerts,
                    const VertexDiffs& diffs,
                    int k)
{
    using Pair = std::pair<float, uint16_t>;
    std::vector<Pair> ranked;
    ranked.reserve(refVerts.size());
    for (size_t i = 0; i < refVerts.size() && i <= 0xFFFF; ++i) {
        float dx = pt.x - refVerts[i].x;
        float dy = pt.y - refVerts[i].y;
        float dz = pt.z - refVerts[i].z;
        ranked.push_back({dx*dx + dy*dy + dz*dz, (uint16_t)i});
    }
    k = std::min(k, (int)ranked.size());
    if (k <= 0) return {0, 0, 0};
    std::partial_sort(ranked.begin(), ranked.begin() + k, ranked.end());

    Vec3  result{0, 0, 0};
    float wSum = 0.0f;
    for (int j = 0; j < k; ++j) {
        auto it = diffs.find(ranked[j].second);
        if (it == diffs.end()) continue;
        float d2 = ranked[j].first;
        float w  = (d2 < 1e-8f) ? 1e8f : 1.0f / d2;
        result += it->second * w;
        wSum   += w;
    }
    return (wSum > 0.0f) ? result * (1.0f / wSum) : Vec3{0, 0, 0};
}

// ── Construction ───────────────────────────────────────────────────────

DrapeConformer::DrapeConformer(const DrapeOptions& opts) : m_opts(opts) {}

// ── Weight function (smoothstep loose falloff) ─────────────────────────

float DrapeConformer::weightForDistance(float d) const {
    const float tD = m_opts.tightDistance;
    const float lD = m_opts.looseDistance;
    if (!(std::isfinite(tD) && std::isfinite(lD)) || tD < 0.0f || lD <= tD)
        return 1.0f;                       // invalid options → safe no-op
    if (d <= tD) return 1.0f;
    if (d >= lD) return 0.0f;
    float t = (d - tD) / (lD - tD);
    float s = t * t * (3.0f - 2.0f * t);   // smoothstep
    float w = 1.0f - s;
    if (std::isfinite(m_opts.falloffPower) && m_opts.falloffPower > 0.0f)
        w = std::pow(w, m_opts.falloffPower);
    return w;
}

// ── Core: per-outfit-vertex weight via BVH point-to-triangle distance ──

std::vector<float> DrapeConformer::buildWeights(
    const std::vector<Vec3>& outVerts,
    const std::vector<Vec3>& bodyVerts,
    const TriList&           bodyTris) const
{
    std::vector<float> weights(outVerts.size(), 1.0f);
    if (bodyVerts.empty() || bodyTris.empty()) return weights;
    // nifly Triangle indices are uint16_t — a body over 65535 verts can't be
    // indexed safely; treat everything as tight (no attenuation) and bail.
    if (bodyVerts.size() > 0xFFFF) return weights;

    // Convert to nifly types; these arrays must outlive the BVH (it stores ptrs).
    std::vector<Vector3> bv;
    bv.reserve(bodyVerts.size());
    for (const auto& v : bodyVerts) bv.push_back(Vector3(v.x, v.y, v.z));

    std::vector<Triangle> bt;
    bt.reserve(bodyTris.size());
    for (const auto& t : bodyTris) {
        if (t[0] >= bv.size() || t[1] >= bv.size() || t[2] >= bv.size())
            continue;                       // skip degenerate / out-of-range
        bt.push_back(Triangle(t[0], t[1], t[2]));
    }
    if (bt.empty()) return weights;

    AABBTree tree(bv.data(), bt.data(), (uint32_t)bt.size(), 100, 2);

    std::vector<IntersectResult> hits;
    for (size_t i = 0; i < outVerts.size(); ++i) {
        Vector3 origin(outVerts[i].x, outVerts[i].y, outVerts[i].z);
        hits.clear();
        tree.IntersectSphere(origin, m_opts.looseDistance, &hits);

        float d = m_opts.looseDistance;     // cap: nothing in range → fully loose
        for (const auto& h : hits)
            if (h.HitDistance < d) d = h.HitDistance;

        weights[i] = weightForDistance(d);
    }
    return weights;
}

// ── Preview classification ─────────────────────────────────────────────

ShapeResult DrapeConformer::classify(const std::vector<float>& weights,
                                     const std::string& shapeName) const {
    ShapeResult sr;
    sr.name        = shapeName;
    sr.vertexCount = (int)weights.size();
    for (float w : weights) {
        if (w >= 0.999f)      ++sr.tightVerts;
        else if (w <= 0.001f) ++sr.farVerts;
        else                  ++sr.looseVerts;
    }
    if (sr.farVerts > sr.tightVerts && sr.farVerts > sr.looseVerts)
        sr.dominantZone = ShapeResult::Zone::Far;
    else if (sr.looseVerts > sr.tightVerts)
        sr.dominantZone = ShapeResult::Zone::Loose;
    else
        sr.dominantZone = ShapeResult::Zone::SkinTight;
    return sr;
}

// ── Loose mode: scale diffs by weight, drop ~zero ──────────────────────

VertexDiffs DrapeConformer::applyLoose(const VertexDiffs& diffs,
                                       const std::vector<float>& weights) const {
    VertexDiffs out;
    out.reserve(diffs.size());
    for (const auto& [idx, d] : diffs) {
        float w = (idx < weights.size()) ? weights[idx] : 1.0f;
        Vec3 scaled = d * w;
        if (scaled.len2() > 1e-12f) out[idx] = scaled;
    }
    return out;
}

// ── Rigid mode: uniform translation to every vertex of the shape ───────

VertexDiffs DrapeConformer::applyRigid(const VertexDiffs& diffs,
                                       int shapeVertCount) const {
    VertexDiffs out;
    if (diffs.empty()) return out;

    Vec3 mean{0, 0, 0};
    for (const auto& [idx, d] : diffs) mean += d;
    mean = mean * (1.0f / (float)diffs.size());
    if (mean.len2() <= 1e-12f) return out;          // no net translation

    int n = std::min(shapeVertCount, 0x10000);      // uint16 index ceiling
    for (int i = 0; i < n; ++i) out[(uint16_t)i] = mean;
    return out;
}

// ── Disk-filter convenience ────────────────────────────────────────────

VertexDiffs DrapeConformer::filterDiffs(const std::vector<Vec3>& outVerts,
                                        const std::vector<Vec3>& bodyVerts,
                                        const TriList&           bodyTris,
                                        const VertexDiffs&       existing,
                                        bool                     rigid) const {
    if (existing.empty()) return existing;
    if (rigid) return applyRigid(existing, (int)outVerts.size());
    if (outVerts.empty() || bodyVerts.empty() || bodyTris.empty()) return existing;

    auto weights = buildWeights(outVerts, bodyVerts, bodyTris);
    return applyLoose(existing, weights);
}

// ── Detect (single pre-loaded shape) ───────────────────────────────────

ShapeResult DrapeConformer::detectShape(const std::vector<Vec3>& outVerts,
                                        const std::vector<Vec3>& bodyVerts,
                                        const TriList&           bodyTris,
                                        const std::string&       shapeName) const {
    if (outVerts.empty()) { ShapeResult sr; sr.name = shapeName; return sr; }
    auto weights = buildWeights(outVerts, bodyVerts, bodyTris);
    return classify(weights, shapeName);
}

// ── File-based convenience (CLI) ───────────────────────────────────────

std::vector<ShapeResult> DrapeConformer::detect(const std::string& outfitNif,
                                                const std::string& refBodyNif) const {
    NifFile outfit, body;
    if (outfit.Load(outfitNif) != 0 || body.Load(refBodyNif) != 0) return {};

    NiShape* bodyShape = largestShape(body);   // refBodyNif is body-only here
    if (!bodyShape) return {};
    auto bodyVerts = loadVerts(body, bodyShape);
    auto bodyTris  = loadTris(bodyShape);

    std::vector<ShapeResult> results;
    for (auto* s : outfit.GetShapes()) {
        if (!s) continue;
        auto outVerts = loadVerts(outfit, s);
        results.push_back(detectShape(outVerts, bodyVerts, bodyTris, s->name.get()));
    }
    return results;
}

std::vector<ShapeResult> DrapeConformer::conform(const std::string& outfitNif,
                                                 const std::string& refBodyNif,
                                                 const VertexDiffs& refBodyDiffs) const {
    NifFile outfit, body;
    if (outfit.Load(outfitNif) != 0 || body.Load(refBodyNif) != 0) return {};

    NiShape* bodyShape = largestShape(body);
    if (!bodyShape) return {};
    auto bodyVerts = loadVerts(body, bodyShape);
    auto bodyTris  = loadTris(bodyShape);

    std::vector<ShapeResult> results;
    for (auto* s : outfit.GetShapes()) {
        if (!s) continue;
        auto outVerts = loadVerts(outfit, s);
        auto weights  = buildWeights(outVerts, bodyVerts, bodyTris);

        // Generate raw conform displacement via IDW from the body's slider diffs.
        VertexDiffs raw;
        for (size_t i = 0; i < outVerts.size() && i <= 0xFFFF; ++i) {
            Vec3 dlt = idwDiff(outVerts[i], bodyVerts, refBodyDiffs, 8);
            if (dlt.len2() > 1e-12f) raw[(uint16_t)i] = dlt;
        }

        ShapeResult sr = classify(weights, s->name.get());
        sr.diffs = applyLoose(raw, weights);
        results.push_back(std::move(sr));
    }
    return results;
}
