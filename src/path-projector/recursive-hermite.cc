// Copyright (c) 2016, Joseph Mirabel
// Authors: Joseph Mirabel (joseph.mirabel@laas.fr)
//

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright
// notice, this list of conditions and the following disclaimer in the
// documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
// DAMAGE.

#include <hpp/constraints/differentiable-function.hh>
#include <hpp/constraints/implicit.hh>
#include <hpp/core/config-projector.hh>
#include <hpp/core/config-validations.hh>
#include <hpp/core/configuration-shooter.hh>
#include <hpp/core/path-validation.hh>
#include <hpp/core/problem.hh>
#include <hpp/core/roadmap.hh>
#include <hpp/core/path-projector.hh>
#include <hpp/core/path.hh>
#include <hpp/pinocchio/device-sync.hh>
#include <hpp/pinocchio/liegroup-element.hh>
#include <hpp/pinocchio/util.hh>
#include <hpp/util/exception-factory.hh>
#include <pinocchio/multibody/data.hpp>
#include <hpp/core/edge.hh>
#include <hpp/core/nearest-neighbor.hh>
#include <hpp/core/node.hh>
#include <hpp/core/path-planner.hh>
#include <hpp/core/path-planning-failed.hh>
#include <hpp/core/path-projector.hh>
#include <hpp/core/path-validation.hh>
#include <hpp/core/path.hh>
#include <hpp/core/problem-target/goal-configurations.hh>
#include <hpp/core/problem.hh>
#include <hpp/core/roadmap.hh>
#include <hpp/core/steering-method.hh>
#include <hpp/util/debug.hh>
#include <hpp/util/timer.hh>
#include <tuple>
#include <hpp/core/path/hermite.hh>
#include <hpp/core/path-vector.hh>
#include <hpp/core/path-projector/recursive-hermite.hh>
#include <iostream>
#include <typeinfo>
using namespace std;
#include "hpp/core/path-projector/recursive-hermite.hh"

#include <hpp/core/problem.hh>
#include <hpp/core/config-projector.hh>
#include <hpp/core/interpolated-path.hh>
#include <hpp/core/path-vector.hh>
#include <hpp/core/path/hermite.hh>
#include <hpp/core/steering-method/hermite.hh>
#include <hpp/util/timer.hh>
#include <limits>
#include <queue>
#include <stack>

namespace hpp {
namespace core {
namespace pathProjector {
RecursiveHermitePtr_t RecursiveHermite::create(
    const DistancePtr_t& distance, const SteeringMethodPtr_t& steeringMethod,
    value_type step) {
  value_type beta = steeringMethod->problem()
                        ->getParameter("PathProjection/RecursiveHermite/Beta")
                        .floatValue();
  hppDout(info, "beta is " << beta);
  return RecursiveHermitePtr_t(
      new RecursiveHermite(distance, steeringMethod, step, beta));
}

RecursiveHermitePtr_t RecursiveHermite::create(const ProblemConstPtr_t& problem,
                                               const value_type& step) {
  return create(problem->distance(), problem->steeringMethod(), step);
}

RecursiveHermite::RecursiveHermite(const DistancePtr_t& distance,
                                   const SteeringMethodPtr_t& steeringMethod,
                                   const value_type& M, const value_type& beta)
    : PathProjector(distance, steeringMethod, false), M_(M), beta_(beta) {
  // beta should be between 0.5 and 1.
  if (beta_ < 0.5 || 1 < beta_)
    throw std::invalid_argument("Beta should be between 0.5 and 1");
  if (!HPP_DYNAMIC_PTR_CAST(hpp::core::steeringMethod::Hermite, steeringMethod))
    throw std::invalid_argument("Steering method should be of type Hermite");
}

bool RecursiveHermite::impl_apply(const PathPtr_t& path,
                                  core::PathPtr_t& proj) const {
  assert(path);
  bool success = false;
  PathVectorPtr_t pv = HPP_DYNAMIC_PTR_CAST(PathVector, path);
  if (!pv) {
    cout << "Passed in the good part of impl_apply" << endl;
    if (!path->constraints() || !path->constraints()->configProjector()) {
      proj = path;
      success = true;
    } else {
      success = project(path, proj);
    }
  } else {
    PathVectorPtr_t res =
        PathVector::create(pv->outputSize(), pv->outputDerivativeSize());
    PathPtr_t part;
    success = true;
    for (size_t i = 0; i < pv->numberPaths(); i++) {
      if (!apply(pv->pathAtRank(i), part)) {
        // We add the path only if part is not NULL and:
        // - either its length is not zero,
        // - or it's not the first one.
        if (part && (part->length() > 0 || i == 0)) {
          res->appendPath(part);
        }
        success = false;
        break;
      }
      res->appendPath(part);
    }
    proj = res;
  }
  assert(proj);
  assert((proj->initial() - path->initial()).isZero());
  assert(!success || (proj->end() - path->end()).isZero());
  proj->checkPath();
  cout << success << endl;

  return success;
}

bool RecursiveHermite::project(const PathPtr_t& path, PathPtr_t& proj) const {
  ConstraintSetPtr_t constraints = path->constraints();
  if (!constraints) {
    proj = path;
    return true;
  }
  const Configuration_t q1 = path->initial();
  const Configuration_t q2 = path->end();
  if (!constraints->isSatisfied(q2)) return false;
  const ConfigProjectorPtr_t& cp = constraints->configProjector();
  if (!cp || cp->dimension() == 0) {
    proj = path;
    return true;
  }
  steeringMethod_->constraints(constraints);
  ProblemConstPtr_t Problem(steeringMethod_->problem());
  ConfigValidationPtr_t cfgValidation(Problem->configValidations());
  core::PathValidationPtr_t pathValidation(Problem->pathValidation());
  ValidationReportPtr_t cfgReport;

  core::PathValidationReportPtr_t pathReport;

  const value_type thr = 2 * cp->errorThreshold() / 2;
  cout << "error threshold : " << cp->errorThreshold() << " M : " << M_ << endl;
  std::vector<HermitePtr_t> ps;
  HermitePtr_t p = HPP_DYNAMIC_PTR_CAST(Hermite, path);
  if (!p) {
    InterpolatedPathPtr_t ip = HPP_DYNAMIC_PTR_CAST(InterpolatedPath, path);
    if (ip) {
      typedef InterpolatedPath::InterpolationPoints_t IPs_t;
      const IPs_t& ips = ip->interpolationPoints();
      cout << "ips length = " << ips.size() << endl;
      int uuuuuu = 0;
      for (auto itera = ips.begin(); itera != ips.end(); itera++){
        cout << uuuuuu << endl << "ips values : \n" << itera->second << endl;
        uuuuuu++;
      }
      ps.reserve(ips.size() - 1);
      IPs_t::const_iterator _ip1 = ips.begin();
      std::advance(_ip1, 1);
      int iter (0);
      for (IPs_t::const_iterator _ip0 = ips.begin(); _ip1 != ips.end();
           ++_ip0) {
        interval_t timeRange (interpolationTimes_[iter], interpolationTimes_[iter+1]);
        cout << "timeRange : " << timeRange.first <<" , " << timeRange.second << endl;
        HermitePtr_t to_add = HPP_DYNAMIC_PTR_CAST(Hermite, steer_with_timeRange(_ip0->second, _ip1->second,timeRange));
        ps.push_back(to_add);
        iter++;
        ++_ip1;
      }
    } else {
      p = HPP_DYNAMIC_PTR_CAST(Hermite, steer(path->initial(), path->end()));
      ps.push_back(p);
    }
  } else {
    ps.push_back(p);
  }
  core::PathPtr_t validPart;
  PathVectorPtr_t res =
      PathVector::create(path->outputSize(), path->outputDerivativeSize());
  bool success = true;
  cout << "Passed after res initilized \n ps.size() = "<< ps.size() << endl;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    p = ps[i];
    p->computeHermiteLength();
    //
    if (p->hermiteLength() < thr) {
      
      res->appendPath(p);
      continue;
    //
    }
    cout << "passed eee" << endl;
    PathVectorPtr_t r =
        PathVector::create(path->outputSize(), path->outputDerivativeSize());
    cout << "Just before recurse\n\n\n" << endl;
    success = recurse(p, r, thr);
    if (success) {
      cout << "recurse fine" << endl;
      cout << r->numberPaths() << endl;
    }
    cout << "After recurse" << endl;
    res->concatenate(r);
    if (!success) break;
  }

  if (!pathValidation->validate(res, false, validPart, pathReport)) {
              hppDout(info, "Path is in collision.");
              cout << "Path is in collision" << endl;
            }
  else {
    cout << "res is validated" << endl;
  }
  cout << "Passed just before benchmark" << endl;

#if HPP_ENABLE_BENCHMARK
  value_type min = std::numeric_limits<value_type>::max(), max = 0,
             totalLength = 0;
  const size_t nbPaths = res->numberPaths();
  for (std::size_t i = 0; i < nbPaths; ++i) {
    PathPtr_t curP = res->pathAtRank(i);
    const value_type l = d(curP->initial(), curP->end());
    if (l < min)
      min = l;
    else if (l > max)
      max = l;
    totalLength += l;
  }
  hppBenchmark("Hermite path: "
               << nbPaths << ", [ " << min << ", "
               << (nbPaths == 0 ? 0 : totalLength / (value_type)nbPaths) << ", "
               << max << "]");
#endif
  if (success) {
    proj = res;
    return true;
  }
  const value_type tmin = path->timeRange().first;
  switch (res->numberPaths()) {
    case 0:
      proj = path->extract(std::make_pair(tmin, tmin));
      break;
    case 1:
      proj = res->pathAtRank(0);
      break;
    default:
      proj = res;
      break;
  }
  return false;
}



bool RecursiveHermite::recurse(const HermitePtr_t& path, PathVectorPtr_t& proj,
                               const value_type& acceptThr) const {

  if (path->hermiteLength() < acceptThr) {
    // TODO this does not work because it is not possible to remove
    // constraints from a path.
    // proj->appendPath (path->copy (ConstraintSetPtr_t()));
    proj->appendPath(path);
    return true;
  } else {
    const value_type t  = path->timeRange().first + path->length() / 2;
    bool success;
    const Configuration_t q1(path->eval(t, success));
    if (!success) {
      hppDout(info, "RHP stopped because it could not project a configuration");
      return false;
    }
    const Configuration_t q0 = path->initial();
    const Configuration_t q2 = path->end();
    // Velocities must be divided by two because each half is rescale
    // from [0, 0.5] to [0, 1]

    const vector_t vHalf = path->velocity(t);

    interval_t timeRange_left (path->timeRange().first, t);
    HermitePtr_t left = HPP_DYNAMIC_PTR_CAST(Hermite, steer_with_timeRange(q0, q1, timeRange_left));
    if (!left) throw std::runtime_error("Not an path::Hermite");
    left->v0(path->v0());
    left->v1(vHalf);
    left->computeHermiteLength();

    interval_t timeRange_right (t, path->timeRange().second);
    HermitePtr_t right = HPP_DYNAMIC_PTR_CAST(Hermite, steer_with_timeRange(q1, q2, timeRange_right));
    if (!right) throw std::runtime_error("Not an path::Hermite");
    right->v0(vHalf);
    right->v1(path->v1());
    right->computeHermiteLength();

    const value_type stopThr = beta_ * path->hermiteLength();
    bool lStop = (left->hermiteLength() > stopThr);
    bool rStop = (right->hermiteLength() > stopThr);
    bool stop = rStop || lStop;
    // This is the inverse of the condition in the RSS paper. Is there a typo in
    // the paper ? if (std::max (left->hermiteLength(), right->hermiteLength())
    // > beta * path->hermiteLength()) {
    if (stop) {
      hppDout(info, "RHP stopped: " << path->hermiteLength() << " * " << beta_
                                    << " -> " << left->hermiteLength() << " / "
                                    << right->hermiteLength());
    }
    if (lStop || !recurse(left, proj, acceptThr)) return false;
    if (stop || !recurse(right, proj, acceptThr)) return false;

    return true;
  }
}
}  // namespace pathProjector
}  // namespace core
}  // namespace hpp
