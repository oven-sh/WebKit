/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTDiagnostics.h"
#include "AOTLimits.h"
#include "AOTWire.h"
#include "B3CompilationReport.h"
#include "B3Limits.h"
#include "B3Origin.h"
#include "JSExportMacros.h"
#include <span>
#include <wtf/Vector.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {
struct Function;
}

struct FrameAddressViolation;
struct NarrowReferenceViolation;

JS_EXPORT_PRIVATE LimitID limitIDOf(B3::Limit);
JS_EXPORT_PRIVATE B3::LimitValues b3LimitValuesFrom(const BuildContext&);

struct BackEndFinding {
    InternalErrorKind kind;
    B3::Origin origin;
    B3::Origin safepoint { };
    String detail { };
};

struct BackEndDecline {
    Wire::DeclineKind kind;
    LimitID limit;
    Wire::LoopDeclineReason reason;
    B3::Origin origin;
};

struct BackEndLimitHit {
    LimitID limit;
    uint32_t count;
    B3::Origin firstOrigin;
};

struct BackEndReport {
    Vector<BackEndFinding, 1> findings;
    Vector<BackEndDecline> declines;
    Vector<BackEndLimitHit, 2> limitHits;
};

JS_EXPORT_PRIVATE BackEndReport collectBackEndReport(const B3::CompilationReport&, std::span<const FrameAddressViolation>, bool collectsDeclines);
JS_EXPORT_PRIVATE void appendFindings(BackEndReport&, std::span<const NarrowReferenceViolation>);
JS_EXPORT_PRIVATE bool reportBackEnd(BuildContext&, Typed::FunctionID, const Typed::Function&, const BackEndReport&, const B3::CompilationReport&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
