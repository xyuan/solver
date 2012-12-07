#include "field.h"
#include "turbulence.h"
#include "ke.h"
#include "rngke.h"
#include "realizableke.h"
#include "kw.h"
#include "les.h"
#include "mp.h"
#include "system.h"
#include "solve.h"
#include "vtk.h"

using namespace std;

/*general properties*/
namespace GENERAL {
	Scalar density = 1;
	Scalar viscosity = 1e-5;
	Scalar conductivity = 1e-4;
	Vector gravity = Vector(0,0,-9.81);

	void enroll(Util::ParamList& params) {
		params.enroll("rho",&density);
		params.enroll("viscosity",&viscosity);
		params.enroll("conductivity",&conductivity);
		params.enroll("gravity",&gravity);
	}
};

/*solvers*/
void piso(istream&);
void diffusion(istream&);
void potential(istream&);
void transport(istream&);

/********************
 * Main application
 *******************/
int main(int argc,char* argv[]) {

	/*message passing object*/
	MP mp(argc,argv);
	ifstream input(argv[1]);

	/*main options*/
	Util::ParamList params("general");
	string sname;
	params.enroll("solver",&sname);
	params.enroll("mesh",&Mesh::gMeshName);
	Mesh::enroll(params);
	GENERAL::enroll(params);
	params.read(input);

	/*Mesh*/
	if(mp.n_hosts > 1) {
		stringstream s;
		s << Mesh::gMeshName << mp.host_id;
		Mesh::gMeshName = s.str();
		if(!System::cd(Mesh::gMeshName))
			return 1;
	}
	Mesh::readMesh();
	Mesh::initGeomMeshFields();
	atexit(Util::cleanup);

	/*call solver*/
	if(!Util::compare(sname,"piso")) {
		piso(input);
	} else if(!Util::compare(sname,"diffusion")) {
		diffusion(input);
	} else if(!Util::compare(sname,"transport")) {
		transport(input);
	} else if(!Util::compare(sname,"potential")) {
		potential(input);
	}

	return 0;
}

/***************************************************************************
 Navier stokes solver using PISO algorithm
 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 References:
	Hrvoje Jasak, "Error analysis and estimation of FVM with 
	applications to fluid flow".
 Description:
    The PISO algorithm is used to solve NS equations on collocated grids 
	using Rhie-Chow interpolation to avoid wiggles in pressure field.

	Prediction
	~~~~~~~~~~
	Discretize and solve the momenum equation with current values of pressure. 
	The velocities obtained will not satisfy continuity unless exact pressure 
	was specified. 

	Correction
	~~~~~~~~~~
	Step 1) 
	  Determine velocity with all terms included except pressure gradient contrib.
	      ap * Up = H(U) - grad(p)
		  Up = H(U) / ap - grad(p) / ap
      Droping grad(p) term:
          Ua = H(U) / ap
    Step 2)
      Solve poisson pressure equation to satisfy continuity with fluxes calculated 
	  from interpolated Ua.
	      div(Up) = 0
		  div(1/ap * grad(p)) = div(H(U)/ap)
		  lap(p,1/ap) = div(Ua)
    Step 3)
	  Correct the velocity with gradient of newly found pressure
	      U -= grad(p)
    These steps are repeated two or more times for transient solutions.
	For steady state problems one is enough.

	Deferred correction approach is used to handle explicit terms from higher order 
	discretization schemes such as CDS and TVD schemes, boundary conditions etc... 
	Thus we repeat the prediction/correction steps one or more times. 
	Again for steady state problems once is enough. 
	
 *************************************************************************/
void piso(istream& input) {
	/*Solver specific parameters*/
	Scalar& rho = GENERAL::density;
	Scalar& viscosity = GENERAL::viscosity;
	Scalar velocity_UR = Scalar(0.8);
	Scalar pressure_UR = Scalar(0.5);
	Int n_PISO = 1;
	Int n_DEFERRED = 0;
	Int n_ORTHO = 0;
	Int LESaverage = 0;

	/*piso options*/
	Util::ParamList params("piso");
	params.enroll("velocity_UR",&velocity_UR);
	params.enroll("pressure_UR",&pressure_UR);
	params.enroll("n_PISO",&n_PISO);
	params.enroll("n_ORTHO",&n_ORTHO);
	params.enroll("n_DEFERRED",&n_DEFERRED);

	VectorCellField U("U",READWRITE);
	ScalarCellField p("p",READWRITE);

	/*turbulence model*/
	enum TurbModel {NONE,MIXING_LENGTH,KE,RNG_KE,REALIZABLE_KE,KW,LES};
	TurbModel turb_model = KE;
	Util::Option* op;
	op = new Util::Option(&turb_model,7,
		"NONE","MIXING_LENGTH","KE","RNG_KE","REALIZABLE_KE","KW","LES");
	params.enroll("turbulence_model",op);
	op = new Util::BoolOption(&LESaverage);
	params.enroll("les_average",op);
	params.read(input);

	/*Select turbulence model*/
	ScalarFacetField F;
	bool Steady,needWallDist = false;

	Turbulence_Model* turb;
	switch(turb_model) {
		case KE:   
			turb = new KE_Model(U,F,rho,viscosity,Steady); 
			break;
		case MIXING_LENGTH:   
			needWallDist = true;
			turb = new MixingLength_Model(U,F,rho,viscosity,Steady); 
			break;
		case RNG_KE:   
			turb = new RNG_KE_Model(U,F,rho,viscosity,Steady); 
			break;
		case REALIZABLE_KE:   
			turb = new REALIZABLE_KE_Model(U,F,rho,viscosity,Steady); 
			break;
		case KW:   
			turb = new KW_Model(U,F,rho,viscosity,Steady); 
			break;
		case LES:  
			needWallDist = true;
			turb = new LES_Model(U,F,rho,viscosity,Steady); 
			break;
		default:
			turb = new Turbulence_Model(U,F,rho,viscosity,Steady); 
			break;
	}
	turb->enroll();

	/*read parameters*/
	Util::read_params(input);

	/*average statistics for LES */
	VectorCellField Uavg(false),Ustd(false);
	ScalarCellField pavg(false),pstd(false);
	if(LESaverage) {
		Uavg.construct("Uavg",READWRITE);
		Ustd.construct("Ustd",READWRITE);
		pavg.construct("pavg",READWRITE);
		pstd.construct("pstd",READWRITE);
	}

	/*instantaneous values*/
	IntVector probe_points;
	for(Int j = 0;j < Mesh::probePoints.size();j++) {
		Vector v = Mesh::probePoints[j];
		Int index = Mesh::findNearest(v);
		probe_points.push_back(index);
	}
	Int probe = probe_points.size();
	ofstream oUi,opi;
	if(probe) {
		oUi.open("Ui");
		opi.open("pi");
	}

	/*Read at selected start time step*/
	Int step,start;
	step = Controls::start_step / Controls::write_interval;
	start = Controls::write_interval * step + 1;
	Mesh::read_fields(step);
	Util::write_vtk(step);

	/*wall distance*/
	if(needWallDist) {
		MP::print("Calculating wall distance.\n");
		Mesh::calc_walldist(step);
		MP::print("Finished.\n");
	}
	/*time*/
	Scalar time_factor = Controls::time_scheme_factor;
	Steady = (Controls::state == Controls::STEADY);
	if(Steady) n_DEFERRED = 0;

	/*Calculate for each time step*/
	VectorCellField gP = -gradV(p);
	F = flx(rho * U); 

	for(Int i = start; i <= Controls::end_step; i++) {
		/*Print step*/
		if(MP::host_id == 0) {
			if(Steady)
				MP::printH("Step %d\n",i);
			else
				MP::printH("Time %f\n",i * Controls::dt);
		}

		/*Deferred corrections loop in case of large time steps*/
		for(Int n = 0;n <= n_DEFERRED;n++) {

			/*Momentum and pressure solution*/
			{
				VectorMeshMatrix M;
				{
					/*convection*/
					{
						ScalarFacetField mu = rho * viscosity;
						M = div(U,F,mu);
					}
					/*turbulent stress*/
					turb->addTurbulentStress(M);
					/*end*/
				}
				/*relax if steady state otherwise add time contribution*/
				if(Steady)
					M.Relax(velocity_UR);
				else {
					/*crank nicolson*/
					if(!equal(time_factor,1)) {
						VectorCellField po = M * U;
						M *= time_factor;
						M.Su -= (1 - time_factor) * po;
					}
					/*time derivative*/
					M += ddt(U,rho);
				}
				/*solve momentum equation*/
				Solve(M == gP);

				/*1/ap*/
				ScalarCellField api = (1 / M.ap);

				/*PISO loop*/
				for(Int j = 0;j < n_PISO;j++) {
					/* Ua = H(U) / ap*/
					U = getRHS(M) * api;
					updateExplicitBCs(U,true);
					/*solve pressure poisson equation to satisfy continuity*/
					{
						ScalarCellField po;
						if(Steady)
							po = p;
						for(Int k = 0;k <= n_ORTHO;k++)
							Solve((lap(p,rho * api * Mesh::cV) += div(rho * U)));
						if(Steady)
							p.Relax(po,pressure_UR);
					}
					gP = -gradV(p);
					/*explicit velocity correction : add pressure contribution*/
					U -= gP * api;
					updateExplicitBCs(U,true);
					/*end*/
				}
			}
			/*update fluctuations*/
			updateExplicitBCs(U,true,true);
			F = flx(rho * U);

			/*solve transport equations*/
			turb->solve();
		}

		/*average*/
		if(LESaverage) {
			Uavg += U;
			pavg += p;
			Ustd += (U * U);
			pstd += (p * p);
		}

		/*store instantaneous values for some locations*/
		if(probe) {
			oUi << i << " ";
			opi << i << " ";
			for(Int j = 0;j < probe_points.size();j++) {
				oUi << U[probe_points[j]] << " ";
				opi << p[probe_points[j]] << " ";
			}
			oUi << endl;
			opi << endl;
		}

		/*write result to file*/
		if((i % Controls::write_interval) == 0) {
			step = i / Controls::write_interval;

			if(LESaverage) {
				VectorCellField Ua = Uavg,Us = Ustd;
				ScalarCellField pa = pavg,ps = pstd;
				Scalar n = Scalar(i);
				Uavg /= n;
				pavg /= n;
				Ustd += Uavg * (n * Uavg - 2 * Ua);
				pstd += pavg * (n * pavg - 2 * pa);
				Ustd = sqrt(Ustd / n);
				pstd = sqrt(pstd / n);

				Mesh::write_fields(step);
				Util::write_vtk(step);

				Uavg = Ua;
				Ustd = Us;
				pavg = pa;
				pstd = ps;
			} else {
				Mesh::write_fields(step);
				Util::write_vtk(step);
			}
		}
		/*end*/
	}
}
/********************************************
 Diffusion solver
 ~~~~~~~~~~~~~~~~
 Solver for pdes of the the parabolic heat equation type:
       d(rho*u)/dt = lap(T,rho*DT)
 ********************************************/
void diffusion(istream& input) {
	/*Solver specific parameters*/
	Scalar& rho = GENERAL::density;
	Scalar DT = Scalar(1);
	Scalar t_UR = Scalar(1);
	Int n_DEFERRED = 0;

	/*diffusion*/
	Util::ParamList params("diffusion");
	params.enroll("DT",&DT);
	params.enroll("t_UR",&t_UR);
	params.enroll("n_DEFERRED",&n_DEFERRED);

	ScalarCellField T("T",READWRITE);

	/*read parameters*/
	Util::read_params(input);

	/*Read at selected start time step*/
	Int step,start;
	step = Controls::start_step / Controls::write_interval;
	start = Controls::write_interval * step + 1;
	Mesh::read_fields(step);
	Util::write_vtk(step);

	/*time*/
	Scalar time_factor = Controls::time_scheme_factor;
	bool Steady = (Controls::state == Controls::STEADY);
	if(Steady) n_DEFERRED = 0;

	/*Calculate for each time step*/
	ScalarFacetField mu = rho * DT;

	for(Int i = start; i <= Controls::end_step; i++) {
		/*Print step*/
		if(MP::host_id == 0) {
			if(Steady)
				MP::printH("Step %d\n",i);
			else
				MP::printH("Time %f\n",i * Controls::dt);
		}
		/*Loop for large time steps*/
		for(Int n = 0;n <= n_DEFERRED;n++) {
			ScalarMeshMatrix M;

			M = -lap(T,mu);

			if(Steady)
				M.Relax(t_UR);
			else {
				if(!equal(time_factor,1)) {
					ScalarCellField po = M * T;
					M *= time_factor;
					M.Su -= (1 - time_factor) * po;
				}
				M += ddt(T,rho);
			}

			Solve(M);
		}
		
		/*write result to file*/
		if((i % Controls::write_interval) == 0) {
			step = i / Controls::write_interval;
			Mesh::write_fields(step);
			Util::write_vtk(step);
		}
	}
}
/***********************************************
  Transport equation solver
  ~~~~~~~~~~~~~~~~~~~~~~~~~
  Given a flow field (U) and values of a scalar at the boundaries, 
  the solver determines the distribution of the scalar.
     dT/dt + div(T,F,mu) = lap(T,mu)
 **********************************************/
void transport(istream& input) {
	/*Solver specific parameters*/
	Scalar& rho = GENERAL::density;
	Scalar DT = Scalar(4e-2);
	Scalar t_UR = Scalar(1);
	Int n_DEFERRED = 0;

	/*transport*/
	Util::ParamList params("transport");
	params.enroll("DT",&DT);
	params.enroll("t_UR",&t_UR);
	params.enroll("n_DEFERRED",&n_DEFERRED);

	VectorCellField U("U",READWRITE);
	ScalarCellField T("T",READWRITE);

	/*read parameters*/
	Util::read_params(input);

	/*Read at selected start time step*/
	Int step,start;
	step = Controls::start_step / Controls::write_interval;
	start = Controls::write_interval * step + 1; 
	Mesh::read_fields(step);
	Util::write_vtk(step);

	/*time*/
	Scalar time_factor = Controls::time_scheme_factor;
	bool Steady = (Controls::state == Controls::STEADY);
	if(Steady) n_DEFERRED = 0;

	/*Calculate for each time step*/
	ScalarFacetField F,mu = rho * DT,gamma;

	for(Int i = start; i <= Controls::end_step; i++) {
		/*Print step*/
		if(MP::host_id == 0) {
			if(Steady)
				MP::printH("Step %d\n",i);
			else
				MP::printH("Time %f\n",i * Controls::dt);
		}

		/*Loop for large time steps*/
		for(Int n = 0;n <= n_DEFERRED;n++) {
			ScalarMeshMatrix M;

			F = flx(rho * U); 
			M = div(T,F,mu) 
				- lap(T,mu);

			if(Steady)
				M.Relax(t_UR);
			else {
				if(!equal(time_factor,1)) {
					ScalarCellField po = M * T;
					M *= time_factor;
					M.Su -= (1 - time_factor) * po;
				}
				M += ddt(T,rho);
			}

			Solve(M);
		}
		/*write result to file*/
		if((i % Controls::write_interval) == 0) {
			step = i / Controls::write_interval;
			Mesh::write_fields(step);
			Util::write_vtk(step);
		}
	}
}
/**************************
    Potential flow solver
	~~~~~~~~~~~~~~~~~~~~~
	In potential flow the velocity field is irrotational (vorticity = curl(U) = 0).
	This assumption fails for boundary layers and wakes that exhibit strong vorticity,
	but it can still be used to initialize the flow field for further simulations.

	For incompressible flow
	       div(U) = 0
    Velocity is the gradient of velocity potential phi
	       U = grad(phi)
		   div(grad(phi)) = 0
		   lap(phi) = 0
    phi is pressure for this solver. The initial flow field will inevitably not satisfy 
	continuity due to imposed boundary conditons. Therefore we solve a pressure poisson 
	equation and then correct the velocity with the gradient of p.
	       lap(p) = div(U)
		   U -= grad(p)
 **************************/
void potential(istream& input) {
	/*Solver specific parameters*/
	Int n_ORTHO = 0;

	/*potential options*/
	Util::ParamList params("potential");
	params.enroll("n_ORTHO",&n_ORTHO);

	VectorCellField U("U",READWRITE);
	ScalarCellField p("p",READ);
	
	/*read parameters*/
	Util::read_params(input);

	/*Read at selected start time step*/
	Int step,start;
	step = Controls::start_step / Controls::write_interval;
	start = Controls::write_interval * step + 1;
	Mesh::read_fields(step);

	/*set internal field to zero*/
	for(Int i = 0;i < Mesh::gBCellsStart;i++) {
		U[i] = Vector(0,0,0);
		p[i] = Scalar(0);
	}
	updateExplicitBCs(U,true);
	updateExplicitBCs(p,true);

	/*solve potential equation*/
	ScalarCellField divU = div(U);
	ScalarFacetField one = Scalar(1);
	for(Int k = 0;k <= n_ORTHO;k++)
		Solve(lap(p,one) == divU);

	/*correct velocity*/
	U -= grad(p);
	updateExplicitBCs(U,true);

	/*write result to file*/
	Mesh::write_fields(step);
	Util::write_vtk(step);
}
/**********************************************************************************
 Wall distance
 ~~~~~~~~~~~~~
	Reference:
	   D.B.Spalding, ‘Calculation of turbulent heat transfer in cluttered spaces
    Description:
	   Poisson equation is solved to get approximate nearest wall distance.
	         lap(phi,1) = -cV
	   The boundary conditions are phi=0 at walls, and grad(phi) = 0 elsewhere.
**********************************************************************************/
void Mesh::calc_walldist(Int step) {
	ScalarCellField phi;
	/*internal*/
	phi = Scalar(0);
	/*boundary*/
	BCondition<Scalar>* bc;
	for(Boundaries::iterator it = gBoundaries.begin();it != gBoundaries.end();++it) {
		string bname = it->first;
		bc = new BCondition<Scalar>(phi.fName);
		bc->bname = bname;
		if(bname.find("WALL") != std::string::npos) {
			bc->cname = "DIRICHLET";
			bc->value = Scalar(0);
		} else {
			bc->cname = "NEUMANN";
			bc->value = Scalar(0);
		}
		bc->init_indices();
		AllBConditions.push_back(bc);
	}
	updateExplicitBCs(phi,true,true);
    /*poisson equation*/
	{
		ScalarFacetField one = Scalar(1);
		Solve(lap(phi,one) == -cV);
	}
	/*wall distance*/
	{
		VectorCellField g = grad(phi);
		g.FillBoundaryValues();
		yWall = sqrt((g & g) + 2 * phi) - mag(g);
	}
	/*write it*/
	yWall.access = WRITE;
	Mesh::write_fields(step);
	Util::write_vtk(step);
	yWall.access = NONE;
}


