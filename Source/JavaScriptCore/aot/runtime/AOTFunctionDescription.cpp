/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFunctionDescription.h"

#if ENABLE(AOT)

#include "AOTFunctionNames.h"
#include "AOTFunctionTables.h"
#include "AOTImage.h"
#include "AOTProgramTables.h"

namespace JSC { namespace AOT {

static_assert(static_cast<unsigned>(TraceVisibility::Hidden) < 4 && static_cast<unsigned>(StackTraceBuiltinKind::HostFunction) < 4 && static_cast<unsigned>(ModuleCode) < 4);

FunctionDescription::FunctionDescription(FunctionID function)
    : m_function(function)
{
    const Image* image = Image::current();
    RELEASE_ASSERT(image);
    const ProgramTables& tables = image->programTables();
    RELEASE_ASSERT(function && function.index() < tables.numberOfFunctions());
    m_record = &tables.traceOfFunction(function.index());
}

String FunctionDescription::name(VM& vm) const
{
    if (Image::current()->isObfuscated())
        return emptyString();
    return functionName(vm, m_function, NameForm::AsWritten);
}

String FunctionDescription::nameInFrames(VM& vm) const
{
    if (Image::current()->isObfuscated())
        return emptyString();
    return functionName(vm, m_function, NameForm::InFrames);
}

CodeType FunctionDescription::codeType() const
{
    return m_record->codeType();
}

bool FunctionDescription::isStrict() const
{
    return m_record->isStrict;
}

bool FunctionDescription::isConstructor() const
{
    return m_record->isConstructor;
}

bool FunctionDescription::isNakedConstructor() const
{
    return m_record->isNakedConstructor;
}

bool FunctionDescription::isNamedBySymbol() const
{
    return m_record->isNamedBySymbol;
}

bool FunctionDescription::isAsync() const
{
    return m_record->isAsync;
}

bool FunctionDescription::isNamedByOwner() const
{
    return m_record->isNamedByOwner;
}

bool FunctionDescription::isInRuntimeModule() const
{
    return functionKindData(m_function).isInRuntimeModule;
}

TraceVisibility FunctionDescription::visibility() const
{
    return m_record->visibility();
}

FunctionID FunctionDescription::sourceFunction() const
{
    return sourceFunctionOf(m_function);
}

bool FunctionDescription::rejectsOnThrow() const
{
    return m_record->rejectsOnThrow;
}

StackTraceBuiltinKind FunctionDescription::builtinKind() const
{
    return m_record->builtinKind();
}

std::optional<SourcePosition> FunctionDescription::definition() const
{
    const Image* image = Image::current();
    for (FunctionID candidate : { m_function, sourceFunction() }) {
        if (auto index = image->imageFunctionOf(candidate))
            return image->positionOf(*index, CallSiteIndex(0));
    }
    return std::nullopt;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
