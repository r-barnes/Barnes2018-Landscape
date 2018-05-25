#include <array>
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <omp.h>  //Used for OpenMP run-time functions
#include "random.hpp"
#include <vector>
#include "fastscape_RB+PQ.hpp"

//Used to handle situations in which OpenMP is not available
//(This scenario has not been extensively tested)
#ifndef _OPENMP
  #define omp_get_thread_num()  0
  #define omp_get_num_threads() 1
  #define omp_get_max_threads() 1
#endif



///Initializing code
FastScape_RBPQ::FastScape_RBPQ(const int width0, const int height0)
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
void FastScape_RBPQ::ComputeReceivers(){
  //Edge cells do not have receivers because they do not distribute their flow
  //to anywhere.

  #pragma omp for collapse(2)
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
void FastScape_RBPQ::ComputeDonors(){
  //The B&W method of developing the donor array has each focal cell F inform
  //its receiving cell R that F is a donor of R. Unfortunately, parallelizing
  //this is difficult because more than one cell might be informing R at any
  //given time. Atomics are a solution, but they impose a performance cost
  //(though using the latest and greatest hardware decreases this penalty).

  //Instead, we invert the operation. Each focal cell now examines its
  //neighbours to see if it receives from them. Each focal cell is then
  //guaranteed to have sole write-access to its location in the donor array.

  //Remember, the outermost ring of cells is a convenience halo, so we don't
  //calculate donors for it.

  #pragma omp for collapse(2) schedule(static) nowait
  for(int y=1;y<height-1;y++)
  for(int x=1;x<width-1;x++){
    const int c = y*width+x;
    ndon[c] = 0; //Cell has no donor neighbours we know about
    for(int ni=0;ni<8;ni++){
      const int n = c+nshift[ni];
      //If the neighbour has a receiving cell and that receiving cell is
      //the current focal cell c
      if(rec[n]!=NO_FLOW && n+nshift[rec[n]]==c){
        donor[8*c+ndon[c]] = n;
        ndon[c]++;
      }
    }
  }
}



///Cells must be ordered so that they can be traversed such that higher cells
///are processed before their lower neighbouring cells. This method creates
///such an order. It also produces a list of "levels": cells which are,
///topologically, neither higher nor lower than each other. Cells in the same
///level can all be processed simultaneously without having to worry about
///race conditions.
void FastScape_RBPQ::GenerateOrder(
  std::vector<int>& stack,
  std::vector<int>& levels,
  int &nlevel
){
  int nstack = 0;

  levels[0]  = 0;
  nlevel     = 1;

  //Outer edge
  #pragma omp for schedule(static) nowait
  for(int y=1;y<height-1;y++){
    stack[nstack++] = y*width+1;          assert(nstack<stack_width);
    stack[nstack++] = y*width+(width-2);  assert(nstack<stack_width);
  }

  #pragma omp for schedule(static) nowait
  for(int x=2;x<width-2;x++){
    stack[nstack++] =          1*width+x; assert(nstack<stack_width);
    stack[nstack++] = (height-2)*width+x; assert(nstack<stack_width);
  }

  //End of outer edge
  levels[nlevel++] = nstack; //Last cell of this level

  //Interior cells
  //TODO: Outside edge is always NO_FLOW. Maybe this can get loaded once?
  //Load cells without dependencies into the queue
  //TODO: Why can't I use nowait here?
  #pragma omp for collapse(2) schedule(static) 
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width -2;x++){
    const int c = y*width+x;
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

  #pragma omp barrier //Must ensure ComputeDonors is done before we start the following

  while(level_bottom<level_top){ //Ensure we parse all the cells
    level_bottom = level_top;    //The top of the previous level we considered is the bottom of the current level
    level_top    = nstack;       //The new top is the end of the stack (last cell added from the previous level)
    for(int si=level_bottom;si<level_top;si++){
      const auto c = stack[si];
      //Load donating neighbours of focal cell into the stack
      for(int k=0;k<ndon[c];k++){
        const auto n = donor[8*c+k];
        stack[nstack++] = n;              
        assert(nstack<stack_width);
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
void FastScape_RBPQ::ComputeFlowAcc(
  const std::vector<int>& stack,
  const std::vector<int>& levels,
  const int &nlevel
){
  for(int i=levels[0];i<levels[nlevel-1];i++){
    const int c = stack[i];
    accum[c] = cell_area;
  }

  //Highly-elevated cells pass their flow to less elevated neighbour cells.
  //The queue is ordered so that higher cells are keyed to higher indices in
  //the queue; therefore, parsing the queue in reverse ensures that fluid
  //flows downhill.

  //We can process the cells in each level in parallel. To prevent race
  //conditions, each focal cell figures out what contirbutions it receives
  //from its neighbours.

  //`nlevel-1` is the upper bound of the stack.
  //`nlevel-2` through `nlevel-1` are the cells which have no higher neighbours (top of the watershed)
  //`nlevel-3` through `nlevel-2` are the first set of cells with higher neighbours, so this is where we start
  for(int li=nlevel-3;li>=1;li--){
    const int lvlstart = levels[li];      //Starting index of level in stack
    const int lvlend   = levels[li+1];    //Ending index of level in stack
    for(int si=lvlstart;si<lvlend;si++){
      const int c = stack[si];
      for(int k=0;k<ndon[c];k++){
        const auto n = donor[8*c+k];
        accum[c]    += accum[n];
      }
    }
  }    
}



///Raise each cell in the landscape by some amount, otherwise it wil get worn
///flat (in this model, with these settings)
void FastScape_RBPQ::AddUplift(
  const std::vector<int>& stack,
  const std::vector<int>& levels,
  const int &nlevel
){
  //We exclude two exterior rings of cells in this example. The outermost ring
  //(the edges of the dataset) allows us to ignore the edges of the dataset,
  //the second-most outer ring (the cells bordering the edge cells of the
  //dataset) are fixed to a specified height in this model. All other cells
  //have heights which actively change and they are altered here.

  //Start at levels[1] so we don't elevate the outer edge
  #pragma omp simd
  for(int i=levels[1];i<levels[nlevel-1];i++){
    const int c = stack[i];
    h[c]       += ueq*dt; 
  }
}



///Decrease he height of cells according to the stream power equation; that
///is, based on a constant K, flow accumulation A, the local slope between
///the cell and its receiving neighbour, and some judiciously-chosen constants
///m and n.
///    h_next = h_current - K*dt*(A^m)*(Slope)^n
///We solve this equation implicitly to preserve accuracy
void FastScape_RBPQ::Erode(
  const std::vector<int>& stack,
  const std::vector<int>& levels,
  const int nlevel
){
  //The cells in each level can be processed in parallel, so we loop over
  //levels starting from the lower-most (the one closest to the NO_FLOW cells)

  //#pragma omp parallel default(none)
  for(int li=2;li<nlevel-1;li++){
    const int lvlstart = levels[li];
    const int lvlend   = levels[li+1];
    #pragma omp simd
    for(int si=lvlstart;si<lvlend;si++){
      const int c = stack[si];         //Cell from which flow originates
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
void FastScape_RBPQ::run(const int nstep){
  Tmr_Overall.start();

  Tmr_Step1_Initialize.start();

  //TODO: Make smaller, explain max
  stack_width = std::max(300000,5*size/omp_get_max_threads()); //Number of stack entries available to each thread
  level_width = std::max(1000,size/omp_get_max_threads());     //Number of level entries available to each thread

  accum.resize(  size);  //Stores flow accumulation
  rec.resize  (  size);  //Array of Receiver directions
  ndon.resize (  size);  //Number of donors each cell has
  donor.resize(8*size);  //Array listing the donors of each cell (up to 8 for a rectangular grid)

  //! initializing rec
  #pragma omp parallel for
  for(int i=0;i<size;i++)
    rec[i] = NO_FLOW;

  #pragma omp parallel for
  for(int i=0;i<size;i++)
    ndon[i] = 0;


  #pragma omp parallel
  {
    std::vector<int> stack(stack_width); //Indices of cells in the order they should be processed

    //A level is a set of cells which can all be processed simultaneously.
    //Topologically, cells within a level are neither descendents or ancestors
    //of each other in a topological sorting, but are the same number of steps
    //from the edge of the dataset.

    //It's difficult to know how much memory should be allocated for levels. For
    //a square DEM with isotropic dispersion this is approximately sqrt(E/2). A
    //diagonally tilted surface with isotropic dispersion may have sqrt(E)
    //levels. A tortorously sinuous river may have up to E*E levels. We
    //compromise and choose a number of levels equal to the perimiter because
    //why not?
    std::vector<int> levels(level_width);
    int  nlevel = 0;

    Tmr_Step1_Initialize.stop();

    for(int step=0;step<=nstep;step++){
      Tmr_Step2_DetermineReceivers.start ();   ComputeReceivers  ();                      Tmr_Step2_DetermineReceivers.stop ();
      Tmr_Step3_DetermineDonors.start    ();   ComputeDonors     ();                      Tmr_Step3_DetermineDonors.stop    ();
      Tmr_Step4_GenerateOrder.start      ();   GenerateOrder     (stack,levels,nlevel);   Tmr_Step4_GenerateOrder.stop      ();
      Tmr_Step5_FlowAcc.start            ();   ComputeFlowAcc    (stack,levels,nlevel);   Tmr_Step5_FlowAcc.stop            ();
      Tmr_Step6_Uplift.start             ();   AddUplift         (stack,levels,nlevel);   Tmr_Step6_Uplift.stop             ();
      Tmr_Step7_Erosion.start            ();   Erode             (stack,levels,nlevel);   Tmr_Step7_Erosion.stop            ();
      #pragma omp barrier //Ensure threads synchronize after erosion so we calculate receivers correctly

      #pragma omp master
      if( step%20==0 ) //Show progress
        std::cout<<"p Step = "<<step<<std::endl;
    }
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
  donor .clear();   donor .shrink_to_fit();
}



///Returns a pointer to the data so that it can be copied, printed, &c.
double* FastScape_RBPQ::getH() {
  return h.data();
}
