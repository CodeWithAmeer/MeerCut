#include "fa/core/rational.h"

#include <cmath>
#include <limits>
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#include <numeric>

namespace fa {

int64_t checkedMul(int64_t a, int64_t b) {
    int64_t r = 0;
#if defined(_MSC_VER) && !defined(__clang__)
    // MSVC has no __builtin_mul_overflow; use the 128-bit product.
    int64_t hi = 0;
    r = _mul128(a, b, &hi);
    // the product fits in 64 bits iff the high word is the sign extension of the low word
    if (hi != (r < 0 ? -1 : 0)) throw RationalOverflow("rational multiply overflow");
#else
    if (__builtin_mul_overflow(a, b, &r)) throw RationalOverflow("rational multiply overflow");
#endif
    return r;
}

int64_t checkedAdd(int64_t a, int64_t b) {
    if ((b > 0 && a > std::numeric_limits<int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<int64_t>::min() - b))
        throw RationalOverflow("rational add overflow");
    return a + b;
}

Rational::Rational(int64_t num, int64_t den) {
    if (den == 0) throw std::invalid_argument("rational with zero denominator");
    if (num == std::numeric_limits<int64_t>::min() || den == std::numeric_limits<int64_t>::min())
        throw RationalOverflow("rational component out of range");
    if (den < 0) {
        num = -num;
        den = -den;
    }
    const int64_t g = std::gcd(num, den);
    num_ = g ? num / g : 0;
    den_ = g ? den / g : 1;
}

int64_t Rational::floor() const {
    int64_t q = num_ / den_;
    if ((num_ % den_) != 0 && num_ < 0) --q;
    return q;
}

int64_t Rational::ceil() const {
    int64_t q = num_ / den_;
    if ((num_ % den_) != 0 && num_ > 0) ++q;
    return q;
}

int64_t Rational::round() const {
    // half away from zero: floor(|x| + 1/2) with sign
    const int64_t a = num_ < 0 ? -num_ : num_;
    const int64_t q = a / den_;
    const int64_t r = a % den_;
    const int64_t mag = (2 * r >= den_) ? q + 1 : q;
    return num_ < 0 ? -mag : mag;
}

Rational Rational::operator-() const { return Rational(-num_, den_); }

Rational Rational::inverse() const {
    if (num_ == 0) throw std::domain_error("inverse of zero rational");
    return Rational(den_, num_);
}

Rational operator+(const Rational& a, const Rational& b) {
    // a/b + c/d with g = gcd(b, d): (a*(d/g) + c*(b/g)) / (b/g*d)
    const int64_t g = std::gcd(a.den_, b.den_);
    const int64_t n = checkedAdd(checkedMul(a.num_, b.den_ / g), checkedMul(b.num_, a.den_ / g));
    return Rational(n, checkedMul(a.den_ / g, b.den_));
}

Rational operator-(const Rational& a, const Rational& b) { return a + (-b); }

Rational operator*(const Rational& a, const Rational& b) {
    // cross-reduce first to keep intermediates small
    const int64_t g1 = std::gcd(a.num_, b.den_);
    const int64_t g2 = std::gcd(b.num_, a.den_);
    const int64_t n1 = g1 ? a.num_ / g1 : 0, d2 = g1 ? b.den_ / g1 : b.den_;
    const int64_t n2 = g2 ? b.num_ / g2 : 0, d1 = g2 ? a.den_ / g2 : a.den_;
    return Rational(checkedMul(n1, n2), checkedMul(d1, d2));
}

Rational operator/(const Rational& a, const Rational& b) { return a * b.inverse(); }

std::strong_ordering operator<=>(const Rational& a, const Rational& b) {
    if (a.den_ == b.den_) return a.num_ <=> b.num_;
    // compare a.num*b.den with b.num*a.den; fall back to exact subtraction on overflow risk
    const Rational d = a - b;
    return d.num_ <=> 0;
}

std::string Rational::toString() const {
    return den_ == 1 ? std::to_string(num_) : std::to_string(num_) + "/" + std::to_string(den_);
}

Rational Rational::parse(const std::string& text) {
    const auto slash = text.find('/');
    try {
        size_t used = 0;
        if (slash == std::string::npos) {
            const int64_t n = std::stoll(text, &used);
            if (used != text.size()) throw std::invalid_argument("trailing characters");
            return Rational(n);
        }
        const std::string ns = text.substr(0, slash), ds = text.substr(slash + 1);
        const int64_t n = std::stoll(ns, &used);
        if (used != ns.size()) throw std::invalid_argument("trailing characters");
        const int64_t d = std::stoll(ds, &used);
        if (used != ds.size()) throw std::invalid_argument("trailing characters");
        return Rational(n, d);
    } catch (const std::logic_error&) {
        throw std::invalid_argument("invalid rational: '" + text + "'");
    }
}

Rational Rational::fromDouble(double value, int64_t maxDen) {
    if (!std::isfinite(value)) throw std::invalid_argument("non-finite value");
    // continued-fraction convergents
    int64_t h0 = 0, h1 = 1, k0 = 1, k1 = 0;
    double x = value;
    for (int i = 0; i < 64; ++i) {
        const double a = std::floor(x);
        if (std::fabs(a) > 9.0e15) break;
        const int64_t ai = static_cast<int64_t>(a);
        const int64_t h2 = checkedAdd(checkedMul(ai, h1), h0);
        const int64_t k2 = checkedAdd(checkedMul(ai, k1), k0);
        if (k2 > maxDen) break;
        h0 = h1; h1 = h2; k0 = k1; k1 = k2;
        const double frac = x - a;
        if (frac < 1e-12) break;
        x = 1.0 / frac;
    }
    return Rational(h1, k1 ? k1 : 1);
}

Rational min(const Rational& a, const Rational& b) { return b < a ? b : a; }
Rational max(const Rational& a, const Rational& b) { return a < b ? b : a; }

}  // namespace fa
