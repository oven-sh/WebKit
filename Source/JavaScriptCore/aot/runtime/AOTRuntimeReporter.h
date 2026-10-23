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
#include <memory>
#include <optional>
#include <span>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Lock.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace JSC {

class VM;

namespace Wasm {
enum class InterpreterFallbackReason : uint8_t;
}

namespace AOT {

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

    bool isEnabled() const { return m_destination.descriptor.has_value(); }
    bool isExplicitlyEnabled() const { return isEnabled() && m_destination.isExplicit; }

    void reportFailedCheck(Instance&, std::optional<uint32_t> typeSite, const RuntimeType& expected, JSValue actual);
    void reportNativeCall(ValidationFailureKind, const String& callee, const String& mismatch);
    void writeAtExit();

    CollectionValidator& collectionValidator() { return m_collectionValidator; }

private:
    using Callers = Vector<String, maxNumberOfCallers>;

    struct Site {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(Site);

        uint64_t id { 0 };
        ASCIILiteral kind;
        String file;
        LineColumn position;
        String function;
        String expected;
        Vector<std::pair<String, uint64_t>, 2> counts;
    };

    static void installExitHook();
    static uint64_t bump(Site&, ASCIILiteral word, const String& name);

    Site* addSite(uint64_t id, NOESCAPE const Invocable<std::unique_ptr<Site>()> auto& create);
    void countAndWrite(VM&, Site&, ASCIILiteral word, const String& name);
    void write(const Site&, const String& got, uint64_t count, const Callers&, bool isTotal);
    void writeTotals();
    void writeCounts();

    VM& m_vm;
    const ReportDestination m_destination;
    Lock m_lock;
    bool m_hasOverflowed { false };
    bool m_hasWrittenAtExit { false };
    UncheckedKeyHashMap<uint64_t, std::unique_ptr<Site>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sites;
    UncheckedKeyHashMap<uint64_t, Site*, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_sitesByCallSite;
    Vector<Site*> m_sitesInOrder;
    HashSet<String> m_reportedNativeCalls;
    CollectionValidator m_collectionValidator;
    RuntimeReporter* m_previous { nullptr };
    RuntimeReporter* m_next { nullptr };
};

uint64_t typeCheckIDOf(std::span<const uint8_t> path, uint32_t position, TypeSiteKind);

void reportFailedCheck(Instance&, uint32_t typeSite, const RuntimeType& expected, JSValue actual);
void writeToStandardError(const String& line);
#if ENABLE(WEBASSEMBLY)
void reportInterpreterFallback(Wasm::InterpreterFallbackReason);
#endif

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
