/*
 * Copyright 2020 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_CONSTRAINTS_H
#define INCLUDE_CONSTRAINTS_H

#include <cassert>
#include <memory>
#include <unordered_set>
#include <utility>
#include <variant>

#include "tools/fuzzer/ast.h"
#include "tools/fuzzer/enum_bitset.h"
#include "tools/fuzzer/expr_gen.h"

namespace fuzzer {

using ScalarMask = EnumBitset<ScalarType>;

class NoType {};
class AnyType {};

class TypeConstraints;

enum class VoidPointerConstraint : bool { Deny, Allow };
enum class ExprCategory : bool { LvalueOrRvalue, Lvalue };

// The type constraints an expression can have. This class represents the fact
// that an expression can be:
//
// - Of any type (AnyType)
// - Of no type at all (NoType) aka unsatisfiable
// - Of a specific type/specific set of types
//
// The reason we have both `SpecificTypes` and `TypeConstraints` is so that
// most typical use cases (scalars, pointers to any type, pointers to void)
// do not perform any sort of heap allocation at all.
class SpecificTypes {
 public:
  SpecificTypes() = default;
  SpecificTypes(ScalarMask scalar_types) : scalar_types_(scalar_types) {}
  SpecificTypes(std::unordered_set<TaggedType> tagged_types)
      : tagged_types_(std::move(tagged_types)) {}

  // Constraints that only allow type `type`.
  explicit SpecificTypes(const Type& type);

  // Constraints corresponding to all types that can be used in a boolean
  // context, i.e. ternary expression condition, logical operators (`&&`, `||`,
  // `!`), etc. These types are:
  // - Integers
  // - Floats
  // - Void/non-void pointers or the null pointer constant `0`
  static SpecificTypes all_in_bool_ctx() {
    SpecificTypes retval;
    retval.scalar_types_ = ~ScalarMask(ScalarType::Void);
    retval.ptr_types_ = AnyType{};
    retval.allows_void_pointer_ = true;

    return retval;
  }

  // Return a set of constraints that allow any pointer type, including void
  // pointers.
  static SpecificTypes make_any_pointer_constraints() {
    SpecificTypes retval;
    retval.ptr_types_ = AnyType{};
    retval.allows_void_pointer_ = true;

    return retval;
  }

  // Return a set of constraints that allow any non-void pointer type.
  static SpecificTypes make_any_non_void_pointer_constraints() {
    SpecificTypes retval;
    retval.ptr_types_ = AnyType{};

    return retval;
  }

  // Make a new set of pointer constraints. If the original constraints permit
  // type T, the new constraints will allow types `T*`, `const T*`, `volatile
  // T*`, and `const volatile T*`.
  static SpecificTypes make_pointer_constraints(
      SpecificTypes constraints,
      VoidPointerConstraint void_ptr_constraint = VoidPointerConstraint::Deny);

  // Is there any type that satisfies these constraints?
  bool satisfiable() const {
    return scalar_types_.any() || !tagged_types_.empty() ||
           !std::holds_alternative<NoType>(ptr_types_) || allows_void_pointer_;
  }

  // Scalar types allowed by these constraints.
  ScalarMask allowed_scalar_types() const { return scalar_types_; }

  // Tagged types allowed by these constraints. An empty
  const std::unordered_set<TaggedType>& allowed_tagged_types() const {
    return tagged_types_;
  }

  // Do these constraints allow any of the types in `mask`?
  bool allows_any_of(ScalarMask mask) const {
    return (scalar_types_ & mask).any();
  }

  // Do these constraints allow any kind of non-void pointer?
  bool allows_non_void_pointer() const {
    return !std::holds_alternative<NoType>(ptr_types_);
  }

  // Do these constraints allow void pointers or the null pointer constant `0`?
  bool allows_void_pointer() const { return allows_void_pointer_; }

  // What kind of types do these constraints allow a pointer to?
  TypeConstraints allowed_to_point_to() const;

 private:
  ScalarMask scalar_types_;
  std::unordered_set<TaggedType> tagged_types_;
  std::variant<NoType, AnyType, std::shared_ptr<SpecificTypes>> ptr_types_;
  bool allows_void_pointer_ = false;
};

// The type constraints an expression can have. This class represents the fact
// that an expression can be:
//
// - Of any type (AnyType)
// - Of no type at all (NoType) aka unsatisfiable
// - Of a specific type/specific set of types
//
// The reason we have both `SpecificTypes` and `TypeConstraints` is so that
// most typical use cases (scalars, pointers to any type, pointers to void)
// do not perform any sort of heap allocation at all.
class TypeConstraints {
 public:
  TypeConstraints() = default;
  TypeConstraints(NoType) {}
  TypeConstraints(AnyType) : constraints_(AnyType()) {}
  TypeConstraints(SpecificTypes constraints) {
    if (constraints.satisfiable()) {
      constraints_ = std::move(constraints);
    }
  }

  // Constraints corresponding to all types that can be used in a boolean
  // context, i.e. ternary expression condition, logical operators (`&&`, `||`,
  // `!`), etc. These types are:
  // - Integers
  // - Floats
  // - Void/non-void pointers
  static TypeConstraints all_in_bool_ctx() {
    return SpecificTypes::all_in_bool_ctx();
  }

  // Do these constraints allow any type at all?
  bool satisfiable() const {
    return !std::holds_alternative<NoType>(constraints_);
  }

  // Do these constraints allow all kinds of types?
  bool allows_any() const {
    return std::holds_alternative<AnyType>(constraints_);
  }

  // Return the specific types allowed (if any) or `nullptr`.
  const SpecificTypes* as_specific_types() const {
    return std::get_if<SpecificTypes>(&constraints_);
  }

  // Do these constraints allow any of the scalar types specified in `mask`?
  bool allows_any_of(ScalarMask mask) const {
    if (!satisfiable()) {
      return false;
    }

    if (allows_any()) {
      return true;
    }

    const auto* specific_types = as_specific_types();
    assert(specific_types != nullptr &&
           "Should never be null, did you introduce a new alternative?");

    return specific_types->allows_any_of(mask);
  }

  // Do these constraints allow any tagged type?
  bool allows_tagged_types() const {
    if (!satisfiable()) {
      return false;
    }

    if (allows_any()) {
      return true;
    }

    const auto* specific_types = as_specific_types();
    assert(specific_types != nullptr && "Did you introduce a new alternative?");

    return !specific_types->allowed_tagged_types().empty();
  }

  // Scalar types allowed by these constraints.
  ScalarMask allowed_scalar_types() const {
    if (!satisfiable()) {
      return ScalarMask();
    }

    if (allows_any()) {
      return ScalarMask::all_set();
    }

    const auto* specific_types = as_specific_types();
    assert(specific_types != nullptr && "Did you introduce a new alternative?");

    return specific_types->allowed_scalar_types();
  }

  // Tagged types allowed by these constraints. A null pointer return value
  // indicates that any kind of tagged type is allowed.
  //
  // TODO: Returning a null pointer to indicate that any kind of tagged type
  // is allowed is really confusing.
  const std::unordered_set<TaggedType>* allowed_tagged_types() const {
    const auto* specific_types = as_specific_types();
    if (specific_types == nullptr) {
      return nullptr;
    }
    return &specific_types->allowed_tagged_types();
  }

  // What kind of types do these constraints allow a pointer to?
  TypeConstraints allowed_to_point_to() const;

  // Make a new set of pointer constraints. If the original constraints permit
  // type T, the new constraints will allow types `T*`, `const T*`, `volatile
  // T*`, and `const volatile T*`.
  TypeConstraints make_pointer_constraints() const;

  // Do these constraints allow void pointers or the null pointer constant `0`?
  bool allows_void_pointer() const {
    if (!satisfiable()) {
      return false;
    }

    if (allows_any()) {
      return true;
    }

    const auto* specific_types = as_specific_types();
    assert(specific_types != nullptr && "Did you introduce a new alternative?");

    return specific_types->allows_void_pointer();
  }

  // Do these constraints allow non-void pointers?
  bool allows_pointer() const {
    if (!satisfiable()) {
      return false;
    }

    if (allows_any()) {
      return true;
    }

    const auto* specific_types = as_specific_types();
    assert(specific_types != nullptr && "Did you introduce a new alternative?");

    return specific_types->allows_non_void_pointer();
  }

  bool allows_type(const Type& type) const;

  // Do these constraints allow a specific type?
  bool allows_type(const QualifiedType& type) const {
    return allows_type(type.type());
  }

 private:
  std::variant<NoType, AnyType, SpecificTypes> constraints_;
};

// The main class that deals with expression constraints.
class ExprConstraints {
 public:
  ExprConstraints() = default;

  // Allow implicit conversion from `TypeConstraints` for convenience (plus,
  // in most cases expressions don't have to be lvalues.
  ExprConstraints(TypeConstraints type_constraints,
                  ExprCategory category = ExprCategory::LvalueOrRvalue)
      : type_constraints_(std::move(type_constraints)),
        must_be_lvalue_((bool)category) {}

  ExprConstraints(ScalarMask mask,
                  ExprCategory category = ExprCategory::LvalueOrRvalue)
      : type_constraints_(TypeConstraints(mask)),
        must_be_lvalue_((bool)category) {}

  // Must the expression we generate be an lvalue?
  bool must_be_lvalue() const { return must_be_lvalue_; }

  // Type constraints of the expression to generate
  const TypeConstraints& type_constraints() const { return type_constraints_; }

 private:
  TypeConstraints type_constraints_;
  bool must_be_lvalue_ = false;
};

inline constexpr ScalarMask INT_TYPES = {
    ScalarType::Bool,           ScalarType::Char,
    ScalarType::UnsignedChar,   ScalarType::SignedChar,
    ScalarType::SignedShort,    ScalarType::UnsignedShort,
    ScalarType::SignedInt,      ScalarType::UnsignedInt,
    ScalarType::SignedLong,     ScalarType::UnsignedLong,
    ScalarType::SignedLongLong, ScalarType::UnsignedLongLong,
};

inline constexpr ScalarMask FLOAT_TYPES = {
    ScalarType::Float,
    ScalarType::Double,
    ScalarType::LongDouble,
};

}  // namespace fuzzer

#endif  // INCLUDE_CONSTRAINTS_H
