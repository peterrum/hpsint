#pragma once

#include <deal.II/base/geometry_info.h>

#include <deal.II/grid/grid_tools.h>

#include <deal.II/numerics/data_out.h>

#include <pf-applications/grain_tracker/distributed_stitching.h>

namespace dealii
{
  template <int dim, int spacedim>
  class MyDataOut : public DataOut<dim, spacedim>
  {
  public:
    void
    write_vtu_in_parallel(
      const std::string &         filename,
      const MPI_Comm &            comm,
      const DataOutBase::VtkFlags vtk_flags = DataOutBase::VtkFlags()) const
    {
      const unsigned int myrank = Utilities::MPI::this_mpi_process(comm);

      std::ofstream ss_out(filename);

      if (myrank == 0) // header
        {
          std::stringstream ss;
          DataOutBase::write_vtu_header(ss, vtk_flags);
          ss_out << ss.rdbuf();
        }

      if (true) // main
        {
          const auto &                  patches      = this->get_patches();
          const types::global_dof_index my_n_patches = patches.size();
          const types::global_dof_index global_n_patches =
            Utilities::MPI::sum(my_n_patches, comm);

          std::stringstream ss;
          if (my_n_patches > 0 || (global_n_patches == 0 && myrank == 0))
            DataOutBase::write_vtu_main(patches,
                                        this->get_dataset_names(),
                                        this->get_nonscalar_data_ranges(),
                                        vtk_flags,
                                        ss);

          const auto temp = Utilities::MPI::gather(comm, ss.str(), 0);

          if (myrank == 0)
            for (const auto &i : temp)
              ss_out << i;
        }

      if (myrank == 0) // footer
        {
          std::stringstream ss;
          DataOutBase::write_vtu_footer(ss);
          ss_out << ss.rdbuf();
        }
    }
  };
} // namespace dealii

namespace Sintering
{
  namespace Postprocessors
  {
    template <int dim, typename VectorType>
    void
    output_grain_contours(const Mapping<dim> &   mapping,
                          const DoFHandler<dim> &background_dof_handler,
                          const VectorType &     vector,
                          const double           iso_level,
                          const std::string      filename,
                          const unsigned int     n_coarsening_steps = 0,
                          const unsigned int     n_subdivisions     = 1,
                          const double           tolerance          = 1e-10)
    {
      const bool has_ghost_elements = vector.has_ghost_elements();

      if (has_ghost_elements == false)
        vector.update_ghost_values();

      // step 0) coarsen background mesh 1 or 2 times to reduce memory
      // consumption

      auto vector_to_be_used                 = &vector;
      auto background_dof_handler_to_be_used = &background_dof_handler;

      parallel::distributed::Triangulation<dim> tria_copy(
        background_dof_handler.get_communicator());
      DoFHandler<dim> dof_handler_copy;
      VectorType      solution_dealii;

      if (n_coarsening_steps != 0)
        {
          tria_copy.copy_triangulation(
            background_dof_handler.get_triangulation());
          dof_handler_copy.reinit(tria_copy);
          dof_handler_copy.distribute_dofs(
            background_dof_handler.get_fe_collection());

          // 1) copy solution so that it has the right ghosting
          const auto partitioner =
            std::make_shared<Utilities::MPI::Partitioner>(
              dof_handler_copy.locally_owned_dofs(),
              DoFTools::extract_locally_relevant_dofs(dof_handler_copy),
              dof_handler_copy.get_communicator());

          solution_dealii.reinit(vector.n_blocks());

          for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
            {
              solution_dealii.block(b).reinit(partitioner);
              solution_dealii.block(b).copy_locally_owned_data_from(
                vector.block(b));
            }

          solution_dealii.update_ghost_values();

          for (unsigned int i = 0; i < n_coarsening_steps; ++i)
            {
              // 2) mark cells for refinement
              for (const auto &cell : tria_copy.active_cell_iterators())
                if (cell->is_locally_owned() &&
                    (static_cast<unsigned int>(cell->level() + 1) ==
                     tria_copy.n_global_levels()))
                  cell->set_coarsen_flag();

              // 3) perform interpolation and initialize data structures
              tria_copy.prepare_coarsening_and_refinement();

              parallel::distributed::
                SolutionTransfer<dim, typename VectorType::BlockType>
                  solution_trans(dof_handler_copy);

              std::vector<const typename VectorType::BlockType *>
                solution_dealii_ptr(solution_dealii.n_blocks());
              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_dealii_ptr[b] = &solution_dealii.block(b);

              solution_trans.prepare_for_coarsening_and_refinement(
                solution_dealii_ptr);

              tria_copy.execute_coarsening_and_refinement();

              dof_handler_copy.distribute_dofs(
                background_dof_handler.get_fe_collection());

              const auto partitioner =
                std::make_shared<Utilities::MPI::Partitioner>(
                  dof_handler_copy.locally_owned_dofs(),
                  DoFTools::extract_locally_relevant_dofs(dof_handler_copy),
                  dof_handler_copy.get_communicator());

              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_dealii.block(b).reinit(partitioner);

              std::vector<typename VectorType::BlockType *> solution_ptr(
                solution_dealii.n_blocks());
              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_ptr[b] = &solution_dealii.block(b);

              solution_trans.interpolate(solution_ptr);
              solution_dealii.update_ghost_values();
            }

          vector_to_be_used                 = &solution_dealii;
          background_dof_handler_to_be_used = &dof_handler_copy;
        }



      // step 1) create surface mesh
      std::vector<Point<dim>>        vertices;
      std::vector<CellData<dim - 1>> cells;
      SubCellData                    subcelldata;

      const GridTools::MarchingCubeAlgorithm<dim,
                                             typename VectorType::BlockType>
        mc(mapping,
           background_dof_handler_to_be_used->get_fe(),
           n_subdivisions,
           tolerance);

      for (unsigned int b = 0; b < vector_to_be_used->n_blocks() - 2; ++b)
        {
          const unsigned int old_size = cells.size();

          mc.process(*background_dof_handler_to_be_used,
                     vector_to_be_used->block(b + 2),
                     iso_level,
                     vertices,
                     cells);

          for (unsigned int i = old_size; i < cells.size(); ++i)
            cells[i].material_id = b;
        }

      Triangulation<dim - 1, dim> tria;

      if (vertices.size() > 0)
        tria.create_triangulation(vertices, cells, subcelldata);
      else
        GridGenerator::hyper_cube(tria, -1e-6, 1e-6);

      Vector<float> vector_grain_id(tria.n_active_cells());
      for (const auto cell : tria.active_cell_iterators())
        vector_grain_id[cell->active_cell_index()] = cell->material_id();

      Vector<float> vector_rank(tria.n_active_cells());
      vector_rank = Utilities::MPI::this_mpi_process(
        background_dof_handler.get_communicator());

      // step 2) output mesh
      MyDataOut<dim - 1, dim> data_out;
      data_out.attach_triangulation(tria);
      data_out.add_data_vector(vector_grain_id, "grain_id");
      data_out.add_data_vector(vector_rank, "subdomain");

      data_out.build_patches();
      data_out.write_vtu_in_parallel(filename,
                                     background_dof_handler.get_communicator());

      if (has_ghost_elements == false)
        vector.zero_out_ghost_values();
    }



    template <int dim, typename VectorType>
    void
    estimate_overhead(const Mapping<dim> &   mapping,
                      const DoFHandler<dim> &background_dof_handler,
                      const VectorType &     vector,
                      const bool             output_mesh = false)
    {
      using Number = typename VectorType::value_type;

      const std::int64_t n_active_cells_0 =
        background_dof_handler.get_triangulation().n_global_active_cells();
      std::int64_t n_active_cells_1 = 0;

      if (output_mesh)
        {
          DataOut<dim> data_out;
          data_out.attach_triangulation(
            background_dof_handler.get_triangulation());
          data_out.build_patches(mapping);
          data_out.write_vtu_in_parallel(
            "reduced_mesh.0.vtu", background_dof_handler.get_communicator());
        }

      for (unsigned int b = 0; b < vector.n_blocks() - 2; ++b)
        {
          parallel::distributed::Triangulation<dim> tria_copy(
            background_dof_handler.get_communicator());
          DoFHandler<dim> dof_handler_copy;
          VectorType      solution_dealii;

          tria_copy.copy_triangulation(
            background_dof_handler.get_triangulation());
          dof_handler_copy.reinit(tria_copy);
          dof_handler_copy.distribute_dofs(
            background_dof_handler.get_fe_collection());

          // 1) copy solution so that it has the right ghosting
          const auto partitioner =
            std::make_shared<Utilities::MPI::Partitioner>(
              dof_handler_copy.locally_owned_dofs(),
              DoFTools::extract_locally_relevant_dofs(dof_handler_copy),
              dof_handler_copy.get_communicator());

          solution_dealii.reinit(vector.n_blocks());

          for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
            {
              solution_dealii.block(b).reinit(partitioner);
              solution_dealii.block(b).copy_locally_owned_data_from(
                vector.block(b));
            }

          solution_dealii.update_ghost_values();

          unsigned int n_active_cells = tria_copy.n_global_active_cells();

          while (true)
            {
              // 2) mark cells for refinement
              Vector<Number> values(
                dof_handler_copy.get_fe().n_dofs_per_cell());
              for (const auto &cell : dof_handler_copy.active_cell_iterators())
                {
                  if (cell->is_locally_owned() == false ||
                      cell->refine_flag_set())
                    continue;

                  cell->get_dof_values(solution_dealii.block(b + 2), values);

                  if (values.linfty_norm() <= 0.05)
                    cell->set_coarsen_flag();
                }

              // 3) perform interpolation and initialize data structures
              tria_copy.prepare_coarsening_and_refinement();

              parallel::distributed::
                SolutionTransfer<dim, typename VectorType::BlockType>
                  solution_trans(dof_handler_copy);

              std::vector<const typename VectorType::BlockType *>
                solution_dealii_ptr(solution_dealii.n_blocks());
              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_dealii_ptr[b] = &solution_dealii.block(b);

              solution_trans.prepare_for_coarsening_and_refinement(
                solution_dealii_ptr);

              tria_copy.execute_coarsening_and_refinement();

              dof_handler_copy.distribute_dofs(
                background_dof_handler.get_fe_collection());

              const auto partitioner =
                std::make_shared<Utilities::MPI::Partitioner>(
                  dof_handler_copy.locally_owned_dofs(),
                  DoFTools::extract_locally_relevant_dofs(dof_handler_copy),
                  dof_handler_copy.get_communicator());

              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_dealii.block(b).reinit(partitioner);

              std::vector<typename VectorType::BlockType *> solution_ptr(
                solution_dealii.n_blocks());
              for (unsigned int b = 0; b < solution_dealii.n_blocks(); ++b)
                solution_ptr[b] = &solution_dealii.block(b);

              solution_trans.interpolate(solution_ptr);
              solution_dealii.update_ghost_values();

              if (n_active_cells == tria_copy.n_global_active_cells())
                break;

              n_active_cells = tria_copy.n_global_active_cells();
            }

          n_active_cells_1 += tria_copy.n_global_active_cells();

          if (output_mesh)
            {
              DataOut<dim> data_out;
              data_out.attach_triangulation(tria_copy);
              data_out.build_patches(mapping);
              data_out.write_vtu_in_parallel(
                "reduced_mesh." + std::to_string(b + 1) + ".vtu",
                background_dof_handler.get_communicator());
            }
        }

      ConditionalOStream pcout(std::cout,
                               Utilities::MPI::this_mpi_process(
                                 background_dof_handler.get_communicator()) ==
                                 0);

      pcout << "Estimation of mesh overhead: "
            << std::to_string((n_active_cells_0 * vector.n_blocks()) * 100 /
                                (n_active_cells_1 + 2 * n_active_cells_0) -
                              100)
            << "%" << std::endl
            << std::endl;
    }



    namespace internal
    {
      template <int dim, typename Number>
      unsigned int
      run_flooding(const typename DoFHandler<dim>::cell_iterator &   cell,
                   const LinearAlgebra::distributed::Vector<Number> &solution,
                   LinearAlgebra::distributed::Vector<Number> &particle_ids,
                   const unsigned int                          id)
      {
        const double threshold_lower     = 0.2;  // TODO
        const double invalid_particle_id = -1.0; // TODO

        if (cell->has_children())
          {
            unsigned int counter = 0;

            for (const auto &child : cell->child_iterators())
              counter += run_flooding<dim>(child, solution, particle_ids, id);

            return counter;
          }

        if (cell->is_locally_owned() == false)
          return 0;

        const auto particle_id = particle_ids[cell->global_active_cell_index()];

        if (particle_id != invalid_particle_id)
          return 0; // cell has been visited

        Vector<double> values(cell->get_fe().n_dofs_per_cell());

        cell->get_dof_values(solution, values);

        if (values.linfty_norm() >= threshold_lower)
          return 0; // cell has no particle

        particle_ids[cell->global_active_cell_index()] = id;

        unsigned int counter = 1;

        for (const auto face : cell->face_indices())
          if (cell->at_boundary(face) == false)
            counter += run_flooding<dim>(cell->neighbor(face),
                                         solution,
                                         particle_ids,
                                         id);

        return counter;
      }
    } // namespace internal

    template <int dim, typename VectorType>
    void
    estimate_porosity(const DoFHandler<dim> &dof_handler,
                      const VectorType &     solution)
    {
      const double invalid_particle_id = -1.0; // TODO

      const auto tria = dynamic_cast<const parallel::TriangulationBase<dim> *>(
        &dof_handler.get_triangulation());

      AssertThrow(tria, ExcNotImplemented());

      const auto comm = dof_handler.get_communicator();

      LinearAlgebra::distributed::Vector<double> particle_ids(
        tria->global_active_cell_index_partitioner().lock());

      // step 1) run flooding and determine local particles and give them
      // local ids
      particle_ids = invalid_particle_id;

      unsigned int counter = 0;
      unsigned int offset  = 0;

      for (const auto &cell : dof_handler.active_cell_iterators())
        if (internal::run_flooding<dim>(
              cell, solution.block(0), particle_ids, counter) > 0)
          counter++;

      // step 2) determine the global number of locally determined particles
      // and give each one an unique id by shifting the ids
      MPI_Exscan(&counter, &offset, 1, MPI_UNSIGNED, MPI_SUM, comm);

      for (auto &particle_id : particle_ids)
        if (particle_id != invalid_particle_id)
          particle_id += offset;

      // step 3) get particle ids on ghost cells and figure out if local
      // particles and ghost particles might be one particle
      particle_ids.update_ghost_values();

      std::vector<std::vector<std::tuple<unsigned int, unsigned int>>>
        local_connectiviy(counter);

      for (const auto &ghost_cell :
           dof_handler.get_triangulation().active_cell_iterators())
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
                         subface <
                         GeometryInfo<dim>::n_subfaces(
                           dealii::internal::SubfaceCase<dim>::case_isotropic);
                         ++subface)
                      add(ghost_cell,
                          ghost_cell->neighbor_child_on_subface(face, subface));
                  }
                else
                  add(ghost_cell, ghost_cell->neighbor(face));
              }
          }

      // step 4) based on the local-ghost information, figure out all
      // particles on all processes that belong togher (unification ->
      // clique), give each clique an unique id, and return mapping from the
      // global non-unique ids to the global ids
      const auto local_to_global_particle_ids =
        GrainTracker::perform_distributed_stitching(comm, local_connectiviy);
    }

  } // namespace Postprocessors
} // namespace Sintering