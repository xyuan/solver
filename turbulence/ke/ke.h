#ifndef __KE_H
#define __KE_H

#include "turbulence.h"

struct KE_Model : public KX_Model {
	/*constructor*/
	KE_Model(VectorCellField&,ScalarFacetField&,Scalar&,Scalar&,bool&);

	/*others*/
	virtual void enroll();
	virtual void solve();
	void calcEddyMu() {
		eddy_mu = (rho * Cmu * k * k) / x;
	};
	Scalar calcX(Scalar ustar,Scalar kappa,Scalar y) {
		return pow(ustar,Scalar(3)) / (kappa * y);
	}
};

#endif