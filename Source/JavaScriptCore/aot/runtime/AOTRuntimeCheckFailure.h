/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTFunctionDescription.h"
#include "JSCJSValue.h"
#include <wtf/StringPrintStream.h>
#include <wtf/text/WTFString.h>

namespace JSC {

struct CallerFrameAndPC;
class JSGlobalObject;
class VM;

namespace AOT {

struct LayoutData;

class RuntimeCheckFailure {
public:
    explicit RuntimeCheckFailure(ASCIILiteral failedCheck)
        : m_failedCheck(failedCheck)
    {
    }

    JS_EXPORT_PRIVATE RuntimeCheckFailure& atReturnAddress(const void*);
    JS_EXPORT_PRIVATE RuntimeCheckFailure& atTopCall(VM&, CallerFrameAndPC* operationFrameRecord = nullptr);
    JS_EXPORT_PRIVATE RuntimeCheckFailure& inFunction(FunctionID);

    template<typename... Parts>
    RuntimeCheckFailure& detail(const Parts&... parts)
    {
        m_detail.print(parts...);
        return *this;
    }

    JS_EXPORT_PRIVATE void report() const;
    JS_EXPORT_PRIVATE void crashInHost(JSGlobalObject*) const;

private:
    String headline() const;

    ASCIILiteral m_failedCheck;
    String m_callSite;
    String m_function;
    StringPrintStream m_detail;
};

JS_EXPORT_PRIVATE ASCIILiteral reportedTypeName(JSValue);
String reportedFunctionName(FunctionID);
String reportedClassName(const LayoutData& classLayout);
String reportedObjectName(const LayoutData&);

} // namespace AOT
} // namespace JSC

#endif // ENABLE(AOT)
