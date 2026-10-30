// Exact rational arithmetic for timestamps and frame rates.
//
// All timeline positions, durations and frame rates are kept as reduced
// fractions of 64-bit integers so that frame <-> time conversions are exact
// and never accumulate floating-point drift (e.g. 30000/1001 fps).
// Every operation checks for overflow and throws instead of wrapping.
#pragma once

#include <compare>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace fa {

class RationalOverflow : public std::overflow_error {
public:
    using std::overflow_error::overflow_error;
};

class Rational {
public:
    constexpr Rational() = default;
    Rational(int64_t num, int64_t den = 1);   // normalises; throws on den == 0

    int64_t num() const { return num_; }
    int64_t den() const { return den_; }

    bool isZero() const { return num_ == 0; }
    bool isInteger() const { return den_ == 1; }
    double toDouble() const { return static_cast<double>(num_) / static_cast<double>(den_); }

    // Rounding to an integer (floor / ceil / round-half-away-from-zero).
    int64_t floor() const;
    int64_t ceil() const;
    int64_t round() const;

    Rational operator-() const;
    Rational inverse() const;   // throws on zero

    friend Rational operator+(const Rational& a, const Rational& b);
    friend Rational operator-(const Rational& a, const Rational& b);
    friend Rational operator*(const Rational& a, const Rational& b);
    friend Rational operator/(const Rational& a, const Rational& b);
    Rational& operator+=(const Rational& o) { return *this = *this + o; }
    Rational& operator-=(const Rational& o) { return *this = *this - o; }
    Rational& operator*=(const Rational& o) { return *this = *this * o; }
    Rational& operator/=(const Rational& o) { return *this = *this / o; }

    friend bool operator==(const Rational& a, const Rational& b) { return a.num_ == b.num_ && a.den_ == b.den_; }
    friend std::strong_ordering operator<=>(const Rational& a, const Rational& b);

    // "n/d" (or "n" when the denominator is 1). Parse accepts "n", "n/d" and "-n/d".
    std::string toString() const;
    static Rational parse(const std::string& text);

    // Closest rational with denominator <= maxDen (continued fractions); for import of
    // floating-point values from foreign formats only — never used internally.
    static Rational fromDouble(double value, int64_t maxDen = 1000000);

private:
    int64_t num_ = 0;
    int64_t den_ = 1;
};

Rational min(const Rational& a, const Rational& b);
Rational max(const Rational& a, const Rational& b);

// Checked 64-bit helpers (throw RationalOverflow).
int64_t checkedMul(int64_t a, int64_t b);
int64_t checkedAdd(int64_t a, int64_t b);

}  // namespace fa
