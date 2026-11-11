#pragma once

/// @file exact_watchlist_cells.h
/// @brief The exact watchlist's cells and their saves, per representation.

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "algebra/compaction_transform.h"
#include "algebra/dense/dense_gf2_vector.h"
#include "algebra/equation.h"
#include "algebra/gf2_vector.h"

namespace xorcle {

/// @brief A cell's bits, read and written whole-cell; `Row` is how a cell or a row of
/// the trail is read, and `npos` is the lead of a cell that is `0 = 0`.
///
/// A save is the cell's state from the bit `from_bit` on; the bits below it are zero.
template <GF2Vector V>
class ExactWatchlistCells {
public:
    using Row = V;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    static const Row* row_of(const V& vector) { return &vector; }

    void reset(std::size_t num_cols) {
        num_cols_ = num_cols;
        cells_.clear();
        num_saves_ = 0;
    }

    void add_pair() {
        cells_.emplace_back(num_cols_);
        cells_.emplace_back(num_cols_);
    }

    Row* cell(std::uint32_t cell) { return &cells_[cell]; }
    const Row* cell(std::uint32_t cell) const { return &cells_[cell]; }

    static std::size_t leading_bit(const Row* row) { return row->leading_bit().value_or(npos); }

    /// @brief Add `row` into `cell`, both leading on `bit`; the bit the sum leads on.
    static std::size_t add_and_next(Row* cell, const Row* row, std::size_t bit) {
        return cell->plus_assign_and_next_bit(*row, bit).value_or(npos);
    }

    static void copy(Row* to, const Row* from) { to->assign_from(*from); }
    static void copy_out(V& to, const Row* cell) { to.assign_from(*cell); }

    std::size_t save_mark() const { return num_saves_; }

    /// @brief Keep the cell's state; where it is kept.
    std::size_t save(const Row* cell, std::size_t /*from_bit*/) {
        if (num_saves_ == saves_.size()) {
            saves_.push_back(*cell);
        } else {
            saves_[num_saves_] = *cell;
        }
        return num_saves_++;
    }

    void copy_back(Row* cell, std::size_t /*from_bit*/, std::size_t offset) const {
        *cell = saves_[offset];
    }

    void drop_saves_from(std::size_t mark) { num_saves_ = mark; }

    /// @brief Relabel every cell to the transform's numbering; a dropped column's bit goes.
    void relabel(const CompactionTransform& transform, std::size_t new_num_cols) {
        const std::size_t old_constant_col_idx = num_cols_ - 1;
        std::vector<std::size_t> bits;
        for (V& cell : cells_) {
            bits.clear();
            cell.for_each_set_bit([&](std::size_t col_idx) {
                const std::size_t new_col_idx = col_idx == old_constant_col_idx
                                                    ? new_num_cols - 1
                                                    : transform.to_new(col_idx);
                if (new_col_idx != CompactionTransform::npos) {
                    bits.push_back(new_col_idx);
                }
            });
            cell.set_zero();
            cell.resize(new_num_cols);
            cell.assign_bits(bits);
        }
        num_cols_ = new_num_cols;
    }

    Equation equation_of(const Row* cell) const {
        std::vector<std::size_t> bits;
        cell->for_each_set_bit([&](std::size_t bit) { bits.push_back(bit); });
        return Equation::from_bits(V::repr, num_cols_ - 1, bits);
    }

private:
    std::size_t num_cols_ = 0;
    std::vector<V> cells_;
    std::vector<V> saves_; ///< The first `num_saves_` are the saved cells.
    std::size_t num_saves_ = 0;
};

/// @brief The dense cells: one arena of words, and a save keeps a cell's words from
/// the one holding `from_bit` on.
template <>
class ExactWatchlistCells<DenseGF2Vector> {
public:
    using Word = DenseGF2Vector::Word;
    using Row = Word;

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    static const Row* row_of(const DenseGF2Vector& vector) { return vector.words(); }

    void reset(std::size_t num_cols) {
        num_cols_ = num_cols;
        num_words_ = (num_cols + word_mask) >> word_shift;
        words_.clear();
        num_save_words_ = 0;
    }

    void add_pair() { words_.resize(words_.size() + 2 * num_words_); }

    Word* cell(std::uint32_t cell) { return words_.data() + cell * num_words_; }
    const Word* cell(std::uint32_t cell) const { return words_.data() + cell * num_words_; }

    std::size_t leading_bit(const Word* words) const {
        return DenseGF2Vector::leading_bit(words, num_words_).value_or(npos);
    }

    std::size_t add_and_next(Word* cell, const Word* row, std::size_t bit) const {
        // Unlike `DenseGF2Vector::plus_assign_and_next_bit`, the words after the sum's
        // first nonzero one are added in a loop of their own.
        for (std::size_t w = bit >> word_shift; w < num_words_; ++w) {
            const Word word = (cell[w] ^= row[w]);
            if (word != 0) {
                for (std::size_t rest = w + 1; rest < num_words_; ++rest) {
                    cell[rest] ^= row[rest];
                }
                return (w << word_shift) + static_cast<std::size_t>(std::countr_zero(word));
            }
        }
        return npos;
    }

    void copy(Word* to, const Word* from) const {
        DenseGF2Vector::copy_words(to, from, num_words_);
    }
    static void copy_out(DenseGF2Vector& to, const Word* cell) { to.assign_from_words(cell); }

    std::size_t save_mark() const { return num_save_words_; }

    std::size_t save(const Word* cell, std::size_t from_bit) {
        const std::size_t offset = num_save_words_;
        const std::size_t first_word = from_bit >> word_shift;
        const std::size_t num_saved = num_words_ - first_word;
        if (save_words_.size() - offset < num_saved) {
            save_words_.resize(std::max(2 * save_words_.size(), offset + num_saved));
        }
        DenseGF2Vector::copy_words(save_words_.data() + offset, cell + first_word, num_saved);
        num_save_words_ = offset + num_saved;
        return offset;
    }

    void copy_back(Word* cell, std::size_t from_bit, std::size_t offset) const {
        const std::size_t first_word = from_bit >> word_shift;
#ifndef NDEBUG
        for (std::size_t w = 0; w < first_word; ++w) {
            assert(cell[w] == 0 && "a cell led below its saved lead under the same watch");
        }
#endif
        DenseGF2Vector::copy_words(cell + first_word, save_words_.data() + offset,
                                   num_words_ - first_word);
    }

    void drop_saves_from(std::size_t mark) { num_save_words_ = mark; }

    void relabel(const CompactionTransform& transform, std::size_t new_num_cols);

    Equation equation_of(const Word* cell) const {
        std::vector<std::size_t> bits;
        for (std::size_t w = 0; w < num_words_; ++w) {
            for (Word word = cell[w]; word != 0; word &= word - 1) {
                bits.push_back((w << word_shift) + static_cast<std::size_t>(std::countr_zero(word)));
            }
        }
        return Equation::from_bits(EquationRepr::dense, num_cols_ - 1, bits);
    }

private:
    static constexpr std::size_t word_shift = DenseGF2Vector::word_shift;
    static constexpr std::size_t word_mask = DenseGF2Vector::word_mask;

    std::size_t num_cols_ = 0;
    std::size_t num_words_ = 0;
    /// @brief Two cells per pair, `A` then `B`, each `num_words_` words.
    std::vector<Word> words_;
    std::vector<Word> save_words_; ///< The saved cells' words are its first `num_save_words_`.
    std::size_t num_save_words_ = 0;
};

} // namespace xorcle
