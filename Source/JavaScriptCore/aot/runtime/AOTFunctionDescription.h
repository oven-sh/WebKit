/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "CodeType.h"
#include "JSExportMacros.h"
#include <optional>
#include <wtf/text/WTFString.h>

namespace JSC {

class JSCell;
class VM;

namespace AOT {

using Typed::FunctionID;
using CompiledFunctionID = Typed::ID<struct CompiledFunctionIDTag>;

enum class TraceVisibility : uint8_t { Visible, VisibleOnlyOnTop, Hidden };
enum class StackTraceBuiltinKind : uint8_t { None, JSBuiltin, JSBuiltinConstructor, HostFunction };

struct FunctionTraceData;
struct SourcePosition;

class FunctionDescription {
public:
    JS_EXPORT_PRIVATE explicit FunctionDescription(FunctionID);

    JS_EXPORT_PRIVATE String name(VM&) const;
    JS_EXPORT_PRIVATE String nameInFrames(VM&, JSCell* callee = nullptr) const;
    JS_EXPORT_PRIVATE CodeType codeType() const;
    JS_EXPORT_PRIVATE bool isStrict() const;
    JS_EXPORT_PRIVATE bool isConstructor() const;
    bool isNakedConstructor() const;
    bool isNamedBySymbol() const;
    bool recordsCallee() const;
    JS_EXPORT_PRIVATE bool isAsync() const;
    JS_EXPORT_PRIVATE bool isNamedByOwner() const;
    JS_EXPORT_PRIVATE bool isInRuntimeModule() const;
    JS_EXPORT_PRIVATE TraceVisibility visibility() const;
    JS_EXPORT_PRIVATE FunctionID sourceFunction() const;
    JS_EXPORT_PRIVATE bool rejectsOnThrow() const;
    JS_EXPORT_PRIVATE StackTraceBuiltinKind builtinKind() const;
    JS_EXPORT_PRIVATE std::optional<SourcePosition> definition() const;

private:
    FunctionID m_function;
    const FunctionTraceData* m_record;
};

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
