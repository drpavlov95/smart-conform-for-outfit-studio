#pragma once
// Isolated As-Rigid-As-Possible (ARAP) deformation module (plan Phase 1b).
// Kept separate from conform_hook.cpp so the heavy libigl/Eigen template
// instantiation and the Eigen<->Windows-header interplay stay out of the main
// solver translation unit. Exposes only plain POD types.
#include <vector>
#include <array>
#include <cstdint>

struct ArapVec3 { float x, y, z; };

// Deform `verts` (triangle mesh `tris`) so that the handle vertices `handleIdx`
// move to `handleTargets`, filling the interior with an As-Rigid-As-Possible
// solve (minimises local distortion; rigid pieces stay rigid). Deterministic:
// fixed `iterations`, no threading. Returns true and updates `verts` on success.
bool SolveArap(std::vector<ArapVec3>& verts,
               const std::vector<std::array<uint32_t, 3>>& tris,
               const std::vector<uint32_t>& handleIdx,
               const std::vector<ArapVec3>& handleTargets,
               int iterations);
