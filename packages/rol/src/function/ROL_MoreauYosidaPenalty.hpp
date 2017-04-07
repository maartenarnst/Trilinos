// @HEADER
// ************************************************************************
//
//               Rapid Optimization Library (ROL) Package
//                 Copyright (2014) Sandia Corporation
//
// Under terms of Contract DE-AC04-94AL85000, there is a non-exclusive
// license for use of this work by or on behalf of the U.S. Government.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// 3. Neither the name of the Corporation nor the names of the
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY SANDIA CORPORATION "AS IS" AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL SANDIA CORPORATION OR THE
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// Questions? Contact lead developers:
//              Drew Kouri   (dpkouri@sandia.gov) and
//              Denis Ridzal (dridzal@sandia.gov)
//
// ************************************************************************
// @HEADER

#ifndef ROL_MOREAUYOSIDAPENALTY_H
#define ROL_MOREAUYOSIDAPENALTY_H

#include "ROL_Objective.hpp"
#include "ROL_BoundConstraint.hpp"
#include "ROL_Vector.hpp"
#include "ROL_Types.hpp"
#include "Teuchos_RCP.hpp"
#include <iostream>

/** @ingroup func_group
    \class ROL::MoreauYosidaPenalty
    \brief Provides the interface to evaluate the Moreau-Yosida penalty function.

    ---
*/


namespace ROL {

template <class Real>
class MoreauYosidaPenalty : public Objective<Real> {
private:
  const Teuchos::RCP<Objective<Real> > obj_;
  const Teuchos::RCP<BoundConstraint<Real> > con_;

  Teuchos::RCP<Vector<Real> > g_;
  Teuchos::RCP<Vector<Real> > l_;
  Teuchos::RCP<Vector<Real> > u_;
  Teuchos::RCP<Vector<Real> > l1_;
  Teuchos::RCP<Vector<Real> > u1_;
  Teuchos::RCP<Vector<Real> > dl1_;
  Teuchos::RCP<Vector<Real> > du1_;
  Teuchos::RCP<Vector<Real> > xlam_;
  Teuchos::RCP<Vector<Real> > v_;
  Teuchos::RCP<Vector<Real> > dv_;
  Teuchos::RCP<Vector<Real> > dv2_;
  Teuchos::RCP<Vector<Real> > lam_;
  Teuchos::RCP<Vector<Real> > tmp_;

  Real mu_;
  Real fval_;
  bool isConEvaluated_;
  int nfval_;
  int ngval_;

  void computePenalty(const Vector<Real> &x) {
    if ( con_->isActivated() ) {
      Real one = 1.0;
      if ( !isConEvaluated_ ) {
        xlam_->set(x);
        xlam_->axpy(one/mu_,*lam_);

        if ( con_->isFeasible(*xlam_) ) {
          l1_->zero(); dl1_->zero();
          u1_->zero(); du1_->zero();
        }
        else {
          // Compute lower penalty component
          l1_->set(*l_);
          con_->pruneLowerInactive(*l1_,*xlam_);
          tmp_->set(*xlam_);
          con_->pruneLowerInactive(*tmp_,*xlam_);
          l1_->axpy(-one,*tmp_);

          // Compute upper penalty component
          u1_->set(*xlam_);
          con_->pruneUpperInactive(*u1_,*xlam_);
          tmp_->set(*u_);
          con_->pruneUpperInactive(*tmp_,*xlam_);
          u1_->axpy(-one,*tmp_);

          // Compute derivative of lower penalty component
          dl1_->set(l1_->dual());
          con_->pruneLowerInactive(*dl1_,*xlam_);

          // Compute derivative of upper penalty component
          du1_->set(u1_->dual());
          con_->pruneUpperInactive(*du1_,*xlam_);
        }

        isConEvaluated_ = true;
      }
    }
  }

public:
  ~MoreauYosidaPenalty() {}

  MoreauYosidaPenalty(const Teuchos::RCP<Objective<Real> > &obj,
                      const Teuchos::RCP<BoundConstraint<Real> > &con, 
                      const ROL::Vector<Real> &x, const Real mu = 1.0)
    : obj_(obj), con_(con), mu_(mu),
      fval_(0), isConEvaluated_(false), nfval_(0), ngval_(0) {

    g_    = x.dual().clone();
    l_    = x.clone();
    l1_   = x.clone();
    dl1_  = x.dual().clone();
    u_    = x.clone();
    u1_   = x.clone();
    du1_  = x.dual().clone();
    xlam_ = x.clone();
    v_    = x.clone();
    dv_   = x.dual().clone();
    dv2_  = x.dual().clone();
    lam_  = x.clone();
    tmp_  = x.clone();

    con_->setVectorToLowerBound(*l_);
    con_->setVectorToUpperBound(*u_);

    lam_->zero();
    //lam_->set(*u_);
    //lam_->plus(*l_);
    //lam_->scale(0.5);
  }

  void updateMultipliers(Real mu, const ROL::Vector<Real> &x) {
    if ( con_->isActivated() ) {
      Real one = 1.0;
      computePenalty(x);

      lam_->set(*u1_);
      lam_->axpy(-one,*l1_);
      lam_->scale(mu_);

      mu_ = mu;
    }

    nfval_ = 0; ngval_ = 0;
    isConEvaluated_ = false;
  }

  Real getObjectiveValue(void) const {
    return fval_;
  }

  Teuchos::RCP<Vector<Real> > getGradient(void) const {
    return g_;
  }

  int getNumberFunctionEvaluations(void) {
    return nfval_;
  }

  int getNumberGradientEvaluations(void) {
    return ngval_;
  }

  /** \brief Update Moreau-Yosida penalty function. 

      This function updates the Moreau-Yosida penalty function at new iterations. 
      @param[in]          x      is the new iterate. 
      @param[in]          flag   is true if the iterate has changed.
      @param[in]          iter   is the outer algorithm iterations count.
  */
  void update( const Vector<Real> &x, bool flag = true, int iter = -1 ) {
    obj_->update(x,flag,iter);
    con_->update(x,flag,iter);
    isConEvaluated_ = false;
  }

  /** \brief Compute value.

      This function returns the Moreau-Yosida penalty value.
      @param[in]          x   is the current iterate.
      @param[in]          tol is a tolerance for inexact Moreau-Yosida penalty computation.
  */
  Real value( const Vector<Real> &x, Real &tol ) {
    Real half = 0.5;
    // Compute objective function value
    fval_ = obj_->value(x,tol);
    nfval_++;
    // Add value of the Moreau-Yosida penalty
    Real fval = fval_;
    if ( con_->isActivated() ) {
      computePenalty(x);
      fval += half*mu_*(l1_->dot(*l1_) + u1_->dot(*u1_));
    }
    return fval;
  }

  /** \brief Compute gradient.

      This function returns the Moreau-Yosida penalty gradient.
      @param[out]         g   is the gradient.
      @param[in]          x   is the current iterate.
      @param[in]          tol is a tolerance for inexact Moreau-Yosida penalty computation.
  */
  void gradient( Vector<Real> &g, const Vector<Real> &x, Real &tol ) {
    // Compute gradient of objective function
    obj_->gradient(*g_,x,tol);
    ngval_++;
    g.set(*g_);
    // Add gradient of the Moreau-Yosida penalty
    if ( con_->isActivated() ) {
      computePenalty(x);
      g.axpy(-mu_,*dl1_);
      g.axpy(mu_,*du1_);
    }
  }

  /** \brief Apply Hessian approximation to vector.

      This function applies the Hessian of the Moreau-Yosida penalty to the vector \f$v\f$.
      @param[out]         hv  is the the action of the Hessian on \f$v\f$.
      @param[in]          v   is the direction vector.
      @param[in]          x   is the current iterate.
      @param[in]          tol is a tolerance for inexact Moreau-Yosida penalty computation.
  */
  void hessVec( Vector<Real> &hv, const Vector<Real> &v, const Vector<Real> &x, Real &tol ) {
    // Apply objective Hessian to a vector
    obj_->hessVec(hv,v,x,tol);
    // Add Hessian of the Moreau-Yosida penalty
    if ( con_->isActivated() ) {
      Real one = 1.0;
      computePenalty(x);

      v_->set(v);
      con_->pruneLowerActive(*v_,*xlam_);
      v_->scale(-one);
      v_->plus(v);
      dv_->set(v_->dual());
      dv2_->set(*dv_);
      con_->pruneLowerActive(*dv_,*xlam_);
      dv_->scale(-one);
      dv_->plus(*dv2_);
      hv.axpy(mu_,*dv_);

      v_->set(v);
      con_->pruneUpperActive(*v_,*xlam_);
      v_->scale(-one);
      v_->plus(v);
      dv_->set(v_->dual());
      dv2_->set(*dv_);
      con_->pruneUpperActive(*dv_,*xlam_);
      dv_->scale(-one);
      dv_->plus(*dv2_);
      hv.axpy(mu_,*dv_);
    }
  }

// Definitions for parametrized (stochastic) objective functions
public:
  void setParameter(const std::vector<Real> &param) {
    Objective<Real>::setParameter(param);
    obj_->setParameter(param);
  }
}; // class MoreauYosidaPenalty

} // namespace ROL

#endif