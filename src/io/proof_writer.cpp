#include "io/proof_writer.h"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace xorcle {

ProofRow lift_row(const Equation& row, std::span<const std::size_t> active_var_to_original,
                  std::size_t original_num_vars) {
    ProofRow lifted = row.bits();
    for (std::size_t& bit : lifted) {
        assert(bit <= active_var_to_original.size() && "a row bit is outside the numbering");
        bit = bit == active_var_to_original.size() ? original_num_vars
                                                   : active_var_to_original[bit];
    }
    // The map need not be monotone, and a row is written in ascending order.
    std::sort(lifted.begin(), lifted.end());
    return lifted;
}

ProofRow lift_row(const VarListEquation& row, std::span<const std::size_t> active_var_to_original,
                  std::size_t original_num_vars) {
    ProofRow lifted;
    lifted.reserve(row.vars.size() + 1);
    for (const std::uint32_t var : row.vars) {
        lifted.push_back(active_var_to_original[var]);
    }
    // A renumbering keeps the variables' order, so the row stays ascending.
    if (row.constant) {
        lifted.push_back(original_num_vars);
    }
    return lifted;
}

void ProofWriter::write_row(const ProofRow& row) {
    assert(!row.empty() && !(row.size() == 1 && row[0] == num_vars_) &&
           "a proof row is 0 = 0 or 0 = 1");
    out_ << ' ';
    const bool constant = row.back() == num_vars_;
    if (constant) {
        out_ << '-';
    }
    const std::size_t num_row_vars = row.size() - (constant ? 1U : 0U);
    for (std::size_t i = 0; i < num_row_vars; ++i) {
        if (i != 0) {
            out_ << '+';
        }
        out_ << row[i] + 1;
    }
}

void ProofWriter::write_reasons(const std::vector<ClauseId>& reasons) {
    out_ << " 0";
    for (const ClauseId reason : reasons) {
        out_ << ' ' << reason + 1;
    }
    out_ << " 0\n";
}

void ProofWriter::write_learned(ClauseId id, const Clause& clause,
                                std::span<const std::size_t> active_var_to_original,
                                const std::vector<ClauseId>& reasons) {
    out_ << id + 1;
    for (const Equation& row : clause) {
        write_row(lift_row(row, active_var_to_original, num_vars_));
    }
    write_reasons(reasons);
}

void ProofWriter::write_learned(ClauseId id, std::span<const ProofRow> rows,
                                const std::vector<ClauseId>& reasons) {
    out_ << id + 1;
    for (const ProofRow& row : rows) {
        write_row(row);
    }
    write_reasons(reasons);
}

void ProofWriter::write_empty(ClauseId id, const std::vector<ClauseId>& reasons) {
    out_ << id + 1;
    write_reasons(reasons);
}

void ProofWriter::write_deleted(const std::vector<ClauseId>& ids) {
    assert(!ids.empty() && "a deletion line names a clause");
    const char* separator = "";
    for (const ClauseId id : ids) {
        out_ << separator << id + 1;
        separator = " ";
    }
    out_ << " 0\n";
}

} // namespace xorcle
