//
// Copyright (c) 2014 CNRS
// Authors: Florent Lamiraux
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


#include <iostream>
#include <typeinfo>
using namespace std;
#include <boost/serialization/utility.hpp>
#include <boost/serialization/weak_ptr.hpp>
#include <hpp/core/config-projector.hh>
#include <hpp/core/path.hh>
#include <hpp/core/time-parameterization.hh>
#include <hpp/pinocchio/configuration.hh>
#include <hpp/util/debug.hh>
#include <hpp/util/indent.hh>
#include <hpp/util/serialization.hh>

#include "extracted-path.hh"

namespace hpp {
namespace core {
namespace timeParameterization {
class HPP_CORE_LOCAL Shift : public TimeParameterization {
 public:
  typedef shared_ptr<Shift> Ptr_t;

  static TimeParameterizationPtr_t createWithCheck(TimeParameterizationPtr_t tp,
                                                   value_type t, value_type s) {
    if (t == 0 && s == 0)
      return tp;
    else
      return create(tp, t, s);
  }

  static Ptr_t create(TimeParameterizationPtr_t tp, value_type t,
                      value_type s) {
    Ptr_t shift = HPP_DYNAMIC_PTR_CAST(Shift, tp);
    if (shift)
      return Ptr_t(new Shift(shift->tp_, shift->t_ + t, shift->s_ + s));
    else
      return Ptr_t(new Shift(tp, t, s));
  }

  Shift(TimeParameterizationPtr_t tp, value_type t, value_type s)
      : tp_(tp), t_(t), s_(s) {}

  value_type value(const value_type& t) const {
    return tp_->value(t + t_) + s_;
  }
  value_type derivative(const value_type& t, const size_type& order) const {
    return tp_->derivative(t + t_, order);
  }
  value_type impl_derivativeBound(const value_type& l,
                                  const value_type& u) const {
    return tp_->derivativeBound(l + t_, u + t_);
  }

  TimeParameterizationPtr_t copy() const { return create(tp_->copy(), t_, s_); }

  TimeParameterizationPtr_t tp_;
  value_type t_;
  value_type s_;
};
}  // namespace timeParameterization

// Constructor with constraints
Path::Path(const interval_t& interval, size_type outputSize,
           size_type outputDerivativeSize,
           const ConstraintSetPtr_t& constraints)
    : paramRange_(interval),
      timeRange_(interval),
      outputSize_(outputSize),
      outputDerivativeSize_(outputDerivativeSize),
      constraints_() {
  cout << "passed by path with interval, outputsizes and constrait" << endl;
  if (constraints) {
    constraints_ = HPP_STATIC_PTR_CAST(ConstraintSet, constraints->copy());
  }
}

// Constructor without constraints
Path::Path(const interval_t& interval, size_type outputSize,
           size_type outputDerivativeSize)
    : paramRange_(interval),
      timeRange_(interval),
      outputSize_(outputSize),
      outputDerivativeSize_(outputDerivativeSize),
      constraints_() {}

// Copy constructor
Path::Path(const Path& path)
    : paramRange_(path.paramRange_),
      timeRange_(path.timeRange_),
      outputSize_(path.outputSize_),
      outputDerivativeSize_(path.outputDerivativeSize_),
      constraints_(),
      timeParam_() {
  if (path.constraints_) {
    constraints_ =
        HPP_STATIC_PTR_CAST(ConstraintSet, path.constraints_->copy());
  }
  if (path.timeParam_) timeParam_ = path.timeParam_->copy();
}

Path::Path(const Path& path, const ConstraintSetPtr_t& constraints)
    : paramRange_(path.paramRange_),
      timeRange_(path.timeRange_),
      outputSize_(path.outputSize_),
      outputDerivativeSize_(path.outputDerivativeSize_),
      constraints_(constraints),
      timeParam_() {
  assert(!path.constraints_);
  if (path.timeParam_) timeParam_ = path.timeParam_->copy();
}

// Initialization after creation
void Path::init(const PathWkPtr_t& self) { weak_ = self; }

bool Path::applyConstraints(ConfigurationOut_t result,
                            const value_type& param) const {
  if (!constraints_) return true;
  if (constraints_->configProjector())
    constraints_->configProjector()->rightHandSideAt(param);
  return constraints_->apply(result);
}

void Path::derivative(vectorOut_t result, const value_type& time,
                      size_type order) const {
  if (timeParam_) {
    switch (order) {
      case 1:
        impl_derivative(result, timeParam_->value(time), 1);
        result *= timeParam_->derivative(time, 1);
        break;
      case 2: {
        vector_t tmp(outputDerivativeSize());
        impl_derivative(tmp, timeParam_->value(time), 2);
        value_type der = timeParam_->derivative(time, 1);
        result.noalias() = tmp * (der * der);

        impl_derivative(tmp, timeParam_->value(time), 1);
        result.noalias() += tmp * timeParam_->derivative(time, 2);
        break;
      }
      default:
        throw std::invalid_argument(
            "Cannot compute the derivative of order greater than 2.");
    }
  } else {
    impl_derivative(result, time, order);
  }
}

PathPtr_t Path::extract(const interval_t& subInterval) const {
  PathPtr_t res;
  if (timeParam_) {
    interval_t paramInterval(timeParam_->value(subInterval.first),
                             timeParam_->value(subInterval.second));
    res = this->impl_extract(paramInterval);
    // TODO Child class that reimplement impl_extract may return
    // a path whose paramRange has been shifted to 0. We must then shift
    // the time parameterization.
    value_type shift_t, shift_s;
    interval_t timeInterval;
    if (subInterval.first > subInterval.second) {
      shift_t = 0;
      shift_s = res->paramRange().first - paramInterval.second;

      if (shift_s != 0) {
        shift_t = subInterval.second;
        timeInterval.first = 0;
        timeInterval.second = subInterval.first - subInterval.second;
      } else {
        shift_t = 0;
        timeInterval = interval_t(subInterval.second, subInterval.first);
      }
    } else {
      shift_t = 0;
      shift_s = res->paramRange().first - paramInterval.first;
      if (shift_s != 0) {
        shift_t = subInterval.first;
        timeInterval.first = 0;
        timeInterval.second = subInterval.second - subInterval.first;
      } else {
        assert(res->paramRange() == paramInterval);
        shift_t = 0;
        timeInterval = subInterval;
      }
    }
    timeParameterization::Shift::createWithCheck(timeParam_, shift_t, shift_s);
#ifndef NDEBUG
    interval_t pr = res->paramRange();
#endif  // NDEBUG
    res->timeParameterization(timeParam_->copy(), timeInterval);
    assert(pr == res->paramRange());
  } else {
    res = this->impl_extract(subInterval);
  }
  return res;
}

PathPtr_t Path::impl_extract(const interval_t& paramInterval) const {
  if (paramInterval == paramRange_) return this->copy();
  return ExtractedPath::create(weak_.lock(), paramInterval);
}

PathPtr_t Path::reverse() const {
  interval_t interval;
  interval.first = this->timeRange_.second;
  interval.second = this->timeRange_.first;
  return this->extract(interval);
}

void Path::checkPath() const {
  using pinocchio::displayConfig;
  if (constraints()) {
    if (constraints_->configProjector())
      constraints_->configProjector()->rightHandSideAt(paramRange_.first);
    if (!constraints()->isSatisfied(initial())) {
      std::stringstream oss;
      hppDout(error, *constraints());
      hppDout(error, initial().transpose());
      oss << "Initial configuration of path does not satisfy the path "
             "constraints: q="
          << displayConfig(initial()) << "; error=";
      vector_t error;
      constraints()->isSatisfied(initial(), error);
      oss << displayConfig(error) << ".";
      throw projection_error(oss.str().c_str());
    }
    if (constraints_->configProjector())
      constraints_->configProjector()->rightHandSideAt(paramRange_.second);
    if (constraints() && !constraints()->isSatisfied(end())) {
      std::stringstream oss;
      hppDout(error, *constraints());
      hppDout(error, displayConfig(end()));
      oss << "End configuration of path does not satisfy the path "
             "constraints: q="
          << displayConfig(end()) << "; error=";
      vector_t error;
      constraints()->isSatisfied(end(), error);
      Configuration_t q = end();
      constraints()->apply(q);
      oss << displayConfig(error) << "; qproj=" << displayConfig(q) << ".\n"
          << *constraints();
      throw projection_error(oss.str().c_str());
    }
  }
}

std::ostream& Path::print(std::ostream& os) const {
  os << "time in [ " << timeRange().first << ", " << timeRange().second << " ]";
  if (timeParam_)
    os << ", param in [ " << paramRange().first << ", " << paramRange().second
       << " ]";
  os << iendl;
  return os;
}

template <class Archive>
void Path::serialize(Archive& ar, const unsigned int version) {
  (void)version;
  ar& BOOST_SERIALIZATION_NVP(timeRange_);
  ar& BOOST_SERIALIZATION_NVP(outputSize_);
  ar& BOOST_SERIALIZATION_NVP(outputDerivativeSize_);
  ar& BOOST_SERIALIZATION_NVP(constraints_);
  if (Archive::is_saving::value && timeParam_)
    throw std::logic_error(
        "At the moment, it is not possible to serialize "
        "a Path with time parameterization.");
  // ar & BOOST_SERIALIZATION_NVP(timeParam_);
  if (!Archive::is_saving::value) {  // handles paramRange_
    timeRange(timeRange_);
  }
  ar& BOOST_SERIALIZATION_NVP(weak_);
}

HPP_SERIALIZATION_IMPLEMENT(Path);
}  //   namespace core
}  // namespace hpp
