/*
libfive: a CAD kernel for modeling with implicit functions
Copyright (C) 2019  Matt Keeter

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this file,
You can obtain one at http://mozilla.org/MPL/2.0/.
*/
#pragma once

#include <array>

#include <Eigen/Eigen>

#include "libfive/render/axes.hpp"
#include "libfive/eval/tape.hpp"
#include "libfive/tree/tree.hpp"

namespace Kernel {

// Forward declarations
template <unsigned N> class SimplexTree;
template <unsigned N> class PerThreadBRep;
class Evaluator;
class Mesh;

/*
 *  A SimplexDebugMesher creates a debug mesh which contains
 *  every tetrahedron in the spatial decomposition of the function.
 */
class SimplexDebugMesher
{
public:
    using Output = Mesh;
    using Input = SimplexTree<3>;

    /*
     *  Constructs a mesher that owns an evaluator,
     *  which is built from the given tree.
     */
    SimplexDebugMesher(PerThreadBRep<3>& m, Tree t);

    /*
     *  Constructs a mesher that has borrowed an evaluator,
     *  which is useful in cases where constructing evaluators
     *  is expensive and they should be re-used.
     */
    SimplexDebugMesher(PerThreadBRep<3>& m, Evaluator* es);

    ~SimplexDebugMesher();

    /*
     *  Called by Dual::walk to construct the triangle mesh
     */
    template <Axis::Axis A>
    void load(const std::array<const SimplexTree<3>*, 4>& ts);

    /*
     *  Simplex meshing needs to walk the top edges of the tree,
     *  because those include tets that we have to run MT on.
     */
    static bool needsTopEdges() { return true; }

protected:
    PerThreadBRep<3>& m;
    Evaluator* eval;
    bool owned;
};

////////////////////////////////////////////////////////////////////////////////

}   // namespace Kernel

