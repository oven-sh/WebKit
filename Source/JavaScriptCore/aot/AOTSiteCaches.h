/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace JSC {

class VM;

namespace AOT {

struct Instance;

class SiteCaches {
public:
    static constexpr unsigned offsetShift = 32;
    static constexpr unsigned indexShift = 33;
    static constexpr uint32_t isOutOfLine = 1;
    static constexpr unsigned wordsOfLocation = 2;
    static constexpr uint32_t missesBetweenReplacements = 8;
    static constexpr uint32_t structureIDOfNoStructure = 1;
    static constexpr unsigned pageShift = 12;
    static constexpr size_t pageSize = 1u << pageShift;
    static constexpr size_t offsetOfTouchedPages = 4 * MB;
    static constexpr size_t maxNumberOfPages = 64 * KB;
    static constexpr size_t offsetOfWords = offsetOfTouchedPages + maxNumberOfPages;
    static constexpr size_t maxNumberOfWords = maxNumberOfPages * pageSize / sizeof(uint64_t);

    static size_t numberOfWords();
    static size_t numberOfPages() { return (numberOfWords() * sizeof(uint64_t) + pageSize - 1) >> pageShift; }
    static size_t endOffset(size_t endOfStates);
    static void clear(Instance&);
    static void clear(VM&);
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
