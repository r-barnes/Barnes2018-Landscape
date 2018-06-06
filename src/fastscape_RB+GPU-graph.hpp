#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
#include "random.hpp"
#include <vector>
#include "fastscape_RB+GPU-graph.hpp"

///Initializing code
FastScape_RBGPUgraph::FastScape_RBGPUgraph(const int width0, const int height0)
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



FastScape_RBGPUgraph::~FastScape_RBGPUgraph(){
  delete[] h;
}


///The receiver of a focal cell is the cell which receives the focal cells'
///flow. Here, we model the receiving cell as being the one connected to the
///focal cell by the steppest gradient. If there is no local gradient, than
///the special value NO_FLOW is assigned.
void FastScape_RBGPUgraph::ComputeReceivers(){
  //Edge cells do not have receivers because they do not distribute their flow
  //to anywhere.

  const int height = this->height;
  const int width  = this->width;

  #pragma acc parallel loop independent collapse(2) present(this,nshift[0:8],h[0:size],rec[0:size]) //default(none) present(this,h,rec,dr,nshift)
  for(int y=2;y<height-2;y++)
  for(int x=2;x<width-2;x++){
    const int c      = y*width+x;

    //The slope must be greater than zero for there to be downhill flow;
    //otherwise, the cell is marked NO_FLOW.
    double max_slope = 0;        //Maximum slope seen so far amongst neighbours
    int    max_n     = NO_FLOW;  //Direction of neighbour which had maximum slope to focal cell

    #pragma acc loop seq
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
void FastScape_RBGPUgraph::ComputeDonors(){
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

  #pragma acc parallel loop independent collapse(2) default(none) present(this,rec,nshift,donor,ndon)
  for(int y=1;y<height-1;y++)
  for(int x=1;x<width-1;x++){
    const int c = y*width+x;
    ndon[c] = 0; //Cell has no donor neighbours we know about
    #pragma acc loop seq
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
void FastScape_RBGPUgraph::GenerateOrder(){
  /*
  //#pragma acc update host(rec[0:size],donor[0:8*size],ndon[0:size])

  int nstack = 0;    //Number of cells currently in the stack

  //Since each value of the `levels` array is later used as the starting value
  //of a for-loop, we include a zero at the beginning of the array.
  levels[0] = 0;
  nlevel    = 1;     //Note that array now contains a single value

  const int height = this->height;
  const int width  = this->width;

  //TODO: Outside edge is always NO_FLOW. Maybe this can get loaded once?
  //Load cells without dependencies into the queue. This will include all of
  //the edge cells.
  #pragma acc parallel loop collapse(2) independent num_gangs(10) default(none) copy(nstack) present(this,rec[0:size],stack[0:stack_width])
  for(int y=1;y<height-1;y++)
  for(int x=1;x<width -1;x++){
    const int c = y*width+x;
    if(rec[c]==NO_FLOW){
      int mystack;
      #pragma acc atomic capture
      mystack = nstack++;
      stack[mystack] = c;
      // assert(mystack<stack_width);
    }
  }
  levels[nlevel++] = nstack; //Last cell of this level
  assert(nlevel<level_width); 

  //#pragma acc update host(stack[0:stack_width])

  //Start with level_bottom=-1 so we get into the loop, it is immediately
  //replaced by level_top.
  int level_bottom = -1;         //First cell of the current level
#ifndef PGI_WAR
  int level_top    =  0;         //Last cell of the current level
#else
  volatile int level_top = 0;    //Last cell of the current level
  int level_num;
#endif

  ///////////////////////////
  //Enabling the pragmas below breaks everything
  ///////////////////////////

  while(level_bottom<level_top){
    level_bottom = level_top;
    level_top    = nstack;

#ifdef PGI_WAR
    level_num    = level_top - level_bottom;
#endif
    #pragma acc parallel loop independent num_gangs(100) default(none) copy(nstack) present(this,ndon[0:size],donor[0:8*size],stack[0:stack_width])
#ifdef PGI_WAR
    for(int si=0;si<level_num;si++){
      const auto c = stack[si+level_bottom];
#else
    for(int si=level_bottom;si<level_top;si++){
      const auto c = stack[si];
#endif
      const auto ncount = ndon[c];
      int mystack;
      #pragma acc atomic capture
      {
        mystack = nstack;
        nstack += ncount;
      }

      //Load donating neighbours of focal cell into the stack
      #pragma acc loop seq
      for(int k=0;k<ndon[c];k++){
        const auto n    = donor[8*c+k];
        stack[mystack++] = n;
        // assert(nstack<=stack_width);
      }
    }

    levels[nlevel++] = nstack; //Start a new level
  }

  // std::cout<<"nstack = "<<nstack<<std::endl;
  // std::cout<<"nlevel = "<<nlevel<<std::endl;

  //End condition for the loop places two identical entries
  //at the end of the stack. Remove one.
  nlevel--;

  //#pragma acc update device(stack[0:size],levels[0:size],nlevel)

  assert(levels[nlevel-1]==nstack);

  #pragma acc update device(nlevel)

  //#pragma acc update device(stack[0:size],levels[0:size],nlevel)
  */
}

///Compute the flow accumulation for each cell: the number of cells whose flow
///ultimately passes through the focal cell multiplied by the area of each
///cell. Each cell could also have its own weighting based on, say, average
///rainfall.
void FastScape_RBGPUgraph::ComputeFlowAcc(){
  const int height = this->height;
  const int width  = this->width;

  //Reset graph traversal arrays
  // std::cout<<"\treset"<<std::endl;
  #pragma acc parallel loop default(none) present(this,graph_visited,graph_mask,graph_updating_mask)
  for(unsigned int c=0;c<size;c++){
    graph_updating_mask[c] = false;
    graph_mask[c]          = false;
    graph_visited[c]       = false;
  }

  //Initialize cell areas to their weights. Here, all the weights are the same.
  // std::cout<<"\tcell area"<<std::endl;
  #pragma acc parallel loop default(none) present(this,accum)
  for(int c=0;c<size;c++)
    accum[c] = cell_area;

  //Find the source nodes
  // std::cout<<"\tseed graph"<<std::endl;
  #pragma acc parallel loop collapse(2) default(none) independent present(this,ndon,graph_mask,graph_visited)
  for(int y=1;y<height-1;y++)
  for(int x=1;x<width -1;x++){
    const int c = y*width+x;
    if(ndon[c]==0){
      graph_mask[c]    = true;
      graph_visited[c] = true;
    }
  }

  // std::cout<<"\tloopy loopy"<<std::endl;
  bool keep_going = true;
  while(keep_going){
    // std::cout<<"\t\tPush around"<<std::endl;
    #pragma acc parallel loop collapse(2) independent default(none) present(this,graph_mask,graph_updating_mask,graph_visited,ndon,nshift)
    for(int y=1;y<height-1;y++)
    for(int x=1;x<width -1;x++){
      const int c = y*width+x;
      if(graph_mask[c]==true){         //Cell is on the frontier
        //Get flow from upstream cells
        graph_mask[c] = false;           //Remove cell from frontier
        #pragma acc loop seq
        for(int k=0;k<ndon[c];k++){
          const auto n = donor[8*c+k];
          accum[c]    += accum[n];
        }
        
        //Add downstream cell to the frontier
        if(rec[c]!=NO_FLOW){
          const int n = c+nshift[rec[c]];  //Cell receiving the flow
          // if(!graph_visited[n])
          graph_updating_mask[n] = true;
        }
      }
    }

    // std::cout<<"\t\tMove frontier"<<std::endl;
    keep_going = false;
    #pragma acc parallel loop default(none) present(this,graph_mask,graph_visited,graph_updating_mask) reduction(||:keep_going)
    for(int c=0;c<size;c++){
      if(graph_updating_mask[c]==true){
        graph_mask[c]          = true;   //Add cell to frontier
        graph_visited[c]       = true;
        keep_going             |= true;
        graph_updating_mask[c] = false;
      }
    }
  }
 




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
  // for(int li=nlevel-3;li>=1;li--){
  //   const int lvlstart = levels[li];      //Starting index of level in stack
  //   const int lvlend   = levels[li+1];    //Ending index of level in stack
  //   #pragma acc parallel loop independent default(none) present(this,accum,levels,nshift,rec,stack)
  //   for(int si=lvlstart;si<lvlend;si++){
  //     const int c = stack[si];
  //     #pragma acc loop seq
  //     for(int k=0;k<ndon[c];k++){
  //       const auto n = donor[8*c+k];
  //       accum[c]    += accum[n];
  //     }
  //   }
  // }    
}



///Raise each cell in the landscape by some amount, otherwise it wil get worn
///flat (in this model, with these settings)
void FastScape_RBGPUgraph::AddUplift(){
  //We exclude two exterior rings of cells in this example. The outermost ring
  //(the edges of the dataset) allows us to ignore the edges of the dataset,
  //the second-most outer ring (the cells bordering the edge cells of the
  //dataset) are fixed to a specified height in this model. All other cells
  //have heights which actively change and they are altered here.
  const int height = this->height;
  const int width  = this->width;

  #pragma acc parallel loop collapse(2) independent default(none) present(this,h)
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
void FastScape_RBGPUgraph::Erode(){
  const int height = this->height;
  const int width  = this->width;

  //Reset graph traversal arrays
  // std::cout<<"\treset"<<std::endl;
  #pragma acc parallel loop default(none) independent present(this,graph_visited,graph_mask,graph_updating_mask)
  for(unsigned int c=0;c<size;c++){
    graph_updating_mask[c] = false;
    graph_mask[c]          = false;
    graph_visited[c]       = false;
  }

  //Find the source nodes
  // std::cout<<"\tseed graph"<<std::endl;
  #pragma acc parallel loop collapse(2) default(none) independent present(this,ndon,graph_mask,graph_visited)
  for(int y=1;y<height-1;y++)
  for(int x=1;x<width -1;x++){
    const int c = y*width+x;
    if(rec[c]==NO_FLOW){
      graph_mask[c]    = true;
      graph_visited[c] = true;
    }
  }

  // std::cout<<"\tloopy loopy"<<std::endl;
  int keep_going = 1;
  while(keep_going>0){
    // std::cout<<"\t\tPush around"<<std::endl;
    #pragma acc parallel loop collapse(2) independent default(none) present(this,graph_mask,graph_updating_mask,graph_visited,ndon,nshift)
    for(int y=1;y<height-1;y++)
    for(int x=1;x<width -1;x++){
      const int c = y*width+x;
      if(graph_mask[c]==true){         //Cell is on the frontier
        graph_mask[c] = false;         //Remove cell from frontier
        
        //Perform erosion
        if(rec[c]!=NO_FLOW){
          const int n = c+nshift[rec[c]];  //Cell receiving the flow

          const double length = dr[rec[c]];
          //`fact` contains a set of values which are constant throughout the integration
          const double fact   = keq*dt*std::pow(accum[c],meq)/std::pow(length,neq);
          const double h0     = h[c];      //Elevation of focal cell
          const double hn     = h[n];      //Elevation of neighbouring (receiving, lower) cell
          double hnew         = h0;        //Current updated value of focal cell
          double hp           = h0;        //Previous updated value of focal cell
          double diff         = 2*tol;     //Difference between current and previous updated values
          #pragma acc loop seq
          while(std::abs(diff)>tol){       //Newton-Rhapson method (run until subsequent values differ by less than a tolerance, which can be set to any desired precision)
            hnew -= (hnew-h0+fact*std::pow(hnew-hn,neq))/(1.+fact*neq*std::pow(hnew-hn,neq-1));
            diff  = hnew - hp;             //Difference between previous and current value of the iteration
            hp    = hnew;                  //Update previous value to new value
          }
          h[c] = hnew;                     //Update value in array          
        }



        // std::cout<<"\t\tMove frontier"<<std::endl;
        //Add upstream cells to the frontier
        #pragma acc loop seq
        for(int k=0;k<ndon[c];k++){
          const auto n = donor[8*c+k];
          // if(!graph_visited[n])
          graph_updating_mask[n] = true;
        }
      }
    }

    keep_going = 0;
    #pragma acc parallel loop default(none) present(this,graph_mask,graph_visited,graph_updating_mask) reduction(+:keep_going)
    for(int c=0;c<size;c++){
      if(graph_updating_mask[c]==true){
        graph_mask[c]          = true;   //Add cell to frontier
        graph_visited[c]       = true;
        keep_going             += 1;
        graph_updating_mask[c] = false;
      }
    }
  }
 


  // //The cells in each level can be processed in parallel, so we loop over
  // //levels starting from the lower-most (the one closest to the NO_FLOW cells)

  // //Level 0 contains all those cells which do not flow anywhere, so we skip it
  // //since their elevations will not be changed via erosion anyway.
  // // #pragma acc parallel default(none) present(this,levels,stack,nshift,rec,accum,h)
  // for(int li=1;li<nlevel-1;li++){
  //   const int lvlstart = levels[li];      //Starting index of level in stack
  //   const int lvlend   = levels[li+1];    //Ending index of level in stack
  //   #pragma acc parallel loop independent default(none) present(this,levels,stack,nshift,rec,accum,h)
  //   for(int si=lvlstart;si<lvlend;si++){
  //     const int c = stack[si];         //Cell from which flow originates
  //     const int n = c+nshift[rec[c]];  //Cell receiving the flow

  //     const double length = dr[rec[c]];
  //     //`fact` contains a set of values which are constant throughout the integration
  //     const double fact   = keq*dt*std::pow(accum[c],meq)/std::pow(length,neq);
  //     const double h0     = h[c];      //Elevation of focal cell
  //     const double hn     = h[n];      //Elevation of neighbouring (receiving, lower) cell
  //     double hnew         = h0;        //Current updated value of focal cell
  //     double hp           = h0;        //Previous updated value of focal cell
  //     double diff         = 2*tol;     //Difference between current and previous updated values
  //     #pragma acc loop seq
  //     while(std::abs(diff)>tol){       //Newton-Rhapson method (run until subsequent values differ by less than a tolerance, which can be set to any desired precision)
  //       hnew -= (hnew-h0+fact*std::pow(hnew-hn,neq))/(1.+fact*neq*std::pow(hnew-hn,neq-1));
  //       diff  = hnew - hp;             //Difference between previous and current value of the iteration
  //       hp    = hnew;                  //Update previous value to new value
  //     }
  //     h[c] = hnew;                     //Update value in array
  //   }
  // }
}



///Run the model forward for a specified number of timesteps. No new
///initialization is done. This allows the model to be stopped, the terrain
///altered, and the model continued. For space-efficiency, a number of
///temporary arrays are created each time this is run, so repeatedly running
///this function for the same model will likely not be performant due to
///reallocations. If that is your use case, you'll want to modify your code
///appropriately.
void FastScape_RBGPUgraph::run(const int nstep){
  Tmr_Overall.start();

  Tmr_Step1_Initialize.start();

  accum  = new double[size];
  rec    = new int[size];
  ndon   = new int[size];
  donor  = new int[8*size];

  graph_mask          = new bool[size];
  graph_updating_mask = new bool[size];
  graph_visited       = new bool[size];

  //! initializing rec
  //#pragma acc parallel loop present(this,rec)
  for(int i=0;i<size;i++)
    rec[i] = NO_FLOW;

  //#pragma acc parallel loop present(this,ndon)
  for(int i=0;i<size;i++)
    ndon[i] = 0;

  #pragma acc enter data copyin(this[0:1],h[0:size],nshift[0:8],rec[0:size],ndon[0:size]) create(accum[0:size],donor[0:8*size],graph_mask[0:size],graph_updating_mask[0:size],graph_visited[0:size])

  //TODO: Make smaller, explain max
  stack_width = size; //Number of stack entries available to each thread
  level_width = size; //Number of level entries available to each thread

  stack  = new int[stack_width];

  //It's difficult to know how much memory should be allocated for levels. For
  //a square DEM with isotropic dispersion this is approximately sqrt(E/2). A
  //diagonally tilted surface with isotropic dispersion may have sqrt(E)
  //levels. A tortorously sinuous river may have up to E*E levels. We
  //compromise and choose a number of levels equal to the perimiter because
  //why not?
  levels = new int[level_width]; //TODO: Make smaller to `2*width+2*height`

  #pragma acc enter data create(stack[0:stack_width],levels[0:level_width])

  Tmr_Step1_Initialize.stop();

  //#pragma acc kernels present(accum,rec,ndon,donor,stack,nlevel,levels)
  //#pragma acc loop seq
  for(int step=0;step<=nstep;step++){
    Tmr_Step2_DetermineReceivers.start ();   ComputeReceivers  ();  Tmr_Step2_DetermineReceivers.stop ();
    Tmr_Step3_DetermineDonors.start    ();   ComputeDonors     ();  Tmr_Step3_DetermineDonors.stop    ();
    Tmr_Step5_FlowAcc.start            ();   ComputeFlowAcc    ();  Tmr_Step5_FlowAcc.stop            ();
    Tmr_Step6_Uplift.start             ();   AddUplift         ();  Tmr_Step6_Uplift.stop             ();
    Tmr_Step7_Erosion.start            ();   Erode             ();  Tmr_Step7_Erosion.stop            ();

    // Tmr_Step4_GenerateOrder.start      ();   GenerateOrder     ();  Tmr_Step4_GenerateOrder.stop      ();
    #pragma omp master
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

  #pragma acc exit data copyout(h[0:size]) delete(this,accum[0:size],rec[0:size],ndon[0:size],donor[0:8*size],stack[0:stack_width],nlevel,levels[0:level_width],graph_visited[0:size],graph_mask[0:size],graph_updating_mask[0:size])

  //Free up memory, except for the resulting landscape height field prior to
  //exiting so that unnecessary space is not used when the model is not being
  //run.
  delete[] accum;
  delete[] rec;
  delete[] ndon;
  delete[] stack;
  delete[] donor;
  delete[] levels;
}



///Returns a pointer to the data so that it can be copied, printed, &c.
double* FastScape_RBGPUgraph::getH() {
  return h;
}
