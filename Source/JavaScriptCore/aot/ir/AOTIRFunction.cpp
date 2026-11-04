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

bool Function::isModuleBody() const
{
    switch (kind) {
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return true;
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static unsigned entryParameterCount(const Function& function, EntryParameter kind)
{
    switch (kind) {
    case EntryParameter::Capture:
        return function.captures.size();
    case EntryParameter::Parameter:
        return function.parameters.size();
    case EntryParameter::This:
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return function.flags.containsAll(presentWithOf(kind)) ? 1 : 0;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static unsigned numberOfEntryParametersBefore(const Function& function, EntryParameter kind)
{
    unsigned count = 0;
    for (EntryParameter earlierKind : allMembersOf<EntryParameter>) {
        if (earlierKind == kind)
            break;
        count += entryParameterCount(function, earlierKind);
    }
    return count;
}

static std::optional<unsigned> singleEntryParameterIndex(const Function& function, EntryParameter kind)
{
    if (!entryParameterCount(function, kind))
        return std::nullopt;
    return numberOfEntryParametersBefore(function, kind);
}

unsigned Function::numberOfEntryParameters() const
{
    unsigned count = 0;
    for (EntryParameter kind : allMembersOf<EntryParameter>)
        count += entryParameterCount(*this, kind);
    return count;
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

unsigned Function::firstParameterIndex() const
{
    return numberOfEntryParametersBefore(*this, EntryParameter::Parameter);
}

std::optional<unsigned> Function::hiddenRestParameterIndex() const
{
    return singleEntryParameterIndex(*this, EntryParameter::HiddenRest);
}

unsigned Function::numberOfPositionalParameters() const
{
    unsigned numberOfRestParameters = has(FunctionFlags::HasRestParameter);
    RELEASE_ASSERT(parameters.size() >= numberOfRestParameters);
    return parameters.size() - numberOfRestParameters;
}

EntryParameter Function::entryParameterKind(unsigned entryParameterIndex) const
{
    unsigned end = 0;
    for (EntryParameter kind : allMembersOf<EntryParameter>) {
        end += entryParameterCount(*this, kind);
        if (entryParameterIndex < end)
            return kind;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

const DeclaredType* Function::declaredEntryParameter(unsigned entryParameterIndex) const
{
    EntryParameter kind = entryParameterKind(entryParameterIndex);
    switch (kind) {
    case EntryParameter::Capture:
        return &captures[entryParameterIndex - numberOfEntryParametersBefore(*this, kind)];
    case EntryParameter::This:
        return &thisType;
    case EntryParameter::Parameter:
        return &parameters[entryParameterIndex - numberOfEntryParametersBefore(*this, kind)];
    case EntryParameter::Callee:
    case EntryParameter::NewTarget:
    case EntryParameter::ArgumentCount:
    case EntryParameter::HiddenRest:
        return nullptr;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function& function)
{
    OptionSet<FunctionCapabilities> capabilities = capabilitiesOf(function.role);
    // The frame of a coroutine's body is saved at each suspension and entered again.
    if (hasSourceBodyOf(function.role) && function.isCoroutine())
        capabilities.remove({ FunctionCapabilities::MayBeInlined, FunctionCapabilities::HasOrdinaryFrame });
    return capabilities;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
