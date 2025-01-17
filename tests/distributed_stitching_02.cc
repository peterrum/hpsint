#include <deal.II/base/mpi.h>

#include <deal.II/distributed/shared_tria.h>

#include <deal.II/dofs/dof_handler.h>
#include <deal.II/dofs/dof_tools.h>

#include <deal.II/fe/fe_q.h>
#include <deal.II/fe/mapping_q1.h>

#include <deal.II/grid/grid_generator.h>

#include <deal.II/numerics/data_out.h>
#include <deal.II/numerics/vector_tools.h>

#include <pf-applications/grain_tracker/distributed_stitching.h>

using namespace dealii;

template <int dim>
class Solution : public Function<dim>
{
public:
  Solution() = default;

  virtual double
  value(const Point<dim> &p, const unsigned int component = 0) const
  {
    (void)component;

    if (p.distance(Point<dim>(1.0, 0.5)) < 0.4)
      return 1.0;
    if (p.distance(Point<dim>(2.0, 0.5)) < 0.4)
      return 1.0;
    if (p.distance(Point<dim>(3.0, 0.75)) < 0.2)
      return 1.0;

    return 0.0;
  }
};

constexpr double invalid_particle_id = -1.0;

int
main(int argc, char **argv)
{
  Utilities::MPI::MPI_InitFinalize mpi_init(argc, argv, 1);

  const MPI_Comm     comm    = MPI_COMM_WORLD;
  const unsigned int n_procs = Utilities::MPI::n_mpi_processes(comm);
  const unsigned int my_rank = Utilities::MPI::this_mpi_process(comm);

  AssertDimension(n_procs, 4);
  (void)n_procs;

  const unsigned int dim = 2;

  FE_Q<dim>      fe{1};
  MappingQ1<dim> mapping;

  // create triangulation and DoFHandler
  parallel::shared::Triangulation<dim> tria(
    comm,
    ::Triangulation<dim>::none,
    true,
    parallel::shared::Triangulation<dim>::partition_custom_signal);

  tria.signals.post_refinement.connect([&]() {
    for (const auto &cell : tria.active_cell_iterators())
      if ((cell->center()[0] < 1.0) && (cell->center()[1] < 0.5))
        cell->set_subdomain_id(0);
      else if ((cell->center()[0] >= 1.0) && (cell->center()[1] < 0.5))
        cell->set_subdomain_id(1);
      else if ((cell->center()[0] < 1.0) && (cell->center()[1] >= 0.5))
        cell->set_subdomain_id(2);
      else
        cell->set_subdomain_id(3);
  });

  GridGenerator::subdivided_hyper_rectangle(tria,
                                            {4, 1},
                                            {0.0, 0.0},
                                            {4.0, 1.0});
  tria.refine_global(7);

  DoFHandler<dim> dof_handler(tria);
  dof_handler.distribute_dofs(fe);

  // create particles
  LinearAlgebra::distributed::Vector<double> solution(
    dof_handler.locally_owned_dofs(),
    DoFTools::extract_locally_active_dofs(dof_handler),
    comm);

  VectorTools::interpolate(mapping, dof_handler, Solution<dim>(), solution);

  Vector<double> ranks(tria.n_active_cells());
  ranks = my_rank;

  // output particles
  DataOut<dim> data_out;
  data_out.add_data_vector(dof_handler, solution, "solution");
  data_out.add_data_vector(ranks,
                           "ranks",
                           DataOut<dim>::DataVectorType::type_cell_data);
  data_out.build_patches(mapping);
  data_out.write_vtu_in_parallel("soltuion.vtu", comm);

  solution.update_ghost_values();

  // step 1) run flooding and determine local particles and give them local ids
  LinearAlgebra::distributed::Vector<double> particle_ids(
    tria.global_active_cell_index_partitioner().lock());
  particle_ids = invalid_particle_id;

  unsigned int counter   = 0;
  unsigned int offset    = 0;
  const double threshold = 1e-9;

  for (const auto &cell : dof_handler.active_cell_iterators())
    if (GrainTracker::run_flooding<dim>(cell,
                                        solution,
                                        particle_ids,
                                        counter,
                                        threshold,
                                        invalid_particle_id) > 0)
      counter++;

  // step 2) determine the global number of locally determined particles and
  // give each one an unique id by shifting the ids
  MPI_Exscan(&counter, &offset, 1, MPI_UNSIGNED, MPI_SUM, comm);

  for (auto &particle_id : particle_ids)
    if (particle_id != invalid_particle_id)
      particle_id += offset;

  // step 3) get particle ids on ghost cells and figure out if local particles
  // and ghost particles might be one particle
  particle_ids.update_ghost_values();

  std::vector<std::vector<std::tuple<unsigned int, unsigned int>>>
    local_connectiviy(counter);

  for (const auto &ghost_cell : tria.active_cell_iterators())
    if (ghost_cell->is_ghost())
      {
        const auto particle_id =
          particle_ids[ghost_cell->global_active_cell_index()];

        if (particle_id == invalid_particle_id)
          continue;

        for (const auto face : ghost_cell->face_indices())
          {
            if (ghost_cell->at_boundary(face))
              continue;

            const auto add = [&](const auto &ghost_cell,
                                 const auto &local_cell) {
              if (local_cell->is_locally_owned() == false)
                return;

              const auto neighbor_particle_id =
                particle_ids[local_cell->global_active_cell_index()];

              if (neighbor_particle_id == invalid_particle_id)
                return;

              auto &temp = local_connectiviy[neighbor_particle_id - offset];
              temp.emplace_back(ghost_cell->subdomain_id(), particle_id);
              std::sort(temp.begin(), temp.end());
              temp.erase(std::unique(temp.begin(), temp.end()), temp.end());
            };

            if (ghost_cell->neighbor(face)->has_children())
              {
                for (unsigned int subface = 0;
                     subface < GeometryInfo<dim>::n_subfaces(
                                 internal::SubfaceCase<dim>::case_isotropic);
                     ++subface)
                  add(ghost_cell,
                      ghost_cell->neighbor_child_on_subface(face, subface));
              }
            else
              add(ghost_cell, ghost_cell->neighbor(face));
          }
      }

  // step 4) based on the local-ghost information, figure out all particles
  // on all processes that belong togher (unification -> clique), give each
  // clique an unique id, and return mapping from the global non-unique
  // ids to the global ids
  const auto local_to_global_particle_ids =
    GrainTracker::perform_distributed_stitching(comm, local_connectiviy);

  // step 5) determine properties of particles (volume, radius, center)
  unsigned int n_particles = 0;

  // ... determine the number of particles
  if (Utilities::MPI::sum(local_to_global_particle_ids.size(), comm) == 0)
    n_particles = 0;
  else
    {
      n_particles = (local_to_global_particle_ids.size() == 0) ?
                      0 :
                      *std::max_element(local_to_global_particle_ids.begin(),
                                        local_to_global_particle_ids.end());
      n_particles = Utilities::MPI::max(n_particles, comm) + 1;
    }

  std::vector<double> particle_info(n_particles * (1 + dim));

  // ... compute local information
  for (const auto &cell : tria.active_cell_iterators())
    if (cell->is_locally_owned())
      {
        const auto particle_id = particle_ids[cell->global_active_cell_index()];

        if (particle_id == invalid_particle_id)
          continue;

        const unsigned int unique_id =
          local_to_global_particle_ids[static_cast<unsigned int>(particle_id) -
                                       offset];

        AssertIndexRange(unique_id, n_particles);

        particle_info[(dim + 1) * unique_id + 0] += cell->measure();

        for (unsigned int d = 0; d < dim; ++d)
          particle_info[(dim + 1) * unique_id + 1 + d] +=
            cell->center()[d] * cell->measure();
      }

  // ... reduce information
  MPI_Reduce(my_rank == 0 ? MPI_IN_PLACE : particle_info.data(),
             particle_info.data(),
             particle_info.size(),
             MPI_DOUBLE,
             MPI_SUM,
             0,
             comm);

  if (my_rank == 0)
    {
      for (unsigned int i = 0; i < n_particles; ++i)
        {
          std::cout << "Particle " << std::to_string(i) << " has volume "
                    << std::sqrt(particle_info[i * (1 + dim)] / numbers::PI)
                    << " and has a center ("
                    << particle_info[i * (1 + dim) + 1] /
                         particle_info[i * (1 + dim)];
          for (unsigned int d = 1; d < dim; ++d)
            std::cout << ", "
                      << particle_info[i * (1 + dim) + 1 + d] /
                           particle_info[i * (1 + dim)];
          std::cout << ")" << std::endl;
        }
    }
}