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

// AUD1.N2 routing (1) — the keystone reroute. GIL-off the per-match scratch
// lives OFF the cell (per-thread buffer, RegExp.cpp banner); flag-off/GIL-on
// this is the old ovectorSpan() behind the Config-page gate. The matchInline
// ASSERT below (and the RELEASE_ASSERTs in RegExp::match/matchConcurrently)
// backstop any future caller that bypasses this routing. AB17e: unified on
// gilOffWithProcessGate (was raw vm.gilOff()) so the per-match flag-off
// predicate matches compileIfNecessary's in this same file — same value by
// the equivalence invariant (VM.h), loads only the read-only Config page
// flag-off.
ALWAYS_INLINE std::span<int> RegExp::ovectorSpan(VM& vm)
{
    if (vm.gilOffWithProcessGate()) [[unlikely]]
        return regExpGilOffPerThreadMatchOvector(*this);
    return m_ovector.mutableSpan();
}

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

ALWAYS_INLINE void RegExp::compileIfNecessary(VM& vm, Yarr::CharSize charSize, std::optional<StringView> sampleString)
{
    // AUD1.N2 residual (A): GIL-off, the lock-free hasCodeFor/m_state fast
    // path has no acquire edge to the cellLock'd publication of
    // m_regExpBytecode / m_regExpJITCode / m_atom — a foreign reader can see
    // m_state == ByteCode with m_regExpBytecode still null (observed null
    // deref, Yarr::interpret). Run {check, compile} under ONE cellLock hold;
    // the lock acquire also orders this thread's subsequent unlocked
    // m_state/bytecode/atom reads in matchInline after the winning compile.
    // CompilerThread callers never reach this (matchInline gates on
    // matchFrom; matchConcurrently already holds the cellLock). Flag-off/
    // GIL-on: byte-identically the historical lock-free path.
    // AB17d (bench I3 follow-up): Config-page gate — this predicate runs on
    // every mutator RegExp match flag-off.
    if (vm.gilOffWithProcessGate()) [[unlikely]] {
        Locker locker { cellLock() };
        if (hasCodeFor(charSize))
            return;
        if (m_state == ParseError)
            return;
        compileHoldingCellLock(locker, &vm, charSize, sampleString);
        return;
    }

    if (hasCodeFor(charSize))
        return;

    if (m_state == ParseError)
        return;

    compile(&vm, charSize, sampleString);
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE int RegExp::matchInline(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset, std::span<int> ovector)
{
#if ENABLE(REGEXP_TRACING)
    m_rtMatchCallCount++;
    m_rtMatchTotalSubjectStringLen += (double)(s.length() - startOffset);
#endif

    // AUD1.N2 residual (A): only the mutator compiles here. CompilerThread
    // entries come from matchConcurrently, which verified hasCodeFor UNDER
    // the cellLock it still holds — calling compileIfNecessary would
    // self-deadlock on its GIL-off locked arm and was a guaranteed no-op
    // anyway (GIL-on behavior unchanged: same no-op, now skipped).
    if constexpr (matchFrom == Yarr::MatchFrom::VMThread)
        compileIfNecessary(vm, s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16, s);

    auto throwError = [&] {
        if (matchFrom == Yarr::MatchFrom::CompilerThread)
            return -1;
        if (nullOrGlobalObject) {
            auto throwScope = DECLARE_THROW_SCOPE(vm);
            throwScope.throwException(nullOrGlobalObject, errorToThrow(nullOrGlobalObject));
        }
        if (!hasHardError(m_constructionErrorCode))
            reset();
        return -1;
    };

    if (m_state == ParseError)
        return throwError();

    ASSERT(ovector.size() >= static_cast<size_t>(offsetVectorSize()));
    // AUD1.N2: GIL-off, the cell-resident m_ovector must never be a match
    // target on any inlined entry path (JIT thunks included) — they are
    // covered by ovectorSpan(VM&) routing at their span origin.
    ASSERT(!vm.gilOff() || ovector.empty() || ovector.data() != m_ovector.mutableSpan().data());
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

        if (result == static_cast<int>(Yarr::JSRegExpResult::JITCodeFailure)) {
            // Punt to the bytecode interpreter. Only the mutator may compile bytecode; the compiler
            // thread must use the bytecode that already exists, and bails out if there is no
            // bytecode.
            if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
                byteCodeCompileIfNecessary(&vm);
                if (m_state == ParseError)
                    return throwError();
            }
            if (!m_regExpBytecode)
                return -1;
            {
                Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);
                result = Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(offsetVector));
            }
        }

#if ENABLE(YARR_JIT_DEBUG)
        if (m_state == JITCode) {
            byteCodeCompileIfNecessary(&vm);
            if (m_state == ParseError)
                return throwError();
            matchCompareWithInterpreter(s, startOffset, offsetVector, result);
        }
#endif
    } else
#endif
    {
        Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);
        result = Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(offsetVector));
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
    // AUD1.N2 residual (A) — same shape and rationale as compileIfNecessary
    // (AB17e: unified on the Config-page gate like its sibling above).
    if (vm.gilOffWithProcessGate()) [[unlikely]] {
        Locker locker { cellLock() };
        if (hasMatchOnlyCodeFor(charSize))
            return;
        if (m_state == ParseError)
            return;
        compileMatchOnlyHoldingCellLock(locker, &vm, charSize, sampleString);
        return;
    }

    if (hasMatchOnlyCodeFor(charSize))
        return;

    if (m_state == ParseError)
        return;

    compileMatchOnly(&vm, charSize, sampleString);
}

template<Yarr::MatchFrom matchFrom>
ALWAYS_INLINE MatchResult RegExp::matchInline(JSGlobalObject* nullOrGlobalObject, VM& vm, StringView s, unsigned startOffset)
{
#if ENABLE(REGEXP_TRACING)
    m_rtMatchOnlyCallCount++;
    m_rtMatchOnlyTotalSubjectStringLen += (double)(s.length() - startOffset);
#endif

    // AUD1.N2 residual (A): mutator-only compile — same rationale as the
    // span-overload matchInline above (matchConcurrently holds the cellLock).
    if constexpr (matchFrom == Yarr::MatchFrom::VMThread)
        compileIfNecessaryMatchOnly(vm, s.is8Bit() ? Yarr::CharSize::Char8 : Yarr::CharSize::Char16, s);

    auto throwError = [&] {
        if (matchFrom == Yarr::MatchFrom::CompilerThread)
            return MatchResult::failed();
        if (nullOrGlobalObject) {
            auto throwScope = DECLARE_THROW_SCOPE(vm);
            throwScope.throwException(nullOrGlobalObject, errorToThrow(nullOrGlobalObject));
        }
        if (!hasHardError(m_constructionErrorCode))
            reset();
        return MatchResult::failed();
    };

    if (m_state == ParseError)
        return throwError();

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
        if (result.start != static_cast<size_t>(Yarr::JSRegExpResult::JITCodeFailure))
            return result;

        // Punt to the bytecode interpreter. Only the mutator may compile bytecode; the compiler
        // thread must use the bytecode that already exists, and bails out if there is no
        // bytecode.
        if constexpr (matchFrom == Yarr::MatchFrom::VMThread) {
            byteCodeCompileIfNecessary(&vm);
            if (m_state == ParseError)
                return throwError();
        }
        if (!m_regExpBytecode)
            return MatchResult::failed();
    }
#endif

    int* offsetVector;
    int result;
    Vector<int, 32> nonReturnedOvector;
    nonReturnedOvector.grow(offsetVectorSize());
    offsetVector = nonReturnedOvector.mutableSpan().data();
    {
        Yarr::MatchingContextHolder regExpContext(vm, this, matchFrom);
        result = Yarr::interpret(m_regExpBytecode.get(), s, startOffset, reinterpret_cast<unsigned*>(offsetVector));
    }
#if REGEXP_FUNC_TEST_DATA_GEN
    RegExpFunctionalTestCollector::get()->outputOneTest(this, s, startOffset, offsetVector, result);
#endif

    if (result >= 0) {
#if ENABLE(REGEXP_TRACING)
        m_rtMatchOnlyFoundCount++;
#endif
        return MatchResult(result, reinterpret_cast<unsigned*>(offsetVector)[1]);
    }

    return MatchResult::failed();
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
