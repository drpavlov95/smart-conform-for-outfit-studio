// Isolated ARAP module (libigl + Eigen). See skin_arap.h. Plan Phase 1b.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "skin_arap.h"

#include <Eigen/Core>
#include <igl/arap.h>

bool SolveArap(std::vector<ArapVec3>& verts,
               const std::vector<std::array<uint32_t, 3>>& tris,
               const std::vector<uint32_t>& handleIdx,
               const std::vector<ArapVec3>& handleTargets,
               int iterations) {
    const int n = static_cast<int>(verts.size());
    if (n == 0 || tris.empty() || handleIdx.empty() ||
        handleIdx.size() != handleTargets.size()) {
        return false;
    }

    Eigen::MatrixXd V(n, 3);
    for (int i = 0; i < n; ++i) {
        V(i, 0) = verts[i].x; V(i, 1) = verts[i].y; V(i, 2) = verts[i].z;
    }
    Eigen::MatrixXi F(static_cast<int>(tris.size()), 3);
    for (int i = 0; i < static_cast<int>(tris.size()); ++i) {
        F(i, 0) = static_cast<int>(tris[i][0]);
        F(i, 1) = static_cast<int>(tris[i][1]);
        F(i, 2) = static_cast<int>(tris[i][2]);
    }
    Eigen::VectorXi b(static_cast<int>(handleIdx.size()));
    for (int i = 0; i < static_cast<int>(handleIdx.size()); ++i) {
        b(i) = static_cast<int>(handleIdx[i]);
    }

    igl::ARAPData data;
    data.max_iter = iterations > 0 ? iterations : 4;   // fixed count -> deterministic
    if (!igl::arap_precomputation(V, F, 3, b, data)) return false;

    Eigen::MatrixXd bc(static_cast<int>(handleIdx.size()), 3);
    for (int i = 0; i < static_cast<int>(handleIdx.size()); ++i) {
        bc(i, 0) = handleTargets[i].x;
        bc(i, 1) = handleTargets[i].y;
        bc(i, 2) = handleTargets[i].z;
    }

    Eigen::MatrixXd U = V;   // initial guess = rest pose
    if (!igl::arap_solve(bc, data, U)) return false;

    for (int i = 0; i < n; ++i) {
        verts[i].x = static_cast<float>(U(i, 0));
        verts[i].y = static_cast<float>(U(i, 1));
        verts[i].z = static_cast<float>(U(i, 2));
    }
    return true;
}
