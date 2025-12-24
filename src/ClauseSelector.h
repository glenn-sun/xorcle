#include <variant>
#include <queue>
#include <unordered_set>
#include "bit/vector.h"
#include "RowSpace.h"
#include "Equation.h"

using ClauseId = std::size_t;
using EquationId = std::size_t;

class ClauseSelectorBase {
protected:
    std::vector<Clause> clauses;
public:
    ClauseSelectorBase() : clauses() {}
    std::size_t get_num_clauses() const { return clauses.size(); }
    const std::vector<Clause>& get_clauses() const { return clauses; }
    Clause& get_clause(ClauseId id) { return clauses[id]; }

    virtual std::optional<ClauseId> next() = 0;
    virtual void add_clause(const Clause& clause) { clauses.push_back(clause); }
    virtual void update_with_new_equation(const Equation& eqn, const Trail& trail) {};
    virtual void update_with_new_watch(ClauseId id, EquationId old_watch, EquationId new_watch) {};
};

class Cyclic : public ClauseSelectorBase {
private:
    ClauseId id;
    std::size_t count;
public:
    Cyclic(std::size_t num_vars) : ClauseSelectorBase(), id(0), count(0) {}
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

    void update_with_new_equation(const Equation& eqn, const Trail& trail) override { 
        count = 0;
    }
};

class LTWatchlist : public ClauseSelectorBase {
private:
    // Map from leading term to list of clauses watching 
    // some equation with that leading term
    std::vector<std::vector<ClauseId>> by_leading_term;

    std::queue<ClauseId> queue;
    std::unordered_set<ClauseId> queue_set;

    void push(ClauseId id) {
        if (!queue_set.contains(id)) {
            queue.push(id);
            queue_set.insert(id);
        }
    }

    void append(const std::vector<ClauseId>& ids) {
        for (ClauseId id : ids) {
            push(id);
        }
    }

    ClauseId get_pop() {
        ClauseId id = queue.front();
        queue.pop();
        queue_set.erase(id);
        return id;
    }

public:
    LTWatchlist(std::size_t num_vars) : ClauseSelectorBase(), by_leading_term(num_vars+1) {}
    std::optional<ClauseId> next() override {
        if (queue.empty()) {
            return std::nullopt;
        }
        return get_pop();
    }

    void add_clause(const Clause& clause) override {
        by_leading_term[clause.row(clause.watched0).first_set()].push_back(clauses.size());
        if (clause.rows() > 1) {
            by_leading_term[clause.row(clause.watched1).first_set()].push_back(clauses.size());
        }
        clauses.push_back(clause);
    }

    // Trail must be before equation is learned
    void update_with_new_equation(const Equation& eqn, const Trail& trail) override {
        // Because of linear combinations, need to check all lists corresponding
        // to reducible leading terms in position at most reduced.first_set()

        Equation reduced = trail.span().reduce_copy(eqn);

        for (std::size_t i = 0; i < reduced.first_set(); ++i) {
            if (trail.span().get_pivot_cols()[i]) {
                append(by_leading_term[i]);
            }
        }
        append(by_leading_term[reduced.first_set()]);
    }

    void update_with_new_watch(ClauseId id, EquationId old_watch, EquationId new_watch) override {
        auto& v = by_leading_term[clauses[id].row(old_watch).first_set()];
        v.erase(std::remove(v.begin(), v.end(), id), v.end());
        by_leading_term[clauses[id].row(new_watch).first_set()].push_back(id);
    }
};

using ClauseSelector = std::variant<Cyclic, LTWatchlist>;