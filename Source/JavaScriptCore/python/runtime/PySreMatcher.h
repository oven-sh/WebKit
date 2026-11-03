/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyUnicode.h"
#include <cctype>
#include <limits>
#include <unicode/uchar.h>
#include <wtf/ASCIICType.h>
#include <wtf/FastMalloc.h>
#include <wtf/Noncopyable.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The matching engine of the module re: an interpreter for the code that Lib/re/_compiler.py generates.
// (Modules/_sre/sre_lib.h in CPython is the reference: which of the possible matches is found, and what the groups hold
// afterwards, follow from the order in which it tries things and from when it saves and restores the marks.)
//
// It knows nothing of Python objects, and runs no Python code.

using SreCode = uint32_t;

static constexpr SreCode sreMagic = 20230612; // As MAGIC in Lib/re/_constants.py.
static constexpr SreCode sreMaxRepeat = std::numeric_limits<SreCode>::max();
static constexpr SreCode sreMaxGroups = std::numeric_limits<int32_t>::max() / 2;
static constexpr unsigned sreCodeBits = 8 * sizeof(SreCode);

// In the order of OPCODES, ATCODES and CHCODES in Lib/re/_constants.py.
enum SreOp : SreCode {
    SreOpFailure,
    SreOpSuccess,
    SreOpAny,
    SreOpAnyAll,
    SreOpAssert,
    SreOpAssertNot,
    SreOpAt,
    SreOpBranch,
    SreOpCategory,
    SreOpCharset,
    SreOpBigCharset,
    SreOpGroupRef,
    SreOpGroupRefExists,
    SreOpIn,
    SreOpInfo,
    SreOpJump,
    SreOpLiteral,
    SreOpMark,
    SreOpMaxUntil,
    SreOpMinUntil,
    SreOpNotLiteral,
    SreOpNegate,
    SreOpRange,
    SreOpRepeat,
    SreOpRepeatOne,
    SreOpSubpattern,
    SreOpMinRepeatOne,
    SreOpAtomicGroup,
    SreOpPossessiveRepeat,
    SreOpPossessiveRepeatOne,
    SreOpGroupRefIgnore,
    SreOpInIgnore,
    SreOpLiteralIgnore,
    SreOpNotLiteralIgnore,
    SreOpGroupRefLocIgnore,
    SreOpInLocIgnore,
    SreOpLiteralLocIgnore,
    SreOpNotLiteralLocIgnore,
    SreOpGroupRefUniIgnore,
    SreOpInUniIgnore,
    SreOpLiteralUniIgnore,
    SreOpNotLiteralUniIgnore,
    SreOpRangeUniIgnore,
};
static_assert(SreOpRangeUniIgnore == 42);

enum SreAt : SreCode {
    SreAtBeginning,
    SreAtBeginningLine,
    SreAtBeginningString,
    SreAtBoundary,
    SreAtNonBoundary,
    SreAtEnd,
    SreAtEndLine,
    SreAtEndString,
    SreAtLocBoundary,
    SreAtLocNonBoundary,
    SreAtUniBoundary,
    SreAtUniNonBoundary,
};

enum SreCategory : SreCode {
    SreCategoryDigit,
    SreCategoryNotDigit,
    SreCategorySpace,
    SreCategoryNotSpace,
    SreCategoryWord,
    SreCategoryNotWord,
    SreCategoryLineBreak,
    SreCategoryNotLineBreak,
    SreCategoryLocWord,
    SreCategoryLocNotWord,
    SreCategoryUniDigit,
    SreCategoryUniNotDigit,
    SreCategoryUniSpace,
    SreCategoryUniNotSpace,
    SreCategoryUniWord,
    SreCategoryUniNotWord,
    SreCategoryUniLineBreak,
    SreCategoryUniNotLineBreak,
};

enum SreFlag : int {
    SreFlagIgnoreCase = 2,
    SreFlagLocale = 4,
    SreFlagMultiLine = 8,
    SreFlagDotAll = 16,
    SreFlagUnicode = 32,
    SreFlagVerbose = 64,
    SreFlagDebug = 128,
    SreFlagASCII = 256,
};

enum SreInfo : SreCode {
    SreInfoPrefix = 1,
    SreInfoLiteral = 2,
    SreInfoCharset = 4,
};

// What a match or a search returns besides 0, no match, and 1, a match.
static constexpr ptrdiff_t sreErrorIllegal = -1; // Code that means nothing.
static constexpr ptrdiff_t sreErrorState = -2; // An UNTIL without its REPEAT.
static constexpr ptrdiff_t sreErrorMemory = -9;

// ---- Characters ----

ALWAYS_INLINE bool sreIsDigit(SreCode ch) { return ch - '0' < 10; }
ALWAYS_INLINE bool sreIsSpace(SreCode ch) { return ch == ' ' || ch - '\t' < 5; }
ALWAYS_INLINE bool sreIsLineBreak(SreCode ch) { return ch == '\n'; }
ALWAYS_INLINE bool sreIsWord(SreCode ch) { return ch < 128 && (isASCIIAlphanumeric(static_cast<char32_t>(ch)) || ch == '_'); }
ALWAYS_INLINE SreCode sreLowerASCII(SreCode ch) { return ch - 'A' < 26 ? ch + ('a' - 'A') : ch; }

// As the C library sees bytes in the current locale.
ALWAYS_INLINE bool sreIsLocaleWord(SreCode ch) { return (ch < 256 && isalnum(static_cast<int>(ch))) || ch == '_'; }
ALWAYS_INLINE SreCode sreLowerLocale(SreCode ch) { return ch < 256 ? static_cast<SreCode>(tolower(static_cast<int>(ch))) : ch; }
ALWAYS_INLINE SreCode sreUpperLocale(SreCode ch) { return ch < 256 ? static_cast<SreCode>(toupper(static_cast<int>(ch))) : ch; }
ALWAYS_INLINE bool sreMatchesIgnoringLocaleCase(SreCode pattern, SreCode ch) { return ch == pattern || sreLowerLocale(ch) == pattern || sreUpperLocale(ch) == pattern; }

// As str.isdecimal(), str.isspace() and str.isalnum() see characters. (PySre.cpp)
enum : uint8_t {
    SreCharacterIsDigit = 1 << 0,
    SreCharacterIsSpace = 1 << 1,
    SreCharacterIsWord = 1 << 2,
    SreCharacterIsLineBreak = 1 << 3,
};
extern uint8_t sreLatin1Properties[256];
bool sreIsUnicodeDigitSlow(SreCode);
bool sreIsUnicodeSpaceSlow(SreCode);
bool sreIsUnicodeWordSlow(SreCode);
SreCode sreUpperUnicodeSlow(SreCode);

ALWAYS_INLINE bool sreIsUnicodeDigit(SreCode ch) { return ch < 256 ? sreLatin1Properties[ch] & SreCharacterIsDigit : sreIsUnicodeDigitSlow(ch); }
ALWAYS_INLINE bool sreIsUnicodeSpace(SreCode ch) { return ch < 256 ? sreLatin1Properties[ch] & SreCharacterIsSpace : sreIsUnicodeSpaceSlow(ch); }
ALWAYS_INLINE bool sreIsUnicodeWord(SreCode ch) { return ch < 256 ? sreLatin1Properties[ch] & SreCharacterIsWord : sreIsUnicodeWordSlow(ch); }
ALWAYS_INLINE bool sreIsUnicodeLineBreak(SreCode ch) { return ch < 256 ? sreLatin1Properties[ch] & SreCharacterIsLineBreak : ch == 0x2028 || ch == 0x2029; }
ALWAYS_INLINE SreCode sreLowerUnicode(SreCode ch) { return ch < 128 ? sreLowerASCII(ch) : Unicode::mapCaseToOne(ch, Unicode::CaseMapping::Lower); }
ALWAYS_INLINE SreCode sreUpperUnicode(SreCode ch) { return ch < 128 ? (ch - 'a' < 26 ? ch - ('a' - 'A') : ch) : sreUpperUnicodeSlow(ch); }

inline bool sreIsInCategory(SreCode category, SreCode ch)
{
    switch (category) {
    case SreCategoryDigit:
        return sreIsDigit(ch);
    case SreCategoryNotDigit:
        return !sreIsDigit(ch);
    case SreCategorySpace:
        return sreIsSpace(ch);
    case SreCategoryNotSpace:
        return !sreIsSpace(ch);
    case SreCategoryWord:
        return sreIsWord(ch);
    case SreCategoryNotWord:
        return !sreIsWord(ch);
    case SreCategoryLineBreak:
        return sreIsLineBreak(ch);
    case SreCategoryNotLineBreak:
        return !sreIsLineBreak(ch);
    case SreCategoryLocWord:
        return sreIsLocaleWord(ch);
    case SreCategoryLocNotWord:
        return !sreIsLocaleWord(ch);
    case SreCategoryUniDigit:
        return sreIsUnicodeDigit(ch);
    case SreCategoryUniNotDigit:
        return !sreIsUnicodeDigit(ch);
    case SreCategoryUniSpace:
        return sreIsUnicodeSpace(ch);
    case SreCategoryUniNotSpace:
        return !sreIsUnicodeSpace(ch);
    case SreCategoryUniWord:
        return sreIsUnicodeWord(ch);
    case SreCategoryUniNotWord:
        return !sreIsUnicodeWord(ch);
    case SreCategoryUniLineBreak:
        return sreIsUnicodeLineBreak(ch);
    case SreCategoryUniNotLineBreak:
        return !sreIsUnicodeLineBreak(ch);
    }
    return false;
}

// ---- The state of a match ----

// A stack of plain data that reports running out of memory.
template<typename T>
class SreStack {
    WTF_MAKE_NONCOPYABLE(SreStack);
public:
    SreStack() = default;
    ~SreStack() { fastFree(m_data); }

    size_t size() const { return m_size; }
    T* data() { return m_data; }
    T& last() { return m_data[m_size - 1]; }
    void shrink(size_t size) { m_size = size; }

    // Room for that many more, which are not initialized. Null: there is no memory.
    ALWAYS_INLINE T* push(size_t count = 1)
    {
        if (count > m_capacity - m_size && !grow(count)) [[unlikely]]
            return nullptr;
        T* result = m_data + m_size;
        m_size += count;
        return result;
    }

private:
    NEVER_INLINE bool grow(size_t count)
    {
        size_t needed = m_size + count;
        size_t capacity = needed + needed / 2 + 32;
        if (capacity > std::numeric_limits<size_t>::max() / sizeof(T))
            return false;
        void* data;
        if (!WTF::tryFastRealloc(m_data, capacity * sizeof(T)).getValue(data))
            return false;
        m_data = static_cast<T*>(data);
        m_capacity = capacity;
        return true;
    }

    T* m_data { nullptr };
    size_t m_size { 0 };
    size_t m_capacity { 0 };
};

// A REPEAT that is being matched.
struct SreRepeat {
    ptrdiff_t count; // How many times the item has matched.
    const SreCode* pattern; // The arguments of the REPEAT.
    const void* lastPosition; // Where the last iteration began: one that consumes nothing must not go on for ever.
    SreRepeat* previous; // The REPEAT around this one.
    SreRepeat* nextFree;
};

// What would be a call of the matcher by itself. The matcher keeps these in a stack of its own, so that how deep it goes is
// bounded by memory, not by the C stack.
struct SreFrame {
    const SreCode* pattern;
    const void* ptr;
    ptrdiff_t count;
    union {
        SreCode character;
        SreRepeat* repeat;
    } u;
    const void* savedLastPosition;
    int lastMark;
    int lastIndex;
    uint8_t resume; // Where in the caller to go on.
    bool toplevel;
};

class SreState {
    WTF_MAKE_NONCOPYABLE(SreState);
public:
    SreState() = default;
    ~SreState()
    {
        fastFree(marks);
        for (size_t i = 0; i < m_allRepeats.size(); ++i)
            fastFree(m_allRepeats.data()[i]);
    }

    // False: there is no memory.
    bool allocateMarks(size_t groups)
    {
        if (!groups)
            return true;
        void* data;
        if (groups > std::numeric_limits<size_t>::max() / (2 * sizeof(void*)) || !tryFastMalloc(groups * 2 * sizeof(void*)).getValue(data))
            return false;
        marks = static_cast<const void**>(data);
        return true;
    }

    // Before each match or search.
    void reset()
    {
        lastMark = -1;
        lastIndex = -1;
        repeat = nullptr;
        frames.shrink(0);
        savedMarks.shrink(0);
        if (m_liveRepeats) {
            // A match that ended in an error left some behind.
            m_freeRepeats = nullptr;
            for (size_t i = 0; i < m_allRepeats.size(); ++i) {
                m_allRepeats.data()[i]->nextFree = m_freeRepeats;
                m_freeRepeats = m_allRepeats.data()[i];
            }
            m_liveRepeats = 0;
        }
    }

    ptrdiff_t indexOf(const void* position) const { return (static_cast<const char*>(position) - static_cast<const char*>(beginning)) / static_cast<ptrdiff_t>(characterSize); }
    const void* positionAt(ptrdiff_t index) const { return static_cast<const char*>(beginning) + index * static_cast<ptrdiff_t>(characterSize); }

    // The subject. The characters are uint8_t, char16_t or char32_t.
    unsigned characterSize { 1 };
    const void* beginning { nullptr }; // Of the whole string.
    const void* start { nullptr }; // Where to look from; after a search, where the match begins.
    const void* end { nullptr }; // What counts as the end of the string.
    const void* limit { nullptr }; // Where the whole string ends. (`start` is past `end` if that is what the caller asked for.)
    const void* ptr { nullptr }; // Where matching is; after a match, its end.

    bool matchAll { false }; // Only a match that reaches the end will do.
    bool mustAdvance { false }; // An empty match at the start will not do.

    // marks[2 * n] and marks[2 * n + 1] are where group n + 1 began and ended. Only those up to lastMark mean anything; a null
    // one has not been set.
    int lastMark { -1 };
    int lastIndex { -1 }; // The last group to have been closed.
    const void** marks { nullptr };

    SreRepeat* repeat { nullptr }; // The innermost REPEAT being matched.

    SreStack<SreFrame> frames;
    SreStack<const void*> savedMarks;

    ALWAYS_INLINE bool pushMarks(int upTo)
    {
        if (upTo < 0)
            return true;
        size_t count = static_cast<size_t>(upTo) + 1;
        const void** saved = savedMarks.push(count);
        if (!saved) [[unlikely]]
            return false;
        memcpy(saved, marks, count * sizeof(void*));
        return true;
    }
    ALWAYS_INLINE void restoreMarks(int upTo)
    {
        if (upTo < 0)
            return;
        size_t count = static_cast<size_t>(upTo) + 1;
        memcpy(marks, savedMarks.data() + savedMarks.size() - count, count * sizeof(void*));
    }
    ALWAYS_INLINE void dropMarks(int upTo)
    {
        if (upTo >= 0)
            savedMarks.shrink(savedMarks.size() - (static_cast<size_t>(upTo) + 1));
    }
    ALWAYS_INLINE void popMarks(int upTo)
    {
        restoreMarks(upTo);
        dropMarks(upTo);
    }

    SreRepeat* allocateRepeat()
    {
        SreRepeat* result = m_freeRepeats;
        if (result)
            m_freeRepeats = result->nextFree;
        else {
            void* data;
            if (!tryFastMalloc(sizeof(SreRepeat)).getValue(data))
                return nullptr;
            SreRepeat** slot = m_allRepeats.push();
            if (!slot) {
                fastFree(data);
                return nullptr;
            }
            result = static_cast<SreRepeat*>(data);
            *slot = result;
        }
        ++m_liveRepeats;
        return result;
    }
    void freeRepeat(SreRepeat* repeat)
    {
        repeat->nextFree = m_freeRepeats;
        m_freeRepeats = repeat;
        --m_liveRepeats;
    }

private:
    SreStack<SreRepeat*> m_allRepeats;
    SreRepeat* m_freeRepeats { nullptr };
    size_t m_liveRepeats { 0 };
};

// Whether the pattern matches at state.ptr. `code` is the whole code of a pattern.
ptrdiff_t sreMatch(SreState&, const SreCode* code);
// Looks for the first match at or after state.start.
ptrdiff_t sreSearch(SreState&, const SreCode* code);

// Whether the code is such as the compiler generates: the matcher trusts it.
bool sreValidate(const SreCode* code, size_t size, ptrdiff_t groups);

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
