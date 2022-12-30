#pragma once

#include <deal.II/lac/petsc_block_sparse_matrix.h>
#include <deal.II/lac/petsc_block_vector.h>
#include <deal.II/lac/petsc_snes.h>
#include <deal.II/lac/petsc_vector.h>

#include <pf-applications/lac/dynamic_block_vector.h>

namespace NonLinearSolvers
{
  using namespace dealii;

  template <typename VectorType>
  struct PETSCVectorTraits;

  template <typename Number>
  struct PETSCVectorTraits<LinearAlgebra::distributed::Vector<Number>>
  {
    using VectorType  = LinearAlgebra::distributed::Vector<Number>;
    using PVectorType = PETScWrappers::MPI::Vector;
    using PMatrixType = PETScWrappers::MatrixBase; // not needed!?

    static void
    copy(VectorType &dst, const PVectorType &src)
    {
      for (const auto i : dst.locally_owned_elements())
        dst[i] = src[i];
    }

    static void
    copy(PVectorType &dst, const VectorType &src)
    {
      for (const auto i : src.locally_owned_elements())
        dst[i] = src[i];
      dst.compress(VectorOperation::insert);
    }

    static std::shared_ptr<PVectorType>
    create(const VectorType &vector)
    {
      return std::make_shared<PVectorType>(vector.locally_owned_elements(),
                                           vector.get_mpi_communicator());
    }
  };

  template <typename Number>
  struct PETSCVectorTraits<
    LinearAlgebra::distributed::DynamicBlockVector<Number>>
  {
    using VectorType  = LinearAlgebra::distributed::DynamicBlockVector<Number>;
    using PVectorType = PETScWrappers::MPI::BlockVector;
    using PMatrixType = PETScWrappers::MPI::BlockSparseMatrix; // not needed!?

    static void
    copy(VectorType &dst, const PVectorType &src)
    {
      for (unsigned int b = 0; b < src.n_blocks(); ++b)
        for (const auto i : dst.block(b).locally_owned_elements())
          dst.block(b)[i] = src.block(b)[i];
    }

    static void
    copy(PVectorType &dst, const VectorType &src)
    {
      for (unsigned int b = 0; b < src.n_blocks(); ++b)
        for (const auto i : src.block(b).locally_owned_elements())
          dst.block(b)[i] = src.block(b)[i];
      dst.compress(VectorOperation::insert);
    }

    static std::shared_ptr<PVectorType>
    create(const VectorType &vector)
    {
      std::vector<IndexSet> index_sets(vector.n_blocks());

      for (unsigned int b = 0; b < vector.n_blocks(); ++b)
        index_sets[b] = vector.block(b).locally_owned_elements();

      return std::make_shared<PVectorType>(
        index_sets, vector.block(0).get_mpi_communicator());
    }
  };

  template <typename VectorType>
  class SNESSolver
  {
  private:
    // TODO: generalize for block vectors
    using VectorTraits = PETSCVectorTraits<VectorType>;
    using PVectorType  = typename VectorTraits::PVectorType;
    using PMatrixType  = typename VectorTraits::PMatrixType; // not needed!?

  public:
    struct AdditionalData
    {
    public:
      AdditionalData(const unsigned int max_iter                       = 10,
                     const double       abs_tol                        = 1.e-20,
                     const double       rel_tol                        = 1.e-5,
                     const unsigned int threshold_nonlinear_iterations = 1,
                     const unsigned int threshold_n_linear_iterations  = 0,
                     const bool         reuse_solver                   = false);

      unsigned int max_iter;
      double       abs_tol;
      double       rel_tol;
      unsigned int threshold_nonlinear_iterations;
      unsigned int threshold_n_linear_iterations;
      bool         reuse_solver;
    };

    SNESSolver(const AdditionalData &additional_data,
               const std::string &   snes_type = "");

    void
    clear();

    unsigned int
    solve(VectorType &vector);

    std::function<int(const VectorType &x, VectorType &f)> residual;

    std::function<int(const VectorType &x)> setup_jacobian;

    std::function<int(const VectorType &x)> setup_preconditioner;

    std::function<
      int(const VectorType &f, VectorType &x, const double tolerance)>
      solve_with_jacobian;

    std::function<
      int(const VectorType &f, VectorType &x, const double tolerance)>
      solve_with_jacobian_and_track_n_linear_iterations;

    std::function<bool()> update_preconditioner_predicate;

  private:
    AdditionalData additional_data;

    const std::string snes_type;

    unsigned int n_residual_evaluations;
    unsigned int n_jacobian_applications;
    unsigned int n_nonlinear_iterations;
    unsigned int n_last_linear_iterations;
  };



  template <typename VectorType>
  SNESSolver<VectorType>::AdditionalData::AdditionalData(
    const unsigned int max_iter,
    const double       abs_tol,
    const double       rel_tol,
    const unsigned int threshold_nonlinear_iterations,
    const unsigned int threshold_n_linear_iterations,
    const bool         reuse_solver)
    : max_iter(max_iter)
    , abs_tol(abs_tol)
    , rel_tol(rel_tol)
    , threshold_nonlinear_iterations(threshold_nonlinear_iterations)
    , threshold_n_linear_iterations(threshold_n_linear_iterations)
    , reuse_solver(reuse_solver)
  {}



  template <typename VectorType>
  SNESSolver<VectorType>::SNESSolver(const AdditionalData &additional_data,
                                     const std::string &   snes_type)
    : additional_data(additional_data)
    , snes_type(snes_type)
    , n_residual_evaluations(0)
    , n_jacobian_applications(0)
    , n_nonlinear_iterations(0)
    , n_last_linear_iterations(0)
  {}



  template <typename VectorType>
  void
  SNESSolver<VectorType>::clear()
  {
    // clear interal counters
    n_residual_evaluations   = 0;
    n_jacobian_applications  = 0;
    n_nonlinear_iterations   = 0;
    n_last_linear_iterations = 0;
  }



  template <typename VectorType>
  unsigned int
  SNESSolver<VectorType>::solve(VectorType &vector)
  {
    if (additional_data.reuse_solver == false)
      clear(); // clear state

    typename PETScWrappers::NonlinearSolverData p_additional_data;

    p_additional_data.snestype           = snes_type;
    p_additional_data.absolute_tolerance = additional_data.abs_tol;
    p_additional_data.relative_tolerance = additional_data.rel_tol;
    p_additional_data.max_it             = additional_data.max_iter;

    PETScWrappers::NonlinearSolver<PVectorType, PMatrixType> solver(
      p_additional_data);

    // create a temporal PETSc vector
    auto pvector = VectorTraits::create(vector);

    // create temporal deal.II vectors
    VectorType tmp_0, tmp_1;
    tmp_0.reinit(vector);
    tmp_1.reinit(vector);

    // wrap deal.II functions
    solver.residual = [&](const PVectorType &X, PVectorType &F) -> int {
      Assert(
        residual,
        ExcMessage(
          "No residual function has been attached to the SNESSolver object."));

      n_residual_evaluations++;

      VectorTraits::copy(tmp_0, X);
      const auto flag = this->residual(tmp_0, tmp_1);
      VectorTraits::copy(F, tmp_1);
      return flag;
    };

    solver.setup_jacobian = [&](const PVectorType &X) -> int {
      Assert(
        setup_jacobian,
        ExcMessage(
          "No setup_jacobian function has been attached to the SNESSolver object."));

      VectorTraits::copy(tmp_1, X);

      auto flag = this->setup_jacobian(tmp_1);

      if (flag != 0)
        return flag;

      if (setup_preconditioner)
        {
          // check if preconditioner needs to be updated
          bool update_preconditioner =
            ((additional_data.threshold_nonlinear_iterations > 0) &&
             ((n_nonlinear_iterations %
               additional_data.threshold_nonlinear_iterations) == 0)) ||
            (solve_with_jacobian_and_track_n_linear_iterations &&
             (n_last_linear_iterations >
              additional_data.threshold_n_linear_iterations));

          if ((update_preconditioner == false) &&
              (update_preconditioner_predicate != nullptr))
            update_preconditioner = update_preconditioner_predicate();

          if (update_preconditioner) // update preconditioner
            flag = setup_preconditioner(tmp_1);
        }

      return flag;
    };

    solver.solve_for_jacobian_system = [&](const PVectorType &src,
                                           PVectorType &      dst) -> int {
      // TODO: tolerance not used
      const double tolerance = 0.0;

      n_nonlinear_iterations++;

      // invert Jacobian
      if (solve_with_jacobian)
        {
          Assert(
            !solve_with_jacobian_and_track_n_linear_iterations,
            ExcMessage(
              "It does not make sense to provide both solve_with_jacobian and "
              "solve_with_jacobian_and_track_n_linear_iterations!"));

          // without tracking of linear iterations
          VectorTraits::copy(tmp_0, src);
          const auto flag = solve_with_jacobian(tmp_0, tmp_1, tolerance);
          VectorTraits::copy(dst, tmp_1);

          return flag;
        }
      else if (solve_with_jacobian_and_track_n_linear_iterations)
        {
          // with tracking of linear iterations
          VectorTraits::copy(tmp_0, src);
          const int n_linear_iterations =
            solve_with_jacobian_and_track_n_linear_iterations(tmp_0,
                                                              tmp_1,
                                                              tolerance);
          VectorTraits::copy(dst, tmp_1);

          if (n_linear_iterations == -1)
            return 1;

          this->n_last_linear_iterations = n_linear_iterations;

          return 0;
        }
      else
        {
          Assert(false,
                 ExcMessage(
                   "Neither a solve_with_jacobian or a "
                   "solve_with_jacobian_and_track_n_linear_iterations function "
                   "has been attached to the SNESSolver object."));

          Assert(false, ExcNotImplemented());
          return 1;
        }
    };

    // solve
    VectorTraits::copy(*pvector, vector);
    const unsigned int n_iterations = solver.solve(*pvector);
    VectorTraits::copy(vector, *pvector);

    return n_iterations;
  }

} // namespace NonLinearSolvers
