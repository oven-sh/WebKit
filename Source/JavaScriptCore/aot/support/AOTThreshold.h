/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// A threshold decides how fast or how large the code is, never what a program prints: how many members are told apart, how deep a walk goes, from which size on another road is taken.
// Most are so high that only a large program meets them. Declared here, a build with AOT_THRESHOLDS_FROM_ENVIRONMENT makes them small, and a small program goes where a large one would:
//   AOT_THRESHOLDS="all=1"                        each at min(its value, 1); never 0, which some divide by
//   AOT_THRESHOLDS="all=1,LongExpectations.mostLayouts=16"   <file without AOT and its ending>.<name>: later words win
//   AOT_THRESHOLDS="draw=<seed>"                  half of them stay, the others are at 1, 2 or 3, by a hash of the seed and the name
// AOT_THRESHOLD stands in a namespace or a function, AOT_CLASS_THRESHOLD in a class.
#if defined(AOT_THRESHOLDS_FROM_ENVIRONMENT)

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace JSC { namespace AOT {

inline double thresholdFromEnvironment(const char* file, const char* name, double usual)
{
    const char* words = getenv("AOT_THRESHOLDS");
    if (!words)
        return usual;
    if (!strncmp(file, "AOT", 3))
        file += 3;
    size_t fileLength = strcspn(file, ".");
    size_t nameLength = strlen(name);
    double value = usual;
    for (const char* word = words; *word;) {
        const char* end = word + strcspn(word, ",");
        const char* equals = static_cast<const char*>(memchr(word, '=', end - word));
        if (equals) {
            size_t length = equals - word;
            double number = strtod(equals + 1, nullptr);
            if (length == fileLength + 1 + nameLength && !memcmp(word, file, fileLength) && word[fileLength] == '.' && !memcmp(word + fileLength + 1, name, nameLength))
                value = number;
            else if (length == 3 && !memcmp(word, "all", 3))
                value = std::min(usual, std::max(number, 1.0));
            else if (length == 4 && !memcmp(word, "draw", 4)) {
                uint64_t hash = 1469598103934665603ull ^ static_cast<uint64_t>(number);
                for (const char* c = file; c < file + fileLength; ++c)
                    hash = (hash ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
                for (const char* c = name; *c; ++c)
                    hash = (hash ^ static_cast<unsigned char>(*c)) * 1099511628211ull;
                hash ^= hash >> 29;
                unsigned drawn = hash % 6;
                value = drawn < 3 ? std::min<double>(usual, drawn + 1) : usual;
            }
        }
        word = *end ? end + 1 : end;
    }
    return value;
}

} } // namespace JSC::AOT

#define AOT_THRESHOLD(type, name, ...) static const type name = static_cast<type>(::JSC::AOT::thresholdFromEnvironment(__FILE_NAME__, #name, static_cast<double>(__VA_ARGS__)))
#define AOT_CLASS_THRESHOLD(type, name, ...) static inline const type name = static_cast<type>(::JSC::AOT::thresholdFromEnvironment(__FILE_NAME__, #name, static_cast<double>(__VA_ARGS__)))

#else

#define AOT_THRESHOLD(type, name, ...) static constexpr type name = __VA_ARGS__
#define AOT_CLASS_THRESHOLD(type, name, ...) static constexpr type name = __VA_ARGS__

#endif
