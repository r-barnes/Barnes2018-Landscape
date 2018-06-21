#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include "random.hpp"
#include <vector>
#include "fastscape_RB+GPU.hpp"

#ifndef _OPENMP
  #define omp_get_team_num() 0
#endif

///Initializing code
FastScape_RBGPU::FastScape_RBGPU(const int width0, const int height0)
  //Initialize code for finding neighbours of a cell
  : nshift{-1,-width0-1,-width0,-width0+1,1,width0+1,width0,width0-1}
{
  Tmr_Overall.start();
  Tmr_Step1_Initialize.start();
  width  = width0;
  height = height0;
  size   = width*height;

  h      = new double[size];

  Tmr_Step1_Initialize.stop();
  Tmr_Overall.stop();
}



FastScape_RBGPU::~FastScape_RBGPU(){
  delete[] h;
}


///The receiver of a focal cell is the cell which receives the focal cells'
///flow. Here, we model the receiving cell as being the one connected to the
///focal cell by the steppest gradient. If there is no local gradient, than
///the special value NO_FLOW is assigned.
void FastScape_RBGPU::ComputeReceivers(){
  //Edge cells do not have receivers because they do not distribute their flow
  //to anywhere.

  const int height = this->height;
  const int width  = this->width;

  #pragma omp target teams
  #pragma omp distribute parallel for simd collapse(2) default(none)
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width-2;x++){
    const int c      = y*width+x;

    //The slope must be greater than zero for there to be downhill flow;
    //otherwise, the cell is marked NO_FLOW.
    double max_slope = 0;        //Maximum slope seen so far amongst neighbours
    int    max_n     = NO_FLOW;  //Direction of neighbour which had maximum slope to focal cell

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
void FastScape_RBGPU::ComputeDonors(){
  const int height = this->height;
  const int width  = this->width;
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

  #pragma omp target teams
  #pragma omp distribute parallel for simd collapse(2) default(none)
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
void FastScape_RBGPU::GenerateOrder(){
//  #pragma omp target update from(rec[0:size],donor[0:8*size],ndon[0:size])
  const int height = this->height;
  const int width  = this->width;

  #pragma omp target teams num_teams(thread_count) thread_limit(1)
  {
    auto &tnlevel = nlevel[omp_get_team_num()];
    auto &tnstack = nstack[omp_get_team_num()];
    auto *tlevels = &levels[omp_get_team_num()*level_width];
    auto *tstack  = &stack [omp_get_team_num()*stack_width];

    //TODO: We may not need to do this
    #pragma omp distribute parallel for simd default(none)
    for(int i=0;i<thread_count*stack_width;i++)
      stack[i] = -1;
    #pragma omp distribute parallel for simd default(none)
    for(int i=0;i<thread_count*level_width;i++)
      levels[i] = -1;

    tlevels[0] = 0;
    tnlevel    = 1;
    tnstack    = 0;

    #pragma omp distribute
    for(int y=1;y<height-1;y++){
      tstack[tnstack++] = y*width+1;          
      tstack[tnstack++] = y*width+(width-2);  
    }

    #pragma omp distribute
    for(int x=2;x<width-2;x++){
      tstack[tnstack++] =          1*width+x; 
      tstack[tnstack++] = (height-2)*width+x; 
    }

    #pragma omp distribute collapse(2)
    for(int y=2;y<height-2;y++)
    for(int x=2;x<width -2;x++){
      const int c = y*width+x;
      if(rec[c]==NO_FLOW){
        tstack[tnstack++] = c;                
      }
    }

    tlevels[tnlevel++] = tnstack; //Last cell of this level

  //Interior cells
  //TODO: Outside edge is always NO_FLOW. Maybe this can get loaded once?
  //Load cells without dependencies into the queue
  //TODO: Why can't I use nowait here?



  // #pragma acc update host(stack[0:5*size],nstack[0:thread_count],levels[0:size],nlevel[0:thread_count])

  //Start with level_bottom=-1 so we get into the loop, it is immediately
  //replaced by level_top.
  // int level_bottom = -1;         //First cell of the current level
  // int level_top    =  0;         //Last cell of the current level


    int level_bottom  = -1;         //First cell of the current level
    int level_top     = 0;         //Last cell of the current level

    while(level_bottom<level_top){ //Ensure we parse all the cells
      level_bottom = level_top;    //The top of the previous level we considered is the bottom of the current level
      level_top    = tnstack;       //The new top is the end of the stack (last cell added from the previous level)
      for(int si=level_bottom;si<level_top;si++){
        const auto c = tstack[si];
        //Load donating neighbours of focal cell into the stack
        for(int k=0;k<ndon[c];k++){
          const auto n = donor[8*c+k];
          tstack[tnstack++] = n;
        }
      }

      tlevels[tnlevel++] = tnstack; //Start a new level
    }

    nlevel[omp_get_team_num()]--;
  }
}

///Compute the flow accumulation for each cell: the number of cells whose flow
///ultimately passes through the focal cell multiplied by the area of each
///cell. Each cell could also have its own weighting based on, say, average
///rainfall.
void FastScape_RBGPU::ComputeFlowAcc(){
  //Initialize cell areas to their weights. Here, all the weights are the
  //same.
  #pragma omp target teams
  #pragma omp distribute parallel for simd default(none)
  for(int i=0;i<size;i++)
    accum[i] = cell_area;

  //Highly-elevated cells pass their flow to less elevated neighbour cells.
  //The queue is ordered so that higher cells are keyed to higher indices in
  //the queue; therefore, parsing the queue in reverse ensures that fluid
  //flows downhill.

  //We can process the cells in each level in parallel. To prevent race
  //conditions, each focal cell figures out what contirbutions it receives
  //from its neighbours.

  //nlevel-1 to nlevel:   Doesn't exist, since nlevel is outside the bounds of level
  //nlevel-2 to nlevel-1: Uppermost heights
  //nlevel-3 to nlevel-2: Region just below the uppermost heights
  // #pragma acc parallel default(none) present(this,accum,levels,nshift,rec,stack)
  #pragma omp target teams num_teams(thread_count)
  {
    auto &tnlevel = nlevel[omp_get_team_num()];
    auto *tlevels = &levels[omp_get_team_num()*level_width];
    auto *tstack  = &stack [omp_get_team_num()*stack_width];
    for(int li=tnlevel-3;li>=1;li--){
      const int lvlstart = tlevels[li];      //Starting index of level in stack
      const int lvlend   = tlevels[li+1];    //Ending index of level in stack
      #pragma omp parallel for simd default(none) shared(tstack)
      for(int si=lvlstart;si<lvlend;si++){
        const int c = tstack[si];
        for(int k=0;k<ndon[c];k++){
          const auto n = donor[8*c+k];
          accum[c]    += accum[n];
        }
      }
    }    
  }
}



///Raise each cell in the landscape by some amount, otherwise it wil get worn
///flat (in this model, with these settings)
void FastScape_RBGPU::AddUplift(){
  //We exclude two exterior rings of cells in this example. The outermost ring
  //(the edges of the dataset) allows us to ignore the edges of the dataset,
  //the second-most outer ring (the cells bordering the edge cells of the
  //dataset) are fixed to a specified height in this model. All other cells
  //have heights which actively change and they are altered here.
  const int height = this->height;
  const int width  = this->width;

  #pragma omp target teams
  #pragma omp distribute parallel for simd default(none)
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width-2;x++){
    const int c = y*width+x;
    h[c]       += ueq*dt; 
  }
}



///Decrease he height of cells according to the stream power equation; that
///is, based on a constant K, flow accumulation A, the local slope between
///the cell and its receiving neighbour, and some judiciously-chosen constants
///m and n.
///    h_next = h_current - K*dt*(A^m)*(Slope)^n
///We solve this equation implicitly to preserve accuracy
void FastScape_RBGPU::Erode(){
  //The cells in each level can be processed in parallel, so we loop over
  //levels starting from the lower-most (the one closest to the NO_FLOW cells)

  //Level 0 contains all those cells which do not flow anywhere, so we skip it
  //since their elevations will not be changed via erosion anyway.
  // #pragma acc parallel default(none) present(this,levels,stack,nshift,rec,accum,h)
  #pragma omp target teams num_teams(thread_count)
  {
    auto &tnlevel = nlevel[omp_get_team_num()];
    auto *tlevels = &levels[omp_get_team_num()*level_width];
    auto *tstack  = &stack [omp_get_team_num()*stack_width];  
  for(int li=1;li<tnlevel-1;li++){
    const int lvlstart = tlevels[li];      //Starting index of level in stack
    const int lvlend   = tlevels[li+1];    //Ending index of level in stack
    #pragma omp parallel for simd default(none) shared(tstack)
    for(int si=lvlstart;si<lvlend;si++){
      const int c = tstack[si];         //Cell from which flow originates
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
}



///Run the model forward for a specified number of timesteps. No new
///initialization is done. This allows the model to be stopped, the terrain
///altered, and the model continued. For space-efficiency, a number of
///temporary arrays are created each time this is run, so repeatedly running
///this function for the same model will likely not be performant due to
///reallocations. If that is your use case, you'll want to modify your code
///appropriately.
void FastScape_RBGPU::run(const int nstep){
  Tmr_Overall.start();

  Tmr_Step1_Initialize.start();

  accum  = new double[size];
  rec    = new int[size];
  ndon   = new int[size];
  donor  = new int[8*size];

  nlevel = new int[thread_count];
  nstack = new int[thread_count];

  //TODO: Make smaller, explain max
  stack_width = std::max(100,10*size/thread_count); //Number of stack entries available to each thread
  level_width = std::max(1000,size/thread_count);     //Number of level entries available to each thread

  stack  = new int[thread_count*stack_width];
  levels = new int[thread_count*level_width];

  //! initializing rec
  //#pragma acc parallel loop present(this,rec)
  for(int i=0;i<size;i++)
    rec[i] = NO_FLOW;

  //#pragma acc parallel loop present(this,ndon)
  for(int i=0;i<size;i++)
    ndon[i] = 0;

  std::cout<<"Transferring memory..."<<std::endl;
  #pragma omp target enter data           \
    map(to:                               \
      this[0:1],                          \
      h[0:size],                          \
      nshift[0:8],                        \
      rec[0:size],                        \
      ndon[0:size]                        \
    )                                     \
    map(alloc:                            \
      accum[0:size],                      \
      donor[0:8*size],                    \
      stack[0:thread_count*stack_width],  \
      levels[0:thread_count*level_width], \
      nlevel[0:thread_count],             \
      nstack[0:thread_count]              \
    )


  std::cout<<"stack_width = "<<stack_width<<std::endl;
  std::cout<<"level_width = "<<level_width<<std::endl;

  std::cout<<"stack size = "<<(thread_count*stack_width)<<std::endl;
  std::cout<<"level size = "<<(thread_count*level_width)<<std::endl;


  //TODO: ADJUST THE FOLLOWING
  //It's difficult to know how much memory should be allocated for levels. For
  //a square DEM with isotropic dispersion this is approximately sqrt(E/2). A
  //diagonally tilted surface with isotropic dispersion may have sqrt(E)
  //levels. A tortorously sinuous river may have up to E*E levels. We
  //compromise and choose a number of levels equal to the perimiter because
  //why not?
  // levels = new int[level_width]; //TODO: Make smaller to `2*width+2*height`

  Tmr_Step1_Initialize.stop();

  //#pragma acc kernels present(accum,rec,ndon,donor,stack,nlevel,levels)
  //#pragma acc loop seq
  for(int step=0;step<=nstep;step++){
    #pragma omp master
    if( step%1==0 ) //Show progress
      std::cout<<"p Step = "<<step<<std::endl;
    Tmr_Step2_DetermineReceivers.start ();   ComputeReceivers  ();  Tmr_Step2_DetermineReceivers.stop ();
    Tmr_Step3_DetermineDonors.start    ();   ComputeDonors     ();  Tmr_Step3_DetermineDonors.stop    ();
    Tmr_Step4_GenerateOrder.start      ();   GenerateOrder     ();  Tmr_Step4_GenerateOrder.stop      ();
    Tmr_Step5_FlowAcc.start            ();   ComputeFlowAcc    ();  Tmr_Step5_FlowAcc.stop            ();
    Tmr_Step6_Uplift.start             ();   AddUplift         ();  Tmr_Step6_Uplift.stop             ();
    Tmr_Step7_Erosion.start            ();   Erode             ();  Tmr_Step7_Erosion.stop            ();
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

  #pragma omp target exit data map(from:h[0:size]) map(delete:this[0:1],accum[0:size],rec[0:size],ndon[0:size],donor[0:8*size],stack[0:thread_count*stack_width],nlevel,levels[0:thread_count*level_width])

  #pragma omp target exit data map(delete:stack[0:thread_count*stack_width],levels[0:thread_count*level_width],nlevel[0:thread_count],nstack[0:thread_count])

  //Free up memory, except for the resulting landscape height field prior to
  //exiting so that unnecessary space is not used when the model is not being
  //run.
  delete[] accum;
  delete[] rec;
  delete[] ndon;
  delete[] stack;
  delete[] donor;
  delete[] levels;
  delete[] nlevel;
  delete[] nstack;
}



///Returns a pointer to the data so that it can be copied, printed, &c.
double* FastScape_RBGPU::getH() {
  return h;
}
