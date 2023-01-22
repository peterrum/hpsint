#pragma once

#include <deal.II/base/point.h>

#include <deal.II/distributed/tria.h>

#include <pf-applications/sintering/tools.h>

#include <pf-applications/grain_tracker/tracker.h>

namespace Sintering
{
  using namespace dealii;

  template <int dim, typename Number, typename VectorizedArrayType>
  struct AdvectionCellDataBase
  {
    Point<dim, VectorizedArrayType>     rc;
    Tensor<1, dim, VectorizedArrayType> force;
    VectorizedArrayType                 volume{-1.};

    bool
    has_non_zero() const
    {
      return std::any_of(volume.begin(), volume.end(), [](const auto &val) {
        return val > 0;
      });
    }

  protected:
    void
    fill(const unsigned int cell_id, const Number *rc_i, const Number *fdata)
    {
      volume[cell_id] = fdata[0];

      for (unsigned int d = 0; d < dim; ++d)
        {
          rc[d][cell_id]    = rc_i[d];
          force[d][cell_id] = fdata[d + 1];
        }
    }

    void
    nullify(const unsigned int cell_id)
    {
      for (unsigned int d = 0; d < dim; ++d)
        {
          rc[d][cell_id]    = 0;
          force[d][cell_id] = 0;
        }
      volume[cell_id] = -1.; // To prevent division by zero
    }
  };

  template <int dim, typename Number, typename VectorizedArrayType>
  struct AdvectionCellData
  {};

  template <typename Number, typename VectorizedArrayType>
  struct AdvectionCellData<2, Number, VectorizedArrayType>
    : public AdvectionCellDataBase<2, Number, VectorizedArrayType>
  {
    static constexpr int dim = 2;

    VectorizedArrayType torque{0};

    void
    fill(const unsigned int cell_id, const Number *rc_i, const Number *fdata)
    {
      AdvectionCellDataBase<dim, Number, VectorizedArrayType>::fill(cell_id,
                                                                    rc_i,
                                                                    fdata);

      torque[cell_id] = fdata[dim];
    }

    void
    nullify(const unsigned int cell_id)
    {
      AdvectionCellDataBase<dim, Number, VectorizedArrayType>::nullify(cell_id);

      torque[cell_id] = 0;
    }

    Tensor<1, dim, VectorizedArrayType>
    cross(const Tensor<1, dim, VectorizedArrayType> &r) const
    {
      Tensor<1, dim, VectorizedArrayType> p;
      p[0] = -r[1];
      p[1] = r[0];
      p *= torque;

      return p;
    }
  };

  template <typename Number, typename VectorizedArrayType>
  struct AdvectionCellData<3, Number, VectorizedArrayType>
    : public AdvectionCellDataBase<3, Number, VectorizedArrayType>
  {
    static constexpr int dim = 3;

    Tensor<1, dim, VectorizedArrayType> torque;

    void
    fill(const unsigned int cell_id, const Number *rc_i, const Number *fdata)
    {
      AdvectionCellDataBase<dim, Number, VectorizedArrayType>::fill(cell_id,
                                                                    rc_i,
                                                                    fdata);

      for (unsigned int d = 0; d < dim; ++d)
        {
          torque[d][cell_id] = fdata[dim + d];
        }
    }

    void
    nullify(const unsigned int cell_id)
    {
      AdvectionCellDataBase<dim, Number, VectorizedArrayType>::nullify(cell_id);

      for (unsigned int d = 0; d < dim; ++d)
        {
          torque[d][cell_id] = 0;
        }
    }

    Tensor<1, dim, VectorizedArrayType>
    cross(const Tensor<1, dim, VectorizedArrayType> &r) const
    {
      return cross_product_3d(torque, r);
    }
  };

  template <int dim, typename Number, typename VectorizedArrayType>
  class AdvectionMechanism
  {
  public:
    // Force, torque and grain volume
    static constexpr unsigned int n_comp_volume_force_torque =
      (dim == 3 ? 7 : 4);

    AdvectionMechanism()
      : is_active(false)
      , mt(0.0)
      , mr(0.0)
      , grain_tracker(nullptr)
    {}

    AdvectionMechanism(
      const double mt,
      const double mr,
      const std::vector<AdvectionCellData<dim, Number, VectorizedArrayType>>
        &current_cell_data)
      : is_active(true)
      , mt(mt)
      , mr(mr)
      , grain_tracker(nullptr)
    {
      this->current_cell_data = current_cell_data;
    }

    AdvectionMechanism(const bool                                enable,
                       const double                              mt,
                       const double                              mr,
                       const GrainTracker::Tracker<dim, Number> &grain_tracker)
      : is_active(enable)
      , mt(mt)
      , mr(mr)
      , grain_tracker(&grain_tracker)
    {}

    void
    reinit(const unsigned int cell) const
    {
      if (grain_table.size(0) > 0)
        {
          const unsigned int n_op    = grain_table.size(1);
          const unsigned int n_lanes = VectorizedArrayType::size();

          current_cell_data.resize(n_op);

          for (unsigned int op = 0; op < n_op; ++op)
            for (unsigned int v = 0; v < n_lanes; ++v)
              {
                const auto index = grain_table[cell][op][v];

                if (index != numbers::invalid_unsigned_int)
                  current_cell_data[op].fill(v,
                                             grain_center(index),
                                             grain_data(index));
                else
                  current_cell_data[op].nullify(v);
              }
        }

      has_velocity_vector.resize(current_cell_data.size());
      for (unsigned int op = 0; op < current_cell_data.size(); ++op)
        has_velocity_vector[op] = current_cell_data.at(op).has_non_zero();
    }

    bool
    has_velocity(const unsigned int order_parameter_id) const
    {
      return has_velocity_vector[order_parameter_id];
    }

    Tensor<1, dim, VectorizedArrayType>
    get_velocity(const unsigned int                     order_parameter_id,
                 const Point<dim, VectorizedArrayType> &r) const
    {
      const auto &op_cell_data = current_cell_data.at(order_parameter_id);

      // Translational velocity
      const auto vt = mt / op_cell_data.volume * op_cell_data.force;

      // Get vector from the particle center to the current point
      const auto r_rc = r - op_cell_data.rc;

      // Rotational velocity
      const auto vr = mr / op_cell_data.volume * op_cell_data.cross(r_rc);

      // Total advection velocity
      const auto v_adv = vt + vr;

      return v_adv;
    }

    void
    set_table_entry(const unsigned int cell,
                    const unsigned int op,
                    const unsigned int v,
                    const unsigned int index)
    {
      grain_table[cell][op][v] = index;
    }

    void
    nullify_table(const unsigned int n_cells, const unsigned int n_op)
    {
      grain_table.reinit({n_cells, n_op, VectorizedArrayType::size()});
      grain_table.fill(numbers::invalid_unsigned_int);
    }

    void
    nullify_data(const unsigned int n_segments)
    {
      grains_data.assign(n_comp_volume_force_torque * n_segments, 0);
      grains_center.assign(dim * n_segments, 0);
    }

    Number *
    grain_data(const unsigned int index)
    {
      return &grains_data[n_comp_volume_force_torque * index];
    }

    const Number *
    grain_data(const unsigned int index) const
    {
      return &grains_data[n_comp_volume_force_torque * index];
    }

    Number *
    grain_center(const unsigned int index)
    {
      return &grains_center[dim * index];
    }

    const Number *
    grain_center(const unsigned int index) const
    {
      return &grains_center[dim * index];
    }

    std::vector<Number> &
    get_grains_data()
    {
      return grains_data;
    }

    const std::vector<Number> &
    get_grains_data() const
    {
      return grains_data;
    }

    bool
    enabled() const
    {
      return is_active;
    }

    template <typename Stream>
    void
    print_forces(Stream &out) const
    {
      out << std::endl;
      out << "Grains segments volumes, forces and torques:" << std::endl;

      for (const auto &[grain_id, grain] : grain_tracker->get_grains())
        {
          for (unsigned int segment_id = 0;
               segment_id < grain.get_segments().size();
               segment_id++)
            {
              const Number *data = grain_data(
                grain_tracker->get_grain_segment_index(grain_id, segment_id));

              Number                 volume(*data++);
              Tensor<1, dim, Number> force(make_array_view(data, data + dim));
              moment_t<dim, Number>  torque(
                create_moment_from_buffer<dim>(data + dim));

              out << "Grain id = " << grain_id
                  << ", segment id = " << segment_id << ": "
                  << "volume = " << volume << " | "
                  << "force  = " << force << " | "
                  << "torque = " << torque << std::endl;
            }
        }

      out << std::endl;
    }

  private:
    const bool   is_active;
    const double mt;
    const double mr;

    mutable std::vector<AdvectionCellData<dim, Number, VectorizedArrayType>>
      current_cell_data;

    const SmartPointer<const GrainTracker::Tracker<dim, Number>> grain_tracker;

    Table<3, unsigned int> grain_table;

    mutable std::vector<bool> has_velocity_vector;

    std::vector<Number> grains_data;
    std::vector<Number> grains_center;
  };
} // namespace Sintering