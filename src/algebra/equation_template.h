#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/equation.h"
#include "algebra/equation_repr.h"
#include "algebra/gf2_vector.h"
#include "algebra/reduce_result.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief The one implementation of `EquationInterface`, written against
/// `GF2Vector`.
///
/// Instantiated once per representation, in that representation's own
/// translation unit. Every per-bit loop -- compaction, uncompaction, bit
/// collection, formatting -- lives here, inside the virtual boundary: a single
/// dispatch from the caller's point of view and a fully inlined direct call from
/// `V`'s.
template <GF2Vector V>
class EquationTemplate final : public EquationInterface {
public:
    explicit EquationTemplate(std::size_t num_bits) : vector_(num_bits) {}

    explicit EquationTemplate(V vec) : vector_(std::move(vec)) {}

    const V& vector() const noexcept { return vector_; }

    V& vector() noexcept { return vector_; }

    EquationRepr repr() const noexcept override { return V::repr; }

    std::unique_ptr<EquationInterface> clone() const override {
        return std::make_unique<EquationTemplate>(*this);
    }

    std::size_t num_bits() const noexcept override { return vector_.num_bits(); }

    void resize(std::size_t num_bits) override { vector_.resize(num_bits); }

    void set_eq_true() noexcept override { vector_.zero(); }

    void assign_bits(std::span<const std::size_t> bits) override { vector_.assign_bits(bits); }

    std::optional<std::size_t> leading_bit() const noexcept override {
        return vector_.leading_bit();
    }

    ReduceResult classify() const noexcept override { return vector_.classify(); }

    void plus_assign(const EquationInterface& other) override {
        vector_.plus_assign(cast(other).vector_);
    }

    void assign_from(const EquationInterface& other) override {
        vector_.assign_from(cast(other).vector_);
    }

    std::unique_ptr<EquationInterface> compact(const CompactionTransform& transform)
        const override {
        V out(transform.new_num_vars() + 1);
        const std::size_t old_constant_bit = transform.old_num_vars();
        const std::size_t new_constant_bit = transform.new_num_vars();
        vector_.for_each_bit([&](std::size_t old_bit) {
            if (old_bit == old_constant_bit) {
                out.set_bit(new_constant_bit);
                return;
            }
            const std::size_t mapped = transform.to_new(old_bit);
            if (mapped != CompactionTransform::npos) {
                out.set_bit(mapped);
            }
        });
        return std::make_unique<EquationTemplate>(std::move(out));
    }

    std::unique_ptr<EquationInterface> uncompact(const CompactionTransform& transform)
        const override {
        V out(transform.old_num_vars() + 1);
        const std::size_t old_constant_bit = transform.old_num_vars();
        const std::size_t new_constant_bit = transform.new_num_vars();
        vector_.for_each_bit([&](std::size_t new_bit) {
            out.set_bit(new_bit == new_constant_bit ? old_constant_bit : transform.to_old(new_bit));
        });
        return std::make_unique<EquationTemplate>(std::move(out));
    }

    void bits(std::vector<std::size_t>& out) const override {
        vector_.for_each_bit([&](std::size_t bit) { out.push_back(bit); });
    }

    bool equals(const EquationInterface& other) const noexcept override {
        return vector_.equals(cast(other).vector_);
    }

    std::size_t hash() const noexcept override { return vector_.hash(); }

    std::string to_string(const PrintMode& mode) const override {
        std::ostringstream oss;
        const std::size_t constant_bit = vector_.num_bits() - 1;
        vector_.for_each_bit([&](std::size_t bit) {
            if (bit == constant_bit) {
                return;
            }
            if (oss.tellp() > 0) {
                oss << " + ";
            }
            oss << "x" << (bit + 1);
        });

        bool constant = vector_.test_bit(constant_bit);
        if (mode.clause_mode == ClausePrintMode::as_clause) {
            constant = !constant;
        }
        switch (mode.equation_mode) {
            case EquationPrintMode::standard:
                if (oss.tellp() == 0) {
                    oss << "0";
                }
                oss << " = " << (constant ? "1" : "0");
                break;
            case EquationPrintMode::equals_0:
                if (oss.tellp() == 0) {
                    oss << (constant ? "1" : "0") << " = 0";
                } else {
                    oss << (constant ? " + 1" : "") << " = 0";
                }
                break;
            case EquationPrintMode::equals_1:
                if (oss.tellp() == 0) {
                    oss << (constant ? "0" : "1") << " = 1";
                } else {
                    oss << (constant ? "" : " + 1") << " = 1";
                }
                break;
        }
        return "[" + oss.str() + "]";
    }

private:
    static const EquationTemplate& cast(const EquationInterface& storage) noexcept {
        assert(storage.repr() == V::repr && "representation mismatch in EquationTemplate");
        return static_cast<const EquationTemplate&>(storage);
    }

    V vector_;
};

/// @brief Reach the concrete GF(2) vector behind an `Equation` handle.
/// @pre `!equation.is_null()` and `equation.repr() == V::repr`; likewise for the
/// mutable overload below.
template <GF2Vector V>
const V& gf2_vector_of(const Equation& equation) noexcept {
    assert(!equation.is_null() && equation.repr() == V::repr);
    return static_cast<const EquationTemplate<V>&>(equation.storage()).vector();
}

template <GF2Vector V>
V& gf2_vector_of(Equation& equation) noexcept {
    assert(!equation.is_null() && equation.repr() == V::repr);
    return static_cast<EquationTemplate<V>&>(equation.storage()).vector();
}

/// @brief Wrap a concrete GF(2) vector in an `Equation` handle.
template <GF2Vector V>
Equation make_equation_from_gf2_vector(V vec) {
    return Equation(std::make_unique<EquationTemplate<V>>(std::move(vec)));
}

} // namespace xorcle
