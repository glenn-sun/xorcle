#include "algebra/equation.h"

#include <cassert>

#include "algebra/backend.h"

namespace xorcle {

Equation::Equation(Algebra backend, std::size_t num_vars)
    : storage_(make_equation_storage(backend, num_vars)) {}

Equation::Equation(const Equation& other)
    : storage_(other.storage_ ? other.storage_->clone() : nullptr) {}

Equation& Equation::operator=(const Equation& other) {
    if (this != &other) {
        storage_ = other.storage_ ? other.storage_->clone() : nullptr;
    }
    return *this;
}

Equation Equation::eq_true(Algebra backend, std::size_t num_vars) {
    return Equation(backend, num_vars);
}

Equation Equation::eq_false(Algebra backend, std::size_t num_vars) {
    const std::size_t constant_column = num_vars;
    return from_columns(backend, num_vars, std::span<const std::size_t>(&constant_column, 1));
}

Equation Equation::from_columns(Algebra backend, std::size_t num_vars,
                                std::span<const std::size_t> columns) {
    Equation out(backend, num_vars);
    out.storage_->assign_columns(columns);
    return out;
}

Algebra Equation::backend() const noexcept {
    assert(storage_ != nullptr);
    return storage_->backend();
}

std::size_t Equation::num_vars() const noexcept {
    assert(storage_ != nullptr);
    return storage_->size() - 1;
}

void Equation::resize(std::size_t num_vars) {
    assert(storage_ != nullptr);
    storage_->resize(num_vars + 1);
}

void Equation::clear() noexcept {
    if (storage_) {
        storage_->clear();
    }
}

void Equation::xor_assign(const Equation& other) {
    assert(storage_ != nullptr && other.storage_ != nullptr);
    assert(storage_->backend() == other.storage_->backend() && "backend mismatch in xor_assign");
    assert(storage_->size() == other.storage_->size() && "width mismatch in xor_assign");
    storage_->xor_assign(*other.storage_);
}

void Equation::assign_from(const Equation& other) {
    assert(storage_ != nullptr && other.storage_ != nullptr);
    assert(storage_->backend() == other.storage_->backend() && "backend mismatch in assign_from");
    assert(storage_->size() == other.storage_->size() && "width mismatch in assign_from");
    storage_->assign_from(*other.storage_);
}

Equation Equation::project(const CompactionTransform& transform) const {
    assert(storage_ != nullptr);
    assert(storage_->size() == transform.old_num_vars() + 1 && "width mismatch in project");
    return Equation(storage_->project(transform));
}

Equation Equation::lift(const CompactionTransform& transform) const {
    assert(storage_ != nullptr);
    assert(storage_->size() == transform.new_num_vars() + 1 && "width mismatch in lift");
    return Equation(storage_->lift(transform));
}

void Equation::assign_columns(std::span<const std::size_t> columns) {
    assert(storage_ != nullptr);
    storage_->assign_columns(columns);
}

void Equation::columns(std::vector<std::size_t>& out) const {
    out.clear();
    if (storage_) {
        storage_->columns(out);
    }
}

bool Equation::operator==(const Equation& other) const noexcept {
    if (storage_ == nullptr || other.storage_ == nullptr) {
        return storage_ == other.storage_;
    }
    if (storage_->backend() != other.storage_->backend()) {
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
