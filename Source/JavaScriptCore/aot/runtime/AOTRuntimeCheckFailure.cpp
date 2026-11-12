/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRuntimeCheckFailure.h"

#if ENABLE(AOT)

#include "AOTCallSiteTable.h"
#include "AOTCompileOptions.h"
#include "AOTCompiledFrame.h"
#include "AOTFunctionNames.h"
#include "AOTImage.h"
#include "AOTLayoutData.h"
#include "AOTModuleHost.h"
#include "AOTStackTrace.h"
#include "JSCInlines.h"
#include <wtf/DataLog.h>
#include <wtf/SetForScope.h>
#include <wtf/StringPrintStream.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

static String writtenNameOf(FunctionID function)
{
    const Image& image = *Image::current();
    if (image.isObfuscated() || !function || function.index() >= image.programTables().numberOfFunctions())
        return { };
    return functionName(FunctionDescription(FunctionDescription(function).sourceFunction()), NameForm::AsWritten);
}

static String describePosition(const Image& image, const TraceFrame& frame)
{
    if (image.isObfuscated()) {
        LineColumn position = obfuscatedPositionOf(frame);
        return makeString(position.line, ':', position.column);
    }
    std::optional<SourcePosition> position = positionOf(frame);
    if (!position || !position->source)
        return { };
    return makeString(displayPathOf(*position), ':', position->lineColumn.line, ':', position->lineColumn.column);
}

RuntimeCheckFailure& RuntimeCheckFailure::atReturnAddress(const void* returnAddress)
{
    const Image* image = Image::current();
    if (!image || !returnAddress)
        return *this;
    CodeLocation location = image->locate(returnAddress);
    if (location.kind != CodeKind::Function)
        return *this;
    std::optional<CallSiteRecord> site = image->callSiteAt(location.function, location.offset);
    if (!site)
        return inFunction(image->rootFunctionOf(location.function));
    FunctionID function = site->inlineFrame ? InlineFrameReader().at(*image, location.function, site->inlineFrame).function : image->rootFunctionOf(location.function);
    String position = describePosition(*image, TraceFrame { CompiledFunctionID(location.function), function, site->callSite, !!site->inlineFrame, false });
    if (position.isEmpty())
        return inFunction(function);
    String name = writtenNameOf(function);
    m_callSite = name.isEmpty() ? position : makeString(position, " ("_s, name, ')');
    return *this;
}

RuntimeCheckFailure& RuntimeCheckFailure::atTopCall(VM& vm, CallerFrameAndPC* operationFrameRecord)
{
    CallFrame* top = operationFrameRecord ? std::bit_cast<CallFrame*>(operationFrameRecord) : vm.topCallFrame;
    if (!Image::current() || !top || !Thread::currentSingleton().stack().contains(top))
        return *this;
    SetForScope topCallFrame(vm.topCallFrame, top);
    return atReturnAddress(CompiledFrameIterator::topFrameReturnAddress(vm));
}

RuntimeCheckFailure& RuntimeCheckFailure::inFunction(FunctionID function)
{
    const Image* image = Image::current();
    if (!image || !function || function.index() >= image->programTables().numberOfFunctions())
        return *this;
    String position;
    for (FunctionID candidate : { function, FunctionDescription(function).sourceFunction() }) {
        if (std::optional<uint32_t> code = position.isEmpty() ? image->imageFunctionOf(candidate) : std::nullopt)
            position = describePosition(*image, TraceFrame { CompiledFunctionID(*code), candidate, CallSiteIndex(0), false, false });
    }
    String name = writtenNameOf(function);
    if (name.isEmpty())
        m_function = position;
    else
        m_function = position.isEmpty() ? name : makeString(name, " ("_s, position, ')');
    return *this;
}

String RuntimeCheckFailure::headline() const
{
    if (!m_callSite.isEmpty())
        return makeString("internal AOT error: "_s, m_failedCheck, " at "_s, m_callSite);
    if (!m_function.isEmpty())
        return makeString("internal AOT error: "_s, m_failedCheck, " in "_s, m_function);
    return makeString("internal AOT error: "_s, m_failedCheck);
}

static bool isTestRun()
{
    const Image* image = Image::current();
    return image && hasFlag(image->header().options.admitted(), AdmittedCode::ReplacedBuiltins);
}

void RuntimeCheckFailure::report() const
{
    StringPrintStream out;
    out.print("bun: ", headline(), "\n");
    if (String detail = m_detail.toString(); !detail.isEmpty())
        out.print("  ", detail, "\n");
    out.print("  This is a bug in Bun, not in your code. ", isTestRun() ? "Run your tests without --aot"_s : "Build with --no-aot"_s, " to work around it,\n");
    out.print("  and please report it: https://github.com/oven-sh/bun/issues\n");
    dataLog(out.toUTF8CString());
}

void RuntimeCheckFailure::crashInHost(JSGlobalObject* realm) const
{
    auto* crashWithReason = realm ? moduleHostOf(realm).crashWithReason : nullptr;
    if (!crashWithReason)
        return;
    UTF8CString reason = makeString("internal AOT error: "_s, m_failedCheck, ": "_s, m_detail.toString()).utf8();
    crashWithReason(byteCast<char>(reason.span()));
}

ASCIILiteral reportedTypeName(JSValue value)
{
    if (!value)
        return "an empty value"_s;
    if (value.isInt32())
        return "int32"_s;
    if (value.isNumber())
        return "double"_s;
    if (value.isUndefined())
        return "undefined"_s;
    if (value.isNull())
        return "null"_s;
    if (value.isBoolean())
        return "boolean"_s;
    if (value.isString())
        return "string"_s;
    if (value.isSymbol())
        return "symbol"_s;
    if (value.isBigInt())
        return "bigint"_s;
    if (!value.isObject())
        return "an internal cell"_s;
    return asObject(value)->isCallable() ? "function"_s : "object"_s;
}

String reportedFunctionName(FunctionID function)
{
    String name = writtenNameOf(function);
    return name.isEmpty() ? String("a function"_s) : makeString('\'', name, '\'');
}

String reportedClassName(const LayoutData& classLayout)
{
    String name = classLayout.isClass() ? writtenNameOf(FunctionID(classLayout.classData().classConstructor)) : String();
    return name.isEmpty() ? String("a class"_s) : makeString("class '"_s, name, '\'');
}

String reportedObjectName(const LayoutData& layout)
{
    return layout.isClass() ? makeString("an instance of "_s, reportedClassName(layout)) : String("an object"_s);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
