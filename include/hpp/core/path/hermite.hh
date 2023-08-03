// Copyright (c) 2016 CNRS
// Authors: Joseph Mirabel
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

#ifndef HPP_CORE_PATH_HERMITE_HH
#define HPP_CORE_PATH_HERMITE_HH

#include <hpp/core/config.hh>
#include <hpp/core/fwd.hh>
#include <hpp/core/path/spline.hh>
#include <hpp/core/config-projector.hh>
#include <hpp/core/path/hermite.hh>
#include <hpp/core/projection-error.hh>
#include <hpp/pinocchio/configuration.hh>
#include <hpp/pinocchio/device.hh>
#include <hpp/pinocchio/liegroup.hh>
#include <hpp/util/debug.hh>

namespace hpp {
namespace core {
namespace path {
/// \addtogroup path
/// \{

class HPP_CORE_DLLAPI Hermite : public Spline<BernsteinBasis, 3> {
 public:
  typedef Spline<BernsteinBasis, 3> parent_t;

  static void all_info_about_hermite_path(HermitePtr_t path){
  
  cout << "timRange" << path->timeRange().first << "    " << path->timeRange().second << endl;
  cout << "initial configuration : \n" << path->initial() << endl;
  hpp::core::Configuration_t q_init(path->outputSize());
  bool suc1 = path->impl_compute(q_init, path->timeRange().first);
  cout << "initial config by impl_compute : \n" << q_init << endl;

  cout << "initial vector v0 : \n" << path->v0() << endl;
  cout << "final configuration : \n" << path->end() << endl;
  hpp::core::Configuration_t q_end(path->outputSize());
  bool suc2 = path->impl_compute(q_end, path->timeRange().second);
  cout << "final config by impl_compute : \n" << q_end << endl;
  
  cout << "final vector v1 : \n" << path->v1() << endl;
  cout << "hermiteLength : \n" << path->hermiteLength() << endl << endl;
}

  /// Destructor
  virtual ~Hermite() {}

  static HermitePtr_t create(const DevicePtr_t& device, ConfigurationIn_t init,
                             ConfigurationIn_t end,
                             ConstraintSetPtr_t constraints) {
    Hermite* ptr = new Hermite(device, init, end, constraints);
    HermitePtr_t shPtr(ptr);
    ptr->init(shPtr);
    return shPtr;
  }

  

  /// Create copy and return shared pointer
  /// \param path path to copy
  static HermitePtr_t createCopy(const HermitePtr_t& path) {
    Hermite* ptr = new Hermite(*path);
    HermitePtr_t shPtr(ptr);
    ptr->init(shPtr);
    return shPtr;
  }

  /// Create copy and return shared pointer
  /// \param path path to copy
  /// \param constraints the path is subject to
  static HermitePtr_t createCopy(const HermitePtr_t& path,
                                 const ConstraintSetPtr_t& constraints) {
    Hermite* ptr = new Hermite(*path, constraints);
    HermitePtr_t shPtr(ptr);
    ptr->init(shPtr);
    return shPtr;
  }

  /// Return a shared pointer to this
  ///
  /// As StaightPath are immutable, and refered to by shared pointers,
  /// they do not need to be copied.
  virtual PathPtr_t copy() const { return createCopy(weak_.lock()); }

  /// Return a shared pointer to a copy of this and set constraints
  ///
  /// \param constraints constraints to apply to the copy
  /// \pre *this should not have constraints.
  virtual PathPtr_t copy(const ConstraintSetPtr_t& constraints) const {
    return createCopy(weak_.lock(), constraints);
  }

  /// Return the internal robot.
  DevicePtr_t device() const;

  void v0(const vectorIn_t& speed) {
    parameters_.row(1) =  speed.transpose() / 3 * (timeRange().second - timeRange().first);
    hermiteLength_ = -1;
  }

  void v1(const vectorIn_t& speed) {
    parameters_.row(2) = parameters_.row(3) - speed.transpose() / 3 * (timeRange().second - timeRange().first);
    hermiteLength_ = -1;
  }

  vector_t v0() const {
    return 3 * (parameters_.row(1) - parameters_.row(0)) / (timeRange().second - timeRange().first);
    // TODO Should be equivalent to
    // vector_t res (outputDerivativeSize());
    // derivative (res, timeRange().first, 1);
    // return res;
  }

  vector_t v1() const { return 3 * (parameters_.row(3) - parameters_.row(2)) /(timeRange().second - timeRange().first); }

  virtual Configuration_t initial() const { return init_; }

  virtual Configuration_t end() const { return end_; }

  const value_type& hermiteLength() const { return hermiteLength_; }

  void computeHermiteLength();

  vector_t velocity(const value_type& t) const;

  static HermitePtr_t create_with_timeRange(const DevicePtr_t& device, ConfigurationIn_t init,
    ConfigurationIn_t end,
    ConstraintSetPtr_t constraints, interval_t timeRange) {
    Hermite* ptr = new Hermite(device, init, end, constraints, timeRange);
    HermitePtr_t shPtr(ptr);
    ptr->init(shPtr);
  return shPtr;
}

 protected:
  /// Print path in a stream
  virtual std::ostream& print(std::ostream& os) const {
    os << "Hermite:" << std::endl;
    Path::print(os);
    os << "initial configuration: " << initial().transpose() << std::endl;
    os << "final configuration:   " << end().transpose() << std::endl;
    return os;
  }

  /// Constructor
  Hermite(const DevicePtr_t& robot, ConfigurationIn_t init,
          ConfigurationIn_t end);

  /// Constructor with constraints
  Hermite(const DevicePtr_t& robot, ConfigurationIn_t init,
          ConfigurationIn_t end, ConstraintSetPtr_t constraints);

  Hermite(const DevicePtr_t& device, ConfigurationIn_t init,
                 ConfigurationIn_t end, ConstraintSetPtr_t constraints, interval_t timeRange)
    : parent_t(device, timeRange, constraints),
      init_(init),
      end_(end),
      hermiteLength_(-1) {
  assert(init.size() == robot_->configSize());
  assert(device);
  base(init);
  parameters_.row(0).setZero();
  pinocchio::difference<hpp::pinocchio::RnxSOnLieGroupMap>(robot_, end, init,
                                                           parameters_.row(3));                                                           
  projectVelocities(init, end);
  
}

  /// Copy constructor
  Hermite(const Hermite& path);

  /// Copy constructor with constraints
  Hermite(const Hermite& path, const ConstraintSetPtr_t& constraints);

  void init(HermitePtr_t self);

 private:
  // void computeVelocities ();
  void projectVelocities(ConfigurationIn_t qi, ConfigurationIn_t qe);

  DevicePtr_t device_;
  Configuration_t init_, end_;
  value_type hermiteLength_;

  HermiteWkPtr_t weak_;
};  // class Hermite
/// \}
}  //   namespace path
}  //   namespace core
}  // namespace hpp
#endif  // HPP_CORE_PATH_HERMITE_HH
 