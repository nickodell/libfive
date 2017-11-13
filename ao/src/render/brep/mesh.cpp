/*
Ao: a CAD kernel for modeling with implicit functions
Copyright (C) 2017  Matt Keeter

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <numeric>
#include <fstream>
#include <boost/algorithm/string/predicate.hpp>

#include "ao/render/brep/mesh.hpp"
#include "ao/render/brep/xtree.hpp"
#include "ao/render/brep/dual.hpp"

namespace Kernel {

template <Axis::Axis A, bool D>
void Mesh::load(const std::array<const XTree<3>*, 4>& ts)
{
    int es[4];
    {   // Unpack edge vertex pairs into edge indices
        auto q = Axis::Q(A);
        auto r = Axis::R(A);
        std::vector<std::pair<unsigned, unsigned>> ev = {
            {q|r, q|r|A},
            {r, r|A},
            {q, q|A},
            {0, A}};
        for (unsigned i=0; i < ts.size(); ++i)
        {
            es[i] = XTree<3>::mt->e[D ? ev[i].first  : ev[i].second]
                                   [D ? ev[i].second : ev[i].first];
            assert(es[i] != -1);
        }
    }

    /*  Find the approximate normal of this polygon in an order-agnostic
     *  way, by the generalized expansion of (b - a) x (c - a) */
    Eigen::Matrix<double, 3, 1> norm(0, 0, 0);
    for (unsigned i=0; i < ts.size(); ++i)
    {
        auto j = (i + 1) % ts.size();

        auto va = ts[i]->level > 0
            ? 0
            : XTree<3>::mt->p[ts[i]->corner_mask][es[i]];
        auto vb = ts[j]->level > 0
            ? 0
            : XTree<3>::mt->p[ts[j]->corner_mask][es[j]];

        norm += ts[i]->vert(va).cross(ts[j]->vert(vb));
    }
    norm.normalize();

    uint32_t vs[4];
    for (unsigned i=0; i < ts.size(); ++i)
    {
        // Load either a patch-specific vertex (if this is a lowest-level,
        // potentially non-manifold cell) or the default vertex
        auto vi = ts[i]->level > 0
            ? 0
            : XTree<3>::mt->p[ts[i]->corner_mask][es[i]];
        assert(vi != -1);

        // Sanity-checking manifoldness of collapsed cells
        assert(ts[i]->level == 0 || ts[i]->vertex_count == 1);

        // Sanity-check the rank of collapsed cells
        assert(ts[i]->rank(vi) > 0);
        assert(ts[i]->rank(vi) <= 3);

        // Figure out which normal id best matches this vertex
        auto match = (ts[i]->norms[vi].leftCols(ts[i]->rank(vi))
                .transpose() * norm)
                .eval();
        size_t r;
        match.cwiseAbs().maxCoeff(&r);

        // Store a new vertex if this one is unpopulated
        if (ts[i]->index(vi, r) == 0)
        {
            ts[i]->index(vi, r) = verts.size();

            // Store the vertex position
            verts.push_back(ts[i]->vert(vi).template cast<float>());

            // Store the normal, flipping as needed
            float sign = (match(r) < 0) ? -1.0f : 1.0f;
            norms.push_back(sign * ts[i]->norms[vi].col(r).template cast<float>());
        }
        vs[i] = ts[i]->index(vi, r);
    }

    // Handle polarity-based windings
    if (!D)
    {
        std::swap(vs[1], vs[2]);
    }

    // Pick a triangulation that prevents triangles from folding back
    // on each other by checking normals.
    std::array<Eigen::Vector3f, 4> norms;
    for (unsigned i=0; i < norms.size(); ++i)
    {
        norms[i] = (verts[vs[(i + 3) % 4]] - verts[vs[i]]).cross
                   (verts[vs[(i + 1) % 4]] - verts[vs[i]]).normalized();
    }
    if (norms[0].dot(norms[3]) > norms[1].dot(norms[2]))
    {
        branes.push_back({vs[0], vs[1], vs[2]});
        branes.push_back({vs[2], vs[1], vs[3]});
    }
    else
    {
        branes.push_back({vs[0], vs[1], vs[3]});
        branes.push_back({vs[0], vs[3], vs[2]});
    }
}

////////////////////////////////////////////////////////////////////////////////

std::unique_ptr<Mesh> Mesh::render(const Tree t, const Region<3>& r,
                                   double min_feature, double max_err)
{
    std::atomic_bool cancel(false);
    std::map<Tree::Id, float> vars;
    return render(t, vars, r, min_feature, max_err, cancel);
}

std::unique_ptr<Mesh> Mesh::render(
            const Tree t, const std::map<Tree::Id, float>& vars,
            const Region<3>& r, double min_feature, double max_err,
            std::atomic_bool& cancel)
{
    // Create the octree (multithreaded and cancellable)
    return mesh(XTree<3>::build(
            t, vars, r, min_feature, max_err, true, cancel), cancel);
}

std::unique_ptr<Mesh> Mesh::render(
        XTreeEvaluator* es,
        const Region<3>& r, double min_feature, double max_err,
        std::atomic_bool& cancel)
{
    return mesh(XTree<3>::build(es, r, min_feature, max_err, true, cancel),
                cancel);
}

std::unique_ptr<Mesh> Mesh::mesh(std::unique_ptr<const XTree<3>> xtree,
                                 std::atomic_bool& cancel)
{
    // Perform marching squares
    auto m = std::unique_ptr<Mesh>(new Mesh());

    if (cancel.load())
    {
        return nullptr;
    }
    else
    {
        Dual<3>::walk(xtree.get(), *m);

#if DEBUG_OCTREE_CELLS
        // Store octree cells as lines
        std::list<const XTree<3>*> todo = {xtree.get()};
        while (todo.size())
        {
            auto t = todo.front();
            todo.pop_front();
            if (t->isBranch())
                for (auto& c : t->children)
                    todo.push_back(c.get());

            static const std::vector<std::pair<uint8_t, uint8_t>> es =
                {{0, Axis::X}, {0, Axis::Y}, {0, Axis::Z},
                 {Axis::X, Axis::X|Axis::Y}, {Axis::X, Axis::X|Axis::Z},
                 {Axis::Y, Axis::Y|Axis::X}, {Axis::Y, Axis::Y|Axis::Z},
                 {Axis::X|Axis::Y, Axis::X|Axis::Y|Axis::Z},
                 {Axis::Z, Axis::Z|Axis::X}, {Axis::Z, Axis::Z|Axis::Y},
                 {Axis::Z|Axis::X, Axis::Z|Axis::X|Axis::Y},
                 {Axis::Z|Axis::Y, Axis::Z|Axis::Y|Axis::X}};
            for (auto e : es)
                m->line(t->cornerPos(e.first).template cast<float>(),
                        t->cornerPos(e.second).template cast<float>());
        }
#endif
        return m;
    }
}

void Mesh::line(const Eigen::Vector3f& a, const Eigen::Vector3f& b)
{
    uint32_t a_ = verts.size();
    verts.push_back(a);
    uint32_t b_ = verts.size();
    verts.push_back(b);

    branes.push_back({a_, a_, b_});
}

////////////////////////////////////////////////////////////////////////////////

bool Mesh::saveSTL(const std::string& filename,
                   const std::list<const Mesh*>& meshes)
{
    if (!boost::algorithm::iends_with(filename, ".stl"))
    {
        std::cerr << "Mesh::saveSTL: filename \"" << filename
                  << "\" does not end in .stl" << std::endl;
    }
    std::ofstream file;
    file.open(filename, std::ios::out);
    if (!file.is_open())
    {
        std::cout << "Mesh::saveSTL: could not open " << filename
                  << std::endl;
        return false;
    }

    // File header (giving human-readable info about file type)
    std::string header = "This is a binary STL exported from Ao.";
    file.write(header.c_str(), header.length());

    // Pad the rest of the header to 80 bytes
    for (int i=header.length(); i < 80; ++i)
    {
        file.put(' ');
    }

    // Write the triangle count to the file
    uint32_t num = std::accumulate(meshes.begin(), meshes.end(), (uint32_t)0,
            [](uint32_t i, const Mesh* m){ return i + m->branes.size(); });
    file.write(reinterpret_cast<char*>(&num), sizeof(num));

    for (const auto& m : meshes)
    {
        for (const auto& t : m->branes)
        {
            // Write out the normal vector for this face (all zeros)
            float norm[3] = {0, 0, 0};
            file.write(reinterpret_cast<char*>(&norm), sizeof(norm));

            // Iterate over vertices (which are indices into the verts list)
            for (unsigned i=0; i < 3; ++i)
            {
                auto v = m->verts[t[i]];
                float vert[3] = {v.x(), v.y(), v.z()};
                file.write(reinterpret_cast<char*>(&vert), sizeof(vert));
            }

            // Write out this face's attribute short
            uint16_t attrib = 0;
            file.write(reinterpret_cast<char*>(&attrib), sizeof(attrib));
        }
    }

    return true;
}

bool Mesh::saveSTL(const std::string& filename)
{
    return saveSTL(filename, {this});
}

}   // namespace Kernel
