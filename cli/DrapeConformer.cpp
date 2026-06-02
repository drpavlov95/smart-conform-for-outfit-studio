#include "DrapeConformer.h"
#include "NifFile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <array>

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

// ── Adjacency ──────────────────────────────────────────────────────────

static std::vector<std::vector<int>> buildAdjacency(
    int nVerts, const std::vector<Triangle>& tris)
{
    std::vector<std::vector<int>> adj(nVerts);
    for (const auto& t : tris) {
        adj[t.p1].push_back(t.p2); adj[t.p1].push_back(t.p3);
        adj[t.p2].push_back(t.p1); adj[t.p2].push_back(t.p3);
        adj[t.p3].push_back(t.p1); adj[t.p3].push_back(t.p2);
    }
    return adj;
}

// ── Inverse-distance-weighted displacement from k nearest ref verts ────
//    Approximates Automorph's proximity cache without the proximity cache.

static Vec3 idwDiff(const Vec3& pt,
                    const std::vector<Vec3>& refVerts,
                    const VertexDiffs& diffs,
                    int k)
{
    using Pair = std::pair<float, uint16_t>;
    std::vector<Pair> ranked;
    ranked.reserve(refVerts.size());
    for (size_t i = 0; i < refVerts.size(); ++i) {
        float dx = pt.x - refVerts[i].x;
        float dy = pt.y - refVerts[i].y;
        float dz = pt.z - refVerts[i].z;
        ranked.push_back({dx*dx + dy*dy + dz*dz, (uint16_t)i});
    }
    k = std::min(k, (int)ranked.size());
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
    return (wSum > 0.0f) ? result * (1.0f / wSum) : Vec3{0,0,0};
}

// ── Constructor ────────────────────────────────────────────────────────

DrapeConformer::DrapeConformer(const DrapeOptions& opts) : m_opts(opts) {}

// ── Private ────────────────────────────────────────────────────────────

float DrapeConformer::clothFactor(float dist) const {
    float t = (dist - m_opts.d_tight) / (m_opts.d_rigid - m_opts.d_tight);
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);   // smoothstep
}

std::vector<float> DrapeConformer::computeDistances(
    const std::vector<Vec3>& outVerts,
    const std::vector<Vec3>& refVerts) const
{
    std::vector<float> dists(outVerts.size(), std::numeric_limits<float>::max());
    for (size_t i = 0; i < outVerts.size(); ++i) {
        float minD2 = std::numeric_limits<float>::max();
        for (const auto& r : refVerts) {
            float dx = outVerts[i].x - r.x;
            float dy = outVerts[i].y - r.y;
            float dz = outVerts[i].z - r.z;
            float d2 = dx*dx + dy*dy + dz*dz;
            if (d2 < minD2) minD2 = d2;
        }
        dists[i] = std::sqrt(minD2);
    }
    return dists;
}

std::vector<std::vector<uint16_t>> DrapeConformer::rigidIslands(
    const std::vector<float>& dists,
    const std::vector<std::vector<int>>& adjacency) const
{
    int n = (int)dists.size();
    std::vector<bool> visited(n, false);
    std::vector<std::vector<uint16_t>> islands;

    for (int i = 0; i < n; ++i) {
        if (visited[i] || dists[i] <= m_opts.d_rigid) continue;
        std::vector<uint16_t> island;
        std::queue<int> q;
        q.push(i); visited[i] = true;
        while (!q.empty()) {
            int v = q.front(); q.pop();
            island.push_back((uint16_t)v);
            for (int nb : adjacency[v]) {
                if (!visited[nb] && dists[nb] > m_opts.d_rigid) {
                    visited[nb] = true;
                    q.push(nb);
                }
            }
        }
        islands.push_back(std::move(island));
    }
    return islands;
}

// ── detect ─────────────────────────────────────────────────────────────

std::vector<ShapeResult> DrapeConformer::detect(
    const std::string& outfitNif,
    const std::string& refBodyNif) const
{
    NifFile outfit, refBody;
    if (outfit.Load(outfitNif) != 0 || refBody.Load(refBodyNif) != 0)
        return {};

    NiShape* refShape = largestShape(refBody);
    if (!refShape) return {};
    auto refVerts = loadVerts(refBody, refShape);

    std::vector<ShapeResult> results;
    for (auto* shape : outfit.GetShapes()) {
        if (!shape) continue;

        auto outVerts = loadVerts(outfit, shape);
        auto dists    = computeDistances(outVerts, refVerts);

        std::vector<Triangle> tris;
        shape->GetTriangles(tris);
        auto adj     = buildAdjacency((int)outVerts.size(), tris);
        auto islands = rigidIslands(dists, adj);

        std::vector<bool> isRigid(outVerts.size(), false);
        for (auto& isl : islands)
            for (uint16_t v : isl) isRigid[v] = true;

        ShapeResult sr;
        sr.name        = shape->name.get();
        sr.vertexCount = (int)outVerts.size();

        for (size_t i = 0; i < outVerts.size(); ++i) {
            if (isRigid[i])                        ++sr.rigidVerts;
            else if (dists[i] < m_opts.d_tight)    ++sr.tightVerts;
            else                                   ++sr.looseVerts;
        }

        if (sr.rigidVerts > sr.tightVerts && sr.rigidVerts > sr.looseVerts)
            sr.dominantZone = ShapeResult::Zone::Rigid;
        else if (sr.looseVerts > sr.tightVerts)
            sr.dominantZone = ShapeResult::Zone::Loose;
        else
            sr.dominantZone = ShapeResult::Zone::SkinTight;

        results.push_back(std::move(sr));
    }
    return results;
}

// ── conform ────────────────────────────────────────────────────────────

std::vector<ShapeResult> DrapeConformer::conform(
    const std::string& outfitNif,
    const std::string& refBodyNif,
    const VertexDiffs& refBodyDiffs) const
{
    NifFile outfit, refBody;
    if (outfit.Load(outfitNif) != 0 || refBody.Load(refBodyNif) != 0)
        return {};

    NiShape* refShape = largestShape(refBody);
    if (!refShape) return {};
    auto refVerts = loadVerts(refBody, refShape);

    std::vector<ShapeResult> results;
    for (auto* shape : outfit.GetShapes()) {
        if (!shape) continue;

        auto outVerts = loadVerts(outfit, shape);
        auto dists    = computeDistances(outVerts, refVerts);

        std::vector<Triangle> tris;
        shape->GetTriangles(tris);
        auto adj     = buildAdjacency((int)outVerts.size(), tris);
        auto islands = rigidIslands(dists, adj);

        // vertex → island index (-1 = not rigid)
        std::vector<int> islandId(outVerts.size(), -1);
        for (int ii = 0; ii < (int)islands.size(); ++ii)
            for (uint16_t v : islands[ii]) islandId[v] = ii;

        // Precompute per-island average translation (mean of all member vertices)
        std::vector<Vec3> islandTranslation(islands.size(), {0,0,0});
        for (int ii = 0; ii < (int)islands.size(); ++ii) {
            Vec3 sum{0,0,0};
            for (uint16_t vi : islands[ii])
                sum += idwDiff(outVerts[vi], refVerts, refBodyDiffs, m_opts.rigid_k);
            if (!islands[ii].empty())
                islandTranslation[ii] = sum * (1.0f / (float)islands[ii].size());
        }

        ShapeResult sr;
        sr.name        = shape->name.get();
        sr.vertexCount = (int)outVerts.size();

        for (size_t vi = 0; vi < outVerts.size(); ++vi) {
            if (islandId[vi] >= 0) {
                // Rigid island: pure average translation, no deformation
                sr.diffs[(uint16_t)vi] = islandTranslation[islandId[vi]];
                ++sr.rigidVerts;
                continue;
            }

            float factor   = clothFactor(dists[vi]);   // 0 = tight, 1 = full loose
            Vec3  fullDiff = idwDiff(outVerts[vi], refVerts, refBodyDiffs, m_opts.rigid_k);

            if (dists[vi] < m_opts.d_tight) {
                // Skin-tight: full automorph-equivalent displacement
                sr.diffs[(uint16_t)vi] = fullDiff;
                ++sr.tightVerts;
            } else {
                // Loose blend: scale deformation down as cloth distance grows
                // At d_rigid the cloth barely moves (rigid island took over above that)
                sr.diffs[(uint16_t)vi] = fullDiff * (1.0f - factor);
                ++sr.looseVerts;
            }
        }

        if (sr.rigidVerts > sr.tightVerts && sr.rigidVerts > sr.looseVerts)
            sr.dominantZone = ShapeResult::Zone::Rigid;
        else if (sr.looseVerts > sr.tightVerts)
            sr.dominantZone = ShapeResult::Zone::Loose;
        else
            sr.dominantZone = ShapeResult::Zone::SkinTight;

        results.push_back(std::move(sr));
    }
    return results;
}

// ── filterDiffs ────────────────────────────────────────────────────────
// Post-processes vanilla Automorph diffs by cloth zone.
// Takes pre-loaded vertex/triangle data so NIF loading happens once per shape,
// not once per OSD entry.

VertexDiffs DrapeConformer::filterDiffs(
    const std::vector<Vec3>& outVerts,
    const std::vector<Vec3>& refVerts,
    const std::vector<std::array<uint16_t,3>>& outTris,
    const VertexDiffs& existing) const
{
    if (outVerts.empty() || refVerts.empty() || existing.empty())
        return existing;

    auto dists = computeDistances(outVerts, refVerts);

    std::vector<std::vector<int>> adj(outVerts.size());
    for (const auto& t : outTris) {
        adj[t[0]].push_back(t[1]); adj[t[0]].push_back(t[2]);
        adj[t[1]].push_back(t[0]); adj[t[1]].push_back(t[2]);
        adj[t[2]].push_back(t[0]); adj[t[2]].push_back(t[1]);
    }

    auto islands = rigidIslands(dists, adj);

    std::vector<int> islandId(outVerts.size(), -1);
    for (int ii = 0; ii < (int)islands.size(); ++ii)
        for (uint16_t v : islands[ii]) islandId[v] = ii;

    // Average existing diff across each rigid island -> pure translation
    std::vector<Vec3> islandAvg(islands.size(), {0,0,0});
    std::vector<int>  islandCnt(islands.size(), 0);
    for (int ii = 0; ii < (int)islands.size(); ++ii) {
        for (uint16_t vi : islands[ii]) {
            auto it = existing.find(vi);
            if (it != existing.end()) {
                islandAvg[ii] += it->second;
                ++islandCnt[ii];
            }
        }
        if (islandCnt[ii] > 0)
            islandAvg[ii] = islandAvg[ii] * (1.0f / islandCnt[ii]);
    }

    VertexDiffs out;
    for (auto& [vi, diff] : existing) {
        if (vi >= (uint16_t)outVerts.size()) { out[vi] = diff; continue; }

        if (islandId[vi] >= 0) {
            // Rigid island: replace per-vertex deform with uniform translation
            out[vi] = islandAvg[islandId[vi]];
        } else {
            // Smooth blend: skin-tight keeps full diff, loose scales it down
            out[vi] = diff * (1.0f - clothFactor(dists[vi]));
        }
    }
    return out;
}

// ── detectShape ────────────────────────────────────────────────────────
// Detect zones for pre-loaded vertex data — no NIF re-load per call.

ShapeResult DrapeConformer::detectShape(
    const std::vector<Vec3>& outVerts,
    const std::vector<Vec3>& refVerts,
    const std::vector<std::array<uint16_t,3>>& outTris,
    const std::string& shapeName) const
{
    ShapeResult sr;
    sr.name        = shapeName;
    sr.vertexCount = (int)outVerts.size();
    if (outVerts.empty() || refVerts.empty()) return sr;

    auto dists = computeDistances(outVerts, refVerts);

    std::vector<std::vector<int>> adj(outVerts.size());
    for (const auto& t : outTris) {
        adj[t[0]].push_back(t[1]); adj[t[0]].push_back(t[2]);
        adj[t[1]].push_back(t[0]); adj[t[1]].push_back(t[2]);
        adj[t[2]].push_back(t[0]); adj[t[2]].push_back(t[1]);
    }

    auto islands = rigidIslands(dists, adj);
    std::vector<bool> isRigid(outVerts.size(), false);
    for (auto& isl : islands)
        for (uint16_t v : isl) isRigid[v] = true;

    for (size_t i = 0; i < outVerts.size(); ++i) {
        if (isRigid[i])                     ++sr.rigidVerts;
        else if (dists[i] < m_opts.d_tight) ++sr.tightVerts;
        else                                ++sr.looseVerts;
    }

    if (sr.rigidVerts > sr.tightVerts && sr.rigidVerts > sr.looseVerts)
        sr.dominantZone = ShapeResult::Zone::Rigid;
    else if (sr.looseVerts > sr.tightVerts)
        sr.dominantZone = ShapeResult::Zone::Loose;
    else
        sr.dominantZone = ShapeResult::Zone::SkinTight;

    return sr;
}
