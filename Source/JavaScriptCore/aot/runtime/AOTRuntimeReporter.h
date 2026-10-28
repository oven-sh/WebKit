/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTRecordTypes.h"
#include "AOTRuntimeValidation.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include "LineColumn.h"
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/SegmentedVector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace Wasm {
enum class InterpreterFallbackReason : uint8_t;
}

namespace AOT {

struct CompiledFrame;
struct Instance;
struct RuntimeType;

struct ReportDestination {
    std::optional<intptr_t> descriptor;
    bool isJSON { false };
    bool isExplicit { false };
};

class RuntimeReporter {
    WTF_MAKE_NONCOPYABLE(RuntimeReporter);
public:
    static constexpr unsigned maxNumberOfSites = 200;
    static constexpr unsigned maxNumberOfKindsAtSite = 8;
    static constexpr unsigned maxLengthOfType = 80;
    static constexpr unsigned maxNumberOfCallers = 3;

    // However the process ends by itself.
    static void callAtExit(void (*)());

    explicit RuntimeReporter(VM&);
    ~RuntimeReporter();

    // Null unless a log was asked for.
    static RuntimeReporter* ifExplicitlyEnabled(VM&);

    void reportFailedCheck(Instance&, TypeSiteID, const RuntimeType& expected, JSValue actual);
    void reportNativeCall(ValidationFailureKind, const String& callee, const String& mismatch);
    void writeAtExit();

private:
    struct Site {
        uint64_t id { 0 };
        ASCIILiteral kind;
        String file;
        LineColumn position;
        String function;
        String expected;
        Vector<std::pair<String, uint64_t>, 2> counts;
    };

    static void installExitHook();

    Site* findOrAddSite(Instance&, TypeSiteID, const RuntimeType& expected, const CompiledFrame*);
    void write(const Site&, const String& got, uint64_t count, std::span<const String> callers, bool isTotal);

    VM& m_vm;
    const ReportDestination m_destination;
    Lock m_lock;
    bool m_hasOverflowed { false };
    bool m_hasWrittenAtExit { false };
    SegmentedVector<Site, 8> m_sites;
    UncheckedKeyHashMap<uint64_t, Site*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sitesByCallSite;
    HashSet<String> m_reportedNativeCalls;
};

uint64_t typeCheckIDOf(std::span<const uint8_t> path, uint32_t position, TypeSiteKind);

void reportFailedCheck(Instance&, uint32_t typeSite, const RuntimeType& expected, JSValue actual);
// A log that was asked for gets every mismatch of a run. Without one the first mismatch ends the run.
void reportOrCrash(VM&, const ValidationFailure&, ASCIILiteral whatIsCalled, const String& callee, const String& mismatch);
void writeToStandardError(const String& line);
// Where the program stands, on its own thread: the places of the innermost frames of compiled code.
Vector<String, RuntimeReporter::maxNumberOfCallers> placesOfInnermostCompiledFrames(VM&);
#if ENABLE(WEBASSEMBLY)
void reportInterpreterFallback(Wasm::InterpreterFallbackReason);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
