﻿/*
  Copyright 2013, 2015 SINTEF ICT, Applied Mathematics.
  Copyright 2014, 2015 Dr. Blatt - HPC-Simulation-Software & Services
  Copyright 2014, 2015 Statoil ASA.
  Copyright 2015 NTNU
  Copyright 2015 IRIS AS

  This file is part of the Open Porous Media project (OPM).

  OPM is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  OPM is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with OPM.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef OPM_BLACKOILMODELEBOS_HEADER_INCLUDED
#define OPM_BLACKOILMODELEBOS_HEADER_INCLUDED

#include <ebos/eclproblem.hh>
#include <ewoms/common/start.hh>

#include <opm/autodiff/BlackoilModelParameters.hpp>
#include <opm/autodiff/StandardWellsDense.hpp>
#include <opm/autodiff/AutoDiffBlock.hpp>
#include <opm/autodiff/AutoDiffHelpers.hpp>
#include <opm/autodiff/GridHelpers.hpp>
#include <opm/autodiff/WellHelpers.hpp>
#include <opm/autodiff/GeoProps.hpp>
#include <opm/autodiff/WellDensitySegmented.hpp>
#include <opm/autodiff/VFPProperties.hpp>
#include <opm/autodiff/VFPProdProperties.hpp>
#include <opm/autodiff/VFPInjProperties.hpp>
#include <opm/autodiff/BlackoilDetails.hpp>
#include <opm/autodiff/BlackoilModelEnums.hpp>
#include <opm/autodiff/NewtonIterationBlackoilInterface.hpp>
#include <opm/autodiff/RateConverter.hpp>

#include <opm/core/grid.h>
#include <opm/core/simulator/SimulatorReport.hpp>
#include <opm/core/linalg/LinearSolverInterface.hpp>
#include <opm/core/linalg/ParallelIstlInformation.hpp>
#include <opm/core/props/phaseUsageFromDeck.hpp>
#include <opm/common/ErrorMacros.hpp>
#include <opm/common/Exceptions.hpp>
#include <opm/common/OpmLog/OpmLog.hpp>
#include <opm/parser/eclipse/Units/Units.hpp>
#include <opm/core/well_controls.h>
#include <opm/simulators/timestepping/SimulatorTimer.hpp>
#include <opm/core/utility/parameters/ParameterGroup.hpp>
#include <opm/parser/eclipse/EclipseState/EclipseState.hpp>
#include <opm/parser/eclipse/EclipseState/Tables/TableManager.hpp>

#include <opm/autodiff/ISTLSolver.hpp>
#include <opm/common/data/SimulationDataContainer.hpp>

#include <dune/istl/owneroverlapcopy.hh>
#include <dune/common/parallel/collectivecommunication.hh>
#include <dune/common/timer.hh>
#include <dune/common/unused.hh>

#include <cassert>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <limits>
#include <vector>
#include <algorithm>
#include <fstream>
#include <iostream>


namespace Ewoms {
namespace Properties {
NEW_TYPE_TAG(EclFlowProblem, INHERITS_FROM(BlackOilModel, EclBaseProblem));
SET_BOOL_PROP(EclFlowProblem, DisableWells, true);
SET_BOOL_PROP(EclFlowProblem, EnableDebuggingChecks, false);
SET_BOOL_PROP(EclFlowProblem, ExportGlobalTransmissibility, true);

// SWATINIT is done by the flow part of flow_ebos. this can be removed once the legacy
// code for fluid and satfunc handling gets fully retired.
SET_BOOL_PROP(EclFlowProblem, EnableSwatinit, false);
}}

namespace Opm {
    /// A model implementation for three-phase black oil.
    ///
    /// The simulator is capable of handling three-phase problems
    /// where gas can be dissolved in oil and vice versa. It
    /// uses an industry-standard TPFA discretization with per-phase
    /// upwind weighting of mobilities.
    template <class TypeTag>
    class BlackoilModelEbos
    {
    public:
        // ---------  Types and enums  ---------
        typedef BlackoilState ReservoirState;
        typedef WellStateFullyImplicitBlackoilDense WellState;
        typedef BlackoilModelParameters ModelParameters;

        typedef typename GET_PROP_TYPE(TypeTag, Simulator)         Simulator;
        typedef typename GET_PROP_TYPE(TypeTag, Grid)              Grid;
        typedef typename GET_PROP_TYPE(TypeTag, ElementContext)    ElementContext;
        typedef typename GET_PROP_TYPE(TypeTag, SolutionVector)    SolutionVector ;
        typedef typename GET_PROP_TYPE(TypeTag, PrimaryVariables)  PrimaryVariables ;
        typedef typename GET_PROP_TYPE(TypeTag, FluidSystem)       FluidSystem;
        typedef typename GET_PROP_TYPE(TypeTag, Indices)           BlackoilIndices;
        typedef typename GET_PROP_TYPE(TypeTag, MaterialLaw)       MaterialLaw;
        typedef typename GET_PROP_TYPE(TypeTag, GlobalEqVector)    GlobalEqVector;
        typedef typename GET_PROP_TYPE(TypeTag, MaterialLawParams) MaterialLawParams;

        typedef double Scalar;
        static const int numEq = BlackoilIndices::numEq;
        static const int contiSolventEqIdx = BlackoilIndices::contiSolventEqIdx;
        static const int contiPolymerEqIdx = BlackoilIndices::contiPolymerEqIdx;
        static const int solventSaturationIdx = BlackoilIndices::solventSaturationIdx;
        static const int polymerConcentrationIdx = BlackoilIndices::polymerConcentrationIdx;

        typedef Dune::FieldVector<Scalar, numEq >        VectorBlockType;
        typedef Dune::FieldMatrix<Scalar, numEq, numEq >        MatrixBlockType;
        typedef Dune::BCRSMatrix <MatrixBlockType>      Mat;
        typedef Dune::BlockVector<VectorBlockType>      BVector;

        typedef ISTLSolver< MatrixBlockType, VectorBlockType, BlackoilIndices::pressureSwitchIdx >  ISTLSolverType;
        //typedef typename SolutionVector :: value_type            PrimaryVariables ;

        // For the conversion between the surface volume rate and resrevoir voidage rate
        using RateConverterType = RateConverter::
            SurfaceToReservoirVoidage<BlackoilPropsAdFromDeck::FluidSystem, std::vector<int> >;

        typedef Opm::FIPData FIPDataType;

        // ---------  Public methods  ---------

        /// Construct the model. It will retain references to the
        /// arguments of this functions, and they are expected to
        /// remain in scope for the lifetime of the solver.
        /// \param[in] param            parameters
        /// \param[in] grid             grid data structure
        /// \param[in] wells            well structure
        /// \param[in] vfp_properties   Vertical flow performance tables
        /// \param[in] linsolver        linear solver
        /// \param[in] eclState         eclipse state
        /// \param[in] terminal_output  request output to cout/cerr
        BlackoilModelEbos(Simulator& ebosSimulator,
                          const ModelParameters& param,
                          const StandardWellsDense<TypeTag>& well_model,
                          const NewtonIterationBlackoilInterface& linsolver,
                          const bool terminal_output
                          )
        : ebosSimulator_(ebosSimulator)
        , grid_(ebosSimulator_.gridManager().grid())
        , istlSolver_( dynamic_cast< const ISTLSolverType* > (&linsolver) )
        , phaseUsage_(phaseUsageFromDeck(eclState()))
        , vfp_properties_(
            eclState().getTableManager().getVFPInjTables(),
            eclState().getTableManager().getVFPProdTables())
        , active_(detail::activePhases(phaseUsage_))
        , has_disgas_(FluidSystem::enableDissolvedGas())
        , has_vapoil_(FluidSystem::enableVaporizedOil())
        , has_solvent_(GET_PROP_VALUE(TypeTag, EnableSolvent))
        , has_polymer_(GET_PROP_VALUE(TypeTag, EnablePolymer))
        , param_( param )
        , well_model_ (well_model)        
        , terminal_output_ (terminal_output)
        , rate_converter_(phaseUsage_, ebosSimulator_.problem().pvtRegionArray().empty()?nullptr:ebosSimulator_.problem().pvtRegionArray().data(), AutoDiffGrid::numCells(grid_), std::vector<int>(AutoDiffGrid::numCells(grid_),0))
        , current_relaxation_(1.0)
        , dx_old_(AutoDiffGrid::numCells(grid_))
        , isBeginReportStep_(false)
        {
            // Wells are active if they are active wells on at least
            // one process.
            int wellsActive = localWellsActive() ? 1 : 0;
            wellsActive = grid_.comm().max(wellsActive);
            wellModel().setWellsActive( wellsActive );
            // compute global sum of number of cells
            global_nc_ = detail::countGlobalCells(grid_);
            if (!istlSolver_)
            {
                OPM_THROW(std::logic_error,"solver down cast to ISTLSolver failed");
            }
        }

        bool isParallel() const
        { return  grid_.comm().size() > 1; }

        const EclipseState& eclState() const
        { return ebosSimulator_.gridManager().eclState(); }

        /// Called once before each time step.
        /// \param[in] timer                  simulation timer
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void prepareStep(const SimulatorTimerInterface& /*timer*/,
                         const ReservoirState& reservoir_state,
                         const WellState& /* well_state */)
        {
            if ( wellModel().wellCollection()->havingVREPGroups() ) {
                updateRateConverter(reservoir_state);
            }
        }


        /// Called once per nonlinear iteration.
        /// This model will perform a Newton-Raphson update, changing reservoir_state
        /// and well_state. It will also use the nonlinear_solver to do relaxation of
        /// updates if necessary.
        /// \param[in] iteration              should be 0 for the first call of a new timestep
        /// \param[in] timer                  simulation timer
        /// \param[in] nonlinear_solver       nonlinear solver used (for oscillation/relaxation control)
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        template <class NonlinearSolverType>
        SimulatorReport nonlinearIteration(const int iteration,
                                           const SimulatorTimerInterface& timer,
                                           NonlinearSolverType& nonlinear_solver,
                                           ReservoirState& reservoir_state,
                                           WellState& well_state)
        {
            SimulatorReport report;
            failureReport_ = SimulatorReport();
            Dune::Timer perfTimer;

            perfTimer.start();
            if (iteration == 0) {
                // For each iteration we store in a vector the norms of the residual of
                // the mass balance for each active phase, the well flux and the well equations.
                residual_norms_history_.clear();
                current_relaxation_ = 1.0;
                dx_old_ = 0.0;
            }

            report.total_linearizations = 1;

            try {
                report += assemble(timer, iteration, reservoir_state, well_state);
                report.assemble_time += perfTimer.stop();
            }
            catch (...) {
                report.assemble_time += perfTimer.stop();
                failureReport_ += report;
                // todo (?): make the report an attribute of the class
                throw; // continue throwing the stick
            }

            std::vector<double> residual_norms;
            perfTimer.reset();
            perfTimer.start();
            // the step is not considered converged until at least minIter iterations is done
            report.converged = getConvergence(timer, iteration,residual_norms) && iteration > nonlinear_solver.minIter();

             // checking whether the group targets are converged
             if (wellModel().wellCollection()->groupControlActive()) {
                  report.converged = report.converged && wellModel().wellCollection()->groupTargetConverged(well_state.wellRates());
             }

            report.update_time += perfTimer.stop();
            residual_norms_history_.push_back(residual_norms);
            if (!report.converged) {
                perfTimer.reset();
                perfTimer.start();
                report.total_newton_iterations = 1;

                // enable single precision for solvers when dt is smaller then 20 days
                //residual_.singlePrecision = (unit::convert::to(dt, unit::day) < 20.) ;

                // Compute the nonlinear update.
                const int nc = AutoDiffGrid::numCells(grid_);
                const int nw = numWells();
                BVector x(nc);
                BVector xw(nw);

                try {
                    solveJacobianSystem(x, xw);
                    report.linear_solve_time += perfTimer.stop();
                    report.total_linear_iterations += linearIterationsLastSolve();
                }
                catch (...) {
                    report.linear_solve_time += perfTimer.stop();
                    report.total_linear_iterations += linearIterationsLastSolve();

                    failureReport_ += report;
                    throw; // re-throw up
                }

                perfTimer.reset();
                perfTimer.start();

                // Stabilize the nonlinear update.
                bool isOscillate = false;
                bool isStagnate = false;
                nonlinear_solver.detectOscillations(residual_norms_history_, iteration, isOscillate, isStagnate);
                if (isOscillate) {
                    current_relaxation_ -= nonlinear_solver.relaxIncrement();
                    current_relaxation_ = std::max(current_relaxation_, nonlinear_solver.relaxMax());
                    if (terminalOutputEnabled()) {
                        std::string msg = "    Oscillating behavior detected: Relaxation set to "
                            + std::to_string(current_relaxation_);
                        OpmLog::info(msg);
                    }
                }
                nonlinear_solver.stabilizeNonlinearUpdate(x, dx_old_, current_relaxation_);

                // Apply the update, with considering model-dependent limitations and
                // chopping of the update.
                Dune::printvector(std::cout, x, "x vector", "row");
                updateState(x,reservoir_state);
                wellModel().updateWellState(xw, well_state);
                // if the solution is updated the solution needs to be comunicated to ebos
                // and the cachedIntensiveQuantities needs to be updated.
                convertInput( iteration, reservoir_state, ebosSimulator_ );
                ebosSimulator_.model().invalidateIntensiveQuantitiesCache(/*timeIdx=*/0);

                report.update_time += perfTimer.stop();
            }
            else{


                const int nw = numWells();
                BVector dw(nw);


                const int nc = AutoDiffGrid::numCells(grid_);
                BVector dx(nc);
                dx = 0;

                // copy the states.
                ReservoirState orgResState = reservoir_state;
                WellState orgWellState = well_state;

                auto& duneB = wellModel().B();
                auto& duneC = wellModel().C();
                auto& duneD = wellModel().D();

                auto& A = ebosSimulator_.model().linearizer().matrixA(); // The gt
                auto& A2 = ebosSimulator_.model().linearizer().matrixA2(); // The gt


                // ------------  create and setup the matrices for the jacobians -----------------------start

                //Dune::printvector(std::cout, dx, "x vector", "row");
                typedef Dune::FieldMatrix<Scalar,2,2> M;
                Dune::BCRSMatrix<M> numJac(9,9, Dune::BCRSMatrix<M>::random);
                Dune::BCRSMatrix<M> adJac(9,9, Dune::BCRSMatrix<M>::random);
                Dune::BCRSMatrix<M> adJac2(9,9, Dune::BCRSMatrix<M>::random);
                Dune::BCRSMatrix<M> diffJac(9,9, Dune::BCRSMatrix<M>::random);
                Dune::BCRSMatrix<M> diffJac2(9,9, Dune::BCRSMatrix<M>::random);

                typedef Dune::FieldMatrix<Scalar,2,3> dimBlockB;
                Dune::BCRSMatrix<dimBlockB> B(9,2, Dune::BCRSMatrix<dimBlockB>::random);
                Dune::BCRSMatrix<dimBlockB> numB(9,2, Dune::BCRSMatrix<dimBlockB>::random);
                Dune::BCRSMatrix<dimBlockB> diffB(9,2, Dune::BCRSMatrix<dimBlockB>::random);
                typedef Dune::FieldMatrix<Scalar,3,2> dimBlockC;
                Dune::BCRSMatrix<dimBlockC> C(2,9, Dune::BCRSMatrix<dimBlockC>::random);
                Dune::BCRSMatrix<dimBlockC> numC(2,9, Dune::BCRSMatrix<dimBlockC>::random);
                Dune::BCRSMatrix<dimBlockC> diffC(2,9, Dune::BCRSMatrix<dimBlockC>::random);
                typedef Dune::FieldMatrix<Scalar,3,3> dimBlockD;
                Dune::BCRSMatrix<dimBlockD> D(2,2, Dune::BCRSMatrix<dimBlockD>::random);
                Dune::BCRSMatrix<dimBlockD> numD(2,2, Dune::BCRSMatrix<dimBlockD>::random);
                Dune::BCRSMatrix<dimBlockD> diffD(2,2, Dune::BCRSMatrix<dimBlockD>::random);


                for(int row=0; row < B.N(); ++row){
                    B.setrowsize(row, 2);
                    numB.setrowsize(row, 2);
                    diffB.setrowsize(row, 2);
                }
                B.endrowsizes();
                numB.endrowsizes();
                diffB.endrowsizes();

                for(int row=0; row < numC.N(); ++row){
                    C.setrowsize(row,9);
                    numC.setrowsize(row, 9);
                    diffC.setrowsize(row, 9);

                }
                C.endrowsizes();
                numC.endrowsizes();
                diffC.endrowsizes();

                for(int row=0; row < D.N(); ++row){
                    D.setrowsize(row, 2);
                    numD.setrowsize(row, 2);
                    diffD.setrowsize(row, 2);
                }
                D.endrowsizes();
                numD.endrowsizes();
                diffD.endrowsizes();

                for (int row=0; row < B.N(); ++row){
                    for (int col=0; col < B.M(); ++col){
                        B.addindex(row, col);
                        numB.addindex(row, col);
                        diffB.addindex(row, col);
                    }
                }
                B.endindices();
                numB.endindices();
                diffB.endindices();

                for (int row=0; row < numC.N(); ++row){
                    for (int col=0; col < numC.M(); ++col){
                        C.addindex(row, col);
                        numC.addindex(row, col);
                        diffC.addindex(row, col);
                    }
                }
                C.endindices();
                numC.endindices();
                diffC.endindices();



                for (int row=0; row < D.N(); ++row){
                    for (int col=0; col < D.M(); ++col){
                        D.addindex(row, col);
                        numD.addindex(row, col);
                        diffD.addindex(row, col);
                    }
                }
                D.endindices();
                numD.endindices();
                diffD.endindices();

                Dune::printmatrix(std::cout, duneC, "AD duneC", "row");

                // Copy the relevant elements from the AD jacobians made by the simulator.

                for(std::size_t row_block=0; row_block < C.N(); ++row_block ){
                    for(std::size_t col_block=0; col_block < C.M(); ++col_block ){
                        if (duneC.exists(row_block, col_block)){
                            C[row_block][col_block][0][0] = duneC[row_block][col_block][0][0];
                            C[row_block][col_block][0][1] = duneC[row_block][col_block][0][1];
                            C[row_block][col_block][1][0] = duneC[row_block][col_block][1][0];
                            C[row_block][col_block][1][1] = duneC[row_block][col_block][1][1];
                            C[row_block][col_block][2][0] = duneC[row_block][col_block][2][0];
                            C[row_block][col_block][2][1] = duneC[row_block][col_block][2][1];
                        }
                    }
                }

                for(std::size_t row_block=0; row_block < D.N(); ++row_block ){
                    for(std::size_t col_block=0; col_block < D.M(); ++col_block ){
                        if (duneD.exists(row_block, col_block)){
                            D[row_block][col_block][0][0] = duneD[row_block][col_block][0][0];
                            D[row_block][col_block][0][1] = duneD[row_block][col_block][0][1];
                            D[row_block][col_block][0][2] = duneD[row_block][col_block][0][2];
                            D[row_block][col_block][1][0] = duneD[row_block][col_block][1][0];
                            D[row_block][col_block][1][1] = duneD[row_block][col_block][1][1];
                            D[row_block][col_block][1][2] = duneD[row_block][col_block][1][2];
                            D[row_block][col_block][2][0] = duneD[row_block][col_block][2][0];
                            D[row_block][col_block][2][1] = duneD[row_block][col_block][2][1];
                            D[row_block][col_block][2][2] = duneD[row_block][col_block][2][2];
                        }
                    }
                }


                // duneB is transposed, B is not!
                for(std::size_t row_block=0; row_block < B.N(); ++row_block ){
                    for(std::size_t col_block=0; col_block < B.M(); ++col_block ){
                        if (duneB.exists(col_block, row_block)){
                            //B[row_block][col_block] = duneB[col_block][row_block].transpose();
                            B[row_block][col_block][0][0] = duneB[col_block][row_block][0][0];
                            B[row_block][col_block][0][1] = duneB[col_block][row_block][1][0];
                            B[row_block][col_block][0][2] = duneB[col_block][row_block][2][0];
                            B[row_block][col_block][1][0] = duneB[col_block][row_block][0][1];
                            B[row_block][col_block][1][1] = duneB[col_block][row_block][1][1];
                            B[row_block][col_block][1][2] = duneB[col_block][row_block][2][1];
                        }
                    }
                }


                // Specify the row sizes.
                for (int row=0; row < 9; ++row){
                    numJac.setrowsize(row, 9);
                    adJac.setrowsize(row, 9);
                    adJac2.setrowsize(row, 9);
                    diffJac.setrowsize(row,9);
                    diffJac2.setrowsize(row,9);
                }
                numJac.endrowsizes();
                adJac.endrowsizes();
                adJac2.endrowsizes();
                diffJac.endrowsizes();
                diffJac2.endrowsizes();

                // Specify where we want to be able to index/place values later.
                for (int row=0; row < 9; ++row){
                    for (int col=0; col < 9; ++col){
                        numJac.addindex(row, col);
                        adJac.addindex(row, col);
                        adJac2.addindex(row, col);
                        diffJac.addindex(row, col);
                        diffJac2.addindex(row, col);
                    }
                }
                numJac.endindices();
                adJac.endindices();
                adJac2.endindices();
                diffJac.endindices();
                diffJac2.endindices();


                // Copy the relevant elements from the AD jacobian made by the simulator.
                for(std::size_t row_block=0; row_block < 9; ++row_block ){
                    for(std::size_t col_block=0; col_block < 9; ++col_block ){
                        if (A.exists(row_block, col_block)){
                            adJac[row_block][col_block][0][0] = A[row_block][col_block][0][0];
                            adJac[row_block][col_block][0][1] = A[row_block][col_block][0][1];
                            adJac[row_block][col_block][1][0] = A[row_block][col_block][1][0];
                            adJac[row_block][col_block][1][1] = A[row_block][col_block][1][1];
                        }
                        if (A2.exists(row_block, col_block)){
                            adJac2[row_block][col_block][0][0] = A2[row_block][col_block][0][0];
                            adJac2[row_block][col_block][0][1] = A2[row_block][col_block][0][1];
                            adJac2[row_block][col_block][1][0] = A2[row_block][col_block][1][0];
                            adJac2[row_block][col_block][1][1] = A2[row_block][col_block][1][1];
                        }
                    }
                }

                // ------------  create and setup the matrices for the jacobians ----------------------- end


                std::vector<Scalar> stateValues(2); // Should not be needed as the difference should be the perturbation size...
                std::vector<GlobalEqVector> residualsMB(2); // {f(x - dx/2), f(x + dx/2)}
                //std::vector<GlobalEqVector> residualsWE(2); // {f(x - dx/2), f(x + dx/2)}
                //std::vector<Scalar> pert_sizes = {-0.000001, -10}; // Using a negative value is the same as applying a positive perturbation.
                std::vector<Scalar> pert_sizes = {-0.000001, -10}; // Using a negative value is the same as applying a positive perturbation.




                std::vector<std::vector<std::vector<Scalar>>> residualsWE;
                // Initialization of residualsWE:
                for (std::size_t i=0; i < 2; ++i){
                    std::vector<std::vector<Scalar >> tmp1;
                    for (std::size_t j=0; j < 2; ++j){
                        std::vector<Scalar> tmp2;
                        for (std::size_t k=0; k < 3; ++k){
                            tmp2.push_back(0);
                        }
                        tmp1.push_back(tmp2);
                    }
                    residualsWE.push_back(tmp1);
                }
                std::cout << 2 << "\t" << 2 << "\t" << 3 << std::endl;
                std::cout << residualsWE.size() << "\t" << residualsWE[0].size() << "\t" << residualsWE[0][0].size() << std::endl;

                enum StateTypes {OIL_PRESSURE = 1, WATER_SATURATION = 0};

                for(std::size_t cell_block=0; cell_block<9; ++cell_block){      // For each grid block.
                    for(int stateType = 0; stateType<2; ++stateType){           // For each phase/state_variable in that grid block
                        residualsMB[0] = 0; residualsMB[1] = 0;
                        // resetting the well residual container:
                        for (std::size_t i=0; i < residualsWE.size(); ++i){
                            std::vector<std::vector<Scalar >> tmp1;
                            for (std::size_t j=0; j < residualsWE[i].size(); ++j){
                                std::vector<Scalar> tmp2;
                                for (std::size_t k=0; k < residualsWE[i][j].size(); ++k){
                                    residualsWE[i][j][k] = 0;
                                }
                            }
                        }

                        for (int i = 0; i < 2; ++i){                            // We are doing central difference

                            ReservoirState tmpResState = reservoir_state; // Copy
                            WellState tmpWellState = well_state; //copy
                            dx = 0; // reset the perturbation vector
                            dx[cell_block][stateType] = (i==0) ? -pert_sizes[stateType]/2 : pert_sizes[stateType]/2; // The first iteration calculates the f(x - dx/2)

                            // Apply the perturbation to the reservoir state variable
                            updateState(dx,tmpResState);
                            //wellModel().updateWellState(xw, well_state);

                            // Send this information to ebos (and also convert it to ebos format)
                            convertInput( iteration, tmpResState, ebosSimulator_ );

                            // Delete the cache and recalculate. If we don't do this, there will be no update/change when calling linearize()
                            ebosSimulator_.model().invalidateIntensiveQuantitiesCache(0); //0 is timeidx

                            // Calculate the residual (and also the ad jacobian)
                            ebosSimulator_.model().linearizer().linearize();

                            // Need to convert the jacobian to "flow format". (Scaling by some factors)
                            auto& ebosJac = ebosSimulator_.model().linearizer().matrix();
                            auto& ebosResid = ebosSimulator_.model().linearizer().residual();
                            convertResults(ebosResid, ebosJac);

                            //Run the well equations too.
                            double dt = timer.currentStepLength();
                            wellModel().assemble(ebosSimulator_, iteration, dt, tmpWellState); // This will affect the residuals and the jacobian in ebosSimulator_


                            // Get a copy of the residuals
                            auto resMB_perturbed = ebosSimulator_.model().linearizer().residual();
                            auto resWE_perturbed_column = wellModel().residual(); // std::vector<double>


                            std::cout << resWE_perturbed_column.size() << std::endl;
/*
                            int wellNr = 0;
                            for (std::size_t j = 0; j < resWE_perturbed_column.size(); ++j){
                                if (j%3 == 0 && j > 1){
                                    wellNr++;
                                }
                                residualsWE[i][wellNr][j%3] = resWE_perturbed_column[j];

                            } */
                            residualsWE[i][0][0] = resWE_perturbed_column[0];
                            residualsWE[i][1][0] = resWE_perturbed_column[1];
                            residualsWE[i][0][1] = resWE_perturbed_column[2];
                            residualsWE[i][1][1] = resWE_perturbed_column[3];
                            residualsMB[i] = resMB_perturbed;

                            //for (int j=0; j<2;++j){
                            //    std::cout << residualsWE[i]
                            //}

                            residualsMB[i] = resMB_perturbed;

                            if (stateType == OIL_PRESSURE){
                                stateValues[i] = tmpResState.pressure()[cell_block]; // pressure of oil
                            }
                            else if(stateType == WATER_SATURATION){
                                stateValues[i] = tmpResState.saturation()[2*(cell_block)];  // saturation of water (times 2 because the variable also contains saturation of oil)
                            }
                        }

                        // Calculate the numerical difference
                        for (std::size_t cell_block_res=0; cell_block_res < 9; ++cell_block_res){
                            for (std::size_t res_nr=0; res_nr < 2; ++res_nr){
                                //numJac: (r_new - r_old) / (p_new - p_old)
                                numJac[cell_block_res][cell_block][res_nr][stateType] = (residualsMB[1][cell_block_res][res_nr] - residualsMB[0][cell_block_res][res_nr])/((stateValues[1] - stateValues[0]));
                            }
                        }
                        for (std::size_t cell_block_res=0; cell_block_res < 2; ++cell_block_res){
                            for (std::size_t res_nr=0; res_nr < 3; ++res_nr){
                                //numJac: (r_new - r_old) / (p_new - p_old)
                                numC[cell_block_res][cell_block][res_nr][stateType] = (residualsWE[1][cell_block_res][res_nr] - residualsWE[0][cell_block_res][res_nr])/((stateValues[1] - stateValues[0]));
                            }
                        }
                    }
                }
                Dune::printmatrix(std::cout, adJac2, "AD A", "row");
                Dune::printmatrix(std::cout, numJac, "numerical A", "row");
                calculateDifference(adJac2, numJac, &diffJac);
                Dune::printmatrix(std::cout, diffJac, "A  difference", "row");

                Dune::printmatrix(std::cout, C, "AD C", "row");
                Dune::printmatrix(std::cout, numC, "numerical C", "row");
                calculateDifference(C, numC, &diffC);
                Dune::printmatrix(std::cout, diffC, "C  difference", "row");




                //std::vector<Scalar> pert_sizes2 = {-0.000001, -0.000001, -10}; // Using a negative value is the same as applying a positive perturbation.
                std::vector<Scalar> pert_sizes2 = {-0.01, -0.01, -10000};

                for(std::size_t well=0; well<2; ++well){      // For each well.
                    for(int stateType = 0; stateType<3; ++stateType){           // For each phase/state_variable/component in that well
                        residualsMB[0] = 0; residualsMB[1] = 0;   // reset
                        // resetting the well residual container:
                        for (std::size_t i=0; i < residualsWE.size(); ++i){
                            std::vector<std::vector<Scalar >> tmp1;
                            for (std::size_t j=0; j < residualsWE[i].size(); ++j){
                                std::vector<Scalar> tmp2;
                                for (std::size_t k=0; k < residualsWE[i][j].size(); ++k){
                                    residualsWE[i][j][k] = 0;
                                }
                            }
                        }

                        for (int i = 0; i < 2; ++i){                            // We are doing central difference

                            ReservoirState tmpResState = reservoir_state; // Copy
                            WellState tmpWellState = well_state; //copy
                            dw = 0; // reset the perturbation vector
                            dw[well][stateType] = (i==0) ? -pert_sizes2[stateType]/2 : pert_sizes2[stateType]/2; // The first iteration calculates the f(x - dx/2)

                            // Apply the perturbation to the reservoir state variable
                            //updateState(dx,tmpResState);
                            wellModel().updateWellState(dw, well_state);

                            // Send this information to ebos (and also convert it to ebos format)
                            convertInput( iteration, tmpResState, ebosSimulator_ );

                            // Delete the cache and recalculate. If we don't do this, there will be no update/change when calling linearize()
                            ebosSimulator_.model().invalidateIntensiveQuantitiesCache(0); //0 is the timeidx

                            // Calculate the residual (and also the ad jacobian)
                            ebosSimulator_.model().linearizer().linearize();

                            // Need to convert the jacobian to "flow format". (Scaling by some factors)
                            auto& ebosJac = ebosSimulator_.model().linearizer().matrix();
                            auto& ebosResid = ebosSimulator_.model().linearizer().residual();
                            convertResults(ebosResid, ebosJac);


                            //Run the well equations too.
                            double dt = timer.currentStepLength();
                            wellModel().assemble(ebosSimulator_, iteration, dt, tmpWellState); // This will affect the residuals and the jacobian in ebosSimulator_



                            // Get a copy of the residuals
                            auto resMB_perturbed = ebosSimulator_.model().linearizer().residual();
                            auto resWE_perturbed_column = wellModel().residual(); // std::vector<double>

                            std::cout << " Start " << std::endl;
                            for (int j=0; j < resWE_perturbed_column.size(); ++j){
                                std::cout << resWE_perturbed_column[j] << std::endl;
                            }
                            std::cout << " Done " << std::endl;

                            //int wellNr = 0;
                            //for (std::size_t j = 0; j < resWE_perturbed_column.size(); ++j){
                            //    if (j%3 == 0 && j > 1){
                            //        wellNr++;
                            //    }
                            //    residualsWE[i][wellNr][j%3] = resWE_perturbed_column[j];

                            //}
                            residualsWE[i][0][1] = resWE_perturbed_column[0];
                            residualsWE[i][1][1] = resWE_perturbed_column[1];
                            residualsWE[i][0][1] = resWE_perturbed_column[2];
                            residualsWE[i][1][1] = resWE_perturbed_column[3];
                            residualsMB[i] = resMB_perturbed;


                            // Calculate the numerical difference
                            for (std::size_t cell_block_res=0; cell_block_res < 9; ++cell_block_res){
                                for (std::size_t res_nr=0; res_nr < 2; ++res_nr){
                                    //numJac: (r_new - r_old) / (p_new - p_old)
                                    numB[cell_block_res][well][res_nr][stateType] = (residualsMB[1][cell_block_res][res_nr] - residualsMB[0][cell_block_res][res_nr])/(-pert_sizes2[stateType]*2);
                                }
                            }
                            for (std::size_t cell_block_res=0; cell_block_res < 2; ++cell_block_res){
                                for (std::size_t res_nr=0; res_nr < 2; ++res_nr){
                                    //numJac: (r_new - r_old) / (p_new - p_old)
                                    numD[cell_block_res][well][res_nr][stateType] = (residualsWE[1][cell_block_res][res_nr] - residualsWE[0][cell_block_res][res_nr])/(-pert_sizes2[stateType]*2);
                                }
                            }
                        }
                    }
                }


                //calculateDifference(B, numB, diffB);
                //Dune::printmatrix(std::cout, diffB, "B  difference", "row");





                //calculateDifference(D, numD, diffD);
                //Dune::printmatrix(std::cout, diffD, "D  difference", "row");
            }

                return report;
        }


        template<typename T>
        void calculateDifference(T mat1, T mat0, T* matDiff){
            for(std::size_t row_block=0; row_block < mat1.N(); ++row_block ){
                for(std::size_t col_block=0; col_block < mat1.M(); ++col_block ){
                    for(std::size_t row_in_block=0; row_in_block < mat1[row_block][col_block].N(); ++row_in_block ){
                        for(std::size_t col_in_block=0; col_in_block < mat1[row_block][col_block].M(); ++col_in_block ){

                            Scalar val1 = (mat1[row_block][col_block][row_in_block][col_in_block]);
                            Scalar val0 = (mat0[row_block][col_block][row_in_block][col_in_block]);
                            auto max = std::max(std::abs(val1),std::abs(val0));

                            if (max > 0.0000001){
                                if ( ( std::abs( val1 - val0) ) <= 0.00001 * max ){ // Count it as zero
                                    matDiff[0][row_block][col_block][row_in_block][col_in_block] = 0.0;
                                }
                                else{
                                    matDiff[0][row_block][col_block][row_in_block][col_in_block] = val1 - val0;
                                }
                            }
                            else{ //Count it as zero
                                matDiff[0][row_block][col_block][row_in_block][col_in_block] = 0.0;
                            }


                        }
                    }
                }
            }

        }

        /*
        void calculateNumericalDifferentiation(BVector& residual0, BVector& residual1, Scalar perturbationSize ,Mat& resultMatrix, int stateType) const{
            for (std::size_t cell_block_res=0; cell_block_res < 9; ++cell_block_res){
                for (std::size_t res_nr=0; res_nr < 2; ++res_nr){
                    // (r_new - r_old) / (p_new - p_old)
                    resultMatrix[][][]
                    numJac[cell_block_res][cell_block][res_nr][stateType] = (residualsMB[1][cell_block_res][res_nr] - residualsMB[0][cell_block_res][res_nr])/((stateValues[1] - stateValues[0]));
                }
            }
        }*/

        void printIf(int c, double x, double y, double eps, std::string type) {
            if (std::abs(x-y) > eps) {
                std::cout << type << " " <<c << ": "<<x << " " << y << std::endl;
            }
        }


        /// Called once after each time step.
        /// In this class, this function does nothing.
        /// \param[in] timer                  simulation timer
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void afterStep(const SimulatorTimerInterface& timer,
                       const ReservoirState& reservoir_state,
                       WellState& well_state)
        {
            DUNE_UNUSED_PARAMETER(timer);
            DUNE_UNUSED_PARAMETER(reservoir_state);
            DUNE_UNUSED_PARAMETER(well_state);
        }

        /// Assemble the residual and Jacobian of the nonlinear system.
        /// \param[in]      reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        /// \param[in]      initial_assembly  pass true if this is the first call to assemble() in this timestep
        SimulatorReport assemble(const SimulatorTimerInterface& timer,
                                 const int iterationIdx,
                                 const ReservoirState& reservoir_state,
                                 WellState& well_state)
        {
            using namespace Opm::AutoDiffGrid;

            SimulatorReport report;

            // when having VREP group control, update the rate converter based on reservoir state
            if ( wellModel().wellCollection()->havingVREPGroups() ) {
                updateRateConverter(reservoir_state);
            }

            // -------- Mass balance equations --------
            assembleMassBalanceEq(timer, iterationIdx, reservoir_state);

            // Store the residual. Only used for testing.
            auto& ebosResidMB = ebosSimulator_.model().linearizer().residual();
            std::ofstream file_residualsMB;
            file_residualsMB.open("/home/joakimra/reservoirmodels/simpleRes/residualsMB.txt", std::ios::app);
            file_residualsMB << ebosResidMB << std::endl << std::endl << std::endl;
            file_residualsMB.close();



            // -------- Well equations ----------
            double dt = timer.currentStepLength();

            try
            {
                report = wellModel().assemble(ebosSimulator_, iterationIdx, dt, well_state);
                // Store the residual. Only used for testing.
                auto ebosResidWE = wellModel().residual();
                std::ofstream file_residualsWE;
                file_residualsWE.open("/home/joakimra/reservoirmodels/simpleRes/residualsWE.txt", std::ios::app);
                for (auto i: ebosResidWE)
                    file_residualsWE << i << " ";

                file_residualsWE << std::endl << std::endl << std::endl;
                file_residualsWE.close();
                const auto& ebosJacConst = ebosSimulator_.model().linearizer().matrix();
                auto& A2 = ebosSimulator_.model().linearizer().matrixA2();
                A2 = ebosJacConst;



                // apply well residual to the residual.
                auto& ebosResid = ebosSimulator_.model().linearizer().residual();
                wellModel().apply(ebosResid);
            }
            catch ( const Dune::FMatrixError& e  )
            {
                OPM_THROW(Opm::NumericalProblem,"Well equation did not converge");
            }

            return report;
        }


         /// \brief compute the relative change between to simulation states
        //  \return || u^n+1 - u^n || / || u^n+1 ||
        double relativeChange( const SimulationDataContainer& previous, const SimulationDataContainer& current ) const
        {
            std::vector< double > p0  ( previous.pressure() );
            std::vector< double > sat0( previous.saturation() );

            const std::size_t pSize = p0.size();
            const std::size_t satSize = sat0.size();

            // compute u^n - u^n+1
            for( std::size_t i=0; i<pSize; ++i ) {
                p0[ i ] -= current.pressure()[ i ];
            }

            for( std::size_t i=0; i<satSize; ++i ) {
                sat0[ i ] -= current.saturation()[ i ];
            }

            // compute || u^n - u^n+1 ||
            const double stateOld  = detail::euclidianNormSquared( p0.begin(),   p0.end(), 1, istlSolver().parallelInformation() ) +
                detail::euclidianNormSquared( sat0.begin(), sat0.end(),
                                              current.numPhases(),
                                              istlSolver().parallelInformation() );

            // compute || u^n+1 ||
            const double stateNew  = detail::euclidianNormSquared( current.pressure().begin(),   current.pressure().end(), 1, istlSolver().parallelInformation() ) +
                detail::euclidianNormSquared( current.saturation().begin(), current.saturation().end(),
                                              current.numPhases(),
                                              istlSolver().parallelInformation() );

            if( stateNew > 0.0 ) {
                return stateOld / stateNew ;
            }
            else {
                return 0.0;
            }
        }


        /// The size (number of unknowns) of the nonlinear system of equations.
        int sizeNonLinear() const
        {
            const int nc = Opm::AutoDiffGrid::numCells(grid_);
            const int nw = numWells();
            return numComponents() * (nc + nw);
        }

        /// Number of linear iterations used in last call to solveJacobianSystem().
        int linearIterationsLastSolve() const
        {
            return istlSolver().iterations();
        }

        /// Solve the Jacobian system Jx = r where J is the Jacobian and
        /// r is the residual.
        void solveJacobianSystem(BVector& x, BVector& xw) const
        {
            const auto& ebosJac = ebosSimulator_.model().linearizer().matrix();
            auto& ebosResid = ebosSimulator_.model().linearizer().residual();

            // set initial guess
            x = 0.0;

            // Solve system.
            if( isParallel() )
            {
                typedef WellModelMatrixAdapter< Mat, BVector, BVector, StandardWellsDense<TypeTag>, true > Operator;
                Operator opA(ebosJac, well_model_, istlSolver().parallelInformation() );
                assert( opA.comm() );
                istlSolver().solve( opA, x, ebosResid, *(opA.comm()) );
            }
            else
            {
                typedef WellModelMatrixAdapter< Mat, BVector, BVector, StandardWellsDense<TypeTag>, false > Operator;
                Operator opA(ebosJac, well_model_);
                istlSolver().solve( opA, x, ebosResid );
            }

            if( xw.size() > 0 )
            {
                // recover wells.
                xw = 0.0;
                wellModel().recoverVariable(x, xw);
            }
        }

        //=====================================================================
        // Implementation for ISTL-matrix based operator
        //=====================================================================

        /*!
           \brief Adapter to turn a matrix into a linear operator.

           Adapts a matrix to the assembled linear operator interface
         */
        template<class M, class X, class Y, class WellModel, bool overlapping >
        class WellModelMatrixAdapter : public Dune::AssembledLinearOperator<M,X,Y>
        {
          typedef Dune::AssembledLinearOperator<M,X,Y> BaseType;

        public:
          typedef M matrix_type;
          typedef X domain_type;
          typedef Y range_type;
          typedef typename X::field_type field_type;

#if HAVE_MPI
          typedef Dune::OwnerOverlapCopyCommunication<int,int> communication_type;
#else
          typedef Dune::CollectiveCommunication< Grid > communication_type;
#endif

          enum {
            //! \brief The solver category.
            category = overlapping ?
                Dune::SolverCategory::overlapping :
                Dune::SolverCategory::sequential
          };

          //! constructor: just store a reference to a matrix
          WellModelMatrixAdapter (const M& A, const WellModel& wellMod, const boost::any& parallelInformation = boost::any() )
              : A_( A ), wellMod_( wellMod ), comm_()
          {
#if HAVE_MPI
            if( parallelInformation.type() == typeid(ParallelISTLInformation) )
            {
              const ParallelISTLInformation& info =
                  boost::any_cast<const ParallelISTLInformation&>( parallelInformation);
              comm_.reset( new communication_type( info.communicator() ) );
            }
#endif
          }

          virtual void apply( const X& x, Y& y ) const
          {
            A_.mv( x, y );
            // add well model modification to y
            wellMod_.apply(x, y );

#if HAVE_MPI
            if( comm_ )
              comm_->project( y );
#endif
          }

          // y += \alpha * A * x
          virtual void applyscaleadd (field_type alpha, const X& x, Y& y) const
          {
            A_.usmv(alpha,x,y);
            // add scaled well model modification to y
            wellMod_.applyScaleAdd( alpha, x, y );

#if HAVE_MPI
            if( comm_ )
              comm_->project( y );
#endif
          }

          virtual const matrix_type& getmat() const { return A_; }

          communication_type* comm()
          {
              return comm_.operator->();
          }

        protected:
          const matrix_type& A_ ;
          const WellModel& wellMod_;
          std::unique_ptr< communication_type > comm_;
        };

        /// Apply an update to the primary variables, chopped if appropriate.
        /// \param[in]      dx                updates to apply to primary variables
        /// \param[in, out] reservoir_state   reservoir state variables
        /// \param[in, out] well_state        well state variables
        void updateState(const BVector& dx,
                         ReservoirState& reservoir_state)
        {
            using namespace Opm::AutoDiffGrid;
            const int np = phaseUsage_.num_phases;

            ElementContext elemCtx( ebosSimulator_ );
            const auto& gridView = ebosSimulator_.gridView();
            const auto& elemEndIt = gridView.template end</*codim=*/0>();
            for (auto elemIt = gridView.template begin</*codim=*/0>();
                 elemIt != elemEndIt;
                 ++elemIt)
            {
                const auto& elem = *elemIt;
                elemCtx.updatePrimaryStencil(elem);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);
                const unsigned cell_idx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                const double& dp = dx[cell_idx][flowPhaseToEbosCompIdx(0)];
                //reservoir_state.pressure()[cell_idx] -= dp;
                double& p = reservoir_state.pressure()[cell_idx];
                const double& dp_rel_max = dpMaxRel();
                const int sign_dp = dp > 0 ? 1: -1;
                p -= sign_dp * std::min(std::abs(dp), std::abs(p)*dp_rel_max);
                p = std::max(p, 0.0);

                // Saturation updates.
                const double dsw = active_[Water] ? dx[cell_idx][flowPhaseToEbosCompIdx(1)] : 0.0;
                const int xvar_ind = active_[Water] ? 2 : 1;
                const double dxvar = active_[Gas] ? dx[cell_idx][flowPhaseToEbosCompIdx(xvar_ind)] : 0.0;

                double dso = 0.0;
                double dsg = 0.0;
                double drs = 0.0;
                double drv = 0.0;

                double maxVal = 0.0;
                // water phase
                maxVal = std::max(std::abs(dsw),maxVal);
                dso -= dsw;
                // gas phase
                switch (reservoir_state.hydroCarbonState()[cell_idx]) {
                case HydroCarbonState::GasAndOil:
                    dsg = dxvar;
                    break;
                case HydroCarbonState::OilOnly:
                    drs = dxvar;
                    break;
                case HydroCarbonState::GasOnly:
                    dsg -= dsw;
                    drv = dxvar;
                    break;
                default:
                    OPM_THROW(std::logic_error, "Unknown primary variable enum value in cell " << cell_idx << ": " << reservoir_state.hydroCarbonState()[cell_idx]);
                }
                dso -= dsg;

                // solvent
                const double dss = has_solvent_ ? dx[cell_idx][BlackoilIndices::solventSaturationIdx] : 0.0;
                dso -= dss;

                // polymer
                const double dc = has_polymer_ ? dx[cell_idx][BlackoilIndices::polymerConcentrationIdx] : 0.0;

                // Appleyard chop process.
                maxVal = std::max(std::abs(dsg),maxVal);
                maxVal = std::max(std::abs(dss),maxVal);

                double step = dsMax()/maxVal;
                step = std::min(step, 1.0);

                const Opm::PhaseUsage& pu = phaseUsage_;
                if (active_[Water]) {
                    double& sw = reservoir_state.saturation()[cell_idx*np + pu.phase_pos[ Water ]];
                    sw -= step * dsw;
                }
                if (active_[Gas]) {
                    double& sg = reservoir_state.saturation()[cell_idx*np + pu.phase_pos[ Gas ]];
                    sg -= step * dsg;
                }

                if (has_solvent_) {
                    double& ss = reservoir_state.getCellData( reservoir_state.SSOL )[cell_idx];
                    ss -= step * dss;
                }

                if (has_polymer_) {
                    double& c = reservoir_state.getCellData( reservoir_state.POLYMER )[cell_idx];
                    c -= step * dc;
                    c = std::max(c, 0.0);
                }

                double& so = reservoir_state.saturation()[cell_idx*np + pu.phase_pos[ Oil ]];
                so -= step * dso;

                // phase for when oil and gas
                if (active_[Gas] && active_[Oil] ) {
                    // const double drmaxrel = drMaxRel();
                    // Update rs and rv
                    if (has_disgas_) {
                        double& rs = reservoir_state.gasoilratio()[cell_idx];
                        rs -= drs;
                        rs = std::max(rs, 0.0);

                    }
                    if (has_vapoil_) {
                        double& rv = reservoir_state.rv()[cell_idx];
                        rv -= drv;
                        rv = std::max(rv, 0.0);
                    }

                    // Sg is used as primal variable for water only cells.
                    const double epsilon = 1e-4; //std::sqrt(std::numeric_limits<double>::epsilon());
                    double& sw = reservoir_state.saturation()[cell_idx*np + pu.phase_pos[ Water ]];
                    double& sg = reservoir_state.saturation()[cell_idx*np + pu.phase_pos[ Gas ]];
                    double& rs = reservoir_state.gasoilratio()[cell_idx];
                    double& rv = reservoir_state.rv()[cell_idx];

                    // phase translation sg <-> rs
                    const HydroCarbonState hydroCarbonState = reservoir_state.hydroCarbonState()[cell_idx];
                    const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);
                    const auto& fs = intQuants.fluidState();
                    switch (hydroCarbonState) {
                    case HydroCarbonState::GasAndOil: {

                        // for the Gas and Oil case rs=rsSat and rv=rvSat
                        rs = FluidSystem::oilPvt().saturatedGasDissolutionFactor(fs.pvtRegionIndex(), reservoir_state.temperature()[cell_idx], reservoir_state.pressure()[cell_idx]);
                        // use gas pressure?
                        rv = FluidSystem::gasPvt().saturatedOilVaporizationFactor(fs.pvtRegionIndex(), reservoir_state.temperature()[cell_idx], reservoir_state.pressure()[cell_idx]);

                        if (sw > (1.0 - epsilon)) // water only i.e. do nothing
                            break;

                        if (sg <= 0.0 && has_disgas_) {
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::OilOnly; // sg --> rs
                            sg = 0;
                            so = 1.0 - sw;
                            if (has_solvent_) {
                                double& ss = reservoir_state.getCellData( reservoir_state.SSOL )[cell_idx];
                                so -= ss;
                            }
                            rs *= (1-epsilon);
                        } else if (so <= 0.0 && has_vapoil_) {
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::GasOnly; // sg --> rv
                            so = 0;
                            sg = 1.0 - sw;
                            if (has_solvent_) {
                                double& ss = reservoir_state.getCellData( reservoir_state.SSOL )[cell_idx];
                                sg -= ss;
                            }
                            rv *= (1-epsilon);
                        }
                        break;
                    }
                    case HydroCarbonState::OilOnly: {
                        if (sw > (1.0 - epsilon)) {
                            // water only change to Sg
                            rs = 0;
                            rv = 0;
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::GasAndOil;
                            //std::cout << "watonly rv -> sg" << cell_idx << std::endl;
                            break;
                        }

                        const double& rsSat = FluidSystem::oilPvt().saturatedGasDissolutionFactor(fs.pvtRegionIndex(), reservoir_state.temperature()[cell_idx], reservoir_state.pressure()[cell_idx]);
                        if (rs > ( rsSat * (1+epsilon) ) ) {
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::GasAndOil;
                            sg = epsilon;
                            so -= epsilon;
                            rs = rsSat;
                        }
                        break;
                    }
                    case HydroCarbonState::GasOnly: {
                        if (sw > (1.0 - epsilon)) {
                            // water only change to Sg
                            rs = 0;
                            rv = 0;
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::GasAndOil;
                            //std::cout << "watonly rv -> sg" << cell_idx << std::endl;
                            break;
                        }

                        const double& rvSat = FluidSystem::gasPvt().saturatedOilVaporizationFactor(fs.pvtRegionIndex(), reservoir_state.temperature()[cell_idx], reservoir_state.pressure()[cell_idx]);
                        if (rv > rvSat * (1+epsilon) ) {
                            reservoir_state.hydroCarbonState()[cell_idx] = HydroCarbonState::GasAndOil;
                            so = epsilon;
                            rv = rvSat;
                            sg -= epsilon;
                        }
                        break;
                    }

                    default:
                        OPM_THROW(std::logic_error, "Unknown primary variable enum value in cell " << cell_idx << ": " << hydroCarbonState);
                    }
                }
            }

        }

        /// Return true if output to cout is wanted.
        bool terminalOutputEnabled() const
        {
            return terminal_output_;
        }

        template <class CollectiveCommunication>
        double convergenceReduction(const CollectiveCommunication& comm,
                                    const double pvSumLocal,
                                    std::vector< Scalar >& R_sum,
                                    std::vector< Scalar >& maxCoeff,
                                    std::vector< Scalar >& B_avg,
                                    std::vector< Scalar >& maxNormWell )
        {
            // Compute total pore volume (use only owned entries)
            double pvSum = pvSumLocal;

            if( comm.size() > 1 )
            {
                // global reduction
                std::vector< Scalar > sumBuffer;
                std::vector< Scalar > maxBuffer;
                const int numComp = B_avg.size();
                sumBuffer.reserve( 2*numComp + 1 ); // +1 for pvSum
                maxBuffer.reserve( 2*numComp );
                for( int compIdx = 0; compIdx < numComp; ++compIdx )
                {
                    sumBuffer.push_back( B_avg[ compIdx ] );
                    sumBuffer.push_back( R_sum[ compIdx ] );
                    maxBuffer.push_back( maxCoeff[ compIdx ] );
                    maxBuffer.push_back( maxNormWell[ compIdx ] );
                }

                // Compute total pore volume
                sumBuffer.push_back( pvSum );

                // compute global sum
                comm.sum( sumBuffer.data(), sumBuffer.size() );

                // compute global max
                comm.max( maxBuffer.data(), maxBuffer.size() );

                // restore values to local variables
                for( int compIdx = 0, buffIdx = 0; compIdx < numComp; ++compIdx, ++buffIdx )
                {
                    B_avg[ compIdx ]    = sumBuffer[ buffIdx ];
                    maxCoeff[ compIdx ] = maxBuffer[ buffIdx ];
                    ++buffIdx;

                    R_sum[ compIdx ]       = sumBuffer[ buffIdx ];
                    maxNormWell[ compIdx ] = maxBuffer[ buffIdx ];
                }

                // restore global pore volume
                pvSum = sumBuffer.back();
            }

            // return global pore volume
            return pvSum;
        }

        /// Compute convergence based on total mass balance (tol_mb) and maximum
        /// residual mass balance (tol_cnv).
        /// \param[in]   timer       simulation timer
        /// \param[in]   dt          timestep length
        /// \param[in]   iteration   current iteration number
        bool getConvergence(const SimulatorTimerInterface& timer, const int iteration, std::vector<double>& residual_norms)
        {
            typedef std::vector< Scalar > Vector;

            const double dt = timer.currentStepLength();
            const double tol_mb    = param_.tolerance_mb_;
            const double tol_cnv   = param_.tolerance_cnv_;
            const double tol_wells = param_.tolerance_wells_;

            const int np = numPhases();
            const int numComp = numComponents();

            Vector R_sum(numComp, 0.0 );
            Vector B_avg(numComp, 0.0 );
            Vector maxCoeff(numComp, std::numeric_limits< Scalar >::lowest() );
            Vector maxNormWell(numComp, 0.0 );

            const auto& ebosModel = ebosSimulator_.model();
            const auto& ebosProblem = ebosSimulator_.problem();

            const auto& ebosResid = ebosSimulator_.model().linearizer().residual();

            ElementContext elemCtx(ebosSimulator_);
            const auto& gridView = ebosSimulator().gridView();
            const auto& elemEndIt = gridView.template end</*codim=*/0, Dune::Interior_Partition>();

            double pvSumLocal = 0.0;
            for (auto elemIt = gridView.template begin</*codim=*/0, Dune::Interior_Partition>();
                 elemIt != elemEndIt;
                 ++elemIt)
            {
                const auto& elem = *elemIt;
                elemCtx.updatePrimaryStencil(elem);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);
                const unsigned cell_idx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& fs = intQuants.fluidState();

                const double pvValue = ebosProblem.porosity(cell_idx) * ebosModel.dofTotalVolume( cell_idx );
                pvSumLocal += pvValue;

                for ( int phaseIdx = 0; phaseIdx < np; ++phaseIdx )
                {
                    const int ebosPhaseIdx = flowPhaseToEbosPhaseIdx(phaseIdx);
                    const int ebosCompIdx = flowPhaseToEbosCompIdx(phaseIdx);

                    B_avg[ phaseIdx ] += 1.0 / fs.invB(ebosPhaseIdx).value();
                    const auto R2 = ebosResid[cell_idx][ebosCompIdx];

                    R_sum[ phaseIdx ] += R2;
                    maxCoeff[ phaseIdx ] = std::max( maxCoeff[ phaseIdx ], std::abs( R2 ) / pvValue );
                }

                if ( has_solvent_ ) {
                    B_avg[ contiSolventEqIdx ] += 1.0 / intQuants.solventInverseFormationVolumeFactor().value();
                    const auto R2 = ebosResid[cell_idx][contiSolventEqIdx];
                    R_sum[ contiSolventEqIdx ] += R2;
                    maxCoeff[ contiSolventEqIdx ] = std::max( maxCoeff[ contiSolventEqIdx ], std::abs( R2 ) / pvValue );
                }
                if (has_polymer_ ) {
                    B_avg[ contiPolymerEqIdx ] += 1.0 / fs.invB(FluidSystem::waterPhaseIdx).value();
                    const auto R2 = ebosResid[cell_idx][contiPolymerEqIdx];
                    R_sum[ contiPolymerEqIdx ] += R2;
                    maxCoeff[ contiPolymerEqIdx ] = std::max( maxCoeff[ contiPolymerEqIdx ], std::abs( R2 ) / pvValue );
                }

            }

            // compute local average in terms of global number of elements
            const int bSize = B_avg.size();
            for ( int i = 0; i<bSize; ++i )
            {
                B_avg[ i ] /= Scalar( global_nc_ );
            }

            // compute maximum of local well residuals
            const Vector& wellResidual = wellModel().residual();
            const int nw = wellResidual.size() / numComp;
            assert(nw * numComp == int(wellResidual.size()));
            for( int compIdx = 0; compIdx < numComp; ++compIdx )
            {
                for ( int w = 0; w < nw; ++w ) {
                    maxNormWell[compIdx] = std::max(maxNormWell[compIdx], std::abs(wellResidual[nw*compIdx + w]));
                }
            }

            // compute global sum and max of quantities
            const double pvSum = convergenceReduction(grid_.comm(), pvSumLocal,
                                                      R_sum, maxCoeff, B_avg, maxNormWell );

            Vector CNV(numComp);
            Vector mass_balance_residual(numComp);
            Vector well_flux_residual(numComp);

            bool converged_MB = true;
            bool converged_CNV = true;
            bool converged_Well = true;
            // Finish computation
            for ( int compIdx = 0; compIdx < numComp; ++compIdx )
            {
                CNV[compIdx]                    = B_avg[compIdx] * dt * maxCoeff[compIdx];
                mass_balance_residual[compIdx]  = std::abs(B_avg[compIdx]*R_sum[compIdx]) * dt / pvSum;
                converged_MB                = converged_MB && (mass_balance_residual[compIdx] < tol_mb);
                converged_CNV               = converged_CNV && (CNV[compIdx] < tol_cnv);
                // Well flux convergence is only for fluid phases, not other materials
                // in our current implementation.
                well_flux_residual[compIdx] = B_avg[compIdx] * maxNormWell[compIdx];
                converged_Well = converged_Well && (well_flux_residual[compIdx] < tol_wells);

                residual_norms.push_back(CNV[compIdx]);
            }

            bool converged = converged_MB && converged_Well;

            // do not care about the cell based residual in the last two Newton
            // iterations
            if (iteration < param_.max_strict_iter_)
                converged = converged && converged_CNV;

            if ( terminal_output_ )
            {
                // Only rank 0 does print to std::cout
                if (iteration == 0) {
                    std::string msg = "Iter";

                    std::vector< std::string > key( numComp );
                    for (int phaseIdx = 0; phaseIdx < numPhases(); ++phaseIdx) {
                        const std::string& phaseName = FluidSystem::phaseName(flowPhaseToEbosPhaseIdx(phaseIdx));
                        key[ phaseIdx ] = std::toupper( phaseName.front() );
                    }
                    if (has_solvent_) {
                        key[ solventSaturationIdx ] = "S";
                    }

                    if (has_polymer_) {
                        key[ polymerConcentrationIdx ] = "P";
                    }

                    for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                        msg += "    MB(" + key[ compIdx ] + ")  ";
                    }
                    for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                        msg += "    CNV(" + key[ compIdx ] + ") ";
                    }
                    for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                        msg += "  W-FLUX(" + key[ compIdx ] + ")";
                    }
                    OpmLog::note(msg);
                }
                std::ostringstream ss;
                const std::streamsize oprec = ss.precision(3);
                const std::ios::fmtflags oflags = ss.setf(std::ios::scientific);
                ss << std::setw(4) << iteration;
                for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                    ss << std::setw(11) << mass_balance_residual[compIdx];
                }
                for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                    ss << std::setw(11) << CNV[compIdx];
                }
                for (int compIdx = 0; compIdx < numComp; ++compIdx) {
                    ss << std::setw(11) << well_flux_residual[compIdx];
                }
                ss.precision(oprec);
                ss.flags(oflags);
                OpmLog::note(ss.str());
            }

            for (int phaseIdx = 0; phaseIdx < numPhases(); ++phaseIdx) {
                const auto& phaseName = FluidSystem::phaseName(flowPhaseToEbosPhaseIdx(phaseIdx));

                if (std::isnan(mass_balance_residual[phaseIdx])
                    || std::isnan(CNV[phaseIdx])
                    || (phaseIdx < numPhases() && std::isnan(well_flux_residual[phaseIdx]))) {
                    OPM_THROW(Opm::NumericalProblem, "NaN residual for phase " << phaseName);
                }
                if (mass_balance_residual[phaseIdx] > maxResidualAllowed()
                    || CNV[phaseIdx] > maxResidualAllowed()
                    || (phaseIdx < numPhases() && well_flux_residual[phaseIdx] > maxResidualAllowed())) {
                    OPM_THROW(Opm::NumericalProblem, "Too large residual for phase " << phaseName);
                }
            }

            return converged;
        }


        /// The number of active fluid phases in the model.
        int numPhases() const
        {
            return phaseUsage_.num_phases;
        }

        int numComponents() const
        {
            if (numPhases() == 2) {
                return 2;
            }
            int numComp = FluidSystem::numComponents;
            if (has_solvent_)
                numComp ++;

            if (has_polymer_)
                numComp ++;

            return numComp;
        }

        /// Wrapper required due to not following generic API
        template<class T>
        std::vector<std::vector<double> >
        computeFluidInPlace(const T&, const std::vector<int>& fipnum) const
        {
            return computeFluidInPlace(fipnum);
        }

        std::vector<std::vector<double> >
        computeFluidInPlace(const std::vector<int>& fipnum) const
        {
            const auto& comm = grid_.comm();
            const auto& gridView = ebosSimulator().gridView();
            const int nc = gridView.size(/*codim=*/0);
            const int maxnp = Opm::BlackoilPhases::MaxNumPhases;
            int ntFip = *std::max_element(fipnum.begin(), fipnum.end());
            ntFip = comm.max(ntFip);

            std::vector<double> tpv(ntFip, 0.0);
            std::vector<double> hcpv(ntFip, 0.0);

            std::vector<std::vector<double> > regionValues(ntFip, std::vector<double>(FIPDataType::fipValues,0.0));

            for (int i = 0; i<FIPDataType::fipValues; i++) {
                fip_.fip[i].resize(nc,0.0);
            }

            ElementContext elemCtx(ebosSimulator_);
            const auto& elemEndIt = gridView.template end</*codim=*/0, Dune::Interior_Partition>();
            for (auto elemIt = gridView.template begin</*codim=*/0, Dune::Interior_Partition>();
                 elemIt != elemEndIt;
                 ++elemIt)
            {
                elemCtx.updatePrimaryStencil(*elemIt);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);

                const unsigned cellIdx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& fs = intQuants.fluidState();

                const int regionIdx = fipnum[cellIdx] - 1;
                if (regionIdx < 0) {
                    // the given cell is not attributed to any region
                    continue;
                }

                // calculate the pore volume of the current cell. Note that the porosity
                // returned by the intensive quantities is defined as the ratio of pore
                // space to total cell volume and includes all pressure dependent (->
                // rock compressibility) and static modifiers (MULTPV, MULTREGP, NTG,
                // PORV, MINPV and friends). Also note that because of this, the porosity
                // returned by the intensive quantities can be outside of the physical
                // range [0, 1] in pathetic cases.
                const double pv =
                    ebosSimulator_.model().dofTotalVolume(cellIdx)
                    * intQuants.porosity().value();

                for (int phase = 0; phase < maxnp; ++phase) {
                    const double b = fs.invB(flowPhaseToEbosPhaseIdx(phase)).value();
                    const double s = fs.saturation(flowPhaseToEbosPhaseIdx(phase)).value();

                    fip_.fip[phase][cellIdx] = b * s * pv;

                    if (active_[ phase ]) {
                        regionValues[regionIdx][phase] += fip_.fip[phase][cellIdx];
                    }
                }

                if (active_[ Oil ] && active_[ Gas ]) {
                    // Account for gas dissolved in oil and vaporized oil
                    fip_.fip[FIPDataType::FIP_DISSOLVED_GAS][cellIdx] = fs.Rs().value() * fip_.fip[FIPDataType::FIP_LIQUID][cellIdx];
                    fip_.fip[FIPDataType::FIP_VAPORIZED_OIL][cellIdx] = fs.Rv().value() * fip_.fip[FIPDataType::FIP_VAPOUR][cellIdx];

                    regionValues[regionIdx][FIPData::FIP_DISSOLVED_GAS] += fip_.fip[FIPData::FIP_DISSOLVED_GAS][cellIdx];
                    regionValues[regionIdx][FIPData::FIP_VAPORIZED_OIL] += fip_.fip[FIPData::FIP_VAPORIZED_OIL][cellIdx];
                }

                const double hydrocarbon = fs.saturation(FluidSystem::oilPhaseIdx).value() + fs.saturation(FluidSystem::gasPhaseIdx).value();

                tpv[regionIdx] += pv;
                hcpv[regionIdx] += pv * hydrocarbon;
            }

            // sum tpv (-> total pore volume of the regions) and hcpv (-> pore volume of the
            // the regions that is occupied by hydrocarbons)
            comm.sum(tpv.data(), tpv.size());
            comm.sum(hcpv.data(), hcpv.size());

            for (auto elemIt = gridView.template begin</*codim=*/0, Dune::Interior_Partition>();
                 elemIt != elemEndIt;
                 ++elemIt)
            {
                const auto& elem = *elemIt;

                elemCtx.updatePrimaryStencil(elem);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);

                unsigned cellIdx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                const int regionIdx = fipnum[cellIdx] - 1;
                if (regionIdx < 0) {
                    // the cell is not attributed to any region. ignore it!
                    continue;
                }

                const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& fs = intQuants.fluidState();

                // calculate the pore volume of the current cell. Note that the
                // porosity returned by the intensive quantities is defined as the
                // ratio of pore space to total cell volume and includes all pressure
                // dependent (-> rock compressibility) and static modifiers (MULTPV,
                // MULTREGP, NTG, PORV, MINPV and friends). Also note that because of
                // this, the porosity returned by the intensive quantities can be
                // outside of the physical range [0, 1] in pathetic cases.
                const double pv =
                    ebosSimulator_.model().dofTotalVolume(cellIdx)
                    * intQuants.porosity().value();

                fip_.fip[FIPDataType::FIP_PV][cellIdx] = pv;
                const double hydrocarbon = fs.saturation(FluidSystem::oilPhaseIdx).value() + fs.saturation(FluidSystem::gasPhaseIdx).value();

                //Compute hydrocarbon pore volume weighted average pressure.
                //If we have no hydrocarbon in region, use pore volume weighted average pressure instead
                if (hcpv[regionIdx] > 1e-10) {
                    fip_.fip[FIPDataType::FIP_WEIGHTED_PRESSURE][cellIdx] = pv * fs.pressure(FluidSystem::oilPhaseIdx).value() * hydrocarbon / hcpv[regionIdx];
                } else {
                    fip_.fip[FIPDataType::FIP_WEIGHTED_PRESSURE][cellIdx] = pv * fs.pressure(FluidSystem::oilPhaseIdx).value() / tpv[regionIdx];
                }

                regionValues[regionIdx][FIPDataType::FIP_PV] += fip_.fip[FIPDataType::FIP_PV][cellIdx];
                regionValues[regionIdx][FIPDataType::FIP_WEIGHTED_PRESSURE] += fip_.fip[FIPDataType::FIP_WEIGHTED_PRESSURE][cellIdx];
            }

            // sum the results over all processes
            for(int regionIdx=0; regionIdx < ntFip; ++regionIdx) {
                comm.sum(regionValues[regionIdx].data(), regionValues[regionIdx].size());
            }

            return regionValues;
        }

        SimulationDataContainer getSimulatorData ( const SimulationDataContainer& localState) const
        {
            typedef std::vector<double> VectorType;

            const auto& ebosModel = ebosSimulator().model();
            const auto& phaseUsage = phaseUsage_;

            // extract everything which can possibly be written to disk
            const int numCells   = ebosModel.numGridDof();
            const int num_phases = numPhases();

            SimulationDataContainer simData( numCells, 0, num_phases );

            //Get shorthands for water, oil, gas
            const int aqua_active = phaseUsage.phase_used[Opm::PhaseUsage::Aqua];
            const int liquid_active = phaseUsage.phase_used[Opm::PhaseUsage::Liquid];
            const int vapour_active = phaseUsage.phase_used[Opm::PhaseUsage::Vapour];

            const int aqua_pos   = phaseUsage.phase_pos[ Opm::PhaseUsage::Aqua ];
            const int liquid_pos = phaseUsage.phase_pos[ Opm::PhaseUsage::Liquid ];
            const int vapour_pos = phaseUsage.phase_pos[ Opm::PhaseUsage::Vapour ];

            VectorType zero;

            VectorType& pressureOil = simData.pressure();
            VectorType& temperature = simData.temperature();
            VectorType& saturation = simData.saturation();

            // WATER
            if( aqua_active ) {
                simData.registerCellData( "1OVERBW", 1 );
                simData.registerCellData( "WAT_DEN", 1 );
                simData.registerCellData( "WAT_VISC", 1 );
                simData.registerCellData( "WATKR", 1 );
            }

            VectorType& bWater   = aqua_active ? simData.getCellData( "1OVERBW" ) : zero;
            VectorType& rhoWater = aqua_active ? simData.getCellData( "WAT_DEN" ) : zero;
            VectorType& muWater  = aqua_active ? simData.getCellData( "WAT_VISC" ) : zero;
            VectorType& krWater  = aqua_active ? simData.getCellData( "WATKR" ) : zero;

            // OIL
            if( liquid_active ) {
                simData.registerCellData( "1OVERBO", 1 );
                simData.registerCellData( "OIL_DEN", 1 );
                simData.registerCellData( "OIL_VISC", 1 );
                simData.registerCellData( "OILKR", 1 );
            }

            VectorType& bOil   = liquid_active ? simData.getCellData( "1OVERBO" ) : zero;
            VectorType& rhoOil = liquid_active ? simData.getCellData( "OIL_DEN" ) : zero;
            VectorType& muOil  = liquid_active ? simData.getCellData( "OIL_VISC" ) : zero;
            VectorType& krOil  = liquid_active ? simData.getCellData( "OILKR" ) : zero;

            // GAS
            if( vapour_active ) {
                simData.registerCellData( "1OVERBG", 1 );
                simData.registerCellData( "GAS_DEN", 1 );
                simData.registerCellData( "GAS_VISC", 1 );
                simData.registerCellData( "GASKR", 1 );
            }

            VectorType& bGas   = vapour_active ? simData.getCellData( "1OVERBG" ) : zero;
            VectorType& rhoGas = vapour_active ? simData.getCellData( "GAS_DEN" ) : zero;
            VectorType& muGas  = vapour_active ? simData.getCellData( "GAS_VISC" ) : zero;
            VectorType& krGas  = vapour_active ? simData.getCellData( "GASKR" ) : zero;

            simData.registerCellData( BlackoilState::GASOILRATIO, 1 );
            simData.registerCellData( BlackoilState::RV, 1 );
            simData.registerCellData( "RSSAT", 1 );
            simData.registerCellData( "RVSAT", 1 );

            VectorType& Rs    = simData.getCellData( BlackoilState::GASOILRATIO );
            VectorType& Rv    = simData.getCellData( BlackoilState::RV );
            VectorType& RsSat = simData.getCellData( "RSSAT" );
            VectorType& RvSat = simData.getCellData( "RVSAT" );

            simData.registerCellData( "PBUB", 1 );
            simData.registerCellData( "PDEW", 1 );

            VectorType& Pb = simData.getCellData( "PBUB" );
            VectorType& Pd = simData.getCellData( "PDEW" );

            simData.registerCellData( "SOMAX", 1 );
            VectorType& somax = simData.getCellData( "SOMAX" );

            // Two components for hysteresis parameters
            // pcSwMdc/krnSwMdc, one for oil-water and one for gas-oil
            simData.registerCellData( "PCSWMDC_GO", 1 );
            simData.registerCellData( "KRNSWMDC_GO", 1 );

            simData.registerCellData( "PCSWMDC_OW", 1 );
            simData.registerCellData( "KRNSWMDC_OW", 1 );

            VectorType& pcSwMdc_go = simData.getCellData( "PCSWMDC_GO" );
            VectorType& krnSwMdc_go = simData.getCellData( "KRNSWMDC_GO" );

            VectorType& pcSwMdc_ow = simData.getCellData( "PCSWMDC_OW" );
            VectorType& krnSwMdc_ow = simData.getCellData( "KRNSWMDC_OW" );

            if (has_solvent_) {
                simData.registerCellData( "SSOL", 1 );
            }
            VectorType& ssol  = has_solvent_ ? simData.getCellData( "SSOL" ) : zero;

            if (has_polymer_) {
                simData.registerCellData( "POLYMER", 1 );
            }
            VectorType& cpolymer  = has_polymer_ ? simData.getCellData( "POLYMER" ) : zero;

            std::vector<int> failed_cells_pb;
            std::vector<int> failed_cells_pd;
            const auto& gridView = ebosSimulator().gridView();
            auto elemIt = gridView.template begin</*codim=*/ 0, Dune::Interior_Partition>();
            const auto& elemEndIt = gridView.template end</*codim=*/ 0, Dune::Interior_Partition>();
            ElementContext elemCtx(ebosSimulator());

            for (; elemIt != elemEndIt; ++elemIt) {
                const auto& elem = *elemIt;

                elemCtx.updatePrimaryStencil(elem);
                elemCtx.updatePrimaryIntensiveQuantities(/*timeIdx=*/0);

                const unsigned cellIdx = elemCtx.globalSpaceIndex(/*spaceIdx=*/0, /*timeIdx=*/0);
                const auto& intQuants = elemCtx.intensiveQuantities(/*spaceIdx=*/0, /*timeIdx=*/0);

                const auto& fs = intQuants.fluidState();

                const int satIdx = cellIdx * num_phases;

                pressureOil[cellIdx] = fs.pressure(FluidSystem::oilPhaseIdx).value();

                temperature[cellIdx] = fs.temperature(FluidSystem::oilPhaseIdx).value();

                somax[cellIdx] = ebosSimulator().model().maxOilSaturation(cellIdx);

                const auto& matLawManager = ebosSimulator().problem().materialLawManager();
                if (matLawManager->enableHysteresis()) {
                    matLawManager->oilWaterHysteresisParams(
                            pcSwMdc_ow[cellIdx],
                            krnSwMdc_ow[cellIdx],
                            cellIdx);
                    matLawManager->gasOilHysteresisParams(
                            pcSwMdc_go[cellIdx],
                            krnSwMdc_go[cellIdx],
                            cellIdx);
                }

                if (aqua_active) {
                    saturation[ satIdx + aqua_pos ] = fs.saturation(FluidSystem::waterPhaseIdx).value();
                    bWater[cellIdx] = fs.invB(FluidSystem::waterPhaseIdx).value();
                    rhoWater[cellIdx] = fs.density(FluidSystem::waterPhaseIdx).value();
                    muWater[cellIdx] = fs.viscosity(FluidSystem::waterPhaseIdx).value();
                    krWater[cellIdx] = intQuants.relativePermeability(FluidSystem::waterPhaseIdx).value();
                }
                if (vapour_active) {
                    saturation[ satIdx + vapour_pos ]  = fs.saturation(FluidSystem::gasPhaseIdx).value();
                    bGas[cellIdx] = fs.invB(FluidSystem::gasPhaseIdx).value();
                    rhoGas[cellIdx] = fs.density(FluidSystem::gasPhaseIdx).value();
                    muGas[cellIdx] = fs.viscosity(FluidSystem::gasPhaseIdx).value();
                    krGas[cellIdx] = intQuants.relativePermeability(FluidSystem::gasPhaseIdx).value();
                    Rs[cellIdx] = fs.Rs().value();
                    Rv[cellIdx] = fs.Rv().value();
                    RsSat[cellIdx] = FluidSystem::saturatedDissolutionFactor(fs,
                                                                             FluidSystem::oilPhaseIdx,
                                                                             intQuants.pvtRegionIndex(),
                                                                             /*maxOilSaturation=*/1.0).value();
                    RvSat[cellIdx] = FluidSystem::saturatedDissolutionFactor(fs,
                                                                             FluidSystem::gasPhaseIdx,
                                                                             intQuants.pvtRegionIndex(),
                                                                             /*maxOilSaturation=*/1.0).value();
                    try {
                        Pb[cellIdx] = FluidSystem::bubblePointPressure(fs, intQuants.pvtRegionIndex()).value();
                    }
                    catch (const NumericalProblem& e) {
                        failed_cells_pb.push_back(cellIdx);
                    }
                    try {
                        Pd[cellIdx] = FluidSystem::dewPointPressure(fs, intQuants.pvtRegionIndex()).value();
                    }
                    catch (const NumericalProblem& e) {
                        failed_cells_pd.push_back(cellIdx);
                    }
                }
                if( liquid_active )
                {
                    saturation[ satIdx + liquid_pos ] = fs.saturation(FluidSystem::oilPhaseIdx).value();
                    bOil[cellIdx] = fs.invB(FluidSystem::oilPhaseIdx).value();
                    rhoOil[cellIdx] = fs.density(FluidSystem::oilPhaseIdx).value();
                    muOil[cellIdx] = fs.viscosity(FluidSystem::oilPhaseIdx).value();
                    krOil[cellIdx] = intQuants.relativePermeability(FluidSystem::oilPhaseIdx).value();
                }

                if (has_solvent_)
                {
                    ssol[cellIdx] = intQuants.solventSaturation().value();
                }

                if (has_polymer_)
                {
                    cpolymer[cellIdx] = intQuants.polymerConcentration().value();
                }

                // hack to make the intial output of rs and rv Ecl compatible.
                // For cells with swat == 1 Ecl outputs; rs = rsSat and rv=rvSat, in all but the initial step
                // where it outputs rs and rv values calculated by the initialization. To be compatible we overwrite
                // rs and rv with the values passed by the localState.
                // Volume factors, densities and viscosities need to be recalculated with the updated rs and rv values.
                if (ebosSimulator_.episodeIndex() < 0 && vapour_active && liquid_active ) {

                    Rs[cellIdx] = localState.getCellData( BlackoilState::GASOILRATIO )[cellIdx];
                    Rv[cellIdx] = localState.getCellData( BlackoilState::RV)[cellIdx];

                    // copy the fluidstate and set the new rs and rv values
                    auto fs_updated = fs;
                    auto rs_eval = fs_updated.Rs();
                    rs_eval.setValue( Rs[cellIdx] );
                    fs_updated.setRs(rs_eval);
                    auto rv_eval = fs_updated.Rv();
                    rv_eval.setValue( Rv[cellIdx] );
                    fs_updated.setRv(rv_eval);

                    //re-compute the volume factors, viscosities and densities.
                    rhoOil[cellIdx] = FluidSystem::density(fs_updated,
                                                           FluidSystem::oilPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();
                    rhoGas[cellIdx] = FluidSystem::density(fs_updated,
                                                           FluidSystem::gasPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();

                    bOil[cellIdx] = FluidSystem::inverseFormationVolumeFactor(fs_updated,
                                                           FluidSystem::oilPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();
                    bGas[cellIdx] = FluidSystem::inverseFormationVolumeFactor(fs_updated,
                                                           FluidSystem::gasPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();

                    muOil[cellIdx] = FluidSystem::viscosity(fs_updated,
                                                           FluidSystem::oilPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();
                    muGas[cellIdx] = FluidSystem::viscosity(fs_updated,
                                                           FluidSystem::gasPhaseIdx,
                                                           intQuants.pvtRegionIndex()).value();

                }
            }

            const size_t max_num_cells_faillog = 20;
            if (failed_cells_pb.size() > 0) {
                std::stringstream errlog;
                errlog << "Finding the bubble point pressure failed for " << failed_cells_pb.size() << " cells [";
                errlog << failed_cells_pb[0];
                const size_t max_elems = std::min(max_num_cells_faillog, failed_cells_pb.size());
                for (size_t i = 1; i < max_elems; ++i) {
                    errlog << ", " << failed_cells_pb[i];
                }
                if (failed_cells_pb.size() > max_num_cells_faillog) {
                    errlog << ", ...";
                }
                errlog << "]";
                OpmLog::warning("Bubble point numerical problem", errlog.str());
            }
            if (failed_cells_pd.size() > 0) {
                std::stringstream errlog;
                errlog << "Finding the dew point pressure failed for " << failed_cells_pd.size() << " cells [";
                errlog << failed_cells_pd[0];
                const size_t max_elems = std::min(max_num_cells_faillog, failed_cells_pd.size());
                for (size_t i = 1; i < max_elems; ++i) {
                    errlog << ", " << failed_cells_pd[i];
                }
                if (failed_cells_pd.size() > max_num_cells_faillog) {
                    errlog << ", ...";
                }
                errlog << "]";
                OpmLog::warning("Dew point numerical problem", errlog.str());
            }

            return simData;
        }

        const FIPDataType& getFIPData() const {
            return fip_;
        }

        const Simulator& ebosSimulator() const
        { return ebosSimulator_; }

        /// return the statistics if the nonlinearIteration() method failed
        const SimulatorReport& failureReport() const
        { return failureReport_; }

    protected:
        const ISTLSolverType& istlSolver() const
        {
            assert( istlSolver_ );
            return *istlSolver_;
        }

        // ---------  Data members  ---------

        Simulator& ebosSimulator_;
        const Grid&            grid_;
        const ISTLSolverType*  istlSolver_;
        const PhaseUsage phaseUsage_;
        VFPProperties                   vfp_properties_;
        // For each canonical phase -> true if active
        const std::vector<bool>         active_;
        // Size = # active phases. Maps active -> canonical phase indices.
        const std::vector<int>          cells_;  // All grid cells
        const bool has_disgas_;
        const bool has_vapoil_;
        const bool has_solvent_;
        const bool has_polymer_;

        ModelParameters                 param_;
        SimulatorReport failureReport_;

        // Well Model
        StandardWellsDense<TypeTag> well_model_;

        /// \brief Whether we print something to std::cout
        bool terminal_output_;
        /// \brief The number of cells of the global grid.
        long int global_nc_;

        // rate converter between the surface volume rates and reservoir voidage rates
        RateConverterType rate_converter_;

        std::vector<std::vector<double>> residual_norms_history_;
        double current_relaxation_;
        BVector dx_old_;
        mutable FIPDataType fip_;

    public:
        /// return the StandardWells object
        StandardWellsDense<TypeTag>&
        wellModel() { return well_model_; }
        const StandardWellsDense<TypeTag>&
        wellModel() const { return well_model_; }

        /// return the Well struct in the StandardWells
        const Wells& wells() const { return well_model_.wells(); }

        /// return true if wells are available in the reservoir
        bool wellsActive() const { return well_model_.wellsActive(); }

        int numWells() const { return wellsActive() ? wells().number_of_wells : 0; }

        /// return true if wells are available on this process
        bool localWellsActive() const { return well_model_.localWellsActive(); }


        void convertInput( const int iterationIdx,
                           const ReservoirState& reservoirState,
                           Simulator& simulator ) const
        {
            SolutionVector& solution = simulator.model().solution( 0 /* timeIdx */ );
            const Opm::PhaseUsage pu = phaseUsage_;

            const int numCells = reservoirState.numCells();
            const int numPhases = phaseUsage_.num_phases;
            const auto& oilPressure = reservoirState.pressure();
            const auto& saturations = reservoirState.saturation();
            const auto& rs          = reservoirState.gasoilratio();
            const auto& rv          = reservoirState.rv();
            for( int cellIdx = 0; cellIdx<numCells; ++cellIdx )
            {
                // set non-switching primary variables
                PrimaryVariables& cellPv = solution[ cellIdx ];
                // set water saturation
                cellPv[BlackoilIndices::waterSaturationIdx] = saturations[cellIdx*numPhases + pu.phase_pos[Water]];

                if (has_solvent_) {
                    cellPv[BlackoilIndices::solventSaturationIdx] = reservoirState.getCellData( reservoirState.SSOL )[cellIdx];
                }

                if (has_polymer_) {
                    cellPv[BlackoilIndices::polymerConcentrationIdx] = reservoirState.getCellData( reservoirState.POLYMER )[cellIdx];
                }


                // set switching variable and interpretation
                if (active_[Gas] ) {
                    if( reservoirState.hydroCarbonState()[cellIdx] == HydroCarbonState::OilOnly && has_disgas_ )
                    {
                        cellPv[BlackoilIndices::compositionSwitchIdx] = rs[cellIdx];
                        cellPv[BlackoilIndices::pressureSwitchIdx] = oilPressure[cellIdx];
                        cellPv.setPrimaryVarsMeaning( PrimaryVariables::Sw_po_Rs );
                    }
                    else if( reservoirState.hydroCarbonState()[cellIdx] == HydroCarbonState::GasOnly && has_vapoil_ )
                    {
                        // this case (-> gas only with vaporized oil in the gas) is
                        // relatively expensive as it requires to compute the capillary
                        // pressure in order to get the gas phase pressure. (the reason why
                        // ebos uses the gas pressure here is that it makes the common case
                        // of the primary variable switching code fast because to determine
                        // whether the oil phase appears one needs to compute the Rv value
                        // for the saturated gas phase and if this is not available as a
                        // primary variable, it needs to be computed.) luckily for here, the
                        // gas-only case is not too common, so the performance impact of this
                        // is limited.
                        typedef Opm::SimpleModularFluidState<double,
                                /*numPhases=*/3,
                                /*numComponents=*/3,
                                FluidSystem,
                                /*storePressure=*/false,
                                /*storeTemperature=*/false,
                                /*storeComposition=*/false,
                                /*storeFugacity=*/false,
                                /*storeSaturation=*/true,
                                /*storeDensity=*/false,
                                /*storeViscosity=*/false,
                                /*storeEnthalpy=*/false> SatOnlyFluidState;
                        SatOnlyFluidState fluidState;
                        fluidState.setSaturation(FluidSystem::waterPhaseIdx, saturations[cellIdx*numPhases + pu.phase_pos[Water]]);
                        fluidState.setSaturation(FluidSystem::oilPhaseIdx, saturations[cellIdx*numPhases + pu.phase_pos[Oil]]);
                        fluidState.setSaturation(FluidSystem::gasPhaseIdx, saturations[cellIdx*numPhases + pu.phase_pos[Gas]]);

                        double pC[/*numPhases=*/3] = { 0.0, 0.0, 0.0 };
                        const MaterialLawParams& matParams = simulator.problem().materialLawParams(cellIdx);
                        MaterialLaw::capillaryPressures(pC, matParams, fluidState);
                        double pg = oilPressure[cellIdx] + (pC[FluidSystem::gasPhaseIdx] - pC[FluidSystem::oilPhaseIdx]);

                        cellPv[BlackoilIndices::compositionSwitchIdx] = rv[cellIdx];
                        cellPv[BlackoilIndices::pressureSwitchIdx] = pg;
                        cellPv.setPrimaryVarsMeaning( PrimaryVariables::Sw_pg_Rv );
                    }
                    else
                    {
                        assert( reservoirState.hydroCarbonState()[cellIdx] == HydroCarbonState::GasAndOil);
                        cellPv[BlackoilIndices::compositionSwitchIdx] = saturations[cellIdx*numPhases + pu.phase_pos[Gas]];
                        cellPv[BlackoilIndices::pressureSwitchIdx] = oilPressure[ cellIdx ];
                        cellPv.setPrimaryVarsMeaning( PrimaryVariables::Sw_po_Sg );
                    }
                } else {
                    // for oil-water case oil pressure should be used as primary variable
                    cellPv[BlackoilIndices::pressureSwitchIdx] = oilPressure[cellIdx];
                }
            }

            if( iterationIdx == 0 )
            {
                simulator.model().solution( 1 /* timeIdx */ ) = solution;
            }
        }

    public:
        int ebosCompToFlowPhaseIdx( const int compIdx ) const
        {
            assert(compIdx < 3);
            const int compToPhase[ 3 ] = { Oil, Water, Gas };
            return compToPhase[ compIdx ];
        }

        int flowToEbosPvIdx( const int flowPv ) const
        {
            const int flowToEbos[ 3 ] = {
                                          BlackoilIndices::pressureSwitchIdx,
                                          BlackoilIndices::waterSaturationIdx,
                                          BlackoilIndices::compositionSwitchIdx
                                        };

            if (flowPv > 2 )
                return flowPv;

            return flowToEbos[ flowPv ];
        }

        int flowPhaseToEbosCompIdx( const int phaseIdx ) const
        {
            const int phaseToComp[ 3 ] = { FluidSystem::waterCompIdx, FluidSystem::oilCompIdx, FluidSystem::gasCompIdx};
            if (phaseIdx > 2 )
                return phaseIdx;
            return phaseToComp[ phaseIdx ];
        }




    private:
        void convertResults(BVector& ebosResid, Mat& ebosJac) const
        {
            const Opm::PhaseUsage pu = phaseUsage_;
            const int numFlowPhases = pu.num_phases;
            const int numCells = ebosJac.N();
            assert( numCells == static_cast<int>(ebosJac.M()) );

            // write the right-hand-side values from the ebosJac into the objects
            // allocated above.
            const auto endrow = ebosJac.end();
            for( int cellIdx = 0; cellIdx < numCells; ++cellIdx )
            {
                const double cellVolume = ebosSimulator_.model().dofTotalVolume(cellIdx);
                auto& cellRes = ebosResid[ cellIdx ];

                unsigned pvtRegionIdx = ebosSimulator_.problem().pvtRegionIndex(cellIdx);

                for( int flowPhaseIdx = 0; flowPhaseIdx < numFlowPhases; ++flowPhaseIdx )
                {
                    const int canonicalFlowPhaseIdx = pu.phase_pos[flowPhaseIdx];
                    const int ebosPhaseIdx = flowPhaseToEbosPhaseIdx(canonicalFlowPhaseIdx);
                    const double refDens = FluidSystem::referenceDensity(ebosPhaseIdx, pvtRegionIdx);
                    cellRes[ flowPhaseToEbosCompIdx( flowPhaseIdx ) ] /= refDens;
                    cellRes[ flowPhaseToEbosCompIdx( flowPhaseIdx ) ] *= cellVolume;
                }
                if (has_solvent_) {
                    // no need to store refDens for all cells?
                    const auto& intQuants = ebosSimulator_.model().cachedIntensiveQuantities(cellIdx, /*timeIdx=*/0);
                    const auto& refDens = intQuants->solventRefDensity();
                    cellRes[ contiSolventEqIdx ] /= refDens;
                    cellRes[ contiSolventEqIdx ] *= cellVolume;
                }
                if (has_polymer_) {
                    cellRes[ contiPolymerEqIdx ] *= cellVolume;
                }
            }

            for( auto row = ebosJac.begin(); row != endrow; ++row )
            {
                const int rowIdx = row.index();
                const double cellVolume = ebosSimulator_.model().dofTotalVolume(rowIdx);
                unsigned pvtRegionIdx = ebosSimulator_.problem().pvtRegionIndex(rowIdx);

                // translate the Jacobian of the residual from the format used by ebos to
                // the one expected by flow
                const auto endcol = row->end();
                for( auto col = row->begin(); col != endcol; ++col )
                {
                    for( int flowPhaseIdx = 0; flowPhaseIdx < numFlowPhases; ++flowPhaseIdx )
                    {
                        const int canonicalFlowPhaseIdx = pu.phase_pos[flowPhaseIdx];
                        const int ebosPhaseIdx = flowPhaseToEbosPhaseIdx(canonicalFlowPhaseIdx);
                        const int ebosCompIdx = flowPhaseToEbosCompIdx(canonicalFlowPhaseIdx);
                        const double refDens = FluidSystem::referenceDensity(ebosPhaseIdx, pvtRegionIdx);
                        for( int pvIdx = 0; pvIdx < numEq; ++pvIdx )
                        {
                            (*col)[ebosCompIdx][flowToEbosPvIdx(pvIdx)] /= refDens;
                            (*col)[ebosCompIdx][flowToEbosPvIdx(pvIdx)] *= cellVolume;
                        }
                    }
                    if (has_solvent_) {
                        // TODO store refDens pr pvtRegion?
                        const auto& intQuants = ebosSimulator_.model().cachedIntensiveQuantities(rowIdx, /*timeIdx=*/0);
                        const auto& refDens = intQuants->solventRefDensity();
                        for( int pvIdx=0; pvIdx < numEq; ++pvIdx )
                        {
                            (*col)[contiSolventEqIdx][flowToEbosPvIdx(pvIdx)] /= refDens;
                            (*col)[contiSolventEqIdx][flowToEbosPvIdx(pvIdx)] *= cellVolume;
                        }
                    }
                    if (has_polymer_) {
                        for( int pvIdx=0; pvIdx < numEq; ++pvIdx )
                        {
                            (*col)[contiPolymerEqIdx][flowToEbosPvIdx(pvIdx)] *= cellVolume;
                        }
                    }

                }
            }
        }

        int flowPhaseToEbosPhaseIdx( const int phaseIdx ) const
        {
            assert(phaseIdx < 3);
            const int flowToEbos[ 3 ] = { FluidSystem::waterPhaseIdx, FluidSystem::oilPhaseIdx, FluidSystem::gasPhaseIdx};
            return flowToEbos[ phaseIdx ];
        }


        void updateRateConverter(const ReservoirState& reservoir_state)
        {
            const int nw = numWells();
            int global_number_wells = nw;

#if HAVE_MPI
            if ( istlSolver_->parallelInformation().type() == typeid(ParallelISTLInformation) )
            {
                const auto& info =
                    boost::any_cast<const ParallelISTLInformation&>(istlSolver_->parallelInformation());
                global_number_wells = info.communicator().sum(global_number_wells);
                if ( global_number_wells )
                {
                    rate_converter_.defineState(reservoir_state, boost::any_cast<const ParallelISTLInformation&>(istlSolver_->parallelInformation()));
                }
            }
            else
#endif
            {
                if ( global_number_wells )
                {
                    rate_converter_.defineState(reservoir_state);
                }
            }
        }


    public:
        void beginReportStep()
        {
            isBeginReportStep_ = true;
        }

        void endReportStep()
        {
            ebosSimulator_.problem().endEpisode();
        }

    private:
        void assembleMassBalanceEq(const SimulatorTimerInterface& timer,
                                   const int iterationIdx,
                                   const ReservoirState& reservoirState)
        {
            ebosSimulator_.startNextEpisode( timer.currentStepLength() );
            ebosSimulator_.setEpisodeIndex( timer.reportStepNum() );
            ebosSimulator_.setTimeStepIndex( timer.reportStepNum() );
            ebosSimulator_.model().newtonMethod().setIterationIndex(iterationIdx);

            static int prevEpisodeIdx = 10000;

            // notify ebos about the end of the previous episode and time step if applicable
            if (isBeginReportStep_) {
                isBeginReportStep_ = false;
                ebosSimulator_.problem().beginEpisode();
            }

            // doing the notifactions here is conceptually wrong and also causes the
            // endTimeStep() and endEpisode() methods to be not called for the
            // simulation's last time step and episode.
            if (ebosSimulator_.model().newtonMethod().numIterations() == 0
                && prevEpisodeIdx < timer.reportStepNum())
            {
                ebosSimulator_.problem().endTimeStep();
            }

            ebosSimulator_.setTimeStepSize( timer.currentStepLength() );
            if (ebosSimulator_.model().newtonMethod().numIterations() == 0)
            {
                ebosSimulator_.problem().beginTimeStep();
            }
            // if the last time step failed we need to update the solution varables in ebos
            // and recalculate the IntesiveQuantities. Also pass the solution initially.
            if ( (timer.lastStepFailed() || timer.reportStepNum()==0) && iterationIdx == 0  ) {
                convertInput( iterationIdx, reservoirState, ebosSimulator_ );
                ebosSimulator_.model().invalidateIntensiveQuantitiesCache(/*timeIdx=*/0);
            }

            ebosSimulator_.problem().beginIteration();
            ebosSimulator_.model().linearizer().linearize();
            ebosSimulator_.problem().endIteration();

            prevEpisodeIdx = ebosSimulator_.episodeIndex();

            auto& ebosJac = ebosSimulator_.model().linearizer().matrix();
            auto& ebosResid = ebosSimulator_.model().linearizer().residual();
            //Dune::printmatrix(std::cout, ebosJac, "J ebos", "row");
            convertResults(ebosResid, ebosJac);
            //std::cout << " ----------------------------------------------------" << std::endl<< std::endl<< std::endl;
            //Dune::printmatrix(std::cout, ebosJac, "J flow", "row");
            //std::cout << " ----------------------------------------------------" << std::endl<< std::endl<< std::endl;
            if (param_.update_equations_scaling_) {
                std::cout << "equation scaling not suported yet" << std::endl;
                //updateEquationsScaling();
            }
            const auto& ebosJacConst = ebosSimulator_.model().linearizer().matrix();
            auto& A = ebosSimulator_.model().linearizer().matrixA();
            A = ebosJacConst;
        }

        double dpMaxRel() const { return param_.dp_max_rel_; }
        double dsMax() const { return param_.ds_max_; }
        double drMaxRel() const { return param_.dr_max_rel_; }
        double maxResidualAllowed() const { return param_.max_residual_allowed_; }

    public:
        bool isBeginReportStep_;
    };
} // namespace Opm

#endif // OPM_BLACKOILMODELBASE_IMPL_HEADER_INCLUDED
