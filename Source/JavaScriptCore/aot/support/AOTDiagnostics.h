/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTLimits.h"
#include "AOTPassID.h"
#include "AOTSchemaEnums.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include "LineColumn.h"
#include <atomic>
#include <optional>
#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/WTFString.h>

namespace JSC { namespace AOT {

namespace Typed {
class Program;
}

using Wire::DiagnosticSeverity;
using Wire::InternalErrorKind;
using Wire::SourceLocation;
using Wire::UserDiagnosticKind;
using Wire::severityOf;

inline constexpr SourceLocation noSourceLocation { Wire::none, 0 };

constexpr bool hasModule(SourceLocation location) { return location.module != noSourceLocation.module; }

constexpr bool failsBuild(UserDiagnosticKind kind)
{
    switch (severityOf(kind)) {
    case DiagnosticSeverity::Error:
        return true;
    case DiagnosticSeverity::Warning:
    case DiagnosticSeverity::Note:
        return false;
    }
    return true;
}

static_assert(failsBuild(UserDiagnosticKind { }), "A record without a kind must fail the build.");

struct Subject {
    Typed::FunctionID function { };
    Typed::BlockID block { };
    Typed::ValueID value { };
    Typed::LayoutID layout { };
    SourceLocation location { noSourceLocation };
};

struct UserDiagnostic {
    UserDiagnosticKind kind;
    Typed::FunctionID function { };
    Typed::BlockID block { };
    Typed::ValueID value { };
    Typed::LayoutID layout { };
    LimitID limit { LimitID::None };
    uint32_t count { 0 };
    // A string of the program that the message quotes.
    Typed::StringID text { };
    SourceLocation location { noSourceLocation };
    // About an int32 promise: the type site of the position, and the least and the greatest number that may enter it.
    SourceLocation origin { noSourceLocation };
    Typed::TypeSiteKind siteKind { Typed::TypeSiteKind::Unknown };
    double minimum { 0 };
    double maximum { 0 };
};

struct InternalError {
    InternalErrorKind kind;
    Typed::FunctionID function { };
    Typed::BlockID block { };
    Typed::ValueID value { };
    Typed::LayoutID layout { };
    LimitID limit { LimitID::None };
    uint32_t count { 0 };
    // A string of the program that the message quotes.
    Typed::StringID text { };
    SourceLocation location { noSourceLocation };
    std::optional<LineColumn> textPosition { };
    PassID pass { PassID::None };
    // What only the reporter knows, in words. It ends the message.
    String detail { };
};

Typed::FunctionID findFunctionAsRead(const Typed::Program*, Typed::FunctionID);
Typed::LayoutID findLayoutAsRead(const Typed::Program*, Typed::LayoutID);
std::optional<SourceLocation> findLocation(const Typed::Program*, const Subject&);

class Diagnostics {
    WTF_MAKE_NONCOPYABLE(Diagnostics);
public:
    Diagnostics() = default;

    void add(const UserDiagnostic&);
    void add(const InternalError&);
    void append(Vector<UserDiagnostic>&&);
    void append(Vector<InternalError>&&);
    bool hasErrors() const { return m_hasErrors.load(std::memory_order_relaxed); }

    Diagnostics(Diagnostics&&);
    Diagnostics& operator=(Diagnostics&&);
    void resolveLocations(const Typed::Program&);
    void finish(const Typed::Program*);

    JS_EXPORT_PRIVATE const Vector<UserDiagnostic>& userDiagnostics() const;
    JS_EXPORT_PRIVATE const Vector<InternalError>& internalErrors() const;
    JS_EXPORT_PRIVATE const String& messageOfUserDiagnostic(size_t index) const;
    JS_EXPORT_PRIVATE const String& messageOfInternalError(size_t index) const;

private:
    template<typename Record> void appendTo(Vector<Record> Diagnostics::*, Vector<Record>&&);

    Lock m_lock;
    Vector<UserDiagnostic> m_userDiagnostics WTF_GUARDED_BY_LOCK(m_lock);
    Vector<InternalError> m_internalErrors WTF_GUARDED_BY_LOCK(m_lock);
    Vector<String> m_userDiagnosticMessages;
    Vector<String> m_internalErrorMessages;
    std::atomic<bool> m_hasErrors { false };
    bool m_isFinished { false };
};

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
