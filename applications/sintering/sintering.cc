// ---------------------------------------------------------------------
//
// Copyright (C) 2020 by the deal.II authors
//
// This file is part of the deal.II library.
//
// The deal.II library is free software; you can use it, redistribute
// it, and/or modify it under the terms of the GNU Lesser General
// Public License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.
// The full text of the license can be found in the file LICENSE.md at
// the top level directory of deal.II.
//
// ---------------------------------------------------------------------

// Sintering of N particles loaded from a CSV file

#ifndef SINTERING_DIM
static_assert(false, "No dimension has been given!");
#endif

#ifndef MAX_SINTERING_GRAINS
static_assert(false, "No grains number has been given!");
#endif

//#define USE_FE_Q_iso_Q1

#ifdef USE_FE_Q_iso_Q1
#  define FE_DEGREE 2
#  define N_Q_POINTS_1D FE_DEGREE * 2
#else
#  define FE_DEGREE 1
#  define N_Q_POINTS_1D FE_DEGREE + 1
#endif

#define WITH_TIMING
//#define WITH_TIMING_OUTPUT

#include <deal.II/base/revision.h>

#include <pf-applications/base/revision.h>

#include <pf-applications/sintering/driver.h>
#include <pf-applications/sintering/initial_values_circle.h>
#include <pf-applications/sintering/initial_values_cloud.h>

#include <cstdlib>

using namespace dealii;

std::string
concatenate_strings(const int argc, char **argv)
{
  std::string result = std::string(argv[0]);

  for (int i = 0; i < argc; ++i)
    result = result + " " + std::string(argv[i]);

  return result;
}

int
main(int argc, char **argv)
{
  Utilities::MPI::MPI_InitFinalize mpi_init(argc, argv, 1);

  ConditionalOStream pcout(std::cout,
                           Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) ==
                             0);

  pcout << "Running: " << concatenate_strings(argc, argv) << std::endl;
  pcout << "  - deal.II (branch: " << DEAL_II_GIT_BRANCH
        << "; revision: " << DEAL_II_GIT_REVISION
        << "; short: " << DEAL_II_GIT_SHORTREV << ")" << std::endl;
  pcout << "  - deal.II (branch: " << PF_APPLICATIONS_GIT_BRANCH
        << "; revision: " << PF_APPLICATIONS_GIT_REVISION
        << "; short: " << PF_APPLICATIONS_GIT_SHORTREV << ")" << std::endl;
  pcout << std::endl;
  pcout << std::endl;

  Sintering::Parameters params;

  if (argc == 1 || std::string(argv[1]) == "--help")
    {
      params.print_help();
      return 0;
    }
  else if (std::string(argv[1]) == "--circle")
    {
      AssertThrow(4 <= argc && argc <= 5, ExcNotImplemented());

      // geometry
      const unsigned int    n_grains        = atoi(argv[2]);
      const double          r0              = atof(argv[3]);
      static constexpr bool is_accumulative = false;

      AssertThrow(n_grains > 0,
                  ExcMessage("Number of grains should be grater than 0!"));
      AssertThrow(r0 > 0,
                  ExcMessage("Particle radius should be grater than 0!"));
      // Output case specific info
      pcout << "Mode:             circle" << std::endl;
      pcout << "Diameter:         " << 15 << std::endl;
      pcout << "Number of grains: " << n_grains << std::endl;
      pcout << std::endl;


      if (argc >= 5)
        params.parse(std::string(argv[4]));

      params.check();
      pcout << "Parameters:" << std::endl;
      params.print_input();
      pcout << std::endl;


      const auto initial_solution =
        std::make_shared<Sintering::InitialValuesCircle<SINTERING_DIM>>(
          r0,
          params.geometry_data.interface_width,
          n_grains,
          params.geometry_data.minimize_order_parameters,
          is_accumulative);

      AssertThrow(
        initial_solution->n_order_parameters() <= MAX_SINTERING_GRAINS,
        Sintering::ExcMaxGrainsExceeded(initial_solution->n_order_parameters(),
                                        MAX_SINTERING_GRAINS));

      Sintering::Problem<SINTERING_DIM> runner(params, initial_solution);
    }
  else if (std::string(argv[1]) == "--cloud")
    {
      AssertThrow(3 <= argc && argc <= 4,
                  ExcMessage("Argument cloud_file has to be provided!"));

      std::string   file_cloud = std::string(argv[2]);
      std::ifstream fstream(file_cloud.c_str());
      const auto particles = Sintering::read_particles<SINTERING_DIM>(fstream);

      // Output case specific info
      pcout << "Mode:       cloud" << std::endl;
      pcout << "Cloud path: " << file_cloud << std::endl;
      pcout << "Particles list:" << std::endl;
      fstream.clear();
      fstream.seekg(0);
      pcout << fstream.rdbuf();
      pcout << std::endl;

      if (argc >= 4)
        params.parse(std::string(argv[3]));

      params.check();

      pcout << "Parameters:" << std::endl;
      params.print_input();
      pcout << std::endl;

      const auto initial_solution =
        std::make_shared<Sintering::InitialValuesCloud<SINTERING_DIM>>(
          particles,
          params.geometry_data.interface_width,
          params.geometry_data.minimize_order_parameters,
          params.geometry_data.interface_buffer_ratio);

      AssertThrow(
        initial_solution->n_order_parameters() <= MAX_SINTERING_GRAINS,
        Sintering::ExcMaxGrainsExceeded(initial_solution->n_order_parameters(),
                                        MAX_SINTERING_GRAINS));

      Sintering::Problem<SINTERING_DIM> runner(params, initial_solution);
    }
  else if (std::string(argv[1]) == "--restart")
    {
      AssertThrow(3 <= argc && argc <= 4, ExcNotImplemented());

      const std::string restart_path = std::string(argv[2]);

      // Output case specific info
      pcout << "Mode:         restart" << std::endl;
      pcout << "Restart path: " << restart_path << std::endl;
      pcout << std::endl;

      if (argc >= 4)
        params.parse(std::string(argv[3]));

      params.check();

      pcout << "Parameters:" << std::endl;
      params.print_input();
      pcout << std::endl;

      Sintering::Problem<SINTERING_DIM> runner(params, restart_path);
    }
  else
    {
      AssertThrow(false, ExcNotImplemented());
    }
}
