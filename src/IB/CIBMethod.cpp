// Filename: CIBMethod.cpp
// Created on 21 Apr 2015 by Amneet Bhalla
//
// Copyright (c) 2002-2014, Amneet Bhalla and Boyce Griffith.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//    * Redistributions of source code must retain the above copyright notice,
//      this list of conditions and the following disclaimer.
//
//    * Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
//    * Neither the name of The University of North Carolina nor the names of its
//      contributors may be used to endorse or promote products derived from
//      this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

/////////////////////////////// INCLUDES /////////////////////////////////////

#include "LocationIndexRobinBcCoefs.h"
#include "ibamr/CIBMethod.h"
#include "ibamr/IBHierarchyIntegrator.h"
#include "ibamr/namespaces.h"
#include "ibtk/IBTK_CHKERRQ.h"
#include "ibtk/HierarchyGhostCellInterpolation.h"
#include "ibtk/IndexUtilities.h"
#include "ibtk/LEInteractor.h"
#include "ibtk/LSiloDataWriter.h"
#include "ibtk/PETScMultiVec.h"
#include "ibtk/RobinPhysBdryPatchStrategy.h"
#include "ibtk/ibtk_utilities.h"

namespace IBAMR
{
/////////////////////////////// STATIC ///////////////////////////////////////
namespace
{
// Empirical (using f(r) and g(r)) mobility matrix generator
extern "C" void getEmpiricalMobilityMatrix(const char* kernel_name,
                                           const double mu,
                                           const double rho,
                                           const double dt,
                                           const double dx,
                                           const double* X,
                                           const int n,
                                           const bool reset_constants,
                                           const double periodic_correction,
                                           const double l_domain,
                                           double* mm);

// RPY mobility matrix generator
extern "C" void getRPYMobilityMatrix(const char* kernel_name,
                                     const double mu,
                                     const double dx,
                                     const double* X,
                                     const int n,
                                     const double periodic_correction,
                                     double* mm);
}

/////////////////////////////// PUBLIC ///////////////////////////////////////

CIBMethod::CIBMethod(const std::string& object_name,
                     Pointer<Database> input_db,
                     const int no_structures,
                     bool register_for_restart)
    : IBMethod(object_name, input_db, register_for_restart), CIBStrategy(no_structures)
{
    // Set some default values
    d_eul_lambda_idx = -1;
    d_output_eul_lambda = false;
    d_lambda_dump_interval = 0;

    // Resize some arrays.
    d_constrained_velocity_fcns_data.resize(d_num_rigid_parts);
    d_struct_lag_idx_range.resize(d_num_rigid_parts);
    d_lambda_filename.resize(d_num_rigid_parts);
    d_reg_filename.resize(d_num_rigid_parts);

    // Initialize object with data read from the input and restart databases.
    const bool from_restart = RestartManager::getManager()->isFromRestart();
    if (from_restart) getFromRestart();
    if (input_db) getFromInput(input_db);

    return;

} // cIBMethod

CIBMethod::~CIBMethod()
{
    // intentionally left blank.
    return;
} // ~cIBMethod

void CIBMethod::registerConstrainedVelocityFunction(ConstrainedNodalVelocityFcnPtr nodalvelfcn,
                                                    ConstrainedCOMVelocityFcnPtr comvelfcn,
                                                    void* ctx,
                                                    unsigned int part)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(part < d_num_rigid_parts);
#endif
    registerConstrainedVelocityFunction(ConstrainedVelocityFcnsData(nodalvelfcn, comvelfcn, ctx), part);

    return;
} // registerConstrainedVelocityFunction

void CIBMethod::registerConstrainedVelocityFunction(const ConstrainedVelocityFcnsData& data, unsigned int part)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(part < d_num_rigid_parts);
#endif
    d_constrained_velocity_fcns_data[part] = data;

    return;
} // registerConstrainedVelocityFunction

int CIBMethod::getStructuresLevelNumber() const
{
    return d_hierarchy->getFinestLevelNumber();

} // getStructuresLevelNumber

int CIBMethod::getStructureHandle(const int lag_idx) const
{
    for (unsigned struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        const std::pair<int, int>& lag_idx_range = d_struct_lag_idx_range[struct_no];
        if (lag_idx_range.first <= lag_idx && lag_idx < lag_idx_range.second) return struct_no;
    }

    return -1;
} // getStructureHandle

void CIBMethod::registerPreProcessSolveFluidEquationsCallBackFcn(preprocessSolveFluidEqn_callbackfcn callback,
                                                                 void* ctx)
{
    d_prefluidsolve_callback_fcns.push_back(callback);
    d_prefluidsolve_callback_fcns_ctx.push_back(ctx);

    return;
} // registerPreProcessSolveFluidEquationsCallBackFcn

void CIBMethod::preprocessSolveFluidEquations(double current_time, double new_time, int cycle_num)
{
    IBMethod::preprocessSolveFluidEquations(current_time, new_time, cycle_num);

    // Call any registered pre-fluid solve callback functions.
    for (unsigned i = 0; i < d_prefluidsolve_callback_fcns.size(); ++i)
    {
        d_prefluidsolve_callback_fcns[i](current_time, new_time, cycle_num, d_prefluidsolve_callback_fcns_ctx[i]);
    }

    return;
} // preprocessSolveFluidEquations

void CIBMethod::registerEulerianVariables()
{
    IBMethod::registerEulerianVariables();

    const IntVector<NDIM> ib_ghosts = getMinimumGhostCellWidth();
    d_eul_lambda_var = new CellVariable<NDIM, double>(d_object_name + "::eul_lambda", NDIM);
    registerVariable(d_eul_lambda_idx, d_eul_lambda_var, ib_ghosts, d_ib_solver->getCurrentContext());

    return;
} // registerEulerianVariables

void CIBMethod::registerEulerianCommunicationAlgorithms()
{

    IBMethod::registerEulerianCommunicationAlgorithms();

    Pointer<RefineAlgorithm<NDIM> > refine_alg_lambda;
    Pointer<RefineOperator<NDIM> > refine_op;
    refine_alg_lambda = new RefineAlgorithm<NDIM>();
    refine_op = NULL;
    refine_alg_lambda->registerRefine(d_eul_lambda_idx, d_eul_lambda_idx, d_eul_lambda_idx, refine_op);
    registerGhostfillRefineAlgorithm(d_object_name + "::eul_lambda", refine_alg_lambda);

    return;
} // registerEulerianCommunicationAlgorithms

void CIBMethod::preprocessIntegrateData(double current_time, double new_time, int num_cycles)
{

    IBMethod::preprocessIntegrateData(current_time, new_time, num_cycles);

    // Set prescribed velocity for the time interval.
    for (unsigned int part = 0; part < d_num_rigid_parts; ++part)
    {
        if (!d_solve_rigid_vel[part])
        {
            d_constrained_velocity_fcns_data[part].comvelfcn(d_current_time, d_trans_vel_current[part],
                                                             d_rot_vel_current[part]);

            d_constrained_velocity_fcns_data[part].comvelfcn(d_half_time, d_trans_vel_half[part], d_rot_vel_half[part]);

            d_constrained_velocity_fcns_data[part].comvelfcn(d_new_time, d_trans_vel_new[part], d_rot_vel_new[part]);
        }
    }

    return;
} // preprocessIntegrateData

void CIBMethod::postprocessIntegrateData(double current_time, double new_time, int num_cycles)
{
    IBMethod::postprocessIntegrateData(current_time, new_time, num_cycles);

    // Dump Lagrange multiplier data.
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (d_lambda_dump_interval && ((d_ib_solver->getIntegratorStep() + 1) % d_lambda_dump_interval == 0))
    {
        Pointer<LData> ptr_lagmultpr = d_l_data_manager->getLData("lambda", finest_ln);
        Vec lambda_petsc_vec_parallel = ptr_lagmultpr->getVec();
        Vec lambda_lag_vec_parallel = NULL;
        Vec lambda_lag_vec_seq = NULL;

        VecDuplicate(lambda_petsc_vec_parallel, &lambda_lag_vec_parallel);
        d_l_data_manager->scatterPETScToLagrangian(lambda_petsc_vec_parallel, lambda_lag_vec_parallel, finest_ln);
        d_l_data_manager->scatterToZero(lambda_lag_vec_parallel, lambda_lag_vec_seq);

        if (SAMRAI_MPI::getRank() == 0)
        {
            PetscScalar* L;
            VecGetArray(lambda_lag_vec_seq, &L);
            int counter_L = -1;
            Eigen::Vector3d total_lambda = Eigen::Vector3d::Zero();

            d_lambda_stream << new_time << std::endl << std::endl;
            for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
            {
                const int no_ib_pts = getNumberOfNodes(struct_no);
                d_lambda_stream << "structure: " << struct_no << " ib_pts: " << no_ib_pts << std::endl;

                for (int i = 0; i < no_ib_pts; ++i)
                {
                    for (int d = 0; d < NDIM; ++d)
                    {
                        d_lambda_stream << L[++counter_L] << "\t";
                        total_lambda[d] += L[counter_L];
                    }
                    d_lambda_stream << std::endl;
                }
                d_lambda_stream << "Net resultant lambda for structure: " << struct_no << " ";

                for (int d = 0; d < NDIM; ++d) d_lambda_stream << total_lambda[d] << "\t";
                d_lambda_stream << std::endl;
                total_lambda.setZero();
            }
            VecRestoreArray(lambda_lag_vec_seq, &L);
        }
        VecDestroy(&lambda_lag_vec_parallel);
        VecDestroy(&lambda_lag_vec_seq);
    }

    if (d_output_eul_lambda)
    {
        // Prepare the LData to spread
        std::vector<Pointer<LData> > spread_lag_data(finest_ln + 1, Pointer<LData>(NULL)),
            position_lag_data(finest_ln + 1, Pointer<LData>(NULL));

        spread_lag_data[finest_ln] = d_l_data_manager->getLData("lambda", finest_ln);
        ;
        position_lag_data[finest_ln] = d_l_data_manager->getLData("X", finest_ln);

        // Initialize the S[lambda] variable to zero.
        for (int ln = 0; ln <= finest_ln; ++ln)
        {
            Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
            for (PatchLevel<NDIM>::Iterator p(level); p; p++)
            {
                Pointer<Patch<NDIM> > patch = level->getPatch(p());
                Pointer<CellData<NDIM, double> > lambda_data = patch->getPatchData(d_eul_lambda_idx);
                lambda_data->fillAll(0.0);
            }
        }
        d_l_data_manager->spread(d_eul_lambda_idx, spread_lag_data, position_lag_data, /*f_phys_bdry_op*/ NULL);
    }

    // New center of mass translational and rotational velocity becomes
    // current velocity for the next time step.
    d_trans_vel_current = d_trans_vel_new;
    d_rot_vel_current = d_rot_vel_new;

    return;
} // postprocessIntegrateData

void CIBMethod::initializeLevelData(Pointer<BasePatchHierarchy<NDIM> > hierarchy,
                                    int level_number,
                                    double init_data_time,
                                    bool can_be_refined,
                                    bool initial_time,
                                    Pointer<BasePatchLevel<NDIM> > old_level,
                                    bool allocate_data)
{
    IBMethod::initializeLevelData(hierarchy, level_number, init_data_time, can_be_refined, initial_time, old_level,
                                  allocate_data);

    // Allocate LData corresponding to the Lagrange multiplier.
    if (initial_time && d_l_data_manager->levelContainsLagrangianData(level_number))
    {
        // Set structure index info.
        std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(level_number);
        std::sort(structIDs.begin(), structIDs.end());
        const unsigned structs_on_this_ln = (unsigned)structIDs.size();

        for (unsigned struct_no = 0; struct_no < structs_on_this_ln; ++struct_no)
        {
            d_struct_lag_idx_range[struct_no] =
                d_l_data_manager->getLagrangianStructureIndexRange(structIDs[struct_no], level_number);
        }

        // Create Lagrange multiplier and regularization data.
        Pointer<IBTK::LData> lag_mul_data = d_l_data_manager->createLData("lambda", level_number, NDIM,
                                                                          /*manage_data*/ true);
        Pointer<IBTK::LData> regulator_data = d_l_data_manager->createLData("regulator", level_number, NDIM,
                                                                            /*manage_data*/ true);

        // Initialize the Lagrange multiplier to zero.
        // Specific value of lambda will be assigned from structure specific input file.
        VecSet(lag_mul_data->getVec(), 0.0);

        // Initialize the regulator data with default value of h^3.
        // Specific weights will be assigned from structure specific input file.
        Vec regulator_petsc_vec = regulator_data->getVec();
        VecSet(regulator_petsc_vec, 1.0);

        if (d_silo_writer)
        {
            d_silo_writer->registerVariableData("lambda", lag_mul_data, level_number);
        }
    }
    return;
} // initializeLevelData

void CIBMethod::initializePatchHierarchy(Pointer<PatchHierarchy<NDIM> > hierarchy,
                                         Pointer<GriddingAlgorithm<NDIM> > gridding_alg,
                                         int u_data_idx,
                                         const std::vector<Pointer<CoarsenSchedule<NDIM> > >& u_synch_scheds,
                                         const std::vector<Pointer<RefineSchedule<NDIM> > >& u_ghost_fill_scheds,
                                         int integrator_step,
                                         double init_data_time,
                                         bool initial_time)
{
    // Initialize various Lagrangian data objects required by the conventional
    // IB method.
    IBMethod::initializePatchHierarchy(hierarchy, gridding_alg, u_data_idx, u_synch_scheds, u_ghost_fill_scheds,
                                       integrator_step, init_data_time, initial_time);

    // Lookup the range of hierarchy levels.
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    if (initial_time)
    {
        // Initialize the S[lambda] variable.
        for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
        {
            Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
            for (PatchLevel<NDIM>::Iterator p(level); p; p++)
            {
                Pointer<Patch<NDIM> > patch = level->getPatch(p());
                Pointer<CellData<NDIM, double> > lambda_data = patch->getPatchData(d_eul_lambda_idx);
                lambda_data->fillAll(0.0);
            }
        }
    } // initial time

    bool from_restart = RestartManager::getManager()->isFromRestart();
    if (from_restart)
    {
        if (d_silo_writer)
        {
            for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
            {
                if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;
                Pointer<LData> lag_mul_data = d_l_data_manager->getLData("lambda", ln);
                d_silo_writer->registerVariableData("lambda", lag_mul_data, ln);
            }
        }
    }

    if (d_output_eul_lambda && d_visit_writer)
    {
        d_visit_writer->registerPlotQuantity("S_lambda", "VECTOR", d_eul_lambda_idx, 0);
        for (unsigned int d = 0; d < NDIM; ++d)
        {
            if (d == 0) d_visit_writer->registerPlotQuantity("S_lambda_x", "SCALAR", d_eul_lambda_idx, d);
            if (d == 1) d_visit_writer->registerPlotQuantity("S_lambda_y", "SCALAR", d_eul_lambda_idx, d);
            if (d == 2) d_visit_writer->registerPlotQuantity("S_lambda_z", "SCALAR", d_eul_lambda_idx, d);
        }
    }

    // Set lambda and regularization weight from input file.
    if (initial_time)
    {
        setInitialLambda(finest_ln);
        setRegularizationWeight(finest_ln);
    }

    return;
} // initializePatchHierarchy

void CIBMethod::interpolateVelocity(const int u_data_idx,
                                    const std::vector<Pointer<CoarsenSchedule<NDIM> > >& u_synch_scheds,
                                    const std::vector<Pointer<RefineSchedule<NDIM> > >& u_ghost_fill_scheds,
                                    const double data_time)
{
    if (d_lag_velvec_is_initialized)
    {
#if !defined(NDEBUG)
        TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_half_time));
#endif
        std::vector<Pointer<LData> >* U_half_data, *X_half_data;
        bool* X_half_needs_ghost_fill;
        getVelocityData(&U_half_data, d_half_time);
        getPositionData(&X_half_data, &X_half_needs_ghost_fill, d_half_time);
        d_l_data_manager->interp(u_data_idx, *U_half_data, *X_half_data, u_synch_scheds, u_ghost_fill_scheds,
                                 data_time);

        d_lag_velvec_is_initialized = false;
    }

    return;
} // interpolateVelocity

void CIBMethod::spreadForce(
    int f_data_idx,
    IBTK::RobinPhysBdryPatchStrategy* f_phys_bdry_op,
    const std::vector<SAMRAI::tbox::Pointer<SAMRAI::xfer::RefineSchedule<NDIM> > >& f_prolongation_scheds,
    double data_time)
{
    if (d_constraint_force_is_initialized)
    {

#if !defined(NDEBUG)
        TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_half_time));
#endif
        IBMethod::spreadForce(f_data_idx, f_phys_bdry_op, f_prolongation_scheds, data_time);
        d_constraint_force_is_initialized = false;
    }

    return;
} // spreadForce

void CIBMethod::eulerStep(const double current_time, const double new_time)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double dt = new_time - current_time;

    // Compute center of mass and moment of inertia of the body at t^n.
    computeCOMandMOIOfStructures(d_center_of_mass_current, d_moment_of_inertia_current, d_X_current_data);

    // Fill the rotation matrix of structures with rotation angle 0.5*(W^n)*dt.
    std::vector<Eigen::Matrix3d> rotation_mat(d_num_rigid_parts, Eigen::Matrix3d::Zero());
    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        for (int i = 0; i < 3; ++i) rotation_mat[struct_no](i, i) = 1.0;
    }
    setRotationMatrix(d_rot_vel_current, rotation_mat, 0.5 * dt);

    // Rotate the body with current rotational velocity about origin
    // and translate the body to predicted position X^n+1/2.
    std::vector<Pointer<LData> >* X_half_data;
    bool* X_half_needs_ghost_fill;
    getPositionData(&X_half_data, &X_half_needs_ghost_fill, d_half_time);
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;

        boost::multi_array_ref<double, 2>& X_half_array = *((*X_half_data)[ln]->getLocalFormVecArray());
        const boost::multi_array_ref<double, 2>& X_current_array = *d_X_current_data[ln]->getLocalFormVecArray();
        const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(ln);
        const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

        // Get structures on this level.
        const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(ln);
        const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
        TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif
        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            const int local_idx = node_idx->getLocalPETScIndex();
            double* const X_half = &X_half_array[local_idx][0];
            const double* const X_current = &X_current_array[local_idx][0];
            Eigen::Vector3d dr = Eigen::Vector3d::Zero();
            Eigen::Vector3d R_dr = Eigen::Vector3d::Zero();

            int struct_handle = 0;
            if (structs_on_this_ln > 1) struct_handle = getStructureHandle(lag_idx);

            for (unsigned int d = 0; d < NDIM; ++d)
            {
                dr[d] = X_current[d] - d_center_of_mass_current[struct_handle][d];
            }

            // Rotate dr vector using the rotation matrix.
            R_dr = rotation_mat[struct_handle] * dr;
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                X_half[d] = d_center_of_mass_current[struct_handle][d] + R_dr[d] +
                            0.5 * dt * d_trans_vel_current[struct_handle][d];
            }
        }
        (*X_half_data)[ln]->restoreArrays();
        d_X_current_data[ln]->restoreArrays();
    }
    *X_half_needs_ghost_fill = true;

    // Compute the COM and MOI at mid-step.
    computeCOMandMOIOfStructures(d_center_of_mass_half, d_moment_of_inertia_half, *X_half_data);

    return;
} // eulerStep

void CIBMethod::midpointStep(const double current_time, const double new_time)
{

    const double dt = new_time - current_time;

    // Fill the rotation matrix of structures with rotation angle (W^n+1)*dt.
    std::vector<Eigen::Matrix3d> rotation_mat(d_num_rigid_parts, Eigen::Matrix3d::Zero());
    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        for (int i = 0; i < 3; ++i) rotation_mat[struct_no](i, i) = 1.0;
    }
    setRotationMatrix(d_rot_vel_half, rotation_mat, dt);

    // Rotate the body with current rotational velocity about origin
    // and translate the body to newer position.
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;

        boost::multi_array_ref<double, 2>& X_new_array = *d_X_new_data[ln]->getLocalFormVecArray();
        const boost::multi_array_ref<double, 2>& X_current_array = *d_X_current_data[ln]->getLocalFormVecArray();
        const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(ln);
        const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

        // Get structures on this level.
        const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(ln);
        const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
        TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            const int local_idx = node_idx->getLocalPETScIndex();
            double* const X_new = &X_new_array[local_idx][0];
            const double* const X_current = &X_current_array[local_idx][0];
            Eigen::Vector3d dr = Eigen::Vector3d::Zero();
            Eigen::Vector3d R_dr = Eigen::Vector3d::Zero();

            int struct_handle = 0;
            if (structs_on_this_ln > 1) struct_handle = getStructureHandle(lag_idx);

            for (unsigned int d = 0; d < NDIM; ++d)
            {
                dr[d] = X_current[d] - d_center_of_mass_current[struct_handle][d];
            }

            // Rotate dr vector using the rotation matrix.
            R_dr = rotation_mat[struct_handle] * dr;
            for (unsigned int d = 0; d < NDIM; ++d)
            {
                X_new[d] =
                    d_center_of_mass_current[struct_handle][d] + R_dr[d] + dt * d_trans_vel_half[struct_handle][d];
            }
        }
        d_X_new_data[ln]->restoreArrays();
        d_X_current_data[ln]->restoreArrays();
    }

    return;
} // midpointStep

void CIBMethod::trapezoidalStep(const double /*current_time*/, const double /*new_time*/)
{
    TBOX_ERROR("CIBMethod does not support trapezoidal time-stepping rule for position update."
               << " Only mid-point rule is supported." << std::endl);

    return;
} // trapezoidalStep

void CIBMethod::registerVisItDataWriter(Pointer<VisItDataWriter<NDIM> > visit_writer)
{
    d_visit_writer = visit_writer;
    return;
} // registerVisItDataWriter

void CIBMethod::putToDatabase(Pointer<Database> db)
{
    IBMethod::putToDatabase(db);

    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        std::ostringstream U, W;
        U << "U_" << struct_no;
        W << "W_" << struct_no;
        db->putDoubleArray(U.str(), &d_trans_vel_current[struct_no][0], 3);
        db->putDoubleArray(W.str(), &d_rot_vel_current[struct_no][0], 3);
    }

    return;
} // putToDatabase

void CIBMethod::setConstraintForce(Vec L, const double data_time, const double scale)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_half_time));
#endif

    const int struct_ln = getStructuresLevelNumber();

    std::vector<Pointer<LData> >* F_half_data;
    bool* F_half_needs_ghost_fill;
    getForceData(&F_half_data, &F_half_needs_ghost_fill, d_half_time);
    Vec F_half = (*F_half_data)[struct_ln]->getVec();
    VecCopy(L, F_half);
    VecScale(F_half, scale);
    *F_half_needs_ghost_fill = true;

    d_constraint_force_is_initialized = true;

    return;
} // setConstraintForce

void CIBMethod::getConstraintForce(Vec* L, const double data_time)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_current_time) ||
                MathUtilities<double>::equalEps(data_time, d_new_time));
#endif
    const int finest_ln = getStructuresLevelNumber();
    Pointer<LData> ptr_lagmultpr = d_l_data_manager->getLData("lambda", finest_ln);
    Vec lambda = ptr_lagmultpr->getVec();
    *L = lambda;

    return;
} // getConstraintForce

void CIBMethod::subtractMeanConstraintForce(Vec L, int f_data_idx, const double scale)
{
    // Temporarily scale the L Vec.
    VecScale(L, scale);

    // Get the underlying array
    PetscScalar* L_array;
    VecGetArray(L, &L_array);
    PetscInt local_size_L;
    VecGetLocalSize(L, &local_size_L);

    double F[NDIM] = { 0.0 };
    const int local_no_ib_pts = local_size_L / NDIM;

    for (int k = 0; k < local_no_ib_pts; ++k)
    {
        for (int d = 0; d < NDIM; ++d)
        {
            F[d] += L_array[k * NDIM + d];
        }
    }
    SAMRAI_MPI::sumReduction(F, NDIM);

    // Subtract the mean from Eulerian body force
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();
    const double vol_domain = getHierarchyMathOps()->getVolumeOfPhysicalDomain();
    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        Pointer<PatchLevel<NDIM> > level = d_hierarchy->getPatchLevel(ln);
        for (PatchLevel<NDIM>::Iterator p(level); p; p++)
        {
            Pointer<Patch<NDIM> > patch = level->getPatch(p());
            Pointer<SideData<NDIM, double> > p_data = patch->getPatchData(f_data_idx);
            const Box<NDIM>& patch_box = patch->getBox();
            for (int axis = 0; axis < NDIM; ++axis)
            {
                for (SideIterator<NDIM> it(patch_box, axis); it; it++)
                {
                    (*p_data)(it()) -= F[axis] / vol_domain;
                }
            }
        }
    }

    // Unscale the vector.
    VecScale(L, 1.0 / scale);

    return;
} // subtractMeanConstraintForce

void CIBMethod::setInterpolatedVelocityVector(Vec /*V*/, const double data_time)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_half_time));
#endif
    d_lag_velvec_is_initialized = true;

    return;
} // setInterpolatedVelocityVector

void CIBMethod::getInterpolatedVelocity(Vec V, const double data_time, const double scale)
{

#if !defined(NDEBUG)
    TBOX_ASSERT(MathUtilities<double>::equalEps(data_time, d_half_time));
#endif

    const int ln = getStructuresLevelNumber();
    std::vector<Pointer<LData> >* U_half_data;
    getVelocityData(&U_half_data, d_half_time);
    VecCopy((*U_half_data)[ln]->getVec(), V);
    VecScale(V, scale);

    return;
} // getInterpolatedVelocity

void CIBMethod::computeMobilityRegularization(Vec D, Vec L, const double scale)
{
    const int struct_ln = getStructuresLevelNumber();
    Pointer<LData> reg_data = d_l_data_manager->getLData("regulator", struct_ln);
    Vec W = reg_data->getVec();
    VecPointwiseMult(D, L, W);
    VecScale(D, scale);

    return;
} // computeMobilityRegularization

unsigned int CIBMethod::getNumberOfNodes(const unsigned int struct_no) const
{
    std::pair<int, int> lag_idx_range = d_struct_lag_idx_range[struct_no];
    return (lag_idx_range.second - lag_idx_range.first);

} // getNumberOfStructuresNodes

void CIBMethod::setRigidBodyVelocity(const unsigned int part, const RigidDOFVector& U, Vec V)
{
    void* ctx = d_constrained_velocity_fcns_data[part].ctx;
    const int struct_ln = getStructuresLevelNumber();

    if (d_constrained_velocity_fcns_data[part].nodalvelfcn)
    {
        d_constrained_velocity_fcns_data[part].nodalvelfcn(V, U, d_X_half_data[struct_ln]->getVec(),
                                                           d_center_of_mass_half[part], d_new_time, ctx);
    }
    else
    {
        // Wrap the PETSc V into LData
        std::vector<int> nonlocal_indices;
        LData V_data("V", V, nonlocal_indices, false);

        boost::multi_array_ref<double, 2>& V_data_array = *V_data.getLocalFormVecArray();
        const boost::multi_array_ref<double, 2>& X_data_array = *d_X_half_data[struct_ln]->getLocalFormVecArray();

        const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(struct_ln);
        const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();
        const std::pair<int, int>& part_idx_range = d_struct_lag_idx_range[part];
        const Eigen::Vector3d& X_com = d_center_of_mass_half[part];

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            if (part_idx_range.first <= lag_idx && lag_idx < part_idx_range.second)
            {
                const int local_idx = node_idx->getLocalPETScIndex();
                double* const V_node = &V_data_array[local_idx][0];
                const double* const X = &X_data_array[local_idx][0];

#if (NDIM == 2)
                V_node[0] = U[0] - U[2] * (X[1] - X_com[1]);
                V_node[1] = U[1] + U[2] * (X[0] - X_com[0]);
#elif(NDIM == 3)
                V_node[0] = U[0] + U[4] * (X[2] - X_com[2]) - U[5] * (X[1] - X_com[1]);
                V_node[1] = U[1] + U[5] * (X[0] - X_com[0]) - U[3] * (X[2] - X_com[2]);
                V_node[2] = U[2] + U[3] * (X[1] - X_com[1]) - U[4] * (X[0] - X_com[0]);
#endif
            }
        }

        // Restore underlying arrays
        V_data.restoreArrays();
        d_X_half_data[struct_ln]->restoreArrays();
    }

    return;
} // setRigidBodyVelocity

void CIBMethod::computeNetRigidGeneralizedForce(const unsigned int part, Vec L, RigidDOFVector& F)
{
    const int struct_ln = getStructuresLevelNumber();

    // Wrap the distributed PETSc Vec L into LData
    std::vector<int> nonlocal_indices;
    LData p_data("P", L, nonlocal_indices, false);
    const boost::multi_array_ref<double, 2>& p_data_array = *p_data.getLocalFormVecArray();
    const boost::multi_array_ref<double, 2>& X_data_array = *d_X_half_data[struct_ln]->getLocalFormVecArray();

    F.setZero();
    const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(struct_ln);
    const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();
    for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
    {
        const LNode* const node_idx = *cit;
        const int lag_idx = node_idx->getLagrangianIndex();
        const int local_idx = node_idx->getLocalPETScIndex();
        const double* const P = &p_data_array[local_idx][0];
        const double* const X = &X_data_array[local_idx][0];
        const unsigned struct_id = getStructureHandle(lag_idx);
        if (struct_id != part) continue;

        const Eigen::Vector3d& X_com = d_center_of_mass_half[part];
#if (NDIM == 2)
        for (int d = 0; d < NDIM; ++d)
        {
            F[d] += P[d];
        }
        F[2] += P[1] * (X[0] - X_com[0]) - P[0] * (X[1] - X_com[1]);
#elif(NDIM == 3)
        for (int d = 0; d < NDIM; ++d)
        {
            F[d] += P[d];
        }
        F[3] += P[2] * (X[1] - X_com[1]) - P[1] * (X[2] - X_com[2]);
        F[4] += P[0] * (X[2] - X_com[2]) - P[2] * (X[0] - X_com[0]);
        F[5] += P[1] * (X[0] - X_com[0]) - P[0] * (X[1] - X_com[1]);
#endif
    }
    SAMRAI_MPI::sumReduction(&F[0], NDIM * (NDIM + 1) / 2);
    p_data.restoreArrays();
    d_X_half_data[struct_ln]->restoreArrays();

    return;
} // computeNetRigidGeneralizedForce

void CIBMethod::copyVecToArray(Vec b, double* array, const std::vector<unsigned int>& struct_ids, const int data_depth)
{
    if (struct_ids.empty()) return;
    const unsigned num_structs = (unsigned)struct_ids.size();

    // Get the Lagrangian indices of the structures.
    std::vector<int> map;
    PetscInt total_nodes = 0;
    for (unsigned k = 0; k < num_structs; ++k)
    {
        total_nodes += getNumberOfNodes(struct_ids[k]);
    }
    map.reserve(total_nodes);
    for (unsigned k = 0; k < num_structs; ++k)
    {
        const std::pair<int, int>& lag_idx_range = d_struct_lag_idx_range[struct_ids[k]];
        const unsigned struct_nodes = getNumberOfNodes(struct_ids[k]);
        for (unsigned j = 0; j < struct_nodes; ++j)
        {
            map.push_back(lag_idx_range.first + j);
        }
    }

    // Map the Lagrangian indices into PETSc indices
    const int struct_ln = getStructuresLevelNumber();
    d_l_data_manager->mapLagrangianToPETSc(map, struct_ln);

    // Wrap the raw data in a PETSc Vec
    Vec array_vec;
    VecCreateSeqWithArray(PETSC_COMM_SELF, /*blocksize*/ 1, total_nodes * data_depth, array, &array_vec);

    // Scatter values from distributed Vec to sequential Vec.
    PetscInt size = total_nodes * data_depth;
    std::vector<PetscInt> vec_indices, array_indices;
    vec_indices.reserve(size);
    array_indices.reserve(size);

    for (PetscInt j = 0; j < total_nodes; ++j)
    {
        PetscInt petsc_idx = map[j];
        for (int d = 0; d < data_depth; ++d)
        {
            array_indices.push_back(j * data_depth + d);
            vec_indices.push_back(petsc_idx * data_depth + d);
        }
    }

    IS is_vec;
    IS is_array;

    ISCreateGeneral(PETSC_COMM_SELF, size, &vec_indices[0], PETSC_COPY_VALUES, &is_vec);
    ISCreateGeneral(PETSC_COMM_SELF, size, &array_indices[0], PETSC_COPY_VALUES, &is_array);

    VecScatter ctx;
    VecScatterCreate(b, is_vec, array_vec, is_array, &ctx);

    VecScatterBegin(ctx, b, array_vec, INSERT_VALUES, SCATTER_FORWARD);
    VecScatterEnd(ctx, b, array_vec, INSERT_VALUES, SCATTER_FORWARD);

    VecScatterDestroy(&ctx);
    ISDestroy(&is_vec);
    ISDestroy(&is_array);

    VecDestroy(&array_vec);

    return;
} // copyVecToArray

void CIBMethod::copyArrayToVec(Vec b, double* array, const std::vector<unsigned>& struct_ids, const int data_depth)
{
    if (struct_ids.empty()) return;
    const unsigned num_structs = (unsigned)struct_ids.size();

    // Get the Lagrangian indices of the structures.
    std::vector<int> map;
    PetscInt total_nodes = 0;
    for (unsigned k = 0; k < num_structs; ++k)
    {
        total_nodes += getNumberOfNodes(struct_ids[k]);
    }
    map.reserve(total_nodes);
    for (unsigned k = 0; k < num_structs; ++k)
    {
        const std::pair<int, int>& lag_idx_range = d_struct_lag_idx_range[struct_ids[k]];
        const unsigned struct_nodes = getNumberOfNodes(struct_ids[k]);
        for (unsigned j = 0; j < struct_nodes; ++j)
        {
            map.push_back(lag_idx_range.first + j);
        }
    }

    // Map the Lagrangian indices into PETSc indices
    const int struct_ln = getStructuresLevelNumber();
    d_l_data_manager->mapLagrangianToPETSc(map, struct_ln);

    // Wrap the raw data in a PETSc Vec
    Vec array_vec;
    VecCreateSeqWithArray(PETSC_COMM_SELF, /*blocksize*/ 1, total_nodes * data_depth, array, &array_vec);

    // Scatter values from sequential Vec to distributed Vec.
    PetscInt size = total_nodes * data_depth;
    std::vector<PetscInt> vec_indices, array_indices;
    vec_indices.reserve(size);
    array_indices.reserve(size);

    for (PetscInt j = 0; j < total_nodes; ++j)
    {
        PetscInt petsc_idx = map[j];
        for (int d = 0; d < data_depth; ++d)
        {
            array_indices.push_back(j * data_depth + d);
            vec_indices.push_back(petsc_idx * data_depth + d);
        }
    }

    IS is_vec;
    IS is_array;

    ISCreateGeneral(PETSC_COMM_SELF, size, &vec_indices[0], PETSC_COPY_VALUES, &is_vec);
    ISCreateGeneral(PETSC_COMM_SELF, size, &array_indices[0], PETSC_COPY_VALUES, &is_array);

    VecScatter ctx;
    VecScatterCreate(array_vec, is_array, b, is_vec, &ctx);

    VecScatterBegin(ctx, array_vec, b, INSERT_VALUES, SCATTER_FORWARD);
    VecScatterEnd(ctx, array_vec, b, INSERT_VALUES, SCATTER_FORWARD);

    VecScatterDestroy(&ctx);
    ISDestroy(&is_vec);
    ISDestroy(&is_array);

    VecDestroy(&array_vec);
    VecDestroy(&array_vec);

    return;
} // copyArrayToVec

void CIBMethod::generateMobilityMatrix(const std::string& /*mat_name*/,
                                       MobilityMatrixType mat_type,
                                       double* mobility_mat,
                                       const std::vector<unsigned>& prototype_struct_ids,
                                       const double* grid_dx,
                                       const double* domain_extents,
                                       double rho,
                                       double mu,
                                       const std::pair<double, double>& scale,
                                       double f_periodic_corr)
{
    const double dt = d_new_time - d_current_time;
    const int struct_ln = getStructuresLevelNumber();
    const char* ib_kernel = d_l_data_manager->getDefaultInterpKernelFunction().c_str();

    // Get the position of nodes
    unsigned num_nodes = 0;
    for (unsigned i = 0; i < prototype_struct_ids.size(); ++i)
    {
        num_nodes += getNumberOfNodes(prototype_struct_ids[i]);
    }
    const int size = num_nodes * NDIM;
    std::vector<double> XW(size);
    Vec X = d_X_half_data[struct_ln]->getVec();
    copyVecToArray(X, &XW[0], prototype_struct_ids, /*depth*/ NDIM);

    // Generate mobility matrix
    if (mat_type == RPY)
    {
        getRPYMobilityMatrix(ib_kernel, mu, grid_dx[0], &XW[0], num_nodes, f_periodic_corr, mobility_mat);
    }
    else if (mat_type == EMPIRICAL)
    {
        getEmpiricalMobilityMatrix(ib_kernel, mu, rho, dt, grid_dx[0], &XW[0], num_nodes, 0, f_periodic_corr,
                                   domain_extents[0], mobility_mat);
    }
    else
    {
        TBOX_ERROR("CIBMethod::generateMobilityMatrix(): Invalid type of a mobility matrix." << std::endl);
    }

    // Regularize the mobility matrix
    Vec W = d_l_data_manager->getLData("regulator", struct_ln)->getVec();
    copyVecToArray(W, &XW[0], prototype_struct_ids, /*depth*/ NDIM);
    for (int i = 0; i < size; ++i)
    {
        for (int j = 0; j < size; ++j)
        {
            mobility_mat[i * size + j] *= scale.first;
            if (i == j)
            {
                mobility_mat[i * size + j] += scale.second * XW[i];
            }
        }
    }

    return;
} // generateMobilityMatrix

/////////////////////////////// PRIVATE //////////////////////////////////////

void CIBMethod::getFromInput(Pointer<Database> input_db)
{
    d_output_eul_lambda = input_db->getBoolWithDefault("output_eul_lambda", d_output_eul_lambda);
    d_lambda_dump_interval = input_db->getIntegerWithDefault("lambda_dump_interval", d_lambda_dump_interval);
    if (d_lambda_dump_interval)
    {
        const bool from_restart = RestartManager::getManager()->isFromRestart();
        std::string dir_name = input_db->getStringWithDefault("lambda_dirname", "./lambda");
        if (!from_restart) Utilities::recursiveMkdir(dir_name);

        if (SAMRAI_MPI::getRank() == 0)
        {
            std::string filename = dir_name + "/" + "lambda";
            if (from_restart)
                d_lambda_stream.open(filename.c_str(), std::ofstream::app | std::ofstream::out);
            else
                d_lambda_stream.open(filename.c_str(), std::ofstream::out);

            d_lambda_stream.precision(16);
            d_lambda_stream << std::scientific;
        }
    }

    if (input_db->keyExists("lambda_filenames"))
    {
        tbox::Array<std::string> lambda_filenames = input_db->getStringArray("lambda_filenames");
        TBOX_ASSERT(lambda_filenames.size() == (int)d_num_rigid_parts);
        for (unsigned struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
        {
            d_lambda_filename[struct_no] = lambda_filenames[struct_no];
        }
    }

    if (input_db->keyExists("weight_filenames"))
    {
        tbox::Array<std::string> weight_filenames = input_db->getStringArray("weight_filenames");
        TBOX_ASSERT(weight_filenames.size() == (int)d_num_rigid_parts);
        for (unsigned struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
        {
            d_reg_filename[struct_no] = weight_filenames[struct_no];
        }
    }

    return;
} // getFromInput

void CIBMethod::getFromRestart()
{
    Pointer<Database> restart_db = RestartManager::getManager()->getRootDatabase();
    Pointer<Database> db;
    if (restart_db->isDatabase(d_object_name))
    {
        db = restart_db->getDatabase(d_object_name);
    }
    else
    {
        TBOX_ERROR("CIBMethod::getFromRestart(): Restart database corresponding to "
                   << d_object_name << " not found in restart file." << std::endl);
    }

    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        std::ostringstream U, W;
        U << "U_" << struct_no;
        W << "W_" << struct_no;
        db->getDoubleArray(U.str(), &d_trans_vel_current[struct_no][0], 3);
        db->getDoubleArray(W.str(), &d_rot_vel_current[struct_no][0], 3);
    }

    return;
} // getFromRestart

void CIBMethod::computeCOMandMOIOfStructures(std::vector<Eigen::Vector3d>& center_of_mass,
                                             std::vector<Eigen::Matrix3d>& moment_of_inertia,
                                             std::vector<Pointer<LData> >& X_data)
{
    const int coarsest_ln = 0;
    const int finest_ln = d_hierarchy->getFinestLevelNumber();

    // Zero out the COM vector.
    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        center_of_mass[struct_no].setZero();
    }

    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;

        const boost::multi_array_ref<double, 2>& X_data_array = *X_data[ln]->getLocalFormVecArray();
        const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(ln);
        const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

        // Get structures on this level.
        const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(ln);
        const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
        TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            const int local_idx = node_idx->getLocalPETScIndex();
            const double* const X = &X_data_array[local_idx][0];

            int struct_handle = 0;
            if (structs_on_this_ln > 1) struct_handle = getStructureHandle(lag_idx);

            for (unsigned int d = 0; d < NDIM; ++d) center_of_mass[struct_handle][d] += X[d];
        }

        for (unsigned struct_no = 0; struct_no < structs_on_this_ln; ++struct_no)
        {
            SAMRAI_MPI::sumReduction(&center_of_mass[struct_no][0], NDIM);
            const int total_nodes = getNumberOfNodes(struct_no);
            center_of_mass[struct_no] /= total_nodes;
        }
        X_data[ln]->restoreArrays();
    }

    // Zero out the moment of inertia tensor.
    for (unsigned struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        moment_of_inertia[struct_no].setZero();
    }

    for (int ln = coarsest_ln; ln <= finest_ln; ++ln)
    {
        if (!d_l_data_manager->levelContainsLagrangianData(ln)) continue;

        const boost::multi_array_ref<double, 2>& X_data_array = *X_data[ln]->getLocalFormVecArray();
        const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(ln);
        const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

        // Get structures on this level.
        const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(ln);
        const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
        TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            const int local_idx = node_idx->getLocalPETScIndex();
            const double* const X = &X_data_array[local_idx][0];

            int struct_handle = 0;
            if (structs_on_this_ln > 1) struct_handle = getStructureHandle(lag_idx);
            const Eigen::Vector3d& X_com = center_of_mass[struct_handle];

#if (NDIM == 2)
            moment_of_inertia[struct_handle](0, 0) += std::pow(X[1] - X_com[1], 2);
            moment_of_inertia[struct_handle](0, 1) += -(X[0] - X_com[0]) * (X[1] - X_com[1]);
            moment_of_inertia[struct_handle](1, 1) += std::pow(X[0] - X_com[0], 2);
            moment_of_inertia[struct_handle](2, 2) += std::pow(X[0] - X_com[0], 2) + std::pow(X[1] - X_com[1], 2);
#endif

#if (NDIM == 3)
            moment_of_inertia[struct_handle](0, 0) += std::pow(X[1] - X_com[1], 2) + std::pow(X[2] - X_com[2], 2);
            moment_of_inertia[struct_handle](0, 1) += -(X[0] - X_com[0]) * (X[1] - X_com[1]);
            moment_of_inertia[struct_handle](0, 2) += -(X[0] - X_com[0]) * (X[2] - X_com[2]);
            moment_of_inertia[struct_handle](1, 1) += std::pow(X[0] - X_com[0], 2) + std::pow(X[2] - X_com[2], 2);
            moment_of_inertia[struct_handle](1, 2) += -(X[1] - X_com[1]) * (X[2] - X_com[2]);
            moment_of_inertia[struct_handle](2, 2) += std::pow(X[0] - X_com[0], 2) + std::pow(X[1] - X_com[1], 2);
#endif
        }

        for (unsigned struct_no = 0; struct_no < structs_on_this_ln; ++struct_no)
        {
            SAMRAI_MPI::sumReduction(&moment_of_inertia[struct_no](0, 0), 9);
        }
        X_data[ln]->restoreArrays();
    }

    // Fill-in symmetric part of inertia tensor.
    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        moment_of_inertia[struct_no](1, 0) = moment_of_inertia[struct_no](0, 1);
        moment_of_inertia[struct_no](2, 0) = moment_of_inertia[struct_no](0, 2);
        moment_of_inertia[struct_no](2, 1) = moment_of_inertia[struct_no](1, 2);
    }

    return;
} // calculateCOMandMOIOfStructures

void CIBMethod::setRegularizationWeight(const int level_number)
{
    Pointer<LData> reg_data = d_l_data_manager->getLData("regulator", level_number);
    Pointer<CartesianGridGeometry<NDIM> > grid_geom = d_hierarchy->getGridGeometry();
    const double* const dx = grid_geom->getDx();
#if (NDIM == 2)
    const double cell_volume = dx[0] * dx[1];
#elif(NDIM == 3)
    const double cell_volume = dx[0] * dx[1] * dx[2];
#endif

    boost::multi_array_ref<double, 2>& reg_data_array = *reg_data->getLocalFormVecArray();
    const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(level_number);
    const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

    // Get structures on this level.
    const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(level_number);
    const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
    TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif

    for (unsigned struct_no = 0; struct_no < structs_on_this_ln; ++struct_no)
    {
        const std::pair<int, int>& lag_idx_range = d_struct_lag_idx_range[struct_no];
        if (d_reg_filename[struct_no].empty())
        {
            for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
            {
                const LNode* const node_idx = *cit;
                const int lag_idx = node_idx->getLagrangianIndex();
                if (lag_idx_range.first <= lag_idx && lag_idx < lag_idx_range.second)
                {
                    const int local_idx = node_idx->getLocalPETScIndex();
                    double* const W = &reg_data_array[local_idx][0];
                    for (unsigned int d = 0; d < NDIM; ++d) W[d] = cell_volume;
                }
            }
            continue;
        }

        // Read weights from file and set it.
        std::ifstream reg_filestream(d_reg_filename[struct_no].c_str(), std::ifstream::in);
        if (!reg_filestream.is_open())
        {
            TBOX_ERROR("CIBMethod::setRegularizationWeight()"
                       << "could not open file" << d_reg_filename[struct_no] << std::endl);
        }

        std::string line_f;
        int lag_pts = -1;
        if (std::getline(reg_filestream, line_f))
        {
            std::istringstream iss(line_f);
            iss >> lag_pts;
            if (lag_pts != (lag_idx_range.second - lag_idx_range.first))
            {
                TBOX_ERROR("CIBMethod::setRegularizationWeight() Total no. of Lagrangian points in the weight file "
                           << d_reg_filename[struct_no] << " not equal to corresponding vertex file." << std::endl);
            }
        }
        else
        {
            TBOX_ERROR("CIBMethod::setRegularizationWeight() Error in the input regularization file "
                       << d_reg_filename[struct_no] << " at line number 0. Total number of Lagrangian  points required."
                       << std::endl);
        }

        std::vector<double> reg_weight(lag_pts);
        for (int k = 0; k < lag_pts; ++k)
        {
            if (std::getline(reg_filestream, line_f))
            {
                std::istringstream iss(line_f);
                iss >> reg_weight[k];
            }
            else
            {
                TBOX_ERROR("CIBMethod::setRegularizationWeight() Error in the input regularization file "
                           << d_reg_filename[struct_no] << " at line number " << k + 1 << std::endl);
            }
        }

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            if (lag_idx_range.first <= lag_idx && lag_idx < lag_idx_range.second)
            {
                const int local_idx = node_idx->getLocalPETScIndex();
                double* const W = &reg_data_array[local_idx][0];
                const double& weight = reg_weight[lag_idx - lag_idx_range.first];

                // For zero weight we do not use any regularization
                if (!MathUtilities<double>::equalEps(weight, 0.0))
                {
                    for (unsigned int d = 0; d < NDIM; ++d)
                        W[d] = cell_volume / reg_weight[lag_idx - lag_idx_range.first];
                }
                else
                {
                    for (unsigned int d = 0; d < NDIM; ++d) W[d] = 0.0;
                }
            }
        }
    }
    reg_data->restoreArrays();

    return;
} // setRegularizationWeight

void CIBMethod::setInitialLambda(const int level_number)
{

    Pointer<IBTK::LData> lambda_data = d_l_data_manager->getLData("lambda", level_number);
    boost::multi_array_ref<double, 2>& lambda_data_array = *lambda_data->getLocalFormVecArray();
    const Pointer<LMesh> mesh = d_l_data_manager->getLMesh(level_number);
    const std::vector<LNode*>& local_nodes = mesh->getLocalNodes();

    // Get structures on this level.
    const std::vector<int> structIDs = d_l_data_manager->getLagrangianStructureIDs(level_number);
    const unsigned structs_on_this_ln = (unsigned)structIDs.size();
#if !defined(NDEBUG)
    TBOX_ASSERT(structs_on_this_ln == d_num_rigid_parts);
#endif

    for (unsigned struct_no = 0; struct_no < structs_on_this_ln; ++struct_no)
    {
        const std::pair<int, int> lag_idx_range = d_struct_lag_idx_range[struct_no];
        if (d_lambda_filename[struct_no].empty()) continue;

        std::ifstream lambda_filestream(d_lambda_filename[struct_no].c_str(), std::ifstream::in);
        if (!lambda_filestream.is_open())
        {
            TBOX_ERROR("CIBMethod::setInitialLambda()"
                       << "could not open file" << d_lambda_filename[struct_no] << std::endl);
        }

        std::string line_f;
        int lag_pts = -1;
        if (std::getline(lambda_filestream, line_f))
        {
            std::istringstream iss(line_f);
            iss >> lag_pts;

            if (lag_pts != (lag_idx_range.second - lag_idx_range.first))
            {
                TBOX_ERROR("CIBMethod::setInitialLambda() Total no. of Lagrangian points in the lambda file "
                           << d_lambda_filename[struct_no] << " not equal to corresponding vertex file." << std::endl);
            }
        }
        else
        {
            TBOX_ERROR("CIBMethod::::setInitialLambda() Error in the input lambda file "
                       << d_lambda_filename[struct_no] << " at line number 0. Total number of Lag pts. required."
                       << std::endl);
        }

        std::vector<double> initial_lambda(lag_pts * NDIM);
        for (int k = 0; k < lag_pts; ++k)
        {
            if (std::getline(lambda_filestream, line_f))
            {
                std::istringstream iss(line_f);
                for (int d = 0; d < NDIM; ++d) iss >> initial_lambda[k * NDIM + d];
            }
            else
            {
                TBOX_ERROR("CIBMethod::setInitialLambda() Error in the input lambda file "
                           << d_lambda_filename[struct_no] << " at line number " << k + 1 << std::endl);
            }
        }

        for (std::vector<LNode*>::const_iterator cit = local_nodes.begin(); cit != local_nodes.end(); ++cit)
        {
            const LNode* const node_idx = *cit;
            const int lag_idx = node_idx->getLagrangianIndex();
            if (lag_idx_range.first <= lag_idx && lag_idx < lag_idx_range.second)
            {
                const int local_idx = node_idx->getLocalPETScIndex();
                double* const L = &lambda_data_array[local_idx][0];
                for (unsigned int d = 0; d < NDIM; ++d)
                    L[d] = initial_lambda[(lag_idx - lag_idx_range.first) * NDIM + d];
            }
        }
    }
    lambda_data->restoreArrays();

    return;
} // setInitialLambda

void CIBMethod::setRotationMatrix(const std::vector<Eigen::Vector3d>& rot_vel,
                                  std::vector<Eigen::Matrix3d>& rot_mat,
                                  const double dt)
{

    for (unsigned int struct_no = 0; struct_no < d_num_rigid_parts; ++struct_no)
    {
        Eigen::Vector3d e = rot_vel[struct_no];
        Eigen::Matrix3d& R = rot_mat[struct_no];
        const double norm_e = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);

        if (norm_e > std::numeric_limits<double>::epsilon())
        {
            const double theta = norm_e * dt;
            for (int i = 0; i < 3; ++i) e[i] /= norm_e;
            const double c_t = cos(theta);
            const double s_t = sin(theta);

            R(0, 0) = c_t + (1.0 - c_t) * e[0] * e[0];
            R(0, 1) = (1.0 - c_t) * e[0] * e[1] - s_t * e[2];
            R(0, 2) = (1.0 - c_t) * e[0] * e[2] + s_t * e[1];
            R(1, 0) = (1.0 - c_t) * e[1] * e[0] + s_t * e[2];
            R(1, 1) = c_t + (1.0 - c_t) * e[1] * e[1];
            R(1, 2) = (1.0 - c_t) * e[1] * e[2] - s_t * e[0];
            R(2, 0) = (1.0 - c_t) * e[2] * e[0] - s_t * e[1];
            R(2, 1) = (1.0 - c_t) * e[2] * e[1] + s_t * e[0];
            R(2, 2) = c_t + (1.0 - c_t) * e[2] * e[2];
        }
    }

    return;
} // setRotationMatrix

} // namespace IBAMR

//////////////////////////////////////////////////////////////////////////////