#include <array>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include "random.hpp"
#include <vector>
#include "fastscape_RB+P.hpp"


///Initializing code
FastScape_RBP::FastScape_RBP(const int width0, const int height0)
  //Initialize code for finding neighbours of a cell
  : nshift{-1,-width0-1,-width0,-width0+1,1,width0+1,width0,width0-1}
{
  Tmr_Overall.start();
  Tmr_Step1_Initialize.start();
  width  = width0;
  height = height0;
  size   = width*height;

  h.resize(size);   //Memory for terrain height

  Tmr_Step1_Initialize.stop();
  Tmr_Overall.stop();
}



///The receiver of a focal cell is the cell which receives the focal cells'
///flow. Here, we model the receiving cell as being the one connected to the
///focal cell by the steppest gradient. If there is no local gradient, than
///the special value NO_FLOW is assigned.
void FastScape_RBP::ComputeReceivers(){
  //Edge cells do not have receivers because they do not distribute their flow
  //to anywhere.
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width-2;x++){
    const int c      = y*width+x;

    //The slope must be greater than zero for there to be downhill flow;
    //otherwise, the cell is marked NO_FLOW.
    double max_slope = 0;        //Maximum slope seen so far amongst neighbours
    int    max_n     = NO_FLOW;  //Direction of neighbour which had maximum slope to focal cell

    //Loop over neighbours
    for(int n=0;n<8;n++){
      const double slope = (h[c] - h[c+nshift[n]])/dr[n]; //Slope to neighbour n
      if(slope>max_slope){    //Is this the steepest slope we've seen?
        max_slope = slope;    //If so, make a note of the slope
        max_n     = n;        //And which cell it came from
      }
    }
    rec[c] = max_n;           //Having considered all neighbours, this is the steepest
  }    
}



///The donors of a focal cell are the neighbours from which it receives flow.
///Here, we identify those neighbours by inverting the Receivers array.
void FastScape_RBP::ComputeDonors(){
  //Initially, we claim that each cell has no donors.
  for(int i=0;i<size;i++)
    ndon[i] = 0;

  //Looping across all cells
  for(int c=0;c<size;c++){
    if(rec[c]==NO_FLOW)
      continue;
    //If this cell passes flow to a downhill cell, make a note of it in that
    //downhill cell's donor array and increment its donor counter
    const auto n       = c+nshift[rec[c]];
    donor[8*n+ndon[n]] = c;
    ndon[n]++;
  }
}



///Cells must be ordered so that they can be traversed such that higher cells
///are processed before their lower neighbouring cells. This method creates
///such an order. It also produces a list of "levels": cells which are,
///topologically, neither higher nor lower than each other. Cells in the same
///level can all be processed simultaneously without having to worry about
///race conditions.
void FastScape_RBP::GenerateOrder(){
  int nstack = 0;    //Number of cells currently in the stack

  //Since each value of the `levels` array is later used as the starting value
  //of a for-loop, we include a zero at the beginning of the array.
  levels[0] = 0;
  nlevel    = 1;     //Note that array now contains a single value

  //Load cells without dependencies into the queue. This will include all of
  //the edge cells.
  for(int c=0;c<size;c++){
    if(rec[c]==NO_FLOW){
      stack[nstack++] = c;
      assert(nstack<stack_width);
    }
  }
  levels[nlevel++] = nstack; //Last cell of this level
  assert(nlevel<level_width); 

  //Start with level_bottom=-1 so we get into the loop, it is immediately
  //replaced by level_top.
  int level_bottom = -1;         //First cell of the current level
  int level_top    =  0;         //Last cell of the current level

  while(level_bottom<level_top){ //Enusre we parse all the cells
    level_bottom = level_top;    //The top of the previous level we considered is the bottom of the current level
    level_top    = nstack;       //The new top is the end of the stack (last cell added from the previous level)
    for(int si=level_bottom;si<level_top;si++){
      const auto c = stack[si];
      //Load donating neighbours of focal cell into the stack
      for(int k=0;k<ndon[c];k++){
        const auto n = donor[8*c+k];
        stack[nstack++] = n;
        assert(nstack<=stack_width);
      }
    }

    levels[nlevel++] = nstack; //Start a new level
  }

  //End condition for the loop places two identical entries
  //at the end of the stack. Remove one.
  nlevel--;

  assert(levels[nlevel-1]==nstack);
}



///Compute the flow accumulation for each cell: the number of cells whose flow
///ultimately passes through the focal cell multiplied by the area of each
///cell. Each cell could also have its own weighting based on, say, average
///rainfall.
void FastScape_RBP::ComputeFlowAcc(){
  //Initialize cell areas to their weights. Here, all the weights are the
  //same.
  for(int i=0;i<size;i++)
    accum[i] = cell_area;

  //Highly-elevated cells pass their flow to less elevated neighbour cells.
  //The queue is ordered so that higher cells are keyed to higher indices in
  //the queue; therefore, parsing the queue in reverse ensures that fluid
  //flows downhill.
  for(int s=size-1;s>=0;s--){
    const int c = stack[s];
    if(rec[c]!=NO_FLOW){
      const int n = c+nshift[rec[c]];
      accum[n]   += accum[c];
    }
  }    
}



///Raise each cell in the landscape by some amount, otherwise it wil get worn
///flat (in this model, with these settings)
void FastScape_RBP::AddUplift(){
  //We exclude two exterior rings of cells in this example. The outermost ring
  //(the edges of the dataset) allows us to ignore the edges of the dataset,
  //the second-most outer ring (the cells bordering the edge cells of the
  //dataset) are fixed to a specified height in this model. All other cells
  //have heights which actively change and they are altered here.
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width-2;x++){
    const int c = y*width+x;
    h[c] += ueq*dt;
  }
}



///Decrease he height of cells according to the stream power equation; that
///is, based on a constant K, flow accumulation A, the local slope between
///the cell and its receiving neighbour, and some judiciously-chosen constants
///m and n.
///    h_next = h_current - K*dt*(A^m)*(Slope)^n
///We solve this equation implicitly to preserve accuracy
void FastScape_RBP::Erode(){
  //The cells in each level can be processed in parallel, so we loop over
  //levels starting from the lower-most (the one closest to the NO_FLOW cells)

  //Level 0 contains all those cells which do not flow anywhere, so we skip it
  //since their elevations will not be changed via erosion anyway.
  for(int li=1;li<nlevel-1;li++){
    const int lvlstart = levels[li];      //Starting index of level in stack
    const int lvlend   = levels[li+1];    //Ending index of level in stack
    const int lvlsize  = lvlend-lvlstart; //Number of cells in the level

    //It's only worth parallelizing if there are enough cells in the level.
    //For small levels it is more efficient to run the code in serial. The if-
    //clause in the OpenMP directive below can be adjusted to a suitable value
    //to account for this.
    #pragma omp parallel for if(lvlsize>500)
    for(int si=lvlstart;si<lvlend;si++){
      const int c = stack[si];         //Cell from which flow originates
      if(rec[c]==NO_FLOW)              //Ignore cells with no receiving neighbour
        continue;
      const int n = c+nshift[rec[c]];  //Cell receiving the flow

      const double length = dr[rec[c]];
      //`fact` contains a set of values which are constant throughout the integration
      const double fact   = keq*dt*std::pow(accum[c],meq)/std::pow(length,neq);
      const double h0     = h[c];      //Elevation of focal cell
      const double hn     = h[n];      //Elevation of neighbouring (receiving, lower) cell
      double hnew         = h0;        //Current updated value of focal cell
      double hp           = h0;        //Previous updated value of focal cell
      double diff         = 2*tol;     //Difference between current and previous updated values
      while(std::abs(diff)>tol){       //Newton-Rhapson method (run until subsequent values differ by less than a tolerance, which can be set to any desired precision)
        hnew -= (hnew-h0+fact*std::pow(hnew-hn,neq))/(1.+fact*neq*std::pow(hnew-hn,neq-1));
        diff  = hnew - hp;             //Difference between previous and current value of the iteration
        hp    = hnew;                  //Update previous value to new value
      }
      h[c] = hnew;                     //Update value in array
    }
  }
}



///Run the model forward for a specified number of timesteps. No new
///initialization is done. This allows the model to be stopped, the terrain
///altered, and the model continued. For space-efficiency, a number of
///temporary arrays are created each time this is run, so repeatedly running
///this function for the same model will likely not be performant due to
///reallocations. If that is your use case, you'll want to modify your code
///appropriately.
void FastScape_RBP::run(const int nstep){
  Tmr_Overall.start();

  Tmr_Step1_Initialize.start();

  stack_width = size; //Number of stack entries available to each thread
  level_width = size; //Number of level entries available to each thread

  accum.resize(  size);  //Stores flow accumulation
  rec.resize  (  size);  //Array of Receiver directions
  ndon.resize (  size);  //Number of donors each cell has
  donor.resize(8*size);  //Array listing the donors of each cell (up to 8 for a rectangular grid)
  stack.resize(stack_width);  //Order in which to process cells

  //It's difficult to know how much memory should be allocated for levels. For
  //a square DEM with isotropic dispersion this is approximately sqrt(E/2). A
  //diagonally tilted surface with isotropic dispersion may have sqrt(E)
  //levels. A tortorously sinuous river may have up to E*E levels. We
  //compromise and choose a number of levels equal to the perimiter because
  //why not?
  levels.resize(2*width+2*height); 

  ///All receivers initially point to nowhere
  #pragma omp parallel for
  for(int i=0;i<size;i++)
    rec[i] = NO_FLOW;

  Tmr_Step1_Initialize.stop();

  for(int step=0;step<=nstep;step++){
    Tmr_Step2_DetermineReceivers.start ();   ComputeReceivers  (); Tmr_Step2_DetermineReceivers.stop ();
    Tmr_Step3_DetermineDonors.start    ();   ComputeDonors     (); Tmr_Step3_DetermineDonors.stop    ();
    Tmr_Step4_GenerateOrder.start      ();   GenerateOrder     (); Tmr_Step4_GenerateOrder.stop      ();
    Tmr_Step5_FlowAcc.start            ();   ComputeFlowAcc    (); Tmr_Step5_FlowAcc.stop            ();
    Tmr_Step6_Uplift.start             ();   AddUplift         (); Tmr_Step6_Uplift.stop             ();
    Tmr_Step7_Erosion.start            ();   Erode             (); Tmr_Step7_Erosion.stop            ();

    if( step%20==0 ) //Show progress
      std::cout<<"p Step = "<<step<<std::endl;
  }

  Tmr_Overall.stop();

  std::cout<<"t Step1: Initialize         = "<<std::setw(15)<<Tmr_Step1_Initialize.elapsed()         <<" microseconds"<<std::endl;                 
  std::cout<<"t Step2: DetermineReceivers = "<<std::setw(15)<<Tmr_Step2_DetermineReceivers.elapsed() <<" microseconds"<<std::endl;                         
  std::cout<<"t Step3: DetermineDonors    = "<<std::setw(15)<<Tmr_Step3_DetermineDonors.elapsed()    <<" microseconds"<<std::endl;                      
  std::cout<<"t Step4: GenerateOrder      = "<<std::setw(15)<<Tmr_Step4_GenerateOrder.elapsed()      <<" microseconds"<<std::endl;                    
  std::cout<<"t Step5: FlowAcc            = "<<std::setw(15)<<Tmr_Step5_FlowAcc.elapsed()            <<" microseconds"<<std::endl;              
  std::cout<<"t Step6: Uplift             = "<<std::setw(15)<<Tmr_Step6_Uplift.elapsed()             <<" microseconds"<<std::endl;             
  std::cout<<"t Step7: Erosion            = "<<std::setw(15)<<Tmr_Step7_Erosion.elapsed()            <<" microseconds"<<std::endl;              
  std::cout<<"t Overall                   = "<<std::setw(15)<<Tmr_Overall.elapsed()                  <<" microseconds"<<std::endl;        

  //Free up memory, except for the resulting landscape height field prior to
  //exiting so that unnecessary space is not used when the model is not being
  //run.
  accum .clear();   accum .shrink_to_fit();
  rec   .clear();   rec   .shrink_to_fit();
  ndon  .clear();   ndon  .shrink_to_fit();
  stack .clear();   stack .shrink_to_fit();
  donor .clear();   donor .shrink_to_fit();
  levels.clear();   levels.shrink_to_fit();
}



///Returns a pointer to the data so that it can be copied, printed, &c.
double* FastScape_RBP::getH() {
  return h.data();
}