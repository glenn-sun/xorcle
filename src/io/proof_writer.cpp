#include "io/proof_writer.h"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <iterator>
#include <system_error>

namespace xorcle {

ProofRow lift_row(const Equation& row, std::span<const std::size_t> active_var_to_original,
                  std::size_t original_num_vars) {
    ProofRow lifted = row.bits();
    for (std::size_t& bit : lifted) {
        assert(bit <= active_var_to_original.size() && "a row bit is outside the numbering");
        bit = bit == active_var_to_original.size() ? original_num_vars
                                                   : active_var_to_original[bit];
    }
    // A renumbering keeps the variables' order, so the row stays ascending.
    assert(std::ranges::is_sorted(lifted) && "the numbering does not keep the variables' order");
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

namespace {

/// The buffer goes to the stream once it holds this many bytes.
constexpr std::size_t flush_bytes = std::size_t{1} << 16;

/// Seven bits a byte, the lowest first, the high bit set on every byte but the last.
void put_varint(std::string& buffer, std::uint64_t number) {
    while (number >= 0x80) {
        buffer.push_back(static_cast<char>((number & 0x7f) | 0x80));
        number >>= 7;
    }
    buffer.push_back(static_cast<char>(number));
}

} // namespace

ProofWriter::ProofWriter(std::ostream& out, std::size_t num_vars, ProofFormat format)
    : out_(out), num_vars_(num_vars), format_(format) {
    buffer_.reserve(flush_bytes);
}

ProofWriter::~ProofWriter() { flush(); }

void ProofWriter::flush() {
    out_.write(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    buffer_.clear();
}

void ProofWriter::put_number(std::uint64_t number) {
    if (format_ == ProofFormat::binary) {
        put_varint(buffer_, number);
        return;
    }
    char digits[20];
    const auto [end, error] = std::to_chars(std::begin(digits), std::end(digits), number);
    assert(error == std::errc{} && "a 64-bit number fits in 20 digits");
    buffer_.append(digits, end);
}

void ProofWriter::end_line() {
    if (format_ == ProofFormat::binary) {
        buffer_.push_back('\0');
    } else {
        buffer_.append(" 0\n");
    }
    if (buffer_.size() >= flush_bytes) {
        flush();
    }
}

void ProofWriter::begin_learned(ClauseId id) {
    if (format_ == ProofFormat::binary) {
        buffer_.push_back('a');
    }
    put_number(id + 1);
}

void ProofWriter::write_row(std::span<const std::size_t> row) {
    assert(!row.empty() && !(row.size() == 1 && row[0] == num_vars_) &&
           "a proof row is 0 = 0 or 0 = 1");
    const bool constant = row.back() == num_vars_;
    const std::span<const std::size_t> vars = row.first(row.size() - (constant ? 1U : 0U));
    if (format_ == ProofFormat::binary) {
        put_varint(buffer_, 2 * vars.size() + (constant ? 1U : 0U));
        std::size_t previous = 0;
        for (const std::size_t var : vars) {
            put_varint(buffer_, var + 1 - previous);
            previous = var + 1;
        }
        return;
    }
    buffer_.push_back(' ');
    if (constant) {
        buffer_.push_back('-');
    }
    for (std::size_t i = 0; i < vars.size(); ++i) {
        if (i != 0) {
            buffer_.push_back('+');
        }
        put_number(vars[i] + 1);
    }
}

void ProofWriter::write_reasons(const std::vector<ClauseId>& reasons) {
    if (format_ == ProofFormat::binary) {
        buffer_.push_back('\0');
    } else {
        buffer_.append(" 0");
    }
    for (const ClauseId reason : reasons) {
        if (format_ == ProofFormat::text) {
            buffer_.push_back(' ');
        }
        put_number(reason + 1);
    }
    end_line();
}

void ProofWriter::write_learned(ClauseId id, const Clause& clause,
                                std::span<const std::size_t> active_var_to_original,
                                const std::vector<ClauseId>& reasons) {
    begin_learned(id);
    for (const Equation& row : clause) {
        write_row(lift_row(row, active_var_to_original, num_vars_));
    }
    write_reasons(reasons);
}

void ProofWriter::write_learned(ClauseId id, std::span<const ProofRow> rows,
                                const std::vector<ClauseId>& reasons) {
    begin_learned(id);
    for (const ProofRow& row : rows) {
        write_row(row);
    }
    write_reasons(reasons);
}

void ProofWriter::write_empty(ClauseId id, const std::vector<ClauseId>& reasons) {
    begin_learned(id);
    write_reasons(reasons);
}

void ProofWriter::write_deleted(const std::vector<ClauseId>& ids) {
    assert(!ids.empty() && "a deletion line names a clause");
    if (format_ == ProofFormat::binary) {
        buffer_.push_back('d');
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (i != 0 && format_ == ProofFormat::text) {
            buffer_.push_back(' ');
        }
        put_number(ids[i] + 1);
    }
    end_line();
}

} // namespace xorcle
