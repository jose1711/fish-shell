// Functions for executing the time builtin.
#include "config.h"  // IWYU pragma: keep

#include "timer.h"

#include <string.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <ctime>

#if HAVE_CURSES_H
#include <curses.h>
#elif HAVE_NCURSES_H
#include <ncurses.h>
#elif HAVE_NCURSES_CURSES_H
#include <ncurses/curses.h>
#endif
#if HAVE_TERM_H
#include <term.h>
#elif HAVE_NCURSES_TERM_H
#include <ncurses/term.h>
#endif

#include "builtin.h"
#include "common.h"
#include "exec.h"
#include "fallback.h"  // IWYU pragma: keep
#include "io.h"
#include "parser.h"
#include "proc.h"
#include "wgetopt.h"
#include "wutil.h"  // IWYU pragma: keep

// Measuring time is always complicated with many caveats. Quite apart from the typical
// gotchas faced by developers attempting to choose between monotonic vs non-monotonic and system vs
// cpu clocks, the fact that we are executing as a shell further complicates matters: we can't just
// observe the elapsed CPU time, because that does not reflect the total execution time for both
// ourselves (internal shell execution time and the time it takes for builtins and functions to
// execute) and any external processes we spawn.

// It would be nice to use the C++1 type-safe <chrono> interfaces to measure elapsed time, but that
// unfortunately is underspecified with regards to user/system time and only provides means of
// querying guaranteed monotonicity and resolution for the various clocks. It can be used to measure
// elapsed wall time nicely, but if we would like to provide information more useful for
// benchmarking and tuning then we must turn to either clock_gettime(2), with extensions for thread-
// and process-specific elapsed CPU time, or times(3) for a standard interface to overall process
// and child user/system time elapsed between snapshots. At least on some systems, times(3) has been
// deprecated in favor of getrusage(2), which offers a wider variety of metrics coalesced for SELF,
// THREAD, or CHILDREN.

// With regards to the C++11 `<chrono>` interface, there are three different time sources (clocks)
// that we can use portably: `system_clock`, `steady_clock`, and `high_resolution_clock`; with
// different properties and guarantees. While the obvious difference is the direct tradeoff between
// period and resolution (higher resolution equals ability to measure smaller time differences more
// accurately, but at the cost of rolling over more frequently), but unfortunately it is not as
// simple as starting two clocks and going with the highest resolution that hasn't rolled over.
// `system_clock` is out because it is always subject to interference due to adjustments from NTP
// servers or super users (as it reflects the "actual" time), but `high_resolution_clock` may or may
// not be aliased to `system_clock` or `steady_clock`. In practice, there's likely no need to worry
// about this too much, a survey <http://howardhinnant.github.io/clock_survey.html> of the different
// libraries indicates that `high_resolution_clock` is either an alias for `steady_clock` (in which
// case it offers no greater resolution) or it is an alias for `system_clock` (in which case, even
// when it offers a greater resolution than `steady_clock` it is not fit for use).

static int64_t micros(struct timeval t) {
    return (static_cast<int64_t>(t.tv_usec) + static_cast<int64_t>(t.tv_sec * 1E6));
};

template <typename D1, typename D2>
static int64_t micros(const std::chrono::duration<D1, D2> &d) {
    return std::chrono::duration_cast<std::chrono::microseconds>(d).count();
};

timer_snapshot_t timer_snapshot_t::take() {
    timer_snapshot_t snapshot;

    getrusage(RUSAGE_SELF, &snapshot.cpu_fish);
    getrusage(RUSAGE_CHILDREN, &snapshot.cpu_children);
    snapshot.wall = std::chrono::steady_clock::now();

    return snapshot;
}

wcstring timer_snapshot_t::print_delta(const timer_snapshot_t &t1, const timer_snapshot_t &t2,
                                       bool verbose /* = true */) {
    int64_t fish_sys_micros = micros(t2.cpu_fish.ru_stime) - micros(t1.cpu_fish.ru_stime);
    int64_t fish_usr_micros = micros(t2.cpu_fish.ru_utime) - micros(t1.cpu_fish.ru_utime);
    int64_t child_sys_micros = micros(t2.cpu_children.ru_stime) - micros(t1.cpu_children.ru_stime);
    int64_t child_usr_micros = micros(t2.cpu_children.ru_utime) - micros(t1.cpu_children.ru_utime);

    // The result from getrusage is not necessarily realtime, it may be cached a few microseconds
    // behind. In the event that execution completes extremely quickly or there is no data (say, we
    // are measuring external execution time but no external processes have been launched), it can
    // incorrectly appear to be negative.
    fish_sys_micros = std::max(int64_t(0), fish_sys_micros);
    fish_usr_micros = std::max(int64_t(0), fish_usr_micros);
    child_sys_micros = std::max(int64_t(0), child_sys_micros);
    child_usr_micros = std::max(int64_t(0), child_usr_micros);

    int64_t net_sys_micros = fish_sys_micros + child_sys_micros;
    int64_t net_usr_micros = fish_usr_micros + child_usr_micros;
    int64_t net_wall_micros = micros(t2.wall - t1.wall);

    enum class tunit {
        minutes,
        seconds,
        milliseconds,
        microseconds,
    };

    auto get_unit = [](int64_t micros) {
        if (micros > 900 * 1E6) {
            return tunit::minutes;
        } else if (micros >= 999995) {  // Move to seconds if we would overflow the %6.2 format.
            return tunit::seconds;
        } else if (micros >= 1000) {
            return tunit::milliseconds;
        } else {
            return tunit::microseconds;
        }
    };

    auto unit_name = [](tunit unit) {
        switch (unit) {
            case tunit::minutes:
                return L"minutes";
            case tunit::seconds:
                return L"seconds";
            case tunit::milliseconds:
                return L"milliseconds";
            case tunit::microseconds:
                return L"microseconds";
        }
        // GCC does not recognize the exhaustive switch above
        return L"";
    };

    auto unit_short_name = [](tunit unit) {
        switch (unit) {
            case tunit::minutes:
                return L"min";
            case tunit::seconds:
                return L"sec";
            case tunit::milliseconds:
                return L"ms";
            case tunit::microseconds:
                return L"μs";
        }
        // GCC does not recognize the exhaustive switch above
        return L"";
    };

    auto convert = [](int64_t micros, tunit unit) {
        switch (unit) {
            case tunit::minutes:
                return micros / 1.0E6 / 60.0;
            case tunit::seconds:
                return micros / 1.0E6;
            case tunit::milliseconds:
                return micros / 1.0E3;
            case tunit::microseconds:
                return micros / 1.0;
        }
        // GCC does not recognize the exhaustive switch above
        return 0.0;
    };

    auto wall_unit = get_unit(net_wall_micros);
    auto cpu_unit = get_unit(std::max(net_sys_micros, net_usr_micros));
    double wall_time = convert(net_wall_micros, wall_unit);
    double usr_time = convert(net_usr_micros, cpu_unit);
    double sys_time = convert(net_sys_micros, cpu_unit);

    wcstring output;
    if (!verbose) {
        append_format(output, L"\nExecuted in  %6.2F %ls"
                              L"\n   usr time  %6.2F %ls"
                              L"\n   sys time  %6.2F %ls"
                              L"\n",
                              wall_time, unit_name(wall_unit),
                              usr_time, unit_name(cpu_unit),
                              sys_time, unit_name(cpu_unit));
    } else {
        auto fish_unit = get_unit(std::max(fish_sys_micros, fish_usr_micros));
        auto child_unit = get_unit(std::max(child_sys_micros, child_usr_micros));
        double fish_usr_time = convert(fish_usr_micros, fish_unit);
        double fish_sys_time = convert(fish_sys_micros, fish_unit);
        double child_usr_time = convert(child_usr_micros, child_unit);
        double child_sys_time = convert(child_sys_micros, child_unit);

        int column2_unit_len = std::max(wcslen(unit_short_name(wall_unit)),
                                        wcslen(unit_short_name(cpu_unit)));
        // TODO: improve layout
        append_format(output,
                      L"%s%s%s"
                      L"\nExecuted in  %6.2F %-*ls    %-*s  %s"
                      L"\n   usr time  %6.2F %-*ls  %6.2F %ls  %6.2F %ls"
                      L"\n   sys time  %6.2F %-*ls  %6.2F %ls  %6.2F %ls",
                      enter_alt_charset_mode, std::string(45, 'q').c_str(), exit_alt_charset_mode,
                      wall_time, column2_unit_len, unit_short_name(wall_unit),
                      static_cast<int>(wcslen(unit_short_name(fish_unit))) + 7, "fish", "external",
                      usr_time, column2_unit_len, unit_short_name(cpu_unit), fish_usr_time,
                      unit_short_name(fish_unit), child_usr_time, unit_short_name(child_unit),
                      sys_time, column2_unit_len, unit_short_name(cpu_unit), fish_sys_time,
                      unit_short_name(fish_unit), child_sys_time, unit_short_name(child_unit));
    }
    return output;
};

static std::vector<timer_snapshot_t> active_timers;

static void pop_timer() {
    auto t1 = active_timers.back();
    active_timers.pop_back();
    auto t2 = timer_snapshot_t::take();

    // Well, this is awkward. By defining `time` as a decorator and not a built-in, there's
    // no associated stream for its output!
    auto output = timer_snapshot_t::print_delta(t1, t2, true);
    std::fwprintf(stderr, L"%S\n", output.c_str());
}

cleanup_t push_timer(bool enabled) {
    if (!enabled) return {[] {}};
    active_timers.emplace_back(timer_snapshot_t::take());
    return {[] { pop_timer(); }};
}
