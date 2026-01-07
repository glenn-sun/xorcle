#pragma once

#include <variant>
#include <queue>
#include <unordered_set>
#include "bit/vector.h"
#include "RowSpace.h"
#include "Equation.h"

using ClauseId = std::size_t;
using EquationId = std::size_t;

enum class WatchType { Zero, One, Sum };

class ClauseSelector {
protected:
    std::vector<Clause> clauses;
public:
    ClauseSelector() : clauses() {}
    virtual ~ClauseSelector() = default;

    std::size_t get_num_clauses() const { return clauses.size(); }
    const std::vector<Clause>& get_clauses() const { return clauses; }
    Clause& get_clause(ClauseId id) { return clauses[id]; }

    virtual std::optional<ClauseId> next() = 0;
    virtual void add_starting_clause(const Clause& clause) { clauses.push_back(clause); }
    // Trail after backjumping
    virtual void add_asserting_clause(const Clause& clause, const Trail& trail) { add_starting_clause(clause); }
    virtual void update_with_new_equation(const Equation& eqn, const Trail& trail) {};
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) {};
};

class Cyclic : public ClauseSelector {
private:
    ClauseId id;
    std::size_t count;
public:
    Cyclic(std::size_t num_vars) : ClauseSelector(), id(0), count(0) {}
    std::optional<ClauseId> next() override {
        if (count == clauses.size()) {
            count = 0;
            return std::nullopt;
        }
        std::size_t old_id = id;
        id = (id + 1) % clauses.size();
        ++count;
        return old_id;
    }

    void add_starting_clause(const Clause& clause) override {
        clauses.push_back(clause);
        id = clauses.size() - 1;
    }

    void update_with_new_equation(const Equation& eqn, const Trail& trail) override { 
        count = 0;
    }
};

class RandomCyclic : public ClauseSelector {
private:
    std::mt19937 rng;
    std::size_t count;
    std::size_t index;
    std::vector<ClauseId> permutation;
public:
    RandomCyclic(std::size_t num_vars) : ClauseSelector(), rng(std::random_device{}()), count(0), index(0), permutation() {}
    std::optional<ClauseId> next() override {
        if (count == clauses.size()) {
            count = 0;
            return std::nullopt;
        }
        std::size_t old_index = index;
        index = (index + 1) % clauses.size();
        ++count;
        return permutation[old_index];
    }

    void add_starting_clause(const Clause& clause) override {
        clauses.push_back(clause);
        std::shuffle(permutation.begin(), permutation.end(), rng);
        permutation.push_back(clauses.size() - 1);
        index = clauses.size() - 1;
    }

    void update_with_new_equation(const Equation& eqn, const Trail& trail) override { 
        count = 0;
    }
};

// class WatchedId {
// private:
//     std::size_t raw_;
//     static constexpr std::size_t null_value = std::numeric_limits<std::size_t>::max();

// public:
//     constexpr WatchedId() noexcept : raw_(null_value) {}
//     constexpr WatchedId(std::size_t raw) noexcept : raw_(raw) {}
//     constexpr WatchedId(ClauseId id, WatchType wt) noexcept
//       : raw_(3 * id + static_cast<std::size_t>(wt)) {}

//     constexpr ClauseId clauseId() const noexcept { return raw_ / 3; }
//     constexpr WatchType watchType() const noexcept { return static_cast<WatchType>(raw_ % 3); }
//     constexpr bool has_value() const noexcept { return raw_ != null_value; }

//     // implicit conversion to size_t
//     constexpr operator std::size_t() const noexcept { return raw_; }

//     friend constexpr auto operator<=>(WatchedId, WatchedId) = default;
// };

// class ExactWatchlist : public ClauseSelector {
// private:
//     std::size_t num_vars;

//     // Reduced copy of every watched equation, maps WatchedId -> Equation
//     std::vector<Equation> equations;

//     // Next pointer, maps WatchedId -> WatchedId
//     std::vector<WatchedId> bucket_next;
//     // Prev pointer, maps WatchedId -> WatchedId
//     std::vector<WatchedId> bucket_prev;
//     // Head pointer, maps variable -> WatchedId
//     // bucket_head[n] is the head of the list currently reducing to 0.
//     std::vector<WatchedId> bucket_head;
//     // Maps WatchedId -> variable
//     std::vector<std::size_t> which_bucket;

//     std::optional<std::size_t> const verify_acylic() {
//         for (std::size_t i = 0; i < bucket_head.size(); ++i) {
//             std::vector<bool> visited(clauses.size() * 3 + 3, false);
//             WatchedId head = bucket_head[i];
//             while (head.has_value()) {
//                 if (visited[head]) {
//                     return i;
//                 }
//                 visited[head] = true;
//                 head = bucket_next[head];
//             }
//         }
//         return std::nullopt;
//     }

//     void add_to_bucket(WatchedId id, std::size_t bucket) {
//         if (!id.has_value()) {
//             std::cout << "id.has_value() == false\n";
//         }
//         if (bucket_head[bucket].has_value()) {
//             bucket_prev[bucket_head[bucket]] = id;
//         }
//         bucket_next[id] = bucket_head[bucket];
//         bucket_prev[id] = WatchedId();
//         which_bucket[id] = bucket;
//         bucket_head[bucket] = id;

//         // auto acyclic = verify_acylic();
//         // if (acyclic.has_value()) {
//         //     std::cout << "cyclic: " << acyclic.value() << "\n";
//         // }
//     }

//     void move_to_bucket(WatchedId id, std::size_t new_bucket, const Trail& trail) {
//         if (new_bucket == which_bucket[id]) {
//             return;
//         }

//         // (A) -- id -- (B)
//         if (bucket_prev[id].has_value()) {
//             // A -> B
//             bucket_next[bucket_prev[id]] = bucket_next[id];
//         } else {
//             // id was head
//             // Now B is head
//             bucket_head[which_bucket[id]] = bucket_next[id];
//         }

//         if (bucket_next[id].has_value()) {
//             // B -> A
//             bucket_prev[bucket_next[id]] = bucket_prev[id];
//         }

//         add_to_bucket(id, new_bucket);
//     }

//     std::queue<ClauseId> queue;

//     Equation working_eqn;

// public:
//     ExactWatchlist(std::size_t num_vars) : ClauseSelector(), num_vars(num_vars), equations(), bucket_next(), bucket_prev(), bucket_head(num_vars+1), which_bucket(), working_eqn(num_vars+1) {} //!

//     std::optional<ClauseId> next() override {
//         if (queue.empty()) {
//             return std::nullopt;
//         }
//         ClauseId id = queue.front();
//         queue.pop();
//         return id;
//     }

//     void add_starting_clause(const Clause& clause) override {
//         equations.push_back(clause.row(clause.watched0));
//         bucket_next.push_back(WatchedId());
//         bucket_prev.push_back(WatchedId());
//         which_bucket.push_back(0);
//         add_to_bucket(WatchedId(clauses.size(), WatchType::Zero), clause.row(clause.watched0).first_set());
        
//         if (clause.rows() > 1) {
//             equations.push_back(clause.row(clause.watched1));
//             bucket_next.push_back(WatchedId());
//             bucket_prev.push_back(WatchedId());
//             which_bucket.push_back(0);
//             add_to_bucket(WatchedId(clauses.size(), WatchType::One), clause.row(clause.watched1).first_set());
        
//             Equation sum = clause.row(clause.watched0) + clause.row(clause.watched1);
//             equations.push_back(sum);
//             bucket_next.push_back(WatchedId());
//             bucket_prev.push_back(WatchedId());
//             which_bucket.push_back(0);
//             add_to_bucket(WatchedId(clauses.size(), WatchType::Sum), sum.first_set());
//         } else {
//             // Add placeholders for watched1 and sum
//             equations.push_back(Equation());
//             bucket_next.push_back(WatchedId());
//             bucket_prev.push_back(WatchedId());
//             which_bucket.push_back(0);
//             equations.push_back(Equation());
//             bucket_next.push_back(WatchedId());
//             bucket_prev.push_back(WatchedId());
//             which_bucket.push_back(0);

//             queue.push(clauses.size());
//         }

//         clauses.push_back(clause);
//     }

//     void add_asserting_clause(const Clause& clause, const Trail& trail) override {
//         add_starting_clause(clause);

//         // Recompute all buckets from new trail
//         // TODO: remove this, make faster
//         std::fill(equations.begin(), equations.end(), Equation());
//         std::fill(bucket_next.begin(), bucket_next.end(), WatchedId());
//         std::fill(bucket_prev.begin(), bucket_prev.end(), WatchedId());
//         std::fill(bucket_head.begin(), bucket_head.end(), WatchedId());
//         std::fill(which_bucket.begin(), which_bucket.end(), 0);
//         queue = std::queue<ClauseId>();

        
//         for (ClauseId id = 0; id < clauses.size(); ++id) {
//             const Clause& c = clauses[id];
//             WatchedId wid_zero = WatchedId(id, WatchType::Zero);
//             WatchedId wid_one = WatchedId(id, WatchType::One);
//             WatchedId wid_sum = WatchedId(id, WatchType::Sum);

//             equations[wid_zero] = c.row(c.watched0);
//             trail.span().reduce(equations[wid_zero]);
//             std::size_t zero_first_set = equations[wid_zero].first_set();
//             if (zero_first_set == bit::vector<>::npos) {
//                 zero_first_set = num_vars;
//             }
//             add_to_bucket(wid_zero, zero_first_set);

//             if (c.rows() > 1) {
//                 equations[wid_one] = c.row(c.watched1);
//                 trail.span().reduce(equations[wid_one]);
//                 std::size_t one_first_set = equations[wid_one].first_set();
//                 if (one_first_set == bit::vector<>::npos) {
//                     one_first_set = num_vars;
//                 }
//                 add_to_bucket(wid_one, one_first_set);

//                 equations[wid_sum] = c.row(c.watched0) + c.row(c.watched1);
//                 trail.span().reduce(equations[wid_sum]);
//                 std::size_t sum_first_set = equations[wid_sum].first_set();
//                 if (sum_first_set == bit::vector<>::npos) {
//                     sum_first_set = num_vars;
//                 }
//                 add_to_bucket(wid_sum, sum_first_set);
//             } else {
//                 queue.push(id);
//             }
//         }

//         WatchedId head = bucket_head[num_vars];
//         while (head.has_value()) {
//             queue.push(head.clauseId());
//             head = bucket_next[head];
//         }
//     }

//     void update_with_new_equation(const Equation& eqn, const Trail& trail) override {
//         std::size_t first_set = eqn.first_set();
//         WatchedId head = bucket_head[first_set];
//         bucket_head[first_set] = WatchedId();

//         while (head.has_value()) {
//             WatchedId next = bucket_next[head];

//             // Reduce equations[head] with eqn
//             equations[head] += eqn;
//             std::size_t new_first_set = equations[head].first_set();

//             // Reclassify equations[head]
//             if (new_first_set == bit::vector<>::npos) {
//                 queue.push(head.clauseId());
//                 add_to_bucket(head, num_vars);
//             } else {
//                 add_to_bucket(head, new_first_set);
//             }

//             head = next;
//         }
//     }

//     void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override {
//         // Find new bucket
//         EquationId new_watch;
//         switch (wt) {
//             case WatchType::Zero:
//                 new_watch = clauses[id].watched0;
//                 break;
//             case WatchType::One:
//                 new_watch = clauses[id].watched1;
//                 break;
//             case WatchType::Sum:
//                 // Should never happen
//                 break;
//         }
//         working_eqn = clauses[id].row(new_watch);
//         trail.span().reduce(working_eqn);
//         std::size_t new_first_set = working_eqn.first_set();

//         // Remove watch from current linked list and add to new linked list
//         WatchedId wid = WatchedId(id, wt);
//         move_to_bucket(wid, new_first_set, trail);

//         // Do the same with sum
//         if (clauses[id].rows() > 1) {
//             working_eqn = clauses[id].row(clauses[id].watched0) + clauses[id].row(clauses[id].watched1);
//             trail.span().reduce(working_eqn);
//             new_first_set = working_eqn.first_set();
//             wid = WatchedId(id, WatchType::Sum);
//             move_to_bucket(wid, new_first_set, trail);
//         }
//     }
// };

// class LTWatchlist : public ClauseSelector {
// private:
//     // Map from leading term to list of clauses watching 
//     // some equation with that leading term
//     std::vector<std::vector<ClauseId>> by_leading_term;

//     std::vector<ClauseId> stack;
//     std::vector<bool> stack_set;

//     void push(ClauseId id) {
//         if (id >= stack_set.size()) {
//             stack.push_back(id);
//             stack_set.resize(id + 1);
//             stack_set[id] = true;
//             return;
//         }
//         if (!stack_set[id]) {
//             stack.push_back(id);
//             stack_set[id] = true;
//         }
//     }

//     void append(const std::vector<ClauseId>& ids) {
//         for (ClauseId id : ids) {
//             push(id);
//         }
//     }

//     ClauseId get_pop() {
//         ClauseId id = stack.back();
//         stack.pop_back();
//         stack_set[id] = false;
//         return id;
//     }

// public:
//     LTWatchlist(std::size_t num_vars) : ClauseSelector(), by_leading_term(num_vars+1) {}
//     std::optional<ClauseId> next() override {
//         if (stack.empty()) {
//             return std::nullopt;
//         }
//         return get_pop();
//     }

//     void add_starting_clause(const Clause& clause) override {
//         by_leading_term[clause.row(clause.watched0).first_set()].push_back(clauses.size());
//         if (clause.rows() > 1) {
//             by_leading_term[clause.row(clause.watched1).first_set()].push_back(clauses.size());
//         }
//         clauses.push_back(clause);
//     }

//     void add_asserting_clause(const Clause& clause, const Trail& trail) override {
//         add_starting_clause(clause);
//         clauses.push_back(clause);
//     }

//     // Equation must not yet be learned into trail
//     void update_with_new_equation(const Equation& eqn, const Trail& trail) override {
//         // Because of linear combinations, need to check all lists corresponding
//         // to reducible leading terms in position at most reduced.first_set()

//         Equation reduced = eqn;
//         trail.span().reduce(reduced);

//         for (std::size_t i = 0; i < reduced.first_set(); ++i) {
//             if (trail.span().get_pivot_cols()[i]) {
//                 append(by_leading_term[i]);
//             }
//         }
//         append(by_leading_term[reduced.first_set()]);
//     }

//     void update_with_new_watch(ClauseId id, EquationId old_watch, WatchType wt, const Trail& trail) override {
//         auto& v = by_leading_term[clauses[id].row(old_watch).first_set()];
//         v.erase(std::remove(v.begin(), v.end(), id), v.end());
//         EquationId new_watch;
//         switch (wt) {
//             case WatchType::Zero:
//                 new_watch = clauses[id].watched0;
//                 break;
//             case WatchType::One:
//                 new_watch = clauses[id].watched1;
//                 break;
//             case WatchType::Sum:
//                 // Should never happen
//                 break;
//         }
//         by_leading_term[clauses[id].row(new_watch).first_set()].push_back(id);
//     }

//     // void reset_after_backjump(const Trail& trail) override {
//     //     queue = std::queue<ClauseId>();
//     //     queue_set = std::vector<bool>();

//     //     for (std::size_t i = 0; i < trail.span().get_pivot_cols().size(); ++i) {
//     //         if (trail.span().get_pivot_cols()[i]) {
//     //             append(by_leading_term[i]);
//     //         }
//     //     }
//     // }
// };