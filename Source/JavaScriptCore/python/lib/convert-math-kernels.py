#!/usr/bin/env python3
#
# Copyright (C) 2026 Apple Inc. All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
# EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
# CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
# EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
# PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
# PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
# OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


# Makes PythonMathKernels.h out of Modules/mathmodule.c of CPython: the functions there that take numbers of C's and give numbers of C's, and the tables that they go by.
#
#     python3 convert-math-kernels.py <the directory that CPython's source is in> > ../PythonMathKernels.h
#
# They are taken as they are, expression for expression, because how a sum is written decides how it is rounded: see "One rounding or two" in the README. What has to do with objects is in PythonMathModule.cpp,
# written by hand.
import re
import sys

root = sys.argv[1]
source = open(root + "/Modules/mathmodule.c").read()
helpers = open(root + "/Modules/_math.h").read()


def matching(text, start, opening, closing):
    "Where what opens at `start` closes, and one more"
    depth = 0
    i = start
    while True:
        # Neither is in a comment or a string anywhere in what is wanted.
        if text[i] == opening:
            depth += 1
        elif text[i] == closing:
            depth -= 1
            if not depth:
                return i + 1
        i += 1


def only(pattern, text, which=0):
    found = list(re.finditer(pattern, text, re.M))
    assert found and (which or len(found) == 1), (pattern, len(found))
    return found[which - 1 if which else 0]


def function(name, text=source, which=0):
    "A function, from the line that says what it returns"
    m = only(r"^(static (?:inline )?[\w ]+\n)%s\(" % re.escape(name), text, which)
    return text[m.start():matching(text, text.index("{", matching(text, m.end() - 1, "(", ")")), "{", "}")] + "\n"


def table(name, text=source):
    "An array, wherever it is, without what it is indented by"
    m = only(r"^( *)static const [\w ]+ %s\[\w*\] = \{" % re.escape(name), text)
    body = text[m.start():text.index("};", m.end()) + 2]
    return "\n".join(line[len(m.group(1)):] for line in body.split("\n")) + "\n"


def line(pattern, text=source):
    return only("^" + pattern + "$", text).group(0) + "\n"


pieces = [
    ("Double and triple length extended precision", [
        line(r"typedef struct\{ double hi; double lo; \} DoubleLength;"),
        function("dl_fast_sum"), function("dl_sum"),
        # The one that goes by fma(), which is the first of two
        function("dl_mul", which=1),
        line(r"typedef struct \{ double hi; double lo; double tiny; \} TripleLength;"),
        line(r"static const TripleLength tl_zero = \{0\.0, 0\.0, 0\.0\};"),
        function("tl_fma"), function("tl_to_d"),
    ]),
    ("gamma() and lgamma()", [
        line(r"static const double pi = [\d.]+;"), line(r"static const double logpi = [\d.]+;"),
        function("m_sinpi"),
        line(r"#define LANCZOS_N 13"), line(r"static const double lanczos_g = [\d.]+;"), line(r"static const double lanczos_g_minus_half = [\d.]+;"),
        table("lanczos_num_coeffs"), table("lanczos_den_coeffs"),
        line(r"#define NGAMMA_INTEGRAL 23"), table("gamma_integral"),
        function("lanczos_sum"), function("m_tgamma"), function("m_lgamma"),
    ]),
    ("remainder() and the logarithms", [
        function("m_remainder"), function("m_log"), function("m_log2"), function("m_log10"), function("_Py_log1p", helpers),
    ]),
    ("isqrt() and factorial()", [
        function("count_set_bits"), table("_approximate_isqrt_tab"), function("_approximate_isqrt"), table("SmallFactorials"),
    ]),
    ("hypot() and dist()", [function("vector_norm")]),
    ("sumprod() and prod()", [function("long_add_would_overflow"), function("_check_long_mult_overflow")]),
    ("degrees() and radians()", [line(r"static const double degToRad = Py_MATH_PI / 180\.0;"), line(r"static const double radToDeg = 180\.0 / Py_MATH_PI;")]),
    ("perm() and comb()", [
        table("reduced_factorial_odd_part"), table("inverted_factorial_odd_part"), table("factorial_trailing_zeros"),
        table("fast_comb_limits1"), table("fast_comb_limits2"), table("fast_perm_limits"),
    ]),
]

# What is spelt otherwise in C++, or is CPython's own way of spelling something. Each has to be there.
spellings = [
    (r"typedef struct ?\{ (.*?) \} (\w+);", r"struct \2 { \1 };"),
    (r"\((DoubleLength|TripleLength)\) \{", r"\1 {"),
    (r"\bPy_NAN\b", "std::numeric_limits<double>::quiet_NaN()"),
    (r"\bPy_INFINITY\b", "std::numeric_limits<double>::infinity()"),
    (r"\bPy_MATH_PI\b", "3.14159265358979323846"),
    (r"\bPy_UNREACHABLE\(\)", "RELEASE_ASSERT_NOT_REACHED()"),
    (r"\bPy_ssize_t\b", "ptrdiff_t"),
    (r"^#if SIZEOF_LONG >= 8$", "#if __SIZEOF_LONG__ >= 8"),
    (r"^ *assert\(.*?\);\n", ""),
    # It is declared before it is defined, in the original.
    (r"^static inline int\n_check_long_mult_overflow", "static inline int\n_check_long_mult_overflow"),
]

text = ""
for title, parts in pieces:
    text += "// ---- " + title + "\n\n" + "\n".join(parts) + "\n"
for pattern, replacement in spellings:
    text, count = re.subn(pattern, replacement, text, flags=re.M | re.S if pattern.startswith("^ *assert") else re.M)
    assert count, pattern
assert "Py" not in re.sub(r"_Py_log1p|/\*.*?\*/|//[^\n]*", "", text, flags=re.S), sorted(set(re.findall(r"\w*Py\w*", text)))

print("""/*
 * Generated by lib/convert-math-kernels.py from Modules/mathmodule.c and Modules/_math.h of CPython. Do not edit.
 *
 * It is CPython's authors' work, and is under CPython's licence: see lib/importlib/LICENSE.
 */

#pragma once

#include <cerrno>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdint>
#include <limits>
#include <wtf/Assertions.h>
#include <wtf/Compiler.h>

// This is to be included where `a * b + c`, written as one expression, is rounded once if the processor can do that, as it is in what CPython is built with. See PythonMathModule.cpp.

namespace JSC { namespace Python { namespace MathKernels {

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

using std::isfinite;
using std::isinf;
using std::isnan;
""")
print(text.rstrip("\n"))
print("""
WTF_ALLOW_UNSAFE_BUFFER_USAGE_END

} } } // namespace JSC::Python::MathKernels""")
