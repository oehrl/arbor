#include <string>
#include <vector>

#include <arbor/util/optional.hpp>
#include <arbor/mechcat.hpp>
#include <arbor/math.hpp>
#include <arbor/cable_cell.hpp>

#include "fvm_layout.hpp"
#include "util/maputil.hpp"
#include "util/rangeutil.hpp"
#include "util/span.hpp"
#include "io/sepval.hpp"

#include "common.hpp"
#include "unit_test_catalogue.hpp"
#include "../common_cells.hpp"

using namespace std::string_literals;
using namespace arb;

using util::make_span;
using util::count_along;
using util::value_by_key;

namespace {
    double area(const segment* s) {
        if (auto soma = s->as_soma()) {
            return math::area_sphere(soma->radius());
        }
        else if (auto cable = s->as_cable()) {
            unsigned nc = cable->num_sub_segments();
            double a = 0;
            for (unsigned i = 0; i<nc; ++i) {
                a += math::area_frustrum(cable->lengths()[i], cable->radii()[i], cable->radii()[i+1]);
            }
            return a;
        }
        else {
            return 0;
        }
    }

    double volume(const segment* s) {
        if (auto soma = s->as_soma()) {
            return math::volume_sphere(soma->radius());
        }
        else if (auto cable = s->as_cable()) {
            unsigned nc = cable->num_sub_segments();
            double v = 0;
            for (unsigned i = 0; i<nc; ++i) {
                v += math::volume_frustrum(cable->lengths()[i], cable->radii()[i], cable->radii()[i+1]);
            }
            return v;
        }
        else {
            return 0;
        }
    }

    std::vector<cable_cell> two_cell_system() {
        std::vector<cable_cell> cells;

        // Cell 0: simple ball and stick (see common_cells.hpp)
        cells.push_back(make_cell_ball_and_stick());

        // Cell 1: ball and 3-stick, but with uneven dendrite
        // length and heterogeneous electrical properties:
        //
        // Bulk resistivity: 90 Ω·cm
        // capacitance:
        //    soma:       0.01  F/m² [default]
        //    segment 1:  0.017 F/m²
        //    segment 2:  0.013 F/m²
        //    segment 3:  0.018 F/m²
        //
        // Soma diameter: 14 µm
        // Some mechanisms: HH (default params)
        //
        // Segment 1 diameter: 1 µm
        // Segment 1 length:   200 µm
        //
        // Segment 2 diameter: 0.8 µm
        // Segment 2 length:   300 µm
        //
        // Segment 3 diameter: 0.7 µm
        // Segment 3 length:   180 µm
        //
        // Dendrite mechanisms: passive (default params).
        // Stimulus at end of segment 2, amplitude 0.45.
        // Stimulus at end of segment 3, amplitude -0.2.
        //
        // All dendrite segments with 4 compartments.

        soma_cell_builder builder(7.);
        auto b1 = builder.add_branch(0, 200, 0.5,  0.5, 4,  "dend");
        auto b2 = builder.add_branch(1, 300, 0.4,  0.4, 4,  "dend");
        auto b3 = builder.add_branch(1, 180, 0.35, 0.35, 4, "dend");
        auto cell = builder.make_cell();

        cell.paint("soma", "hh");
        cell.paint("dend", "pas");

        using ::arb::reg::branch;
        cell.paint(branch(b1), membrane_capacitance{0.017});
        cell.paint(branch(b2), membrane_capacitance{0.013});
        cell.paint(branch(b3), membrane_capacitance{0.018});

        cell.place(mlocation{2,1}, i_clamp{5.,  80., 0.45});
        cell.place(mlocation{3,1}, i_clamp{40., 10.,-0.2});

        cell.default_parameters.axial_resistivity = 90;

        cells.push_back(std::move(cell));
        return cells;
    }

    void check_two_cell_system(std::vector<cable_cell>& cells) {
        ASSERT_EQ(2u, cells[0].num_branches());
        ASSERT_EQ(cells[0].segment(1)->num_compartments(), 4u);
        ASSERT_EQ(cells[1].num_branches(), 4u);
        ASSERT_EQ(cells[1].segment(1)->num_compartments(), 4u);
        ASSERT_EQ(cells[1].segment(2)->num_compartments(), 4u);
        ASSERT_EQ(cells[1].segment(3)->num_compartments(), 4u);
    }
} // namespace

TEST(fvm_layout, topology) {
    std::vector<cable_cell> cells = two_cell_system();
    check_two_cell_system(cells);

    fvm_discretization D = fvm_discretize(cells, neuron_parameter_defaults);

    // Expected CV layouts for cells, segment indices in paren.
    //
    // Cell 0:
    //
    // CV: |  0     ][1| 2 | 3 | 4 |5|
    //     [soma (0)][  segment (1)  ]
    // 
    // Cell 1:
    //
    // CV: |  6     ][7| 8 | 9 | 10| 11 | 12 | 13 | 14 | 15|
    //     [soma (2)][  segment (3)  ][  segment (4)       ]
    //                                [  segment (5)       ]
    //                                  | 16 | 17 | 18 | 19|

    EXPECT_EQ(2u, D.ncell);
    EXPECT_EQ(20u, D.ncv);

    unsigned nseg = 6;
    EXPECT_EQ(nseg, D.segments.size());

    // General sanity checks:

    ASSERT_EQ(D.ncell, D.cell_segment_part().size());
    ASSERT_EQ(D.ncell, D.cell_cv_part().size());

    ASSERT_EQ(D.ncv, D.parent_cv.size());
    ASSERT_EQ(D.ncv, D.cv_to_cell.size());
    ASSERT_EQ(D.ncv, D.face_conductance.size());
    ASSERT_EQ(D.ncv, D.cv_area.size());
    ASSERT_EQ(D.ncv, D.cv_capacitance.size());

    // Partitions of CVs and segments by cell:

    using spair = std::pair<fvm_size_type, fvm_size_type>;
    using ipair = std::pair<fvm_index_type, fvm_index_type>;

    EXPECT_EQ(spair(0, 2),    D.cell_segment_part()[0]);
    EXPECT_EQ(spair(2, nseg), D.cell_segment_part()[1]);

    EXPECT_EQ(ipair(0, 6),       D.cell_cv_part()[0]);
    EXPECT_EQ(ipair(6, D.ncv), D.cell_cv_part()[1]);

    // Segment and CV parent relationships:

    using ivec = std::vector<fvm_index_type>;

    EXPECT_EQ(ivec({0,0,1,2,3,4,6,6,7,8,9,10,11,12,13,14,11,16,17,18}), D.parent_cv);

    EXPECT_FALSE(D.segments[0].has_parent());
    EXPECT_EQ(1, D.segments[1].parent_cv);

    EXPECT_FALSE(D.segments[2].has_parent());
    EXPECT_EQ(7, D.segments[3].parent_cv);
    EXPECT_EQ(11, D.segments[4].parent_cv);
    EXPECT_EQ(11, D.segments[5].parent_cv);

    // Segment CV ranges (half-open, exclusing parent):

    EXPECT_EQ(ipair(0,1), D.segments[0].cv_range());
    EXPECT_EQ(ipair(2,6), D.segments[1].cv_range());
    EXPECT_EQ(ipair(6,7), D.segments[2].cv_range());
    EXPECT_EQ(ipair(8,12), D.segments[3].cv_range());
    EXPECT_EQ(ipair(12,16), D.segments[4].cv_range());
    EXPECT_EQ(ipair(16,20), D.segments[5].cv_range());

    // CV to cell index:

    for (auto ci: make_span(D.ncell)) {
        for (auto cv: make_span(D.cell_cv_part()[ci])) {
            EXPECT_EQ(ci, (fvm_size_type)D.cv_to_cell[cv]);
        }
    }
}

TEST(fvm_layout, diam_and_area) {
    std::vector<cable_cell> cells = two_cell_system();
    check_two_cell_system(cells);

    fvm_discretization D = fvm_discretize(cells, neuron_parameter_defaults);

    // Note: stick models have constant diameter segments.
    // Refer to comment above for CV vs. segment layout.

    EXPECT_FLOAT_EQ(12.6157, D.diam_um[0]);
    EXPECT_FLOAT_EQ(1.0 ,    D.diam_um[1]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[2]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[3]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[4]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[5]);

    EXPECT_FLOAT_EQ(14.0,    D.diam_um[6]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[7]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[8]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[9]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[10]);
    EXPECT_FLOAT_EQ(1.0,     D.diam_um[11]);
    EXPECT_FLOAT_EQ(0.8,     D.diam_um[12]);
    EXPECT_FLOAT_EQ(0.8,     D.diam_um[13]);
    EXPECT_FLOAT_EQ(0.8,     D.diam_um[14]);
    EXPECT_FLOAT_EQ(0.8,     D.diam_um[15]);
    EXPECT_FLOAT_EQ(0.7,     D.diam_um[16]);
    EXPECT_FLOAT_EQ(0.7,     D.diam_um[17]);
    EXPECT_FLOAT_EQ(0.7,     D.diam_um[18]);
    EXPECT_FLOAT_EQ(0.7,     D.diam_um[19]);

    std::vector<double> A;
    for (auto ci: make_span(D.ncell)) {
        for (auto si: make_span(cells[ci].num_branches())) {
            A.push_back(area(cells[ci].segment(si)));
        }
    }

    unsigned n = 4; // compartments per dendritic segment
    EXPECT_FLOAT_EQ(A[0],       D.cv_area[0]);
    EXPECT_FLOAT_EQ(A[1]/(2*n), D.cv_area[1]);
    EXPECT_FLOAT_EQ(A[1]/n,     D.cv_area[2]);
    EXPECT_FLOAT_EQ(A[1]/n,     D.cv_area[3]);
    EXPECT_FLOAT_EQ(A[1]/n,     D.cv_area[4]);
    EXPECT_FLOAT_EQ(A[1]/(2*n), D.cv_area[5]);

    EXPECT_FLOAT_EQ(A[2],       D.cv_area[6]);
    EXPECT_FLOAT_EQ(A[3]/(2*n), D.cv_area[7]);
    EXPECT_FLOAT_EQ(A[3]/n,     D.cv_area[8]);
    EXPECT_FLOAT_EQ(A[3]/n,     D.cv_area[9]);
    EXPECT_FLOAT_EQ(A[3]/n,     D.cv_area[10]);
    EXPECT_FLOAT_EQ((A[3]+A[4]+A[5])/(2*n), D.cv_area[11]);
    EXPECT_FLOAT_EQ(A[4]/n,     D.cv_area[12]);
    EXPECT_FLOAT_EQ(A[4]/n,     D.cv_area[13]);
    EXPECT_FLOAT_EQ(A[4]/n,     D.cv_area[14]);
    EXPECT_FLOAT_EQ(A[4]/(2*n), D.cv_area[15]);
    EXPECT_FLOAT_EQ(A[5]/n,     D.cv_area[16]);
    EXPECT_FLOAT_EQ(A[5]/n,     D.cv_area[17]);
    EXPECT_FLOAT_EQ(A[5]/n,     D.cv_area[18]);
    EXPECT_FLOAT_EQ(A[5]/(2*n), D.cv_area[19]);

    // Confirm proportional allocation of surface capacitance:

    // CV 9 should have area-weighted sum of the specific
    // capacitance from segments 3, 4 and 5 (cell 1 segments
    // 1, 2 and 3 respectively).

    double cm1 = 0.017, cm2 = 0.013, cm3 = 0.018;

    double c = A[3]/(2*n)*cm1+A[4]/(2*n)*cm2+A[5]/(2*n)*cm3;
    EXPECT_FLOAT_EQ(c, D.cv_capacitance[11]);

    double cm0 = neuron_parameter_defaults.membrane_capacitance.value();
    c = A[2]*cm0;
    EXPECT_FLOAT_EQ(c, D.cv_capacitance[6]);

    // Confirm face conductance within a constant diameter
    // equals a/h·1/rL where a is the cross sectional
    // area, and h is the compartment length (given the
    // regular discretization).

    auto cable = cells[1].segment(2)->as_cable();
    double a = volume(cable)/cable->length();
    EXPECT_FLOAT_EQ(math::pi<double>*0.8*0.8/4, a);

    double rL = 90;
    double h = cable->length()/4;
    double g = a/h/rL; // [µm·S/cm]
    g *= 100; // [µS]

    EXPECT_FLOAT_EQ(g, D.face_conductance[13]);
}

TEST(fvm_layout, mech_index) {
    std::vector<cable_cell> cells = two_cell_system();
    check_two_cell_system(cells);

    // Add four synapses of two varieties across the cells.
    cells[0].place(mlocation{1, 0.4}, "expsyn");
    cells[0].place(mlocation{1, 0.4}, "expsyn");
    cells[1].place(mlocation{2, 0.4}, "exp2syn");
    cells[1].place(mlocation{3, 0.4}, "expsyn");

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;

    fvm_discretization D = fvm_discretize(cells, gprop.default_parameters);
    fvm_mechanism_data M = fvm_build_mechanism_data(gprop, cells, D);

    auto& hh_config = M.mechanisms.at("hh");
    auto& expsyn_config = M.mechanisms.at("expsyn");
    auto& exp2syn_config = M.mechanisms.at("exp2syn");

    using ivec = std::vector<fvm_index_type>;
    using fvec = std::vector<fvm_value_type>;

    // HH on somas of two cells, with CVs 0 and 5.
    // Proportional area contrib: soma area/CV area.

    EXPECT_EQ(mechanismKind::density, hh_config.kind);
    EXPECT_EQ(ivec({0,6}), hh_config.cv);

    fvec norm_area({area(cells[0].soma())/D.cv_area[0], area(cells[1].soma())/D.cv_area[6]});
    EXPECT_TRUE(testing::seq_almost_eq<double>(norm_area, hh_config.norm_area));

    // Three expsyn synapses, two 0.4 along segment 1, and one 0.4 along segment 5.
    // These two synapses can be coalesced into 1 synapse
    // 0.4 along => second (non-parent) CV for segment.

    EXPECT_EQ(ivec({3, 17}), expsyn_config.cv);

    // One exp2syn synapse, 0.4 along segment 4.

    EXPECT_EQ(ivec({13}), exp2syn_config.cv);

    // There should be a K and Na ion channel associated with each
    // hh mechanism node.

    ASSERT_EQ(1u, M.ions.count("na"s));
    ASSERT_EQ(1u, M.ions.count("k"s));
    EXPECT_EQ(0u, M.ions.count("ca"s));

    EXPECT_EQ(ivec({0,6}), M.ions.at("na"s).cv);
    EXPECT_EQ(ivec({0,6}), M.ions.at("k"s).cv);
}

struct exp_instance {
    int cv;
    int multiplicity;
    std::vector<unsigned> targets;
    double e;
    double tau;

    template <typename Seq>
    exp_instance(int cv, const Seq& tgts, double e, double tau):
        cv(cv), multiplicity(util::size(tgts)), e(e), tau(tau)
    {
        targets.reserve(util::size(tgts));
        for (auto t: tgts) targets.push_back(t);
        util::sort(targets);
    }

    bool matches(const exp_instance& I) const {
        return I.cv==cv && I.e==e && I.tau==tau && I.targets==targets;
    }

    bool is_in(const arb::fvm_mechanism_config& C) const {
        std::vector<unsigned> _;
        auto part = util::make_partition(_, C.multiplicity);
        auto& evals = *value_by_key(C.param_values, "e");
        // Handle both expsyn and exp2syn by looking for "tau1" if "tau"
        // parameter is not found.
        auto& tauvals = value_by_key(C.param_values, "tau")?
            *value_by_key(C.param_values, "tau"):
            *value_by_key(C.param_values, "tau1");

        for (auto i: make_span(C.multiplicity.size())) {
            exp_instance other(C.cv[i],
                               util::subrange_view(C.target, part[i]),
                               evals[i],
                               tauvals[i]);
            if (matches(other)) return true;
        }
        return false;
    }
};

TEST(fvm_layout, coalescing_synapses) {
    using ivec = std::vector<fvm_index_type>;

    auto syn_desc = [&](const char* name, double val0, double val1) {
        mechanism_desc m(name);
        m.set("e", val0);
        m.set("tau", val1);
        return m;
    };

    auto syn_desc_2 = [&](const char* name, double val0, double val1) {
        mechanism_desc m(name);
        m.set("e", val0);
        m.set("tau1", val1);
        return m;
    };

    cable_cell_global_properties gprop_no_coalesce;
    gprop_no_coalesce.default_parameters = neuron_parameter_defaults;
    gprop_no_coalesce.coalesce_synapses = false;

    cable_cell_global_properties gprop_coalesce;
    gprop_coalesce.default_parameters = neuron_parameter_defaults;
    gprop_coalesce.coalesce_synapses = true;

    using L=std::initializer_list<unsigned>;

    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.5}, "expsyn");
        cell.place(mlocation{1, 0.7}, "expsyn");
        cell.place(mlocation{1, 0.9}, "expsyn");

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        auto &expsyn_config = M.mechanisms.at("expsyn");
        EXPECT_EQ(ivec({2, 3, 4, 5}), expsyn_config.cv);
        EXPECT_EQ(ivec({1, 1, 1, 1}), expsyn_config.multiplicity);
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.5}, "exp2syn");
        cell.place(mlocation{1, 0.7}, "expsyn");
        cell.place(mlocation{1, 0.9}, "exp2syn");

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        auto &expsyn_config = M.mechanisms.at("expsyn");
        EXPECT_EQ(ivec({2, 4}), expsyn_config.cv);
        EXPECT_EQ(ivec({1, 1}), expsyn_config.multiplicity);

        auto &exp2syn_config = M.mechanisms.at("exp2syn");
        EXPECT_EQ(ivec({3, 5}), exp2syn_config.cv);
        EXPECT_EQ(ivec({1, 1}), exp2syn_config.multiplicity);
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.5}, "expsyn");
        cell.place(mlocation{1, 0.7}, "expsyn");
        cell.place(mlocation{1, 0.9}, "expsyn");

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_no_coalesce, {cell}, D);

        auto &expsyn_config = M.mechanisms.at("expsyn");
        EXPECT_EQ(ivec({2, 3, 4, 5}), expsyn_config.cv);
        EXPECT_TRUE(expsyn_config.multiplicity.empty());
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.5}, "exp2syn");
        cell.place(mlocation{1, 0.7}, "expsyn");
        cell.place(mlocation{1, 0.9}, "exp2syn");

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_no_coalesce, {cell}, D);

        auto &expsyn_config = M.mechanisms.at("expsyn");
        EXPECT_EQ(ivec({2, 4}), expsyn_config.cv);
        EXPECT_TRUE(expsyn_config.multiplicity.empty());

        auto &exp2syn_config = M.mechanisms.at("exp2syn");
        EXPECT_EQ(ivec({3, 5}), exp2syn_config.cv);
        EXPECT_TRUE(exp2syn_config.multiplicity.empty());
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.3}, "expsyn");
        cell.place(mlocation{1, 0.7}, "expsyn");
        cell.place(mlocation{1, 0.7}, "expsyn");

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        auto &expsyn_config = M.mechanisms.at("expsyn");
        EXPECT_EQ(ivec({2, 4}), expsyn_config.cv);
        EXPECT_EQ(ivec({2, 2}), expsyn_config.multiplicity);
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 0, 0.2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 0, 0.2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 0.1, 0.2));
        cell.place(mlocation{1, 0.7}, syn_desc("expsyn", 0.1, 0.2));

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        std::vector<exp_instance> instances{
            exp_instance(2, L{0, 1}, 0., 0.2),
            exp_instance(2, L{2}, 0.1, 0.2),
            exp_instance(4, L{3}, 0.1, 0.2),
        };
        auto& config = M.mechanisms.at("expsyn");
        for (auto& instance: instances) {
            EXPECT_TRUE(instance.is_in(config));
        }
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.7}, syn_desc("expsyn", 0, 3));
        cell.place(mlocation{1, 0.7}, syn_desc("expsyn", 1, 3));
        cell.place(mlocation{1, 0.7}, syn_desc("expsyn", 0, 3));
        cell.place(mlocation{1, 0.7}, syn_desc("expsyn", 1, 3));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 0, 2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 1, 2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 0, 2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn", 1, 2));

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        std::vector<exp_instance> instances{
            exp_instance(2, L{4, 6}, 0.0, 2.0),
            exp_instance(2, L{5, 7}, 1.0, 2.0),
            exp_instance(4, L{0, 2}, 0.0, 3.0),
            exp_instance(4, L{1, 3}, 1.0, 3.0),
        };
        auto& config = M.mechanisms.at("expsyn");
        for (auto& instance: instances) {
            EXPECT_TRUE(instance.is_in(config));
        }
    }
    {
        cable_cell cell = make_cell_ball_and_stick();

        // Add synapses of two varieties.
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn",  1, 2));
        cell.place(mlocation{1, 0.3}, syn_desc_2("exp2syn", 4, 1));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn",  1, 2));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn",  5, 1));
        cell.place(mlocation{1, 0.3}, syn_desc_2("exp2syn", 1, 3));
        cell.place(mlocation{1, 0.3}, syn_desc("expsyn",  1, 2));
        cell.place(mlocation{1, 0.7}, syn_desc_2("exp2syn", 2, 2));
        cell.place(mlocation{1, 0.7}, syn_desc_2("exp2syn", 2, 1));
        cell.place(mlocation{1, 0.7}, syn_desc_2("exp2syn", 2, 1));
        cell.place(mlocation{1, 0.7}, syn_desc_2("exp2syn", 2, 2));

        fvm_discretization D = fvm_discretize({cell}, neuron_parameter_defaults);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop_coalesce, {cell}, D);

        for (auto &instance: {exp_instance(2, L{0,2,5}, 1, 2),
                              exp_instance(2, L{3},     5, 1)}) {
            EXPECT_TRUE(instance.is_in(M.mechanisms.at("expsyn")));
        }

        for (auto &instance: {exp_instance(2, L{4},   1, 3),
                              exp_instance(2, L{1},   4, 1),
                              exp_instance(4, L{7,8}, 2, 1),
                              exp_instance(4, L{6,9}, 2, 2)}) {
            EXPECT_TRUE(instance.is_in(M.mechanisms.at("exp2syn")));
        }
    }
}

TEST(fvm_layout, synapse_targets) {
    std::vector<cable_cell> cells = two_cell_system();

    // Add synapses with different parameter values so that we can
    // ensure: 1) CVs for each synapse mechanism are sorted while
    // 2) the target index for each synapse corresponds to the
    // original ordering.

    const unsigned nsyn = 7;
    std::vector<double> syn_e(nsyn);
    for (auto i: count_along(syn_e)) {
        syn_e[i] = 0.1*(1+i);
    }

    auto syn_desc = [&](const char* name, int idx) {
        return mechanism_desc(name).set("e", syn_e.at(idx));
    };

    cells[0].place(mlocation{1, 0.9}, syn_desc("expsyn", 0));
    cells[0].place(mlocation{0, 0.5}, syn_desc("expsyn", 1));
    cells[0].place(mlocation{1, 0.4}, syn_desc("expsyn", 2));

    cells[1].place(mlocation{2, 0.4}, syn_desc("exp2syn", 3));
    cells[1].place(mlocation{1, 0.4}, syn_desc("exp2syn", 4));
    cells[1].place(mlocation{3, 0.4}, syn_desc("expsyn", 5));
    cells[1].place(mlocation{3, 0.7}, syn_desc("exp2syn", 6));

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;

    fvm_discretization D = fvm_discretize(cells, gprop.default_parameters);
    fvm_mechanism_data M = fvm_build_mechanism_data(gprop, cells, D);

    ASSERT_EQ(1u, M.mechanisms.count("expsyn"));
    ASSERT_EQ(1u, M.mechanisms.count("exp2syn"));

    auto& expsyn_cv = M.mechanisms.at("expsyn").cv;
    auto& expsyn_target = M.mechanisms.at("expsyn").target;
    auto& expsyn_e = value_by_key(M.mechanisms.at("expsyn").param_values, "e"s).value();

    auto& exp2syn_cv = M.mechanisms.at("exp2syn").cv;
    auto& exp2syn_target = M.mechanisms.at("exp2syn").target;
    auto& exp2syn_e = value_by_key(M.mechanisms.at("exp2syn").param_values, "e"s).value();

    EXPECT_TRUE(util::is_sorted(expsyn_cv));
    EXPECT_TRUE(util::is_sorted(exp2syn_cv));

    using uvec = std::vector<fvm_size_type>;
    uvec all_target_indices;
    util::append(all_target_indices, expsyn_target);
    util::append(all_target_indices, exp2syn_target);
    util::sort(all_target_indices);

    uvec nsyn_iota;
    util::assign(nsyn_iota, make_span(nsyn));
    EXPECT_EQ(nsyn_iota, all_target_indices);

    for (auto i: count_along(expsyn_target)) {
        EXPECT_EQ(syn_e[expsyn_target[i]], expsyn_e[i]);
    }

    for (auto i: count_along(exp2syn_target)) {
        EXPECT_EQ(syn_e[exp2syn_target[i]], exp2syn_e[i]);
    }
}

namespace {
    double wm_impl(double wa, double xa) {
        return wa? xa/wa: 0;
    }

    template <typename... R>
    double wm_impl(double wa, double xa, double w, double x, R... rest) {
        return wm_impl(wa+w, xa+w*x, rest...);
    }

    // Computed weighted mean (w*x + ...) / (w + ...).
    template <typename... R>
    double wmean(double w, double x, R... rest) {
        return wm_impl(w, w*x, rest...);
    }
}

TEST(fvm_layout, density_norm_area) {
    // Test area-weighted linear combination of density mechanism parameters.

    // Create a cell with 4 segments:
    //   - Soma (segment 0) plus three dendrites (1, 2, 3) meeting at a branch point.
    //   - HH mechanism on all segments.
    //   - Dendritic segments are given 3 compartments each.
    //
    // The CV corresponding to the branch point should comprise the terminal
    // 1/6 of segment 1 and the initial 1/6 of segments 2 and 3.
    //
    // The HH mechanism current density parameters ('gnabar', 'gkbar' and 'gl') are set
    // differently for each segment:
    //
    //   soma:      all default values (gnabar = 0.12, gkbar = .036, gl = .0003)
    //   segment 1: gl = .0002
    //   segment 2: gkbar = .05
    //   segment 3: gkbar = .07, gl = .0004
    //
    // Geometry:
    //   segment 1: 100 µm long, 1 µm diameter cylinder.
    //   segment 2: 200 µm long, diameter linear taper from 1 µm to 0.2 µm.
    //   segment 3: 150 µm long, 0.8 µm diameter cylinder.
    //
    // Use divided compartment view on segments to compute area contributions.

    soma_cell_builder builder(12.6157/2.0);

    //                 p  len   r1   r2  ncomp tag
    builder.add_branch(0, 100, 0.5, 0.5,     3, "reg1");
    builder.add_branch(1, 200, 0.5, 0.1,     3, "reg2");
    builder.add_branch(1, 150, 0.4, 0.4,     3, "reg3");

    double dflt_gkbar = .036;
    double dflt_gl = 0.0003;

    double seg1_gl = .0002;
    double seg2_gkbar = .05;
    double seg3_gkbar = .0004;
    double seg3_gl = .0004;

    auto hh_0 = mechanism_desc("hh");

    auto hh_1 = mechanism_desc("hh");
    hh_1["gl"] = seg1_gl;

    auto hh_2 = mechanism_desc("hh");
    hh_2["gkbar"] = seg2_gkbar;

    auto hh_3 = mechanism_desc("hh");
    hh_3["gkbar"] = seg3_gkbar;
    hh_3["gl"] = seg3_gl;

    auto cell = builder.make_cell();
    cell.paint("soma", std::move(hh_0));
    cell.paint("reg1", std::move(hh_1));
    cell.paint("reg2", std::move(hh_2));
    cell.paint("reg3", std::move(hh_3));

    std::vector<cable_cell> cells{std::move(cell)};

    int ncv = 11; //ncomp + 1
    std::vector<double> expected_gkbar(ncv, dflt_gkbar);
    std::vector<double> expected_gl(ncv, dflt_gl);

    auto div_by_ends = [](const cable_segment* cable) {
        return div_compartment_by_ends(cable->num_compartments(), cable->radii(), cable->lengths());
    };
    auto& segs = cells[0].segments();
    double soma_area = area(segs[0].get());
    auto seg1_divs = div_by_ends(segs[1]->as_cable());
    auto seg2_divs = div_by_ends(segs[2]->as_cable());
    auto seg3_divs = div_by_ends(segs[3]->as_cable());

    // CV 0: soma
    // CV1: left of segment 1
    expected_gl[0] = dflt_gl;
    expected_gl[1] = seg1_gl;

    expected_gl[2] = seg1_gl;
    expected_gl[3] = seg1_gl;

    // CV 4: mix of right of segment 1 and left of segments 2 and 3.
    expected_gkbar[4] = wmean(seg1_divs(2).right.area, dflt_gkbar, seg2_divs(0).left.area, seg2_gkbar, seg3_divs(0).left.area, seg3_gkbar);
    expected_gl[4] = wmean(seg1_divs(2).right.area, seg1_gl, seg2_divs(0).left.area, dflt_gl, seg3_divs(0).left.area, seg3_gl);

    // CV 5-7: just segment 2
    expected_gkbar[5] = seg2_gkbar;
    expected_gkbar[6] = seg2_gkbar;
    expected_gkbar[7] = seg2_gkbar;

    // CV 8-10: just segment 3
    expected_gkbar[8] = seg3_gkbar;
    expected_gkbar[9] = seg3_gkbar;
    expected_gkbar[10] = seg3_gkbar;
    expected_gl[8] = seg3_gl;
    expected_gl[9] = seg3_gl;
    expected_gl[10] = seg3_gl;

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;

    fvm_discretization D = fvm_discretize(cells, gprop.default_parameters);
    fvm_mechanism_data M = fvm_build_mechanism_data(gprop, cells, D);

    // Check CV area assumptions.
    // Note: area integrator used here and in `fvm_multicell` may differ, and so areas computed may
    // differ some due to rounding area, even given that we're dealing with simple truncated cones
    // for segments. Check relative error within a tolerance of (say) 10 epsilon.

    double area_relerr = 10*std::numeric_limits<double>::epsilon();
    EXPECT_TRUE(testing::near_relative(D.cv_area[0],
        soma_area, area_relerr));
    EXPECT_TRUE(testing::near_relative(D.cv_area[1],
        seg1_divs(0).left.area, area_relerr));
    EXPECT_TRUE(testing::near_relative(D.cv_area[2],
        seg1_divs(0).right.area+seg1_divs(1).left.area, area_relerr));
    EXPECT_TRUE(testing::near_relative(D.cv_area[4],
        seg1_divs(2).right.area+seg2_divs(0).left.area+seg3_divs(0).left.area, area_relerr));
    EXPECT_TRUE(testing::near_relative(D.cv_area[7],
        seg2_divs(2).right.area, area_relerr));

    // Grab the HH parameters from the mechanism.

    EXPECT_EQ(1u, M.mechanisms.size());
    ASSERT_EQ(1u, M.mechanisms.count("hh"));
    auto& hh_params = M.mechanisms.at("hh").param_values;

    auto& gkbar = value_by_key(hh_params, "gkbar"s).value();
    auto& gl = value_by_key(hh_params, "gl"s).value();

    EXPECT_TRUE(testing::seq_almost_eq<double>(expected_gkbar, gkbar));
    EXPECT_TRUE(testing::seq_almost_eq<double>(expected_gl, gl));
}

TEST(fvm_layout, valence_verify) {
    auto cell = soma_cell_builder(6).make_cell();
    cell.paint("soma", "test_cl_valence");
    std::vector<cable_cell> cells{std::move(cell)};

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;

    fvm_discretization D = fvm_discretize(cells, neuron_parameter_defaults);

    mechanism_catalogue testcat = make_unit_test_catalogue();
    gprop.catalogue = &testcat;

    // Missing the 'cl' ion:
    EXPECT_THROW(fvm_build_mechanism_data(gprop, cells, D), cable_cell_error);

    // Adding ion, should be fine now:
    gprop.default_parameters.ion_data["cl"] = { 1., 1., 0. };
    gprop.ion_species["cl"] = -1;
    EXPECT_NO_THROW(fvm_build_mechanism_data(gprop, cells, D));

    // 'cl' ion has wrong charge:
    gprop.ion_species["cl"] = -2;
    EXPECT_THROW(fvm_build_mechanism_data(gprop, cells, D), cable_cell_error);
}

TEST(fvm_layout, ion_weights) {
    // Create a cell with 4 segments:
    //   - Soma (segment 0) plus three dendrites (1, 2, 3) meeting at a branch point.
    //   - Dendritic segments are given 1 compartments each.
    //
    //         /
    //        d2
    //       /
    //   s0-d1
    //       \.
    //        d3
    //
    // The CV corresponding to the branch point should comprise the terminal
    // 1/2 of segment 1 and the initial 1/2 of segments 2 and 3.
    //
    // Geometry:
    //   soma 0: radius 5 µm
    //   dend 1: 100 µm long, 1 µm diameter cylinder, tag 2
    //   dend 2: 200 µm long, 1 µm diameter cylinder, tag 3
    //   dend 3: 100 µm long, 1 µm diameter cylinder, tag 4
    //
    // The radius of the soma is chosen such that the surface area of soma is
    // the same as a 100µm dendrite, which makes it easier to describe the
    // expected weights.

    auto construct_cell = []() {
        soma_cell_builder builder(5);
        builder.add_branch(0, 100, 0.5, 0.5, 1, "dend");
        builder.add_branch(1, 200, 0.5, 0.5, 1, "dend");
        builder.add_branch(1, 100, 0.5, 0.5, 1, "dend");
        return builder.make_cell();
    };

    using uvec = std::vector<fvm_size_type>;
    using ivec = std::vector<fvm_index_type>;
    using fvec = std::vector<fvm_value_type>;

    uvec mech_branches[] = {
        {0}, {0,2}, {2, 3}, {0, 1, 2, 3}, {3}
    };

    ivec expected_ion_cv[] = {
        {0}, {0, 2, 3}, {2, 3, 4}, {0, 1, 2, 3, 4}, {2, 4}
    };

    fvec expected_init_iconc[] = {
        {0.}, {0., 1./2, 0.}, {1./4, 0., 0.}, {0., 0., 0., 0., 0.}, {3./4, 0.}
    };

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;

    fvm_value_type cai = gprop.default_parameters.ion_data["ca"].init_int_concentration;
    fvm_value_type cao = gprop.default_parameters.ion_data["ca"].init_ext_concentration;

    for (auto& v: expected_init_iconc) {
        for (auto& iconc: v) {
            iconc *= cai;
        }
    }

    for (auto run: count_along(mech_branches)) {
        SCOPED_TRACE("run "+std::to_string(run));
        auto c = construct_cell();

        for (auto i: mech_branches[run]) {
            c.paint(reg::branch(i), "test_ca");
        }

        std::vector<cable_cell> cells{std::move(c)};

        fvm_discretization D = fvm_discretize(cells, gprop.default_parameters);
        fvm_mechanism_data M = fvm_build_mechanism_data(gprop, cells, D);

        ASSERT_EQ(1u, M.ions.count("ca"s));
        auto& ca = M.ions.at("ca"s);

        EXPECT_EQ(expected_ion_cv[run], ca.cv);

        EXPECT_TRUE(testing::seq_almost_eq<fvm_value_type>(expected_init_iconc[run], ca.init_iconc));

        EXPECT_TRUE(util::all_of(ca.init_econc, [cao](fvm_value_type v) { return v==cao; }));
    }
}

TEST(fvm_layout, revpot) {
    // Create two cells with three ions 'a', 'b' and 'c'.
    // Configure a reversal potential mechanism that writes to 'a' and
    // another that writes to 'b' and 'c'.
    //
    // Confirm:
    //     * Inconsistencies between revpot mech assignments are caught at discretization.
    //     * Reversal potential mechanisms are only extended where there exists another
    //       mechanism that reads them.

    mechanism_catalogue testcat = make_unit_test_catalogue();

    soma_cell_builder builder(5);
    builder.add_branch(0, 100, 0.5, 0.5, 1, "dend");
    builder.add_branch(1, 200, 0.5, 0.5, 1, "dend");
    builder.add_branch(1, 100, 0.5, 0.5, 1, "dend");
    auto cell = builder.make_cell();
    cell.paint("soma", "read_eX/c");
    cell.paint("soma", "read_eX/a");
    cell.paint("dend", "read_eX/a");

    std::vector<cable_cell> cells{cell, cell};

    cable_cell_global_properties gprop;
    gprop.default_parameters = neuron_parameter_defaults;
    gprop.catalogue = &testcat;

    gprop.ion_species = {{"a", 1}, {"b", 2}, {"c", 3}};
    gprop.add_ion("a", 1, 10., 0, 0);
    gprop.add_ion("b", 2, 30., 0, 0);
    gprop.add_ion("c", 3, 50., 0, 0);

    gprop.default_parameters.reversal_potential_method["a"] = "write_eX/a";
    mechanism_desc write_eb_ec = "write_multiple_eX/x=b,y=c";

    {
        // need to specify ion "c" as well.
        auto test_gprop = gprop;
        test_gprop.default_parameters.reversal_potential_method["b"] = write_eb_ec;

        fvm_discretization D = fvm_discretize(cells, test_gprop.default_parameters);
        EXPECT_THROW(fvm_build_mechanism_data(test_gprop, cells, D), cable_cell_error);
    }

    {
        // conflict with ion "c" on second cell.
        auto test_gprop = gprop;
        test_gprop.default_parameters.reversal_potential_method["b"] = write_eb_ec;
        test_gprop.default_parameters.reversal_potential_method["c"] = write_eb_ec;
        cells[1].default_parameters.reversal_potential_method["c"] = "write_eX/c";

        fvm_discretization D = fvm_discretize(cells, test_gprop.default_parameters);
        EXPECT_THROW(fvm_build_mechanism_data(test_gprop, cells, D), cable_cell_error);
    }

    auto& cell1_prop = cells[1].default_parameters;
    cell1_prop.reversal_potential_method.clear();
    cell1_prop.reversal_potential_method["b"] = write_eb_ec;
    cell1_prop.reversal_potential_method["c"] = write_eb_ec;

    fvm_discretization D = fvm_discretize(cells, gprop.default_parameters);
    fvm_mechanism_data M = fvm_build_mechanism_data(gprop, cells, D);

    // Only CV which needs write_multiple_eX/x=b,y=c is the soma (first CV)
    // of the second cell.
    auto soma1_index = D.cell_cv_bounds[1];
    ASSERT_EQ(1u, M.mechanisms.count(write_eb_ec.name()));
    EXPECT_EQ((std::vector<fvm_index_type>(1, soma1_index)), M.mechanisms.at(write_eb_ec.name()).cv);
}
