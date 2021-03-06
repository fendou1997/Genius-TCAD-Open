/********************************************************************************/
/*     888888    888888888   88     888  88888   888      888    88888888       */
/*   8       8   8           8 8     8     8      8        8    8               */
/*  8            8           8  8    8     8      8        8    8               */
/*  8            888888888   8   8   8     8      8        8     8888888        */
/*  8      8888  8           8    8  8     8      8        8            8       */
/*   8       8   8           8     8 8     8      8        8            8       */
/*     888888    888888888  888     88   88888     88888888     88888888        */
/*                                                                              */
/*       A Three-Dimensional General Purpose Semiconductor Simulator.           */
/*                                                                              */
/*                                                                              */
/*  Copyright (C) 2007-2008                                                     */
/*  Cogenda Pte Ltd                                                             */
/*                                                                              */
/*  Please contact Cogenda Pte Ltd for license information                      */
/*                                                                              */
/*  Author: Gong Ding   gdiso@ustc.edu                                          */
/*                                                                              */
/********************************************************************************/


#include "elem.h"
#include "simulation_system.h"
#include "resistance_region.h"
#include "solver_specify.h"
#include "boundary_info.h"


using PhysicalUnit::kb;
using PhysicalUnit::e;
using PhysicalUnit::ns;

#define DEBUG

///////////////////////////////////////////////////////////////////////
//----------------Function and Jacobian evaluate---------------------//
///////////////////////////////////////////////////////////////////////

// solve current continuation equation div(J)=0 where J=sigma*E and E=-grad(phi)


/*---------------------------------------------------------------------
 * fill solution vector with value.
 */
void MetalSimulationRegion::DDM1_Fill_Value(Vec x, Vec L)
{
  std::vector<int> ix;
  std::vector<PetscScalar> y;
  std::vector<PetscScalar> s;

  ix.reserve(this->n_node());
  y.reserve(this->n_node());
  s.reserve(this->n_node());

  const double sigma = this->get_conductance();

  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;
    const FVM_NodeData * node_data = fvm_node->node_data();

    ix.push_back(fvm_node->global_offset());
    y.push_back(node_data->psi());
    s.push_back(1.0/(sigma*fvm_node->volume()));
  }

  if( ix.size() )
  {
    VecSetValues(x, ix.size(), &ix[0], &y[0], INSERT_VALUES) ;
    VecSetValues(L, ix.size(), &ix[0], &s[0], INSERT_VALUES) ;
  }
}



/*---------------------------------------------------------------------
 * build function and its jacobian for DDML1 solver
 */
void MetalSimulationRegion::DDM1_Function(PetscScalar * x, Vec f, InsertMode &add_value_flag)
{

  // note, we will use ADD_VALUES to set values of vec f
  // if the previous operator is not ADD_VALUES, we should assembly the vec
  if( (add_value_flag != ADD_VALUES) && (add_value_flag != NOT_SET_VALUES) )
  {
    VecAssemblyBegin(f);
    VecAssemblyEnd(f);
  }

  // set local buf here
  std::vector<int>          iy;
  std::vector<PetscScalar>  y;

  iy.reserve(2*n_edge());
  y.reserve(2*n_edge());
  
  const PetscScalar T   = T_external();
  

  // search all the edges of this region, do integral over control volume...
  const_edge_iterator it = edges_begin();
  const_edge_iterator it_end = edges_end();
  for(; it!=it_end; ++it)
  {
    // fvm_node of node1
    const FVM_Node * fvm_n1 = (*it).first;
    // fvm_node of node2
    const FVM_Node * fvm_n2 = (*it).second;

    // fvm_node_data of node1
    const FVM_NodeData * n1_data =  fvm_n1->node_data();
    // fvm_node_data of node2
    const FVM_NodeData * n2_data =  fvm_n2->node_data();

    const unsigned int n1_local_offset = fvm_n1->local_offset();
    const unsigned int n2_local_offset = fvm_n2->local_offset();

    {
      // electrostatic potential, as independent variable
      PetscScalar V1   =  x[n1_local_offset];
      PetscScalar V2   =  x[n2_local_offset];
      PetscScalar E    = (V2-V1)/fvm_n1->distance(fvm_n2);

      // truncated to positive
      double S = std::abs(fvm_n1->cv_surface_area(fvm_n2));

      // "flux" from node 2 to node 1
      PetscScalar f = mt->basic->CurrentDensity(E, T)*S;

      // ignore thoese ghost nodes
      if( fvm_n1->on_processor() )
      {
        iy.push_back(fvm_n1->global_offset());
        y.push_back(f);
      }

      if( fvm_n2->on_processor() )
      {
        iy.push_back(fvm_n2->global_offset());
        y.push_back(-f);
      }
    }
  }


  double cap = _aux_capacitance/(_total_nodes_in_connected_resistance_region+1);
  double res = _aux_resistance*(_total_nodes_in_connected_resistance_region+1);

  // process aux cap/res
  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;
    const FVM_NodeData * fvm_node_data = fvm_node->node_data();

    PetscScalar V   =  x[fvm_node->local_offset()];
    PetscScalar current = -cap*(V-fvm_node_data->psi())/SolverSpecify::dt - (V-fvm_node_data->psi())/res;

    iy.push_back(fvm_node->global_offset());                  // save index in the buffer
    y.push_back( current);                                    // save value in the buffer
  }


  if(iy.size()) VecSetValues(f, iy.size(), &iy[0], &y[0], ADD_VALUES);

  // after the first scan, every nodes are updated.
  // however, boundary condition should be processed later.

  // the last operator is ADD_VALUES
  add_value_flag = ADD_VALUES;

}






/*---------------------------------------------------------------------
 * build function and its jacobian for DDML1 solver
 */
void MetalSimulationRegion::DDM1_Jacobian(PetscScalar * x, SparseMatrix<PetscScalar> *jac, InsertMode &add_value_flag)
{

  //the indepedent variable number, since we only process edges, 2 is enough
  adtl::AutoDScalar::numdir=2;

  //synchronize with material database
  mt->set_ad_num(adtl::AutoDScalar::numdir);

  // note, we will use ADD_VALUES to set values of matrix J

  const PetscScalar T   = T_external();

  // search all the edges of this region, do integral over control volume...
  const_edge_iterator it = edges_begin();
  const_edge_iterator it_end = edges_end();
  for(; it!=it_end; ++it)
  {
    // fvm_node of node1
    const FVM_Node * fvm_n1 = (*it).first;
    // fvm_node of node2
    const FVM_Node * fvm_n2 = (*it).second;

    // fvm_node_data of node1
    const FVM_NodeData * n1_data =  fvm_n1->node_data();
    // fvm_node_data of node2
    const FVM_NodeData * n2_data =  fvm_n2->node_data();

    const unsigned int n1_local_offset = fvm_n1->local_offset();
    const unsigned int n2_local_offset = fvm_n2->local_offset();

    // the row/colume position of variables in the matrix
    PetscInt row[2],col[2];
    row[0] = col[0] = fvm_n1->global_offset();
    row[1] = col[1] = fvm_n2->global_offset();

    // here we use AD, however it is great overkill for such a simple problem.
    {
      // electrostatic potential, as independent variable
      AutoDScalar V1   =  x[n1_local_offset];   V1.setADValue(0,1.0);
      AutoDScalar V2   =  x[n2_local_offset];   V2.setADValue(1,1.0);
      AutoDScalar E    = (V2-V1)/fvm_n1->distance(fvm_n2);

      // truncated to positive
      double S = std::abs(fvm_n1->cv_surface_area(fvm_n2));
      AutoDScalar f = mt->basic->CurrentDensity(E, T)*S;

      // ignore thoese ghost nodes

      if( fvm_n1->on_processor() )
      {
        jac->add_row(  row[0],  2,  &col[0],  f.getADValue() );
      }

      if( fvm_n2->on_processor() )
      {
        jac->add_row(  row[1],  2,  &col[0],  (-f).getADValue() );
      }
    }
  }


  adtl::AutoDScalar::numdir=1;
  //synchronize with material database
  mt->set_ad_num(adtl::AutoDScalar::numdir);


  double cap = _aux_capacitance/(_total_nodes_in_connected_resistance_region+1);
  double res = _aux_resistance*(_total_nodes_in_connected_resistance_region+1);

  // process aux cap/res
  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;
    const FVM_NodeData * fvm_node_data = fvm_node->node_data();

    AutoDScalar V   =  x[fvm_node->local_offset()];  V.setADValue(0, 1.0);
    AutoDScalar current = -cap*(V-fvm_node_data->psi())/SolverSpecify::dt - (V-fvm_node_data->psi())/res;

    jac->add( fvm_node->global_offset(),  fvm_node->global_offset(),  current.getADValue(0) );
  }

  // boundary condition should be processed later!

  // the last operator is ADD_VALUES
  add_value_flag = ADD_VALUES;

}



void MetalSimulationRegion::DDM1_Pseudo_Time_Step_Function(PetscScalar * x, Vec f, InsertMode &add_value_flag)
{
  // note, we will use ADD_VALUES to set values of vec f
  // if the previous operator is not ADD_VALUES, we should assembly the vec first!
  if( (add_value_flag != ADD_VALUES) && (add_value_flag != NOT_SET_VALUES) )
  {
    VecAssemblyBegin(f);
    VecAssemblyEnd(f);
  }

  if(this->connect_to_low_resistance_solderpad()) return;

  // we need a capacitor here for pseudo time step smoother.
  const double sigma = this->get_conductance();

  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;

    // C/T = \sigma*L, set T to 0.1ns
    const double cap = sigma*pow(fvm_node->volume()*_z_width, 1.0/3.0)*SolverSpecify::PseudoTimeCMOSTime/_z_width/this->n_node();

    const FVM_NodeData * node_data = fvm_node->node_data();
    const unsigned int local_offset  = fvm_node->local_offset();
    const unsigned int global_offset = fvm_node->global_offset();

    PetscScalar V   =  x[local_offset];                         // electrostatic potential
    PetscScalar f_V = -cap*(V-node_data->psi())/SolverSpecify::PseudoTimeStepMetal;

    VecSetValue(f, global_offset, f_V, ADD_VALUES);
  }

  // the last operator is ADD_VALUES
  add_value_flag = ADD_VALUES;
}


void MetalSimulationRegion::DDM1_Pseudo_Time_Step_Jacobian(PetscScalar * x, SparseMatrix<PetscScalar> *jac, InsertMode &add_value_flag)
{

  if(this->connect_to_low_resistance_solderpad()) return;

  //the indepedent variable number, 1 for each node
  adtl::AutoDScalar::numdir = 1;
  //synchronize with material database
  mt->set_ad_num(adtl::AutoDScalar::numdir);

  // we need a capacitor here for pseudo time step smoother.
  const double sigma = this->get_conductance();

  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;

    // C/T = \sigma*L, set T to 0.1ns
    const double cap = sigma*pow(fvm_node->volume()*_z_width, 1.0/3.0)*SolverSpecify::PseudoTimeCMOSTime/_z_width/this->n_node();

    const FVM_NodeData * node_data = fvm_node->node_data();
    const unsigned int local_offset  = fvm_node->local_offset();
    const unsigned int global_offset = fvm_node->global_offset();

    AutoDScalar V(x[local_offset]);   V.setADValue(0, 1.0);              // psi
    AutoDScalar f_V = -cap*(V-node_data->psi())/SolverSpecify::PseudoTimeStepMetal;

    jac->add( global_offset,  global_offset,  f_V.getADValue(0) );
  }

  // the last operator is ADD_VALUES
  add_value_flag = ADD_VALUES;
}



int MetalSimulationRegion::DDM1_Pseudo_Time_Step_Convergence_Test(PetscScalar * x)
{

  if(this->connect_to_low_resistance_solderpad()) return 0;

  unsigned int unconvergend = 0;

  const double sigma = this->get_conductance();
  const_processor_node_iterator node_it = on_processor_nodes_begin();
  const_processor_node_iterator node_it_end = on_processor_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    const FVM_Node * fvm_node = *node_it;

    // C/T = \sigma*L, set T to 0.1ns
    const double cap = sigma*pow(fvm_node->volume()*_z_width, 1.0/3.0)*SolverSpecify::PseudoTimeCMOSTime/_z_width/this->n_node();
    const FVM_NodeData * node_data = fvm_node->node_data();
    const unsigned int local_offset  = fvm_node->local_offset();

    PetscScalar V   =  x[local_offset];                         // electrostatic potential
    PetscScalar fV_abs = std::abs(-cap*(V-node_data->psi())/SolverSpecify::PseudoTimeStepMetal);
    PetscScalar V_rel  = std::abs(cap*(V-node_data->psi()))/(std::abs(V) + std::abs(node_data->psi()) + 1e-10);
    if( fV_abs > SolverSpecify::PseudoTimeTolRelax*0.5*(SolverSpecify::elec_continuity_abs_toler+SolverSpecify::hole_continuity_abs_toler) &&
        V_rel > SolverSpecify::relative_toler)
    {
      unconvergend++;
    }
  }

  return unconvergend;
}



void MetalSimulationRegion::DDM1_Update_Solution(PetscScalar *lxx)
{

  local_node_iterator node_it = on_local_nodes_begin();
  local_node_iterator node_it_end = on_local_nodes_end();
  for(; node_it!=node_it_end; ++node_it)
  {
    FVM_Node * fvm_node = *node_it;
    FVM_NodeData * node_data = fvm_node->node_data();

    //update psi
    node_data->psi_last() =  node_data->psi();
    node_data->psi() = lxx[fvm_node->local_offset()];
  }

  // addtional work: compute electrical field for all the node.
  // however, the electrical field is always zero. We needn't do anything here.

}





