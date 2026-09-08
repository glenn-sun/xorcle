#include "io/proof_raw_file.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

namespace xorcle {

namespace {

/// @brief What every raw proof file starts with, and the format it names.
constexpr char raw_proof_magic[] = {'X', 'R', 'A', 'W'};
constexpr unsigned char raw_proof_version = 1;

/// @brief The record tags, in the order `RawRecord` lists its alternatives.
enum class RawTag : unsigned char {
    learned = 1,
    empty = 2,
    compaction = 3,
    deleted = 4,
};

/// @brief A temporary path next to the final one, unique per writer instance.
std::filesystem::path make_temp_path(const std::filesystem::path& final_path) {
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    std::ostringstream name;
    name << final_path.filename().string() << ".tmp." << now;
    return final_path.parent_path() / name.str();
}

} // namespace

AtomicOutputFile::AtomicOutputFile(std::filesystem::path final_path, std::ios::openmode mode)
    : final_path_(std::move(final_path)),
      temp_path_(make_temp_path(final_path_)),
      out_(temp_path_, mode) {
    if (!out_.is_open()) {
        throw RawProofError("unable to open temp file: " + temp_path_.string());
    }
}

AtomicOutputFile::~AtomicOutputFile() {
    if (finalized_) {
        return;
    }
    out_.close();
    std::error_code ec;
    std::filesystem::remove(temp_path_, ec);
}

void AtomicOutputFile::finalize(bool keep) {
    if (finalized_) {
        return;
    }
    finalized_ = true;

    out_.flush();
    const bool write_failed = !out_.good();
    out_.close();

    std::error_code ec;
    if (!keep) {
        std::filesystem::remove(temp_path_, ec);
        return;
    }
    if (write_failed) {
        std::filesystem::remove(temp_path_, ec);
        throw RawProofError("failed to write " + temp_path_.string());
    }

    std::filesystem::remove(final_path_, ec);
    ec.clear();
    std::filesystem::rename(temp_path_, final_path_, ec);
    if (!ec) {
        return;
    }

    // Rename fails across filesystems; fall back to a copy so the file lands.
    ec.clear();
    std::filesystem::copy_file(temp_path_, final_path_,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        throw RawProofError("failed to write the final file: " + final_path_.string());
    }
    ec.clear();
    std::filesystem::remove(temp_path_, ec);
}

RawRow lift_row(const Equation& row, std::span<const std::size_t> active_var_to_original,
                std::size_t original_num_vars) {
    RawRow lifted = row.bits();
    for (std::size_t& bit : lifted) {
        assert(bit <= active_var_to_original.size() && "a row bit is outside the numbering");
        bit = bit == active_var_to_original.size() ? original_num_vars
                                                   : active_var_to_original[bit];
    }
    // The map need not be monotone, and the format wants ascending bits.
    std::sort(lifted.begin(), lifted.end());
    return lifted;
}

RawProofWriter::RawProofWriter(std::filesystem::path final_path, std::size_t num_vars,
                               std::size_t num_input_clauses)
    : file_(std::move(final_path), std::ios::out | std::ios::binary),
      num_vars_(num_vars),
      num_input_clauses_(num_input_clauses) {
    file_.out().write(raw_proof_magic, sizeof(raw_proof_magic));
    file_.out().put(static_cast<char>(raw_proof_version));
    write_varint(num_vars_);
    write_varint(num_input_clauses_);
}

void RawProofWriter::write_varint(std::size_t value) {
    do {
        const unsigned char byte = static_cast<unsigned char>(value & 0x7fu);
        value >>= 7;
        file_.out().put(static_cast<char>(value != 0 ? byte | 0x80u : byte));
    } while (value != 0);
}

void RawProofWriter::write_row(const RawRow& row) {
    write_varint(row.size());
    std::size_t previous = 0;
    for (const std::size_t bit : row) {
        assert((previous == 0 || bit > previous) && "row bits must be ascending and distinct");
        write_varint(bit - previous);
        previous = bit;
    }
}

void RawProofWriter::write_ids(const std::vector<ClauseId>& ids) {
    write_varint(ids.size());
    for (const ClauseId id : ids) {
        write_varint(id);
    }
}

void RawProofWriter::write_learned(ClauseId id, const Clause& clause,
                                   std::span<const std::size_t> active_var_to_original,
                                   const std::vector<ClauseId>& reasons) {
    file_.out().put(static_cast<char>(RawTag::learned));
    write_varint(id);
    write_varint(clause.rows());
    for (const Equation& row : clause) {
        write_row(lift_row(row, active_var_to_original, num_vars_));
    }
    write_ids(reasons);
}

void RawProofWriter::write_empty(ClauseId id, const std::vector<ClauseId>& reasons) {
    file_.out().put(static_cast<char>(RawTag::empty));
    write_varint(id);
    write_ids(reasons);
}

void RawProofWriter::write_compaction(const std::vector<RawUnit>& units) {
    file_.out().put(static_cast<char>(RawTag::compaction));
    write_varint(units.size());
    for (const RawUnit& unit : units) {
        write_row(unit.row);
        write_varint(unit.reason);
    }
}

void RawProofWriter::write_deleted(const std::vector<ClauseId>& ids) {
    assert(!ids.empty() && "a deletion record names at least one clause");
    file_.out().put(static_cast<char>(RawTag::deleted));
    write_ids(ids);
}

RawProofFileSource::RawProofFileSource(const std::filesystem::path& path)
    : in_(path, std::ios::in | std::ios::binary) {
    if (!in_.is_open()) {
        throw RawProofError("unable to open raw proof: " + path.string());
    }

    char magic[sizeof(raw_proof_magic)] = {};
    in_.read(magic, sizeof(magic));
    if (!in_.good() || !std::equal(std::begin(magic), std::end(magic),
                                   std::begin(raw_proof_magic))) {
        throw RawProofError("not a raw proof file: " + path.string());
    }
    const unsigned char version = read_byte();
    if (version != raw_proof_version) {
        throw RawProofError("raw proof version " + std::to_string(version) +
                            " is not the version this build writes");
    }
    num_vars_ = read_varint();
    num_input_clauses_ = read_varint();
    records_begin_ = in_.tellg();
}

unsigned char RawProofFileSource::read_byte() {
    const std::ifstream::int_type byte = in_.get();
    if (byte == std::ifstream::traits_type::eof()) {
        throw RawProofError("the raw proof ends inside a record");
    }
    return static_cast<unsigned char>(byte);
}

std::size_t RawProofFileSource::read_varint() {
    std::size_t value = 0;
    for (std::size_t shift = 0;; shift += 7) {
        if (shift >= sizeof(std::size_t) * 8) {
            throw RawProofError("the raw proof holds an oversized varint");
        }
        const unsigned char byte = read_byte();
        value |= static_cast<std::size_t>(byte & 0x7fu) << shift;
        if ((byte & 0x80u) == 0) {
            return value;
        }
    }
}

RawRow RawProofFileSource::read_row() {
    RawRow row(read_varint());
    std::size_t previous = 0;
    for (std::size_t& bit : row) {
        previous += read_varint();
        bit = previous;
    }
    return row;
}

std::vector<ClauseId> RawProofFileSource::read_ids() {
    std::vector<ClauseId> ids(read_varint());
    for (ClauseId& id : ids) {
        id = read_varint();
    }
    return ids;
}

bool RawProofFileSource::next(RawRecord& record) {
    const std::ifstream::int_type tag = in_.get();
    if (tag == std::ifstream::traits_type::eof()) {
        return false;
    }

    switch (static_cast<RawTag>(tag)) {
        case RawTag::learned: {
            RawLearned learned;
            learned.id = read_varint();
            learned.rows.resize(read_varint());
            for (RawRow& row : learned.rows) {
                row = read_row();
            }
            learned.reasons = read_ids();
            record = std::move(learned);
            return true;
        }
        case RawTag::empty: {
            RawEmpty empty;
            empty.id = read_varint();
            empty.reasons = read_ids();
            record = std::move(empty);
            return true;
        }
        case RawTag::compaction: {
            RawCompaction compaction;
            compaction.units.resize(read_varint());
            for (RawUnit& unit : compaction.units) {
                unit.row = read_row();
                unit.reason = read_varint();
            }
            record = std::move(compaction);
            return true;
        }
        case RawTag::deleted: {
            record = RawDeleted{read_ids()};
            return true;
        }
    }
    throw RawProofError("the raw proof holds an unknown record tag " + std::to_string(tag));
}

void RawProofFileSource::rewind() {
    in_.clear();
    in_.seekg(records_begin_);
}

} // namespace xorcle
