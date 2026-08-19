#include "algebra/equation.h"

#include <cassert>

namespace xorcle {

Equation::Equation(EquationRepr repr, std::size_t num_vars)
    : storage_(make_equation_interface(repr, num_vars)) {}

Equation::Equation(const Equation& other)
    : storage_(other.storage_ ? other.storage_->clone() : nullptr) {}

Equation& Equation::operator=(const Equation& other) {
    if (this != &other) {
        storage_ = other.storage_ ? other.storage_->clone() : nullptr;
    }
    return *this;
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
    out.storage_->assign_bits(bits);
    return out;
}

EquationRepr Equation::repr() const noexcept {
    assert(storage_ != nullptr);
    return storage_->repr();
}

std::size_t Equation::num_vars() const noexcept {
    assert(storage_ != nullptr);
    return storage_->num_bits() - 1;
}

void Equation::resize(std::size_t num_vars) {
    assert(storage_ != nullptr);
    storage_->resize(num_vars + 1);
}

void Equation::set_eq_true() noexcept {
    if (storage_) {
        storage_->set_eq_true();
    }
}

void Equation::plus_assign(const Equation& other) {
    assert(storage_ != nullptr && other.storage_ != nullptr);
    assert(storage_->repr() == other.storage_->repr() &&
           "representation mismatch in plus_assign");
    assert(storage_->num_bits() == other.storage_->num_bits() && "width mismatch in plus_assign");
    storage_->plus_assign(*other.storage_);
}

void Equation::assign_from(const Equation& other) {
    assert(storage_ != nullptr && other.storage_ != nullptr);
    assert(storage_->repr() == other.storage_->repr() &&
           "representation mismatch in assign_from");
    assert(storage_->num_bits() == other.storage_->num_bits() && "width mismatch in assign_from");
    storage_->assign_from(*other.storage_);
}

Equation Equation::compact(const CompactionTransform& transform) const {
    assert(storage_ != nullptr);
    assert(storage_->num_bits() == transform.old_num_vars() + 1 && "width mismatch in compact");
    return Equation(storage_->compact(transform));
}

Equation Equation::uncompact(const CompactionTransform& transform) const {
    assert(storage_ != nullptr);
    assert(storage_->num_bits() == transform.new_num_vars() + 1 && "width mismatch in uncompact");
    return Equation(storage_->uncompact(transform));
}

void Equation::assign_bits(std::span<const std::size_t> bits) {
    assert(storage_ != nullptr);
    storage_->assign_bits(bits);
}

void Equation::bits(std::vector<std::size_t>& out) const {
    out.clear();
    if (storage_) {
        storage_->bits(out);
    }
}

bool Equation::operator==(const Equation& other) const noexcept {
    if (storage_ == nullptr || other.storage_ == nullptr) {
        return storage_ == other.storage_;
    }
    if (storage_->repr() != other.storage_->repr()) {
        return false;
    }
    return storage_->equals(*other.storage_);
}

std::string Equation::to_string(const PrintMode& mode) const {
    if (!storage_) {
        return "[]";
    }
    return storage_->to_string(mode);
}

} // namespace xorcle
