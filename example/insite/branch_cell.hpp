#pragma once

#include <array>
#include <random>

#include <nlohmann/json.hpp>

#include <arbor/common_types.hpp>
#include <arbor/cable_cell.hpp>

#include <sup/json_params.hpp>

// Parameters used to generate the random cell morphologies.
struct cell_parameters {
    cell_parameters() = default;

    //  Maximum number of levels in the cell (not including the soma)
    unsigned max_depth = 5;

    // The following parameters are described as ranges.
    // The first value is at the soma, and the last value is used on the last level.
    // Values at levels in between are found by linear interpolation.
    std::array<double,2> branch_probs = {1.0, 0.5}; //  Probability of a branch occuring.
    std::array<unsigned,2> compartments = {20, 2};  //  Compartment count on a branch.
    std::array<double,2> lengths = {200, 20};       //  Length of branch in μm.

    // The number of synapses per cell.
    unsigned synapses = 1;
};

cell_parameters parse_cell_parameters(nlohmann::json& json) {
    cell_parameters params;
    sup::param_from_json(params.max_depth, "depth", json);
    sup::param_from_json(params.branch_probs, "branch-probs", json);
    sup::param_from_json(params.compartments, "compartments", json);
    sup::param_from_json(params.lengths, "lengths", json);
    sup::param_from_json(params.synapses, "synapses", json);

    return params;
}

// Helper used to interpolate in branch_cell.
template <typename T>
double interp(const std::array<T,2>& r, unsigned i, unsigned n) {
    double p = i * 1./(n-1);
    double r0 = r[0];
    double r1 = r[1];
    return r[0] + p*(r1-r0);
}

arb::cable_cell branch_cell(arb::cell_gid_type gid, const cell_parameters& params) {
    arb::sample_tree tree;

    // Add soma.
    double soma_radius = 12.6157/2.0;
    tree.append(arb::mnpos, {{0,0,0,soma_radius}, 1}); // For area of 500 μm².

    std::vector<std::vector<unsigned>> levels;
    levels.push_back({0});

    // Standard mersenne_twister_engine seeded with gid.
    std::mt19937 gen(gid);
    std::uniform_real_distribution<double> dis(0, 1);

    double dend_radius = 0.5; // Diameter of 1 μm for each cable.

    double dist_from_soma = soma_radius;
    for (unsigned i=0; i<params.max_depth; ++i) {
        // Branch prob at this level.
        double bp = interp(params.branch_probs, i, params.max_depth);
        // Length at this level.
        double l = interp(params.lengths, i, params.max_depth);
        // Number of compartments at this level.
        unsigned nc = std::round(interp(params.compartments, i, params.max_depth));

        std::vector<unsigned> sec_ids;
        for (unsigned sec: levels[i]) {
            for (unsigned j=0; j<2; ++j) {
                if (dis(gen)<bp) {
                    auto z = dist_from_soma;
                    auto p = tree.append(sec, {{0,0,z,dend_radius}, 3});
                    if (nc>1) {
                        auto dz = l/nc;
                        for (unsigned k=1; k<nc; ++k) {
                            p = tree.append(p, {{0,0,z+k*dz, dend_radius}, 3});
                        }
                    }
                    sec_ids.push_back(tree.append(p, {{0,0,z+l,dend_radius}, 3}));
                }
            }
        }
        if (sec_ids.empty()) {
            break;
        }
        levels.push_back(sec_ids);

        dist_from_soma += l;
    }

    arb::label_dict d;

    using arb::reg::tagged;
    d.set("soma",      tagged(1));
    d.set("dendrites", join(tagged(3), tagged(4)));

    arb::cable_cell cell(arb::morphology(tree, true), d, true);

    cell.paint("soma", "hh");
    cell.paint("dendrites", "pas");
    cell.default_parameters.axial_resistivity = 100; // [Ω·cm]

    // Add spike threshold detector at the soma.
    cell.place(arb::mlocation{0,0}, arb::threshold_detector{10});

    // Add a synapse to the mid point of the first dendrite.
    cell.place(arb::mlocation{1, 0.5}, "expsyn");

    // Add additional synapses that will not be connected to anything.
    for (unsigned i=1u; i<params.synapses; ++i) {
        cell.place(arb::mlocation{1, 0.5}, "expsyn");
    }

    return cell;
}
