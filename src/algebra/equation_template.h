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
#include "algebra/equation_repr.h"
#include "algebra/gf2_vector.h"
#include "algebra/reduce_result.h"
#include "util/print_mode.h"

namespace xorcle {

/// @brief The abstract interface behind `Equation`.
/// Contains all functionality involving per-bit loops, to avoid virtual
/// dispatch at low levels.
class EquationInterface {
public:
    virtual ~EquationInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<EquationInterface> clone() const = 0;

    virtual std::size_t num_bits() const noexcept = 0;

    virtual void resize(std::size_t num_bits) = 0;

    virtual void set_eq_true() noexcept = 0;

    virtual void assign_bits(std::span<const std::size_t> bits) = 0;

    virtual std::optional<std::size_t> leading_bit() const noexcept = 0;

    virtual ReduceResult classify() const noexcept = 0;

    virtual void plus_assign(const EquationInterface& other) = 0;

    virtual void assign_from(const EquationInterface& other) = 0;

    virtual std::unique_ptr<EquationInterface> compact(const CompactionTransform& transform)
        const = 0;

    virtual std::unique_ptr<EquationInterface> uncompact(const CompactionTransform& transform)
        const = 0;

    virtual std::vector<std::size_t> bits() const = 0;

    virtual bool equals(const EquationInterface& other) const noexcept = 0;

    virtual std::size_t hash() const noexcept = 0;

    virtual std::string to_string(const PrintMode& mode) const = 0;
};

/// @brief Implements `EquationInterface` against any `GF2Vector`.
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

    void set_eq_true() noexcept override { vector_.set_zero(); }

    void assign_bits(std::span<const std::size_t> bits) override { vector_.assign_bits(bits); }

    std::optional<std::size_t> leading_bit() const noexcept override {
        return vector_.leading_bit();
    }

    /// @brief The last bit is the constant bit, so a vector leading on it has no
    /// other bit set.
    ReduceResult classify() const noexcept override {
        const std::optional<std::size_t> leading = vector_.leading_bit();
        if (!leading) {
            return ReduceResult::eq_true();
        }
        if (*leading + 1 == vector_.num_bits()) {
            return ReduceResult::eq_false();
        }
        return ReduceResult::leading(*leading);
    }

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
        vector_.for_each_set_bit([&](std::size_t old_bit) {
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
        vector_.for_each_set_bit([&](std::size_t new_bit) {
            out.set_bit(new_bit == new_constant_bit ? old_constant_bit : transform.to_old(new_bit));
        });
        return std::make_unique<EquationTemplate>(std::move(out));
    }

    std::vector<std::size_t> bits() const override {
        std::vector<std::size_t> out;
        vector_.for_each_set_bit([&](std::size_t bit) { out.push_back(bit); });
        return out;
    }

    bool equals(const EquationInterface& other) const noexcept override {
        return vector_.equals(cast(other).vector_);
    }

    std::size_t hash() const noexcept override { return vector_.hash(); }

    std::string to_string(const PrintMode& mode) const override {
        std::ostringstream oss;
        const std::size_t constant_bit = vector_.num_bits() - 1;
        vector_.for_each_set_bit([&](std::size_t bit) {
            if (bit == constant_bit) {
                return;
            }
            if (oss.tellp() > 0) {
                oss << " + ";
            }
            oss << "x" << (bit + 1);
        });

        bool constant = vector_.get_bit(constant_bit);
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
    /// @brief The public entries reaching here -- `Equation::plus_assign()`,
    /// `Equation::assign_from()` and `Equation::operator==()` -- have each
    /// already established that `interface` shares this template's
    /// representation, so the downcast is not re-checked.
    static const EquationTemplate& cast(const EquationInterface& interface) noexcept {
        return static_cast<const EquationTemplate&>(interface);
    }

    V vector_;
};

} // namespace xorcle
