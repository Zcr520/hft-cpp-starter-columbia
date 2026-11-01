// Supplemental Week 8 Lab exercises (not part of the unmodified course CI).
// Build from the repository root:
// c++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude
// tests/week8_extra_test.cpp -o /tmp/week8_extra && /tmp/week8_extra
#include "rolling_counter.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

struct Welford {
    std::uint64_t n = 0;
    double mean = 0.0;
    double m2 = 0.0;

    void add(double x) {
        ++n;
        const double delta = x - mean;
        mean += delta / static_cast<double>(n);
        m2 += delta * (x - mean);
    }
    double variance() const { return n > 1 ? m2 / (n - 1) : 0.0; }
    double stddev() const { return std::sqrt(variance()); }
};

static bool near(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}

static int failures = 0;
static void result(const char* name, bool ok) {
    std::cout << "RESULT|" << name << '|' << (ok ? "pass" : "fail") << '\n';
    failures += !ok;
}

int main() {
    {
        RollingCounter rc(1000);
        for (std::uint64_t t = 0; t < 1000; ++t) rc.add(t);
        result("rc_lab_window", rc.count(999) == 1000);
        result("rc_cutoff_equality", rc.count(1000) == 999); // event ts=0 expires
        for (std::uint64_t t = 1000; t < 1500; ++t) rc.add(t);
        result("rc_lab_mixed", rc.count(1999) == 500);
        result("rc_empty_after_expiry", rc.count(5000) == 0);
    }
    {
        RollingCounter rc(10);
        rc.add(7);
        rc.add(7);
        rc.add(9);
        result("rc_duplicates", rc.count(9) == 3);
        result("rc_duplicate_expiry", rc.count(17) == 1);
    }
    {
        RollingCounter rc(0);
        rc.add(0);
        result("rc_zero_window", rc.count(0) == 0);
    }
    {
        RollingCounter rc(10);
        // Exercise physical wraparound for more than two full ring revolutions.
        bool ok = true;
        for (std::uint64_t t = 0; t < (1u << 21) + 23u; ++t) {
            rc.add(t);
            if (rc.count(t) != (t < 10 ? t + 1 : 10)) {
                ok = false;
                break;
            }
        }
        result("rc_wraparound", ok);
    }
    {
        RollingCounter rc(1);
        for (std::uint64_t i = 0; i < (1u << 20) + 7u; ++i) rc.add(0);
        result("rc_capacity_growth", rc.count(0) == (1u << 20) + 7u);
        result("rc_growth_expiry", rc.count(1) == 0);
    }
    {
        Welford w;
        for (double x : {1.0, 2.0, 3.0, 4.0, 5.0}) w.add(x);
        const bool ok = w.n == 5 && near(w.mean, 3.0) &&
                        near(w.variance(), 2.5) &&
                        near(w.stddev(), std::sqrt(2.5));
        result("welford_five_values", ok);
        std::cout << "METRIC|welford_5_mean|" << w.mean << '\n';
        std::cout << "METRIC|welford_5_stddev|" << w.stddev() << '\n';
    }
    {
        Welford w;
        for (int x = 0; x < 1000; ++x) w.add(static_cast<double>(x));
        const double expected_sample_variance = 1000.0 * 1001.0 / 12.0;
        const bool ok = w.n == 1000 && near(w.mean, 499.5) &&
                        near(w.variance(), expected_sample_variance, 1e-6);
        result("welford_1000_values", ok);
        std::cout << "METRIC|welford_1000_mean|" << w.mean << '\n';
        std::cout << "METRIC|welford_1000_variance|" << w.variance() << '\n';
        std::cout << "METRIC|welford_1000_stddev|" << w.stddev() << '\n';
    }
    return failures ? 1 : 0;
}
