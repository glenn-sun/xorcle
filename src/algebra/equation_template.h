#pragma once

#include <array>
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

/// @brief The abstract interface behind `Equation`, containing all
/// functionality involving per-bit loops.
class EquationInterface {
public:
    virtual ~EquationInterface() = default;

    virtual EquationRepr repr() const noexcept = 0;

    virtual std::unique_ptr<EquationInterface> clone() const = 0;

    virtual std::size_t num_bits() const noexcept = 0;

    virtual void resize(std::size_t num_bits) = 0;

    virtual void set_eq_true() noexcept = 0;

    virtual void set_eq_false() noexcept = 0;

    virtual void assign_bits(std::span<const std::size_t> bits) = 0;

    virtual std::optional<std::size_t> leading_bit() const noexcept = 0;

    virtual ReduceResult classify() const noexcept = 0;

    virtual void plus_assign(const EquationInterface& other) = 0;

    virtual void assign_from(const EquationInterface& other) = 0;

    virtual std::unique_ptr<EquationInterface> compact(const CompactionTransform& transform)
        const = 0;

    virtual std::vector<std::size_t> bits() const = 0;

    virtual std::size_t num_set_bits() const noexcept = 0;

    virtual bool equals(const EquationInterface& other) const noexcept = 0;

    virtual bool equal_variables(const EquationInterface& other) const noexcept = 0;

    virtual std::size_t hash() const noexcept = 0;

    virtual std::size_t variables_hash() const noexcept = 0;

    virtual std::string to_string(const PrintMode& mode) const = 0;
};

/// @brief Whether the equation `vector` holds is `0 = 0`, `0 = 1`, or a real
/// equation with a leading bit.
template <GF2Vector V>
ReduceResult classify(const V& vector) noexcept {
    const std::optional<std::size_t> leading = vector.leading_bit();
    if (!leading) {
        return ReduceResult::eq_true();
    }
    if (*leading + 1 == vector.num_bits()) {
        return ReduceResult::eq_false();
    }
    return ReduceResult::leading(*leading);
}

/// @brief Implements `EquationInterface` against any `GF2Vector`, allocated in one
/// block with the vector's buffer.
template <GF2Vector V>
class EquationTemplate final : public EquationInterface {
public:
    /// @brief The all-zeros vector of `num_bits` bits.
    static std::unique_ptr<EquationTemplate> create(std::size_t num_bits) {
        return std::unique_ptr<EquationTemplate>(
            new (BufferBytes{V::buffer_bytes(num_bits)}) EquationTemplate(num_bits));
    }

    static void operator delete(void* block) noexcept { ::operator delete(block); }

    const V& vector() const noexcept { return vector_; }

    V& vector() noexcept { return vector_; }

    EquationRepr repr() const noexcept override { return V::repr; }

    std::unique_ptr<EquationInterface> clone() const override {
        return std::unique_ptr<EquationTemplate>(
            new (BufferBytes{V::buffer_bytes(vector_.num_bits())}) EquationTemplate(*this));
    }

    std::size_t num_bits() const noexcept override { return vector_.num_bits(); }

    void resize(std::size_t num_bits) override { vector_.resize(num_bits); }

    void set_eq_true() noexcept override { vector_.set_zero(); }

    void set_eq_false() noexcept override {
        vector_.set_zero();
        vector_.set_bit(vector_.num_bits() - 1);
    }

    void assign_bits(std::span<const std::size_t> bits) override { vector_.assign_bits(bits); }

    std::optional<std::size_t> leading_bit() const noexcept override {
        return vector_.leading_bit();
    }

    ReduceResult classify() const noexcept override { return xorcle::classify(vector_); }

    void plus_assign(const EquationInterface& other) override {
        vector_.plus_assign(cast(other).vector_);
    }

    void assign_from(const EquationInterface& other) override {
        vector_.assign_from(cast(other).vector_);
    }

    std::unique_ptr<EquationInterface> compact(const CompactionTransform& transform)
        const override {
        std::unique_ptr<EquationTemplate> compacted = create(transform.new_num_vars() + 1);
        V& out = compacted->vector_;
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
        return compacted;
    }

    std::vector<std::size_t> bits() const override {
        // One pass over the words, through a buffer that up to 64 bits fill
        // before the list takes them in one allocation.
        std::array<std::size_t, 64> buffer;
        std::size_t num_buffered = 0;
        std::vector<std::size_t> out;
        vector_.for_each_set_bit([&](std::size_t bit) {
            if (num_buffered == buffer.size()) {
                out.insert(out.end(), buffer.begin(), buffer.end());
                num_buffered = 0;
            }
            buffer[num_buffered++] = bit;
        });
        out.insert(out.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(num_buffered));
        return out;
    }

    std::size_t num_set_bits() const noexcept override { return vector_.num_set_bits(); }

    bool equals(const EquationInterface& other) const noexcept override {
        return vector_.equals(cast(other).vector_);
    }

    bool equal_variables(const EquationInterface& other) const noexcept override {
        return vector_.equals_except_last_bit(cast(other).vector_);
    }

    std::size_t hash() const noexcept override { return vector_.hash(); }

    std::size_t variables_hash() const noexcept override { return vector_.hash_except_last_bit(); }

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
    /// @brief Downcast an interface whose representation the caller has already checked.
    static const EquationTemplate& cast(const EquationInterface& interface) noexcept {
        return static_cast<const EquationTemplate&>(interface);
    }

    struct BufferBytes {
        std::size_t value;
    };

    static void* operator new(std::size_t size, BufferBytes buffer_bytes) {
        return ::operator new(size + buffer_bytes.value);
    }

    static void operator delete(void* block, BufferBytes) noexcept { ::operator delete(block); }

    explicit EquationTemplate(std::size_t num_bits) : vector_(num_bits, buffer()) {}

    EquationTemplate(const EquationTemplate& other)
        : EquationInterface(), vector_(other.vector_, buffer()) {}

    /// @brief The `V::buffer_bytes()` bytes allocated after this object.
    void* buffer() noexcept { return this + 1; }

    V vector_;
};

} // namespace xorcle
