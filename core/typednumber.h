// typednumber.h — read a number a user typed into one of the editor's value fields.
//
// NOT strtod, and not sscanf("%lf"): both follow LC_NUMERIC, and that is the HOST's locale rather
// than ours — a host that calls setlocale(LC_ALL, "") under a locale which writes 2,5 makes
// strtod("-3.5") stop at the point and return -3. The same defect was found and fixed in this
// editor's grandparent for the same field. And not std::from_chars, which ignores the locale and
// would otherwise be the answer: its floating-point overloads need libc++ 20, the macOS build's
// toolchain is older than that, and whether a given AppleClang carries them cannot be checked from
// the machine this is developed on. What a value field accepts is so narrow that a parser for it is
// shorter than the argument about which library function to trust.
//
// The grammar is the whole of what the fields admit (acceptsTextChar): an optional sign, digits,
// and at most one decimal mark, which may be '.' or ',' so that either habit works. Like strtod it
// reads the longest prefix that is a number and ignores the rest, so "1-2" is 1; unlike strtod it
// requires a digit, so "-", "." or an emptied field is not a number and leaves the value alone.
//
// There is no exponent and no "inf"/"nan", so the result is always finite — which matters, because
// the editor is built with -ffast-math and an isfinite() test there would fold to true. The value
// is formed as one integer divided by one power of ten, both exact for the eight characters a field
// holds, so the single division is the only rounding and "7.5" is exactly 7.5.

#pragma once

#include <string>

namespace Rations
{

inline bool parseTypedNumber(const std::string &text, double &out)
{
    size_t i = 0;
    const size_t n = text.size();
    bool negative = false;
    if (i < n && (text[i] == '+' || text[i] == '-'))
        negative = (text[i++] == '-');

    double mantissa = 0.0;
    double scale = 1.0;
    int digits = 0;
    bool seenMark = false;
    for (; i < n; ++i) {
        const char ch = text[i];
        if (ch >= '0' && ch <= '9') {
            mantissa = mantissa * 10.0 + static_cast<double>(ch - '0');
            if (seenMark)
                scale *= 10.0;
            ++digits;
        } else if ((ch == '.' || ch == ',') && !seenMark) {
            seenMark = true;
        } else {
            break;
        }
    }
    if (digits == 0)
        return false;
    out = (negative ? -mantissa : mantissa) / scale;
    return true;
}

} // namespace Rations
