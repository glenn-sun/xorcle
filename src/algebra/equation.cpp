#include "algebra/equation.h"

#include <cassert>

namespace xorcle {

Equation::Equation(EquationRepr repr, std::size_t num_vars)
    : interface_(make_equation_interface(repr, num_vars + 1)) {}

Equation::Equation(const Equation& other)
    : interface_(other.interface_ ? other.interface_->clone() : nullptr) {}

Equation& Equation::operator=(const Equation& other) {
    if (this != &other) {
        interface_ = other.interface_ ? other.interface_->clone() : nullptr;
    }
    return *this;
}

Equation Equation::with_num_bits(EquationRepr repr, std::size_t num_bits) {
    return Equation(make_equation_interface(repr, num_bits));
}

Equation Equation::eq_true(EquationRepr repr, std::size_t num_vars) {
    return Equation(repr, num_vars);
}

Equation Equation::eq_false(EquationRepr repr, std::size_t num_vars) {
    const std::size_t constant_bit = num_vars;
    return from_bits(repr, num_vars, std::span<const std::size_t>(&constant_bit, 1));
}

Equation Equation::from_bits(EquationRepr repr, std::size_t num_vars,
                             std::span<const std::size_t> bits) {
    Equation out(repr, num_vars);
    out.interface_->assign_bits(bits);
    return out;
}

EquationRepr Equation::repr() const noexcept {
    assert(interface_ != nullptr && "repr() on a null equation handle");
    return interface_->repr();
}

std::size_t Equation::num_vars() const noexcept {
    assert(interface_ != nullptr && "num_vars() on a null equation handle");
    return interface_->num_bits() - 1;
}

void Equation::resize(std::size_t num_vars) {
    assert(interface_ != nullptr && "resize on a null equation handle");
    interface_->resize(num_vars + 1);
}

void Equation::set_eq_true() noexcept {
    assert(interface_ != nullptr && "set_eq_true on a null equation handle");
    interface_->set_eq_true();
}

void Equation::set_eq_false() noexcept {
    assert(interface_ != nullptr && "set_eq_false on a null equation handle");
    interface_->set_eq_false();
}

void Equation::plus_assign(const Equation& other) {
    assert(interface_ != nullptr && other.interface_ != nullptr &&
           "plus_assign on a null equation handle");
    assert(interface_->repr() == other.interface_->repr() &&
           "representation mismatch in plus_assign");
    assert(interface_->num_bits() == other.interface_->num_bits() &&
           "bit count mismatch in plus_assign");
    interface_->plus_assign(*other.interface_);
}

void Equation::assign_from(const Equation& other) {
    assert(interface_ != nullptr && other.interface_ != nullptr &&
           "assign_from on a null equation handle");
    assert(interface_->repr() == other.interface_->repr() &&
           "representation mismatch in assign_from");
    assert(interface_->num_bits() == other.interface_->num_bits() &&
           "bit count mismatch in assign_from");
    interface_->assign_from(*other.interface_);
}

Equation Equation::compact(const CompactionTransform& transform) const {
    assert(interface_ != nullptr && "compact on a null equation handle");
    assert(interface_->num_bits() == transform.old_num_vars() + 1 &&
           "bit count mismatch in compact");
    return Equation(interface_->compact(transform));
}


void Equation::assign_bits(std::span<const std::size_t> bits) {
    assert(interface_ != nullptr && "assign_bits on a null equation handle");
    interface_->assign_bits(bits);
}

std::vector<std::size_t> Equation::bits() const {
    if (!interface_) {
        return {};
    }
    return interface_->bits();
}

bool Equation::operator==(const Equation& other) const noexcept {
    if (interface_ == nullptr || other.interface_ == nullptr) {
        return interface_ == other.interface_;
    }
    if (interface_->repr() != other.interface_->repr()) {
        return false;
    }
    return interface_->equals(*other.interface_);
}

std::string Equation::to_string(const PrintMode& mode) const {
    if (!interface_) {
        return "[]";
    }
    return interface_->to_string(mode);
}

} // namespace xorcle
