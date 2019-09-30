#include <stdio.h>
#include <math.h>
#include <mpi.h>
#include <string.h>
#include "ErrorExceptions.h"
#include "macros_and_parameters.h"
#include "typedefs.h"
#include "global_data.h"
#include "Fluxes.h"
#include "GridList.h"
#include "ExternalBoundary.h"
#include "Grid.h"
#include "fortran.def"
#include "CosmologyParameters.h"
#include "StarParticleData.h"
#include "phys_constants.h"

    extern "C"  void FORTRAN_NAME(cic_deposit)(float* xPosition, float* yPosition,
        float* zPosition, int* gridRank, int* nParticles, 
        float* DepositQuantity, float* FieldToDepositTo,
        float* leftEdge, int* xGridDim, int* yGridDim, 
        int* zGridDim, float* gridDx, float* cloudsize);
    int GetUnits(float *DensityUnits, float *LengthUnits,
	     float *TemperatureUnits, float *TimeUnits,
	     float *VelocityUnits, float *MassUnits, float Time);
    int transformComovingWithStar(float* Density, float* Metals, 
        float* MetalsSNII, float* MetalsSNIA,
        float* Vel1, float* Vel2, float* Vel3, 
        float* TE, float* GE,
        float up, float vp, float wp,
        int sizeX, int sizeY, int sizeZ, int direction);
    int FindField(int field, int farray[], int numfields);


int grid::MechStars_DepositFeedback(float ejectaEnergy, 
                        float ejectaMass, float ejectaMetal, 
                        float* totalMetals, float* temperature,
                        float* up, float* vp, float* wp,
                        float* xp, float* yp, float* zp,
                        int ip, int jp, int kp,
                        int size, float* muField, int winds, int nSNII,
                        int nSNIA, float starMetal, int isP3){
    
    /*
     This routine will create an isocahedron of coupling particles, where we determine
        the feedback quantities.  The vertices of the isocahedron are ~coupled particles
        and all have radius dx from the source particle. 
        Each vertex particle will then be CIC deposited to the grid!
    */
    //printf("In Feedback deposition\n");
    bool debug = false;
    bool criticalDebug = false;
    bool printout = debug && !winds;
    int index = ip+jp*GridDimension[0]+kp*GridDimension[0]*GridDimension[1];
    if (printout) printf("Host index = %d", index);
    int DensNum, GENum, TENum, Vel1Num, Vel2Num, Vel3Num;
    
    /* Compute size (in floats) of the current grid. */
    float stretchFactor =1.0;//1.5/sin(M_PI/10.0);  // How far should cloud particles be from their host
                                // in units of dx. Since the cloud forms a sphere shell, stretchFactor > 1 is not recommended
    size = 1;
    for (int dim = 0; dim < GridRank; dim++)
        size *= GridDimension[dim];
    float DeNum, HINum, HIINum, HeINum, HeIINum, HeIIINum,
                        HMNum, H2INum, H2IINum, DINum, DIINum, HDINum;
    /* Find fields: density, total energy, velocity1-3. */

    if (this->IdentifyPhysicalQuantities(DensNum, GENum, Vel1Num, Vel2Num,
                        Vel3Num, TENum) == FAIL) {
        fprintf(stderr, "Error in IdentifyPhysicalQuantities.\n");
        return FAIL;
    }
    /* Set the units */
    float DensityUnits = 1, LengthUnits = 1, TemperatureUnits = 1,
            TimeUnits = 1, VelocityUnits = 1, MassUnits = 1;
    if (GetUnits(&DensityUnits, &LengthUnits, &TemperatureUnits,
            &TimeUnits, &VelocityUnits, 
            &MassUnits, this->Time) == FAIL) {
        fprintf(stderr, "Error in GetUnits.\n");
        return FAIL;    
    } 
    FLOAT dx = CellWidth[0][0];        

    if (printout)
        printf("depositing quantities: Energy %e, Mass %e, Metals %e\n",
            ejectaEnergy, ejectaMass, ejectaMetal);

    /* 
        get metallicity field and set flag; assumed true thoughout feedback
        since so many quantities are metallicity dependent
     */
    int MetallicityField = FALSE, MetalNum, MetalIaNum=-1, MetalIINum=-1, SNColourNum=-1;
    if ((MetalNum = FindField(Metallicity, FieldType, NumberOfBaryonFields))
        != -1)
        MetallicityField = TRUE;
    else{
        fprintf(stdout, "MechStars only functions with metallicity field enabled!");
        ENZO_FAIL("Grid_MechStarsDepositFeedback: 91");
        MetalNum = 0;
    }
    if(StarMakerTypeIaSNe)
        MetalIaNum = FindField(MetalSNIaDensity, FieldType, NumberOfBaryonFields);
    if (StarMakerTypeIISNeMetalField)
        MetalIINum = FindField(MetalSNIIDensity, FieldType, NumberOfBaryonFields);
    if (MechStarsSeedField)
        SNColourNum = FindField(SNColour, FieldType, NumberOfBaryonFields);
    
    if (MechStarsSeedField && SNColourNum<0) ENZO_FAIL("Cant use Seed Field without SNColour field");
    /* set other units that we need */
    MassUnits = DensityUnits*pow(LengthUnits*dx, 3)/SolarMass; //Msun!
    float EnergyUnits = DensityUnits*pow(LengthUnits*dx, 3) 
                    * VelocityUnits*VelocityUnits;//[g cm^2/s^2] -> code_energy
    float MomentaUnits = MassUnits*VelocityUnits;  
    if (printout) printf("Making pointer copies\n");
    /* Make copys of fields to work with. These will conatin the added deposition
        of all quantities and be coupled to the grid after the cic deposition. */
    float* density = BaryonField[DensNum];
    float* metals = BaryonField[MetalNum];
    float* metalsII = BaryonField[MetalIINum];
    float* metalsIA = BaryonField[MetalIaNum];
    float* metalsIII =BaryonField[SNColourNum];
    float* u = BaryonField[Vel1Num];
    float* v = BaryonField[Vel2Num];
    float* w = BaryonField[Vel3Num];
    float* totalEnergy = BaryonField[TENum];
    float* gasEnergy = BaryonField[GENum];
    // memset(density, 0, size*sizeof(float));
    // memset(metals, 0, size*sizeof(float));
    // memset(metalsII, 0, size*sizeof(float));
    // memset(metalsIA, 0, size*sizeof(float));
    // memset(metalsIII, 0, size*sizeof(float));
    // memset(u, 0, size*sizeof(float));
    // memset(v, 0, size*sizeof(float));
    // memset(w, 0, size*sizeof(float));
    // memset(totalEnergy, 0, size*sizeof(float));
    // memset(gasEnergy, 0, size*sizeof(float));
    // for (int i=0; i<size; i++){
    //     density[i] = 0.0;
    //     metals[i] = 0.0;
    //     metalsII [i] = 0.0;
    //     metalsIA[i] = 0.0;
    //     metalsIII[i] = 0.0;
    //     u[i] = 0.0;
    //     v[i] =0.0;
    //     w[i] = 0.0;
    //     totalEnergy[i] = 0.0;
    //     gasEnergy[i] = 0.0;
    // }
    /* Transform coordinates so that metals is fraction (rho metal/rho baryon)
        u, v, w -> respective momenta.  Use -1 to reverse transform after.*/
    /* these temp arrays are implicitly comoving with the star! */
    /* Array of coordinates of the isocahedron vertices scaled by r=dx */
    
    float phi = (1.0+sqrt(5))/2.0; //Golden Ratio
    float iphi = 1.0/phi; // inverse GR
    /* Particle Vectors */

    
    /* A DODECAHEDRON+ISOCAHEDRON */
        int nCouple = 32;
        float A = stretchFactor*dx;
        float cloudSize=stretchFactor*dx;
        if (printout) printf("Making cloud n=%d", nCouple);

	/* each coupled particle is at the vertex of a compound of a dodecahedron and isocahedron */

        FLOAT CloudParticleVectorX [] = //{-1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1};
                                            {1, 1, 1, 1, -1, -1, -1, -1, 0, 0, 0, 0,
                                            iphi, iphi, -iphi, -iphi, phi, phi, -phi, -phi, 
                                            0, 0, 0, 0, 1, 1, -1, -1, phi,-phi, phi,-phi};
        FLOAT CloudParticleVectorY [] = //{1,1,1,0,0,-1,-1,-1,0,1,1,1,0,0,-1,-1,-1,1,1,1,0,0,-1,-1,-1,0};
                                            {1,1,-1,-1, 1, 1, -1, -1, iphi, iphi, -iphi, -iphi,
                                            phi, -phi, phi,-phi, 0, 0, 0, 0,1, 1, -1, -1, 
                                            phi, -phi, -phi, phi, 0, 0, 0, 0};
        FLOAT CloudParticleVectorZ []  = //{1,0,-1,1,-1,1,0,-1,0,1,0,-1,1,-1,1,0,-1,1,0,-1,1,-1,1,0,-1,0};
                                            {1,-1, 1,-1, 1,-1, 1,-1, phi,-phi, phi,-phi, 
                                            0, 0, 0, 0, iphi, -iphi, iphi, -iphi, 
                                            phi, -phi, -phi, phi, 0, 0, 0, 0, 1, 1, -1, -1};
        float weightsVector [nCouple];
            /* Set position of feedback cloud particles */

            FLOAT CloudParticlePositionX [nCouple];
            FLOAT CloudParticlePositionY [nCouple];
            FLOAT CloudParticlePositionZ [nCouple];

                /*all possible values of x,y,z with origin particle at x=y=z=0.0 */
            
            for (int cpInd = 0; cpInd < nCouple; cpInd++){
                FLOAT norm = sqrt(CloudParticleVectorX[cpInd]*CloudParticleVectorX[cpInd]
                    + CloudParticleVectorY[cpInd]*CloudParticleVectorY[cpInd]
                    + CloudParticleVectorZ[cpInd]*CloudParticleVectorZ[cpInd]);
                    float xbaMag = A*A*norm*norm;
                /* in this cloud, take the coupling particle position as 0.5, 0.5, 0.5 */
                CloudParticlePositionX[cpInd] = *xp-CloudParticleVectorX[cpInd]/norm*
                            A;
                CloudParticlePositionY[cpInd] = *yp-CloudParticleVectorY[cpInd]/norm*
                            A;
                CloudParticlePositionZ[cpInd] = *zp-CloudParticleVectorZ[cpInd]/norm*
                            A;
                weightsVector[cpInd] = 0.5*(1.-1./(1.+1./4./M_PI/26./xbaMag/M_PI));
                /* turn the vectors back into unit-vectors */
                CloudParticleVectorZ[cpInd] /= norm;
                CloudParticleVectorY[cpInd] /= norm;
                CloudParticleVectorX[cpInd] /= norm;  
                
            }
            float weightsSum = 0.0;
            for (int wind = 0; wind < nCouple; wind++){
                weightsSum += weightsVector[wind];
            }
            for (int wind = 0; wind < nCouple; wind++){
                weightsVector[wind] /= weightsSum;
            }
    /* Each particle gets 1/32 of energy, momenta, mass, and metal.  There
    are no varying vector / scalar weights to worry about.  The momenta coupled
    is simply \hat(r_ba) p/12 for r_ba the vector from source to coupled
    particle.  */

    /* transform to comoving with the star and take velocities to momenta.
        Take Energy densities to energy
     */
    if (printout) printf("Calling transform\n");
    transformComovingWithStar(BaryonField[DensNum], BaryonField[MetalNum], 
                        BaryonField[MetalIINum], BaryonField[MetalIaNum],
                        BaryonField[Vel1Num],BaryonField[Vel2Num],BaryonField[Vel3Num],
                        BaryonField[TENum], BaryonField[GENum],
                        *up, *vp, *wp, GridDimension[0], GridDimension[1],
                        GridDimension[2], 1);
    /* Use averaged quantities across multiple cells so that deposition is stable.
        vmean is used to determine whether the supernova shell calculation should proceed:
            M_shell > 0 iff v_shell > v_gas */
    float zmean=0, dmean=0, nmean=0, vmean;
    if (printout) fprintf(stdout, "Generating local mean information\n");
    for (int ind = -1; ind <= 1; ind++){
        zmean += totalMetals[index+ind]*BaryonField[DensNum][index+ind];
        zmean += totalMetals[index+GridDimension[0]*ind]*BaryonField[DensNum][index+GridDimension[0]*ind];
        zmean += totalMetals[index+GridDimension[0]*GridDimension[1]*ind]*BaryonField[DensNum][index+GridDimension[0]*GridDimension[1]*ind];

        // if (printout) fprintf(stdout, "MuField = %f %f %f\nmax = %d ; %d %d %d\n",
        //     muField[index+ind], muField[index+GridDimension[0]*ind], muField[index+ind*GridDimension[0]*GridDimension[1]], GridDimension[0]*GridDimension[1]*GridDimension[2],
        //     index+ind, index+GridDimension[0]*ind, index+ind*GridDimension[0]*GridDimension[1]);
        nmean += BaryonField[DensNum][index+ind]*BaryonField[DensNum][index+ind]*DensityUnits/mh/max(0.6,muField[index+ind]);
        nmean += BaryonField[DensNum][index+GridDimension[0]*ind]*
            BaryonField[DensNum][index+GridDimension[0]*ind]*DensityUnits/mh/max(muField[index+GridDimension[0]*ind],0.6);
        nmean += BaryonField[DensNum][index+GridDimension[0]*GridDimension[1]*ind]
            *BaryonField[DensNum][index+GridDimension[0]*GridDimension[1]*ind]*DensityUnits/mh/max(0.6,muField[index+ind*GridDimension[0]*GridDimension[1]]);

        dmean += BaryonField[DensNum][index+ind];
        dmean += BaryonField[DensNum][index+GridDimension[0]*ind];
        dmean += BaryonField[DensNum][index+GridDimension[0]*GridDimension[1]*ind];
        
        vmean += BaryonField[Vel1Num][index+ind]*BaryonField[Vel1Num][index+ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel1Num][index+GridDimension[0]*ind]*BaryonField[Vel1Num][index+GridDimension[0]*ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel1Num][index+GridDimension[0]*GridDimension[1]*ind]*BaryonField[Vel1Num][index+GridDimension[0]*GridDimension[1]*ind]*VelocityUnits*VelocityUnits;

        vmean += BaryonField[Vel2Num][index+ind]*BaryonField[Vel2Num][index+ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel2Num][index+GridDimension[0]*ind]*BaryonField[Vel2Num][index+GridDimension[0]*ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel2Num][index+GridDimension[0]*GridDimension[1]*ind]*BaryonField[Vel2Num][index+GridDimension[0]*GridDimension[1]*ind]*VelocityUnits*VelocityUnits;

        vmean += BaryonField[Vel3Num][index+ind]*BaryonField[Vel3Num][index+ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel3Num][index+GridDimension[0]*ind]*BaryonField[Vel3Num][index+GridDimension[0]*ind]*VelocityUnits*VelocityUnits;
        vmean += BaryonField[Vel3Num][index+GridDimension[0]*GridDimension[1]*ind]*BaryonField[Vel3Num][index+GridDimension[0]*GridDimension[1]*ind]*VelocityUnits*VelocityUnits;

    }
    vmean = sqrt(vmean/dmean/dmean);
    zmean /= (dmean*0.02);
    nmean /= (dmean);
    
    dmean = dmean*DensityUnits/9.0;
    nmean = max(nmean, 1e-3);
    float zZsun = max(zmean, 1e-8);
    float fz = (zZsun < 0.01)? (2.0): (pow(zZsun, -0.14));

    /* Cooling radius as in Hopkins, but as an average over cells */
    float CoolingRadius = 28.4 *
        pow(max(0.001,nmean), -3.0/7.0)
        *pow(ejectaEnergy/1.0e51, 2.0/7.0)* fz;
    if (printout)fprintf(stdout, "cooling radius [pc] = %e\n %f %e %f %e %e \n", 
            CoolingRadius, nmean, ejectaEnergy/1e51, fz, zmean, dmean);
    /* Calculate coupled energy scaled by reduction to account for unresolved
    cooling, then use that energy to calculate momenta*/
    float coupledEnergy = ejectaEnergy;

    if (printout)fprintf(stdout, "Dx [pc] = %f\n", dx*LengthUnits/pc_cm);
    float dxRatio = stretchFactor*dx*LengthUnits/pc_cm/CoolingRadius;
    if (winds) dxRatio = min(stretchFactor*dx*LengthUnits/pc_cm/CoolingRadius, 20);
    float pEjectMod = pow(2.0*ejectaEnergy*(ejectaMass*SolarMass), 0.5)/SolarMass/1e5;
    /* We want to couple one of four phases: free expansion, Sedov-taylor, shell formation, or terminal 
    The first three phases are take forms from Kim & Ostriker 2015, the last from Cioffi 1988*/
    float cellwidth = stretchFactor*dx*LengthUnits/pc_cm;
    float p_free = 0.0;//sqrt(ejectaMass*SolarMass*ejectaEnergy)/SolarMass/1e5;//1.73e4*sqrt(ejectaMass*ejectaEnergy/1e51/3.); // free exp. momentum eq 15
    float r_free = 2.75*pow(ejectaMass/3/nmean, 1./3.); // free exp radius eq 2
    
    
    // assuming r_sedov == dx, solve for t3
    float t3_sedov = pow( cellwidth*pc_cm
            /(5.0*pc_cm*pow(ejectaEnergy/1e51/nmean, 1.0/5.0)), 5./2.); 
    float p_sedov = 2.21e4*pow(ejectaEnergy/1e51, 4./5.)
            * pow(nmean, 1./5.)* pow(t3_sedov, 3./5.); // eq 16

      // shell formation radius eq 8
    float r_shellform = 22.6*pow(ejectaEnergy/1e51, 0.29)*pow(nmean, -0.42); 
    // p_sf = m_sf*v_sf eq 9,11
    float p_shellform = 3.1e5*pow(ejectaEnergy/1e51, 0.94)*pow(nmean, -0.13) ; // p_sf = m_sf*v_sf eq 9,11
  
    /* termninal momentum */
    float pTerminal = 4.8e5*pow(nmean, -1.0/7.0)
                * pow(ejectaEnergy/1e51, 13.0/14.0) * fz; // cioffi 1988, as written in Hopkins 2018

    float coupledMomenta = 0.0;
    float eKinetic = 0.0;
    if(printout)fprintf(stdout, "RADII: %e %e %e t_3=%e\n", r_free, r_shellform, CoolingRadius, t3_sedov);
    
    /* Select the mode of coupling */

    if (cellwidth < r_free){
        coupledMomenta = p_free;
        if(printout)fprintf(stdout, "Coupling free expansion\n");}
    if (cellwidth > r_free && cellwidth < r_shellform){
        coupledMomenta = min(p_sedov, p_shellform*cellwidth/r_shellform);
        if(printout)fprintf(stdout, "Coupling S-T phase\n");}
    if (cellwidth > r_shellform && cellwidth < CoolingRadius){
        coupledMomenta = min(p_shellform+(cellwidth-r_shellform)*(pTerminal-p_shellform)/(CoolingRadius-r_shellform), pTerminal);
        if(printout)fprintf(stdout, "Coupling shell-forming stage\n");}
    if (cellwidth > CoolingRadius){
        coupledMomenta = pTerminal;
        if(printout)fprintf(stdout, "Coupling terminal momenta\n");}
    if (printout)fprintf(stdout, "Calculated p = %e\n", coupledMomenta);


    /* fading radius of a SNR.  For real scale invariance, the momentum deposited should go to zero for large dx!*/
    // float *temperature = new float[size];
    // this->ComputeTemperatureField(temperature);
    // float Gcode = GravConst*DensityUnits*pow(TimeUnits,2);
    // float KBcode = kboltz*MassUnits/(LengthUnits*dx)/pow(TimeUnits,2);
    // float cSound = sqrt(5/3*kboltz*temperature[index]/mh/muField[index])/1e5; //km/s
    // float r_fade = 66.0*pow(ejectaEnergy/1e51, 0.32)*pow(nmean, -0.37)*pow(min(cSound/10, .1), -2.0/5.0);
    // if(printout) fprintf(stdout, "Rfade = %e cs = %e \n", r_fade, cSound);
    // delete [] temperature;

    //    coupledMomenta = (cellwidth > r_fade)?(coupledMomenta*pow(r_fade/cellwidth,3/2)):(coupledMomenta);
    float shellMass = 0.0, shellVelocity = 0.0;
    if(printout) printf("Coupled momentum: %e\n", coupledMomenta);
    /* 
        If resolution is in a range comparable to Rcool and
        Analytic SNR shell mass is on, adjust the shell mass 
        upper range of applicability for shell mass is determined by
        local average gas velocity (v_shell ~ v_gas = no shell)
    */
    if (cellwidth > r_shellform && coupledEnergy > 0
        && AnalyticSNRShellMass){
            shellVelocity = 413.0 *pow(nmean, 1.0/7.0)
                *pow(zZsun, 3.0/14.0)*pow(coupledEnergy/EnergyUnits/1e51, 1.0/14.0)
                *pow(dxRatio, -7.0/3.0);//km/s
            
            if (shellVelocity > vmean){ 
                shellMass = max(8e3, coupledMomenta/shellVelocity); //Msun
                
                /* cant let host cells evacuate completely!  
                    Shell mass will be evacuated from central cells by CIC a negative mass,
                    so have to check that the neighbors can handle it too*/
                for (int ind = -1; ind<=1; ind++){
                        float minD = min(BaryonField[DensNum][index+ind],BaryonField[DensNum][index+GridDimension[0]*ind]);
                        minD = min(minD,BaryonField[DensNum][index+GridDimension[0]*GridDimension[1]*ind]);
                        if (shellMass >= 0.25*minD*MassUnits)
                        shellMass = 0.25*minD*MassUnits;
                }
                    
                         
            }

    }

    if (shellMass < 0.0){
       fprintf(stdout, "Shell mass = %e Velocity= %e P = %e",
            shellMass, shellVelocity, coupledMomenta);
         ENZO_FAIL("SM_deposit: 252");
    }
    float coupledMass = shellMass+ejectaMass;
    eKinetic = coupledMomenta*coupledMomenta
                    /(2.0*dmean*pow(LengthUnits*CellWidth[0][0], 3)/SolarMass)*SolarMass*1e10;
    if (printout)fprintf(stdout, "Ekinetic = %e Mass = %e\n", 
                eKinetic, dmean*pow(LengthUnits*CellWidth[0][0], 3)/SolarMass);
    if (eKinetic > 1e60) ENZO_FAIL("Ekinetic > reasonability!\n");


    float coupledGasEnergy = max(ejectaEnergy-eKinetic, 0);
    if (printout)fprintf(stdout, "Coupled Gas Energy = %e\n",coupledGasEnergy);
    if (dxRatio > 2.0)
         coupledGasEnergy = (DepositUnresolvedEnergyAsThermal)
                            ?(coupledGasEnergy)
                            :(coupledGasEnergy*pow(dxRatio, -6.5));
   
    float shellMetals = zZsun*0.02 * shellMass;
    float coupledMetals = 0.0, SNIAmetals = 0.0, SNIImetals = 0.0, P3metals = 0.0;
    if (winds) coupledMetals = ejectaMetal ;//+ shellMetals; // winds only couple to metallicity
    SNIAmetals = (StarMakerTypeIaSNe) ? nSNIA * 1.4 : 0.0;
    if (!StarMakerTypeIaSNe && nSNIA > 0)
        coupledMetals += nSNIA*1.4;
    SNIImetals = (StarMakerTypeIISNeMetalField)? nSNII*(1.91+0.0479*max(starMetal, 1.65)) : 0.0;
    if (!StarMakerTypeIISNeMetalField && nSNII > 0)
        coupledMetals += nSNII*(1.91+0.0479*max(starMetal, 1.65));
    if (isP3 && MechStarsSeedField) P3metals = ejectaMetal;



    if (printout) fprintf(stdout, "Coupled Metals: %e %e %e %e %e\n", ejectaMetal, SNIAmetals, SNIImetals, shellMetals, P3metals);

    /* 
        Critical debug compares the pre-feedback and post-feedback field sums.  Note that this doesn't 
        work well on vector quantities outside of an ideal test.
    */
    float preMass = 0, preZ = 0, preP = 0, prePmag=0, preTE = 0, preGE = 0, preZII=0, preZIa = 0;
    float dsum = 0.0, zsum=0.0, psum=0.0, psqsum =0.0, tesum=0.0, gesum=0.0, kesum=0.0;
    float postMass = 0, postZ = 0, postP = 0, postPmag = 0, postTE = 0, postGE = 0, postZII=0, postZIa = 0;
    if (criticalDebug && !winds){
        for (int i=0; i<size; ++i){
            preMass += BaryonField[DensNum][i];
            preZ += BaryonField[MetalNum][i];
            if (StarMakerTypeIISNeMetalField)
                preZII += BaryonField[MetalIINum][i];
            if (StarMakerTypeIaSNe)
                preZIa += BaryonField[MetalIaNum][i];
            if (MechStarsSeedField) 
               preZ += BaryonField[SNColourNum][i];
            preP += BaryonField[Vel1Num][i]+BaryonField[Vel2Num][i]+BaryonField[Vel3Num][i];
            prePmag += pow(BaryonField[Vel1Num][i]*MomentaUnits,2)+
                pow(BaryonField[Vel2Num][i]*MomentaUnits,2)
                +pow(BaryonField[Vel3Num][i]*MomentaUnits,2);
            preTE += BaryonField[TENum][i];
            preGE += BaryonField[GENum][i];
        }
    }
    /* Reduce coupled quantities to per-particle quantity and convert to 
        code units.
        Hopkins has complicated weights due to complicated geometry. 
            This implementation is simple since our coupled particles are 
            spherically symmetric about the feedback particle*/
    

    coupledEnergy = min(eKinetic, ejectaEnergy);
    coupledEnergy /= EnergyUnits;
    coupledGasEnergy /= EnergyUnits;
    coupledMass /= MassUnits;
    coupledMetals /= MassUnits;
    coupledMomenta /= MomentaUnits;
    SNIAmetals /= MassUnits;
    SNIImetals /= MassUnits;
    P3metals /= MassUnits;
    /* CIC deposit the particles with their respective quantities */

    float LeftEdge[3] = {CellLeftEdge[0][0], CellLeftEdge[1][0], CellLeftEdge[2][0]};
    if (printout) fprintf(stdout, "Entering CIC Loop over cloud particles\n");

    for (int n = 0; n < nCouple; ++n){
        //fprintf(stdout, "Weight %d = %f", n, weightsVector[n]);
        FLOAT pX = coupledMomenta*CloudParticleVectorX[n]*weightsVector[n];
        FLOAT pY = coupledMomenta*CloudParticleVectorY[n]*weightsVector[n];
        FLOAT pZ = coupledMomenta*CloudParticleVectorZ[n]*weightsVector[n];
        float eCouple = coupledEnergy*weightsVector[n];
        float geCouple = coupledGasEnergy * weightsVector[n];
        float mCouple = coupledMass * weightsVector[n];
        float zCouple = coupledMetals * weightsVector[n];
        float zIICouple = SNIImetals * weightsVector[n];
        float zIACouple = SNIAmetals * weightsVector[n];
        float p3Couple = P3metals * weightsVector[n];
        int np = 1;

        FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
            &CloudParticlePositionZ[n], &GridRank, &np,&mCouple, &density[0], LeftEdge, 
            &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        
        if (StarMakerTypeIaSNe && StarMakerTypeIISNeMetalField) 
            zCouple += zIICouple + zIACouple;
        // printf("Zcpl = %e", zCouple);
        FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
            &CloudParticlePositionZ[n], &GridRank, &np,&zCouple, &metals[0], LeftEdge, 
            &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        if (pX != 0.0){
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank, &np,&pX, &u[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        }
        if (pY != 0.0)
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&pY, &v[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        if (pZ != 0.0)
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&pZ, &w[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        if (coupledEnergy > 0 && DualEnergyFormalism && geCouple > 0)
            eCouple += geCouple;
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&eCouple, &totalEnergy[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
        if (geCouple > 0)
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&geCouple, &gasEnergy[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize); 
        if (StarMakerTypeIISNeMetalField && zIICouple > 0.0)
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&zIICouple, &metalsII[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);             
        if (StarMakerTypeIaSNe && zIACouple > 0.0)
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&zIACouple, &metalsIA[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);  
        if (MechStarsSeedField && p3Couple > 0.0){
            if (printout)printf("Coupling %f to pIII metals %d\n",p3Couple*MassUnits, n);
            FORTRAN_NAME(cic_deposit)(&CloudParticlePositionX[n], &CloudParticlePositionY[n],
                &CloudParticlePositionZ[n], &GridRank,&np,&p3Couple, &metalsIII[0], LeftEdge, 
                &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);  
        }
    }
    /* Deposit one negative mass particle centered on star to account for 
        shell mass leaving host cells .  Same for metals that were evacuated*/
    int np = 1;
    shellMass *= -1/MassUnits;
    FORTRAN_NAME(cic_deposit)(xp, yp, zp, &GridRank,&np,&shellMass, &density[0], LeftEdge, 
        &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
    shellMetals *= -1/MassUnits;   
    FORTRAN_NAME(cic_deposit)(xp, yp, zp, &GridRank,&np,&shellMetals, &metals[0], LeftEdge, 
        &GridDimension[0], &GridDimension[1], &GridDimension[2], &dx, &cloudSize);
    


    /* Couple the faux deposition grids to to the real grids */
    // for (int i = 0; i < size; i++){ 
                
    //         BaryonField[DensNum][i] += density[i];

    //         //Metals transformed back to fractional forms in transform routine
            
    //         BaryonField[MetalNum][i] += metals[i]; 
    //         if (StarMakerTypeIaSNe)
    //             BaryonField[MetalIaNum][i] += metalsIA[i];
    //         if (StarMakerTypeIISNeMetalField)
    //             BaryonField[MetalIINum][i] += metalsII[i];
    //         if (PopIIISupernovaUseColour && SNColourNum != -1) 
    //             BaryonField[SNColourNum][i] += metalsIII[i];
    //         if (PopIIISupernovaUseColour && SNColourNum == -1)
    //             BaryonField[MetalNum][i] += metalsIII[i]; 
    //         BaryonField[TENum][i] +=
    //                     totalEnergy[i]/BaryonField[DensNum][i];
            
    //         BaryonField[GENum][i] +=
    //                     gasEnergy[i]/BaryonField[DensNum][i];
    //         BaryonField[Vel1Num][i] += u[i];
    //         BaryonField[Vel2Num][i] += v[i];
    //         BaryonField[Vel3Num][i] += w[i];
    // }

    if (criticalDebug && !winds){
        for (int i = 0; i< size ; i++){
            postMass += BaryonField[DensNum][i];
            postZ += BaryonField[MetalNum][i];
            if (MetalIINum > 0)
                postZII += BaryonField[MetalIINum][i];
            if (MetalIaNum > 0)
                postZIa += BaryonField[MetalIaNum][i];
            if (SNColourNum > 0) postZ += BaryonField[SNColourNum][i];
            postP += BaryonField[Vel1Num][i]+BaryonField[Vel2Num][i]+BaryonField[Vel3Num][i];
            postPmag += pow(BaryonField[Vel1Num][i]*MomentaUnits,2)+
                pow(BaryonField[Vel2Num][i]*MomentaUnits,2)
                +pow(BaryonField[Vel3Num][i]*MomentaUnits,2);
            postTE += BaryonField[TENum][i];
            postGE += BaryonField[GENum][i];
        }
        fprintf(stderr, "Difference quantities: dxRatio = %f dMass = %e dZ = %e dzII = %e dxIa = %e  P = %e |P| = %e TE = %e GE = %e coupledGE = %e Ej = %e Mej = %e Zej = %e\n",
                dxRatio, (postMass-preMass)*MassUnits, (postZ-preZ)*MassUnits, 
                (postZII-preZII)*MassUnits, (postZIa-preZIa)*MassUnits,
                (postP - preP)*MomentaUnits,
                (sqrt(postPmag) - sqrt(prePmag)),
                 (postTE-preTE)*EnergyUnits, (postGE-preGE)*EnergyUnits,
                coupledGasEnergy*EnergyUnits*nCouple, ejectaEnergy, 
                ejectaMass, ejectaMetal);
        if(isnan(postMass) || isnan(postTE) || isnan(postPmag) || isnan(postZ)){
            fprintf(stderr, "NAN IN GRID: %e %e %e %e\n", postMass, postTE, postZ, postP);
            ENZO_FAIL("MechStars_depositFeedback.C: 395\n")
        if (postGE-preGE < 0.0)
            ENZO_FAIL("471");
        }
    }

    /* Transform the grid back */

    transformComovingWithStar(BaryonField[DensNum], BaryonField[MetalNum], 
                        BaryonField[MetalIINum], BaryonField[MetalIaNum],
                        BaryonField[Vel1Num],BaryonField[Vel2Num],
                        BaryonField[Vel3Num],
                        BaryonField[TENum], BaryonField[GENum],
                        *up, *vp, *wp, 
                        GridDimension[0], GridDimension[1],
                        GridDimension[2], -1);


    // delete [] u,v,w, gasEnergy, totalEnergy;
    return SUCCESS;
}