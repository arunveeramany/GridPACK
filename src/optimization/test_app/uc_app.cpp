/*
 *     Copyright (c) 2013 Battelle Memorial Institute
 *     Licensed under modified BSD License. A copy of this license can be found
 *     in the LICENSE file in the top level directory of this distribution.
 */
// -------------------------------------------------------------
/**
 * @file   uc_app.cpp
 * @author 
 * @date   
 * 
 * @brief  
 * 
 * 
 */
// -------------------------------------------------------------

#include <iostream>
#include <vector>
#include <cstring>
#include <stdio.h>
#include <fstream>
#include <sstream>
#include <string>

#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp> // needed of is_any_of()
#include "uc_optimizer.hpp"
#include "uc_factory.hpp"
#include "uc_app.hpp"
#include "boost/smart_ptr/shared_ptr.hpp"
#include "gridpack/parser/PTI23_parser.hpp"
#include "gridpack/parser/hash_distr.hpp"
#include "mpi.h"
#include <ilcplex/ilocplex.h>
#include <stdlib.h>



// Calling program for unit commitment optimization application

/**
 * Basic constructor
 */
gridpack::unit_commitment::UCApp::UCApp(void)
{
}

/**
 * Basic destructor
 */
gridpack::unit_commitment::UCApp::~UCApp(void)
{
}

/**
 * Get time series data for loads and reserves and move them to individual
 * buses
 * @param filename name of file containing load and reserves time series
 * data
 */
void gridpack::unit_commitment::UCApp::getLoadsAndReserves(const char* filename)
{
  int me = p_network->communicator().rank();
  std::ifstream input;
  int nvals = 0;
  int i;
  gridpack::utility::StringUtils util;
  if (me == 0) {
    input.open(filename);
    if (!input.is_open()) {
      char buf[512];
      sprintf(buf,"Failed to open file with load and reserve time series data: %s\n\n",
          filename);
      throw gridpack::Exception(buf);
    }
    std::string line;
    std::getline(input,line);
    std::vector<std::string>  split_line;
    // tokenize the first line and find out how many values are in time series
    // data
    boost::algorithm::split(split_line, line, boost::algorithm::is_any_of(","),
        boost::token_compress_on);
    nvals = split_line.size();
    nvals = (nvals-1)/2;
  }
  p_network->communicator().sum(&nvals,1);
  const int nsize = 2*nvals*sizeof(double);
  typedef char[nsize] ts_data;
  std::vector<ts_data> series; 
  std::vector<int> bus_ids;
  double *lptr, *rptr;
  // read in times series values on each of the buses
  if (me == 0) {
    while (std::getline(input, line)) {
      ts_data data;
      lptr = static_cast<double*>(data);
      rptr = lptr + nvals;
      boost::algorithm::split(split_line, line, boost::algorithm::is_any_of(","),
          boost::token_compress_on);
      bus_ids.push_back(atoi(split_line[0].c_str()));
      for (i=0; i<nvals; i++) {
        lptr[i] = atof(split line[2*i+1].c_str());
        rptr[i] = atof(split line[2*i+2].c_str());
      }
      series.push_back(data);
    }
    input.close();
  }
  // distribute data from process 0 to the processes that have the corresponding
  // buses
  gridpack::hash_distr::HashDistribution<p_network,ts_data,ts_data> hash;
  hash.distributeBusValues(bus_ids; series);
  nvals = size.bus_ids;
  for (i=0; i<nvals; i++) {
    gridpack::unit_commitment::UCBus *bus;
    bus = dynamic_cast<gridpack::unit_commitment::UCBus*>(
        p_network->getBus(bus_ids[i]).get());
    lptr = static_cast<double*>(series[i]);
    rptr = lptr + nvals;
    bus->setTimeSeries(series[i].load,series[i].reserve);
  }
}

/**
 * Execute application
 * @param argc number of arguments
 * @param argv list of character strings
 */
typedef IloArray<IloIntVarArray> IntArray2;
typedef IloArray<IloNumVarArray> NumArray2;

void gridpack::unit_commitment::UCApp::execute(int argc, char** argv)
{
  // load input file
  gridpack::parallel::Communicator world;
  p_network.reset(new UCNetwork(world));

  // read configuration file
  std::string filename = "uc_test.raw";

  // Read in external PTI file with network configuration
  gridpack::parser::PTI23_parser<UCNetwork> parser(p_network);
  parser.parse(filename.c_str());

  // partition network
  p_network->partition();

  // load uc parameters
  filename = "gen.uc";
//  if (filename.size() > 0) parser.parse(filename.c_str());
//  parser.parse(filename.c_str());
//#if 0
  parser.externalParse(filename.c_str());
//#endif


  // create factory
  gridpack::unit_commitment::UCFactory factory(p_network);
  factory.load();

  // create optimization object
  gridpack::unit_commitment::UCoptimizer optim(p_network);
  // get uc parameters
  optim.getUCparam();
  int idx = 5644;
  double dlo;
  double dhi;
  optim.optVariableBounds(idx, &dlo, &dhi);
  printf("lo-hi %f %f\n",dlo,dhi);

  // prepare for optimization
  double rval;
  int ival;
  int me = MPI::COMM_WORLD.Get_rank();

  // create lp env
  IloEnv env;
  if(me == 0) {
  //
  // read demands and reserve from an input file
  //
    std::vector<std::vector<double> > loads;
    std::ifstream fin("loads.txt");
    std::string line;
    while (std::getline(fin, line)) {
      std::vector<double> lineData;           // create a new row
      double val;
      std::istringstream lineStream(line); 
      while (lineStream >> val) {          // for each value in line
        lineData.push_back(val);           // add to the current row
      }
      loads.push_back(lineData);         // add row to loads
    }
    fin.close();
    int size = loads.size();
//    const IloInt numHorizons = size;
    int totalV = optim.numOptVariables();
    const IloInt numHorizons = optim.totalHorizons;
//    const IloInt numHorizons = 2;
    const IloInt numUnits = optim.totalGen;
//
// create array on each process to store results from optimization
    int arr_size = numUnits*numHorizons;
    double *sol_power = new double[arr_size] ();
    int *sol_onOff = new int[arr_size] ();
//   
    IloNumArray minPower(env);
    IloNumArray maxPower(env);
    IloNumArray minUpTime(env);
    IloNumArray minDownTime(env);
    IloNumArray costConst(env);
    IloNumArray costLinear(env);
    IloNumArray costQuad(env);
    IloNumArray demand(env);
    IloNumArray reserve(env);
    IloNumArray iniLevel(env);
    IloNumArray shutCap(env);
    IloNumArray startCap(env);
    IloNumArray startUp(env);
    IloNumArray rampUp(env);
    IloNumArray rampDown(env);
    IloNumArray initPeriod(env) ;
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_minPower[i];
      minPower.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_maxPower[i];
      maxPower.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_minUpTime[i];
      minUpTime.add(ival);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_minDownTime[i];
      minDownTime.add(ival);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_costConst[i];
      costConst.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_costLinear[i];
      costLinear.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_costQuad[i];
      costQuad.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_iniLevel[i];
      iniLevel.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_startUp[i];
      startUp.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_startCap[i];
      startCap.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_shutCap[i];
      shutCap.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_rampUp[i];
      rampUp.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_rampDown[i];
      rampDown.add(rval);
    }
    for (int i = 0; i < numUnits; i++) {
      rval = optim.uc_initPeriod[i];
      initPeriod.add(rval);
    }
    for (int i = 0; i < numHorizons; i++) {
      rval = loads[i][0];
      demand.add(rval);
    }
    for (int i = 0; i < numHorizons; i++) {
      rval = loads[i][1];
      reserve.add(rval);
    }
    // Parameters for cost function
    // Variable arrays
    IntArray2 onOff;
    IntArray2 start_Up;
    IntArray2 shutDown;
    NumArray2 powerProduced;
    NumArray2 powerReserved;
    onOff = IntArray2(env,numHorizons);
    for (IloInt p = 0; p < numHorizons; p++) {
      onOff[p] = IloIntVarArray(env, numUnits,0,1);
    }
    start_Up = IntArray2(env,numHorizons);
    for (IloInt p = 0; p < numHorizons; p++) {
      start_Up[p] = IloIntVarArray(env, numUnits,0,1);
    }
    shutDown = IntArray2(env,numHorizons);
    for (IloInt p = 0; p < numHorizons; p++) {
      shutDown[p] = IloIntVarArray(env, numUnits,0,1);
    }
    powerProduced = NumArray2(env,numHorizons);
    for (IloInt p = 0; p < numHorizons; p++) {
      powerProduced[p] = IloNumVarArray(env, 0.0,maxPower,ILOFLOAT);
    }
    powerReserved = NumArray2(env,numHorizons);
    for (IloInt p = 0; p < numHorizons; p++) {
      powerReserved[p] = IloNumVarArray(env, 0.0,maxPower,ILOFLOAT);
    }
//  Objective
    IloModel ucmdl(env);
    IloExpr obj(env);
    for (IloInt p = 1; p < numHorizons; p++) {
      for (IloInt i = 0; i < numUnits; i++) {
         obj += costConst[i]*onOff[p][i]
              + startUp[i]*start_Up[p][i]
              + costLinear[i]*powerProduced[p][i]
              + costQuad[i]*powerProduced[p][i]*powerProduced[p][i];
      }
    }

    ucmdl.add(IloMinimize(env,obj));
    obj.end();
//
//  Constraints
//  
//  Initial state, treat as constraint
    for(int i=0; i< numUnits; i++) {
      ucmdl.add(onOff[0][i] == 1);
      ucmdl.add(start_Up[0][i] == 0);
      ucmdl.add(shutDown[0][i] == 0);
      ucmdl.add(powerProduced[0][i] == iniLevel[i]);
    }
    IloInt upDnPeriod;
    for (IloInt p = 1; p < numHorizons; p++) {
      IloExpr expr3(env);
      for (IloInt i = 0; i < numUnits; i++) {
         IloExpr expr1(env);
         IloExpr expr2(env);
         expr1 = powerProduced[p][i] - 10000*onOff[p][i];
         ucmdl.add( expr1 <= 0);
         expr2 = powerProduced[p][i] - minPower[i]*onOff[p][i];
         ucmdl.add( expr2 >= 0);
// ramp up constraint
         expr1 = powerProduced[p][i]+powerReserved[p][i]-powerProduced[p-1][i];
         ucmdl.add( expr1 <= rampUp[i]);
// ramp down constraint
         expr2 = powerProduced[p-1][i]-powerProduced[p][i];
         ucmdl.add( expr2 <= rampDown[i]);

         expr1.end();
         expr2.end();
// minium up and down time
// on at horizon p
         IloExpr upDnIndicator(env);
         upDnIndicator = onOff[p][i] - onOff[p-1][i];
         if(p == 1) {
          IloInt initP = (int)(initPeriod[i]);
          IloExpr upDnIndicator0(env);
//
          upDnIndicator0 = onOff[p-1][i]-onOff[p][i];
          upDnPeriod = std::min(numHorizons, (p+(int)(minUpTime[i]+0.5)-initP));
          for (IloInt j = p; j < upDnPeriod; j++) {
             ucmdl.add( upDnIndicator0 - 10000*onOff[j][i] <= 0);
          }
          upDnIndicator0.end();
         } else{
           upDnPeriod = std::min(numHorizons, (p+(int)(minUpTime[i]+0.5)));
          for (IloInt j = p; j < upDnPeriod; j++) {
           ucmdl.add( upDnIndicator - 10000*onOff[j][i] <= 0);
          }
         }
// start up, previous off
         ucmdl.add( upDnIndicator - 10000*start_Up[p][i] <= 0);
         upDnIndicator.end();
// off at horizon p
         upDnIndicator = 1 - (onOff[p-1][i] - onOff[p][i]);
         upDnPeriod = std::min(numHorizons, (p+(int)(minDownTime[i]+0.5)));
         for (IloInt j = p; j < upDnPeriod; j++) {
           ucmdl.add( upDnIndicator - 10000*onOff[j][i] <= 0);
         }
// shut down, previous on
         upDnIndicator = onOff[p-1][i] - onOff[p][i];
         ucmdl.add( upDnIndicator - 10000*shutDown[p][i] <= 0);
         upDnIndicator.end();
// generation limits
// startup at horizon p
         expr1 = powerProduced[p][i]-minPower[i]+powerReserved[p][i];
         expr2 = (maxPower[i]-minPower[i])*onOff[p][i]
               -(maxPower[i]-startCap[i])*start_Up[p][i];
         ucmdl.add( expr1 <= expr2);
         expr1.end();
         expr2.end();
// shutdown at horizon p 
         expr1 = powerProduced[p-1][i]-minPower[i]+powerReserved[p-1][i];
         expr2 = (maxPower[i]-minPower[i])*onOff[p-1][i]
               -(maxPower[i]-shutCap[i])*shutDown[p][i];
         ucmdl.add( expr1 <= expr2);
         expr1.end();
         expr2.end();
      }
      expr3 = IloSum(powerProduced[p]);
      ucmdl.add( expr3 == demand[p]);
      expr3.end();
      expr3 = IloSum(powerReserved[p]);
      ucmdl.add( expr3 >= reserve[p]);
//printf("reserve- %f\n",reserve[p]);
      expr3.end();
    }
// Solve model
    IloCplex cplex(env);
// setup parallel mode and number of threads
//    cplex.setParam(IloCplex::ParallelMode, 1);
//    cplex.setParam(IloCplex::Threads, 2);
    cplex.extract(ucmdl);
    cplex.exportModel("test_uc.lp");
    std::ofstream logfile("cplex.log");
    cplex.setOut(logfile);
    cplex.setWarning(logfile);
    cplex.solve();
    cplex.out() << "solution status = " << cplex.getStatus() << std::endl;
    cplex.out() << "cost   = " << cplex.getObjValue() << std::endl;
    int busid;
    for (IloInt i = 0; i < numUnits; i++) {
      busid = optim.busID[i];
      for (IloInt p = 0; p < numHorizons; p++) {
//#if 0
          env.out() << "At time " << p << " Power produced by unit " << i << " " << "on bus  " << busid << " " <<
          cplex.getValue(onOff[p][i]) << "  " <<
          cplex.getValue(start_Up[p][i]) << "  " <<
          cplex.getValue(shutDown[p][i]) << "  " <<
          cplex.getValue(powerReserved[p][i]) << "  " <<
          cplex.getValue(powerProduced[p][i]) << std::endl;
//#endif
#if 0
          rval = cplex.getValue(powerProduced[p][i]);
          if (!data->setValue("POWER_PRODUCED",rval,i)) {
            data->addValue("POWER_PRODUCED",rval,i);
          }
          rval = cplex.getValue(powerReserved[p][i]);
          if (!data->setValue("POWER_RESERVED",rval,i)) {
            data->addValue("POWER_RESERVED",rval,i);
          }
#endif
      }
    }

  }
  env.end();
} 
