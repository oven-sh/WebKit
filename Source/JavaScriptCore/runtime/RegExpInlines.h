/*
 *  Copyright (C) 1999-2001, 2004 Harri Porten (porten@kde.org)
 *  Copyright (c) 2007, 2008, 2016 Apple Inc. All rights reserved.
 *  Copyright (C) 2009 Torch Mobile, Inc.
 *  Copyright (C) 2010 Peter Varga (pvarga@inf.u-szeged.hu), University of Szeged
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#pragma once

#include "RegExp.h"
#include "JSCInlines.h"
#include "Yarr.h"
#include "YarrInterpreter.h"
#include "YarrJIT.h"
#include "YarrMatchingContextHolder.h"

#define REGEXP_FUNC_TEST_DATA_GEN 0

#if REGEXP_FUNC_TEST_DATA_GEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#endif

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

#if REGEXP_FUNC_TEST_DATA_GEN
class RegExpFunctionalTestCollector {
    // This class is not thread safe.
protected:
    static const char* const s_fileName;

public:
    static RegExpFunctionalTestCollector* get();

    ~RegExpFunctionalTestCollector();

    void outputOneTest(RegExp*, StringView, int, int*, int);
    void clearRegExp(RegExp* regExp)
    {
        if (regExp == m_lastRegExp)
            m_lastRegExp = 0;
    }

private:
    RegExpFunctionalTestCollector();

    void outputEscapedString(StringView, bool escapeSlash = false);

    static RegExpFunctionalTestCollector* s_instance;
    FILE* m_file;
    RegExp* m_lastRegExp;
};
#endif // REGEXP_FUNC_TEST_DATA_GEN

template<typename CellType, SubspaceAccess mode>
inline GCClient::IsoSubspace* RegExp::subspaceFor(VM& vm)
{
    return &vm.regExpSpace();
}

inline Structure* RegExp::createStructure(VM& vm, JSGlobalObject* globalObject, JSValue prototype)
{
    return Structure::create(vm, globalObject, prototype, TypeInfo(CellType, StructureFlags), info());
}

ALWAYS_INLINE bool RegExp::hasCodeFor(Yarr::CharSize charSize)
{
    if (hasCode()) {
#if ENABLE(YARR_JIT)
        if (m_state != JITCode)
            return true;
        ASSERT(m_regExpJITCode);
        if ((charSize == Yarr::CharSize::Char8) && (m_regExpJITCode->has8BitCode()))
            return true;
        if ((charSize == Yarr::CharSize::Char16) && (m_regExpJITCode->has16BitCode()))
            return true;
#else
        UNUSED_PARAM(charSize);
        return true;
#endif
    }
    return false;
}

// The low bits of the heap's marking version: one per full collection.
ALWAYS_INLINE uint8_t RegExp::currentUseEpoch(VM& vm)
{
    return static_cast<uint8_t>(vm.heap.objectSpace().markingVersion());
}

// Only the VM's thread notes the use. A compiler thread (matchConcurrently()) runs code that is already there, and writes nothing here.
template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE void RegExp::noteUse(VM& vm)
{
    if constexpr (matchFrom == Yarr::MatchFrom::VMThread)
        m_lastUseEpoch = currentUseEpoch(vm);
}

ALWAYS_INLINE void RegExp::compileIfNecessary(VM& vm, Yarr::CharSize charSize, std::optional<StringView> sampleString)
{
    if (hasCodeFor(charSize))
        return;

    if (m_state == ParseError)
        return;

    compile(&vm, charSize, sampleString);
}

// Runs the bytecode interpreter and returns what JIT code returns: the start of the match, -1 for no
// match, or below -1 the JSRegExpResult the search was abandoned with.
template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::interpretInline(VM& vm, StringView s, unsigned startOffset, int* offsetVector)
{
    Yarr::JSRegExpResult result;
    {
        Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);
        result = Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(offsetVector));
    }
    if (result == Yarr::JSRegExpResult::Match)
        return offsetVector[0];
    if (result == Yarr::JSRegExpResult::NoMatch)
        return -1;
    ASSERT(Yarr::isAbandoned(result));
    return static_cast<int>(result);
}

// m_state is ParseError: the pattern did not compile. The VM's thread throws for it, and every
// caller sees no match. This and the helpers below are not lambdas in matchInlineOnce: one that
// captures by reference costs every match the stores that build its closure.
template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::matchParseError(JSGlobalObject* nullOrGlobalObject, VM& vm)
{
    if constexpr (matchFrom == Yarr::MatchFrom::VMThread)
        throwParseError(nullOrGlobalObject, vm);
    return -1;
}

// The search was abandoned (Yarr::isAbandoned). With a global object this throws and returns -1: the
// caller sees no match and the exception. A caller with none has nothing to throw on, so it gets the
// code, which is below -1 and is not a failed match. That is every compiler thread: matchConcurrently()
// reads the code as no answer.
template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::matchAbandoned(JSGlobalObject* nullOrGlobalObject, int code)
{
    ASSERT(Yarr::isAbandoned(static_cast<Yarr::JSRegExpResult>(code)));
    if (matchFrom == Yarr::MatchFrom::CompilerThread || !nullOrGlobalObject)
        return code;
    throwMatchAbandoned(nullOrGlobalObject, static_cast<Yarr::JSRegExpResult>(code));
    return -1;
}

// The same in MatchResult::start, where JIT code puts a code. -1 there is WTF::notFound: MatchResult::failed().
template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE MatchResult RegExp::matchAbandonedResult(JSGlobalObject* nullOrGlobalObject, int code)
{
    return MatchResult { static_cast<size_t>(static_cast<ptrdiff_t>(matchAbandoned<matchFrom>(nullOrGlobalObject, code))), 0 };
}

template<Yarr::MatchFrom matchFrom>
NEVER_INLINE int RegExp::matchInlineAtCodePointBoundaries(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset, std::span<int> ovector)
{
    if (splitsSurrogatePair(s, startOffset))
        --startOffset;
    int result = matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset, ovector);
    while (result > static_cast<int>(startOffset) && splitsSurrogatePair(s, result)) {
        startOffset = result + 1;
        result = matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset, ovector);
    }
    return result;
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::matchInline(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset, std::span<int> ovector)
{
    // Two separate predictable branches keep the common paths to a flag test plus a
    // tail call; only a unicode pattern on a 16-bit subject needs the boundary fixup.
    if (!eitherUnicode()) [[likely]]
        return matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset, ovector);
    if (s.is8Bit()) [[likely]]
        return matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset, ovector);
    return matchInlineAtCodePointBoundaries<matchFrom>(nullOrGlobalObject, vm, s, startOffset, ovector);
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::matchInlineOnce(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset, std::span<int> ovector)
{
#if ENABLE(REGEXP_TRACING)
    m_rtMatchCallCount++;
    m_rtMatchTotalSubjectStringLen += (double)(s.length() - startOffset);
#endif

    if (s.length() - startOffset < m_minimumSize)
        return -1;

    noteUse<matchFrom>(vm);
    compileIfNecessary(vm, s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16, s);

    if (m_state == ParseError)
        return matchParseError<matchFrom>(nullOrGlobalObject, vm);

    ASSERT(ovector.size() >= static_cast<size_t>(offsetVectorSize()));
    int* offsetVector = ovector.data();

    if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
        if (hasValidAtom()) {
            size_t found = s.find(vm.adaptiveStringSearcherTables(), atom(), startOffset);
            if (found == notFound)
                return -1;
            offsetVector[0] = found;
            offsetVector[1] = found + atom().length();
            return found;
        }
    }

    int result;
#if ENABLE(YARR_JIT)
    if (m_state == JITCode) {
        {
            ASSERT(m_regExpJITCode);
            Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);

            if (s.is8Bit())
                result = m_regExpJITCode->execute(s.span8(), startOffset, offsetVector, &regExpContext).start;
            else
                result = m_regExpJITCode->execute(s.span16(), startOffset, offsetVector, &regExpContext).start;
        }

        // A match or -1 is the answer. Below -1 the JIT code wants the interpreter (JITCodeFailure)
        // or abandoned the search: one compare on the way out of every JIT match covers both.
        if (result < -1) [[unlikely]] {
            if (result == static_cast<int>(Yarr::JSRegExpResult::JITCodeFailure)) {
                // Punt to the bytecode interpreter. Only the mutator may compile bytecode; the compiler
                // thread must use the bytecode that already exists, and bails out if there is no
                // bytecode.
                if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
                    byteCodeCompileIfNecessary(&vm);
                    if (m_state == ParseError)
                        return matchParseError<matchFrom>(nullOrGlobalObject, vm);
                }
                if (!m_regExpBytecode) {
                    ASSERT(matchFrom == Yarr::MatchFrom::CompilerThread);
                    return result;
                }
                result = interpretInline<matchFrom>(vm, s, startOffset, offsetVector);
            }
            if (result < -1)
                return matchAbandoned<matchFrom>(nullOrGlobalObject, result);
        }

#if ENABLE(YARR_JIT_DEBUG)
        if (m_state == JITCode) {
            byteCodeCompileIfNecessary(&vm);
            if (m_state == ParseError)
                return matchParseError<matchFrom>(nullOrGlobalObject, vm);
            matchCompareWithInterpreter(s, startOffset, offsetVector, result);
        }
#endif
    } else
#endif
    {
        result = interpretInline<matchFrom>(vm, s, startOffset, offsetVector);
        if (result < -1) [[unlikely]]
            return matchAbandoned<matchFrom>(nullOrGlobalObject, result);
    }

    ASSERT(result >= -1);

#if REGEXP_FUNC_TEST_DATA_GEN
    RegExpFunctionalTestCollector::get()->outputOneTest(this, s, startOffset, offsetVector, result);
#endif

#if ENABLE(REGEXP_TRACING)
    if (result != -1)
        m_rtMatchFoundCount++;
#endif

    return result;
}

ALWAYS_INLINE bool RegExp::hasMatchOnlyCodeFor(Yarr::CharSize charSize)
{
    if (hasCode()) {
#if ENABLE(YARR_JIT)
        if (m_state != JITCode)
            return true;
        ASSERT(m_regExpJITCode);
        if ((charSize == Yarr::CharSize::Char8) && (m_regExpJITCode->has8BitCodeMatchOnly()))
            return true;
        if ((charSize == Yarr::CharSize::Char16) && (m_regExpJITCode->has16BitCodeMatchOnly()))
            return true;
#else
        UNUSED_PARAM(charSize);
        return true;
#endif
    }

    return false;
}

ALWAYS_INLINE void RegExp::compileIfNecessaryMatchOnly(VM& vm, Yarr::CharSize charSize, std::optional<StringView> sampleString)
{
    if (hasMatchOnlyCodeFor(charSize))
        return;

    if (m_state == ParseError)
        return;

    compileMatchOnly(&vm, charSize, sampleString);
}

template<Yarr::MatchFrom matchFrom>
NEVER_INLINE MatchResult RegExp::matchInlineAtCodePointBoundaries(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset)
{
    if (splitsSurrogatePair(s, startOffset))
        --startOffset;
    MatchResult result = matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset);
    while (result && result.start > startOffset && splitsSurrogatePair(s, result.start)) {
        startOffset = result.start + 1;
        result = matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset);
    }
    return result;
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE MatchResult RegExp::matchInline(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset)
{
    if (!eitherUnicode()) [[likely]]
        return matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset);
    if (s.is8Bit()) [[likely]]
        return matchInlineOnce<matchFrom>(nullOrGlobalObject, vm, s, startOffset);
    return matchInlineAtCodePointBoundaries<matchFrom>(nullOrGlobalObject, vm, s, startOffset);
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE MatchResult RegExp::matchInlineOnce(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset)
{
#if ENABLE(REGEXP_TRACING)
    m_rtMatchOnlyCallCount++;
    m_rtMatchOnlyTotalSubjectStringLen += (double)(s.length() - startOffset);
#endif

    if (s.length() - startOffset < m_minimumSize)
        return MatchResult::failed();

    noteUse<matchFrom>(vm);
    compileIfNecessaryMatchOnly(vm, s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16, s);

    if (m_state == ParseError) {
        matchParseError<matchFrom>(nullOrGlobalObject, vm);
        return MatchResult::failed();
    }

    if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
        if (hasValidAtom()) {
            size_t found = StringView(s).find(vm.adaptiveStringSearcherTables(), atom(), startOffset);
            if (found == notFound)
                return MatchResult::failed();
            return MatchResult { found, found + atom().length() };
        }
    }

#if ENABLE(YARR_JIT)
    if (m_state == JITCode) {
        MatchResult result;
        {
            ASSERT(m_regExpJITCode);
            Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);

            if (s.is8Bit())
                result = m_regExpJITCode->execute(s.span8(), startOffset, &regExpContext);
            else
                result = m_regExpJITCode->execute(s.span16(), startOffset, &regExpContext);
        }

#if ENABLE(REGEXP_TRACING)
        if (!result)
            m_rtMatchOnlyFoundCount++;
#endif
        // JIT code returns a negative JSRegExpResult where the start would go. -1 (no match) is an
        // answer; below it the code wants the interpreter (JITCodeFailure) or abandoned the search.
        if (static_cast<ptrdiff_t>(result.start) >= -1) [[likely]]
            return result;
        if (result.start != static_cast<size_t>(Yarr::JSRegExpResult::JITCodeFailure))
            return matchAbandonedResult<matchFrom>(nullOrGlobalObject, static_cast<int>(result.start));

        // Punt to the bytecode interpreter. Only the mutator may compile bytecode; the compiler
        // thread must use the bytecode that already exists, and bails out if there is no
        // bytecode.
        if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
            byteCodeCompileIfNecessary(&vm);
            if (m_state == ParseError) {
                matchParseError<matchFrom>(nullOrGlobalObject, vm);
                return MatchResult::failed();
            }
        }
        if (!m_regExpBytecode) {
            ASSERT(matchFrom == Yarr::MatchFrom::CompilerThread);
            return result;
        }
    }
#endif

    int* offsetVector;
    int result;
    Vector<int, 32> nonReturnedOvector;
    nonReturnedOvector.grow(offsetVectorSize());
    offsetVector = nonReturnedOvector.mutableSpan().data();
    result = interpretInline<matchFrom>(vm, s, startOffset, offsetVector);
#if REGEXP_FUNC_TEST_DATA_GEN
    RegExpFunctionalTestCollector::get()->outputOneTest(this, s, startOffset, offsetVector, result);
#endif

    if (result >= 0) {
#if ENABLE(REGEXP_TRACING)
        m_rtMatchOnlyFoundCount++;
#endif
        return MatchResult(result, reinterpret_cast<unsigned*>(offsetVector)[1]);
    }

    if (result < -1) [[unlikely]]
        return matchAbandonedResult<matchFrom>(nullOrGlobalObject, result);

    return MatchResult::failed();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
