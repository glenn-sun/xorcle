#pragma once

#include <mutex>
#include <string>
#include <fstream>

#include "util/Stats.h"

namespace stats {

class StatsSink {
public:
    virtual ~StatsSink() = default;
    void log(const AggregateStats& stats);

protected:
    virtual void log_impl(const AggregateStats& stats) = 0;

private:
    std::mutex mu_;
};

class ConsoleStatsSink : public StatsSink {
protected:
    void log_impl(const AggregateStats& stats) override;
};

class CsvStatsSink : public StatsSink {
public:
    explicit CsvStatsSink(std::string path);

protected:
    void log_impl(const AggregateStats& stats) override;

private:
    std::string path_;
    std::ofstream file_;
    bool header_written_ = false;

    void ensure_header();
    static std::string csv_escape(const std::string& value);
    static std::string fmt_opt_double(const std::optional<double>& value);
};

} // namespace stats
