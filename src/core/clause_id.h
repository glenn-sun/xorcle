#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>

namespace xorcle {

/// @brief Identifier of a clause: its index in the clause database.
using ClauseId = std::size_t;

/// @brief A row index into a clause, not into the trail span.
using ClauseRowIdx = std::size_t;

/// @brief The active clause ids in ascending order, as a range.
///
/// Walking it costs the active clauses, not every id issued. Adding a clause or
/// making one inactive invalidates a walk in progress.
class ActiveClauseIds {
public:
    class Iterator {
    public:
        Iterator(const ClauseId* listed, const ClauseId* listed_end,
                 const std::uint8_t* is_active) noexcept
            : listed_(listed), listed_end_(listed_end), is_active_(is_active) {
            skip_inactive();
        }

        ClauseId operator*() const noexcept { return *listed_; }

        Iterator& operator++() noexcept {
            ++listed_;
            skip_inactive();
            return *this;
        }

        bool operator==(std::default_sentinel_t) const noexcept { return listed_ == listed_end_; }

    private:
        void skip_inactive() noexcept {
            while (listed_ != listed_end_ && is_active_[*listed_] == 0) {
                ++listed_;
            }
        }

        const ClauseId* listed_;
        const ClauseId* listed_end_;
        const std::uint8_t* is_active_;
    };

    /// @param listed_ids Ascending ids, every active one among them.
    /// @param is_active Per clause id, nonzero for an active clause.
    ActiveClauseIds(const std::vector<ClauseId>& listed_ids,
                    const std::vector<std::uint8_t>& is_active) noexcept
        : listed_ids_(&listed_ids), is_active_(&is_active) {}

    Iterator begin() const noexcept {
        return {listed_ids_->data(), listed_ids_->data() + listed_ids_->size(), is_active_->data()};
    }

    std::default_sentinel_t end() const noexcept { return std::default_sentinel; }

    /// @pre `id` is a clause id of the database.
    bool contains(ClauseId id) const noexcept { return (*is_active_)[id] != 0; }

    /// @brief At least the number of active ids.
    std::size_t max_size() const noexcept { return listed_ids_->size(); }

private:
    const std::vector<ClauseId>* listed_ids_;
    const std::vector<std::uint8_t>* is_active_;
};

} // namespace xorcle
