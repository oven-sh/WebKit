/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTIRFunction.h"

#if ENABLE(AOT)

#include "AOTDiagnostics.h"

namespace JSC { namespace AOT { namespace Typed {

static_assert(BodyValue::noPosition == Wire::none, "A position that the front end left out reads as no position.");

const DeclaredType& Function::result() const
{
    RELEASE_ASSERT(results.size() == 1);
    return results[0];
}

ConstructorForm Function::constructorForm() const
{
    switch (kind) {
    case FunctionKind::Plain:
        return hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }) ? ConstructorForm::NotConstructor : ConstructorForm::Base;
    case FunctionKind::BaseConstructor:
        return ConstructorForm::Base;
    case FunctionKind::DerivedConstructor:
        return ConstructorForm::Derived;
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return ConstructorForm::NotConstructor;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Wire::SourceLocation Function::locationOf(const BodyValue& value) const
{
    if (!value)
        return noSourceLocation;
    if (std::optional<uint32_t> written = value.writtenPosition())
        return locationAt(*written);
    return locationAt(value.position());
}

Wire::SourceLocation Function::locationOf(const BodyTerminator& terminator) const
{
    if (!terminator)
        return noSourceLocation;
    return locationAt(terminator.position());
}

Wire::SourceLocation Function::locationAt(uint32_t positionInBody) const
{
    if (!module || positionInBody == BodyValue::noPosition)
        return noSourceLocation;
    return { module.index(), positionInBody };
}

Wire::SourceLocation Function::declarationLocation() const
{
    return locationAt(position);
}

static std::optional<unsigned> singleEntryParameterIndex(const Function& function, EntryParameter kind)
{
    if (!function.numberOfEntryParametersOfKind(kind))
        return std::nullopt;
    return function.numberOfEntryParametersBefore(kind);
}

std::optional<unsigned> Function::thisParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::This);
}

std::optional<unsigned> Function::calleeParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::Callee);
}

std::optional<unsigned> Function::newTargetParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::NewTarget);
}

std::optional<unsigned> Function::argumentCountParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::ArgumentCount);
}

std::optional<unsigned> Function::hiddenRestParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::HiddenRest);
}

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function& function)
{
    OptionSet<FunctionCapabilities> capabilities = capabilitiesOf(function.role);
    if (hasSourceBodyOf(function.role) && function.isCoroutine())
        capabilities.remove({ FunctionCapabilities::MayBeInlined, FunctionCapabilities::HasOrdinaryFrame });
    return capabilities;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
