/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBody.h"
#include "AOTSchemaRecords.h"
#include "AOTWire.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

enum class ConstructorForm : uint8_t { NotConstructor, Base, Derived };

struct Function : FunctionRecord {
    Body body;

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool hasAny(OptionSet<FunctionFlags> set) const { return flags.containsAny(set); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasBody() const { return !!body; }

    const DeclaredType& result() const;

    bool isModuleBody() const
    {
        static_assert(numberOfEnumerators<FunctionKind> == 10, "say here whether the new kind of function is the body of a module");
        return kind == FunctionKind::Module || kind == FunctionKind::CommonJsModule;
    }
    bool isCoroutine() const { return hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }); }
    ConstructorForm constructorForm() const;
    bool takesThisFromConstruction() const { return kind == FunctionKind::Plain && constructorForm() == ConstructorForm::Base; }

    bool isHiddenInStackTraces() const { return has(FunctionCapabilities::IsHiddenInStackTraces) || has(FunctionFlags::IsHiddenInStackTraces); }

    Wire::SourceLocation locationOf(const BodyValue&) const;
    Wire::SourceLocation locationOf(const BodyTerminator&) const;
    Wire::SourceLocation locationAt(uint32_t position) const;
    Wire::SourceLocation declarationLocation() const;

    ALWAYS_INLINE unsigned numberOfEntryParametersOfKind(EntryParameter kind) const
    {
        switch (kind) {
        case EntryParameter::Capture:
            return captures.size();
        case EntryParameter::Parameter:
            return parameters.size();
        case EntryParameter::This:
        case EntryParameter::Callee:
        case EntryParameter::NewTarget:
        case EntryParameter::ArgumentCount:
        case EntryParameter::HiddenRest:
            return flags.containsAll(presentWithOf(kind)) ? 1 : 0;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    ALWAYS_INLINE unsigned numberOfEntryParametersBefore(EntryParameter kind) const
    {
        unsigned count = 0;
        for (EntryParameter earlierKind : allMembersOf<EntryParameter>) {
            if (earlierKind == kind)
                break;
            count += numberOfEntryParametersOfKind(earlierKind);
        }
        return count;
    }
    ALWAYS_INLINE unsigned numberOfEntryParameters() const
    {
        unsigned count = 0;
        for (EntryParameter kind : allMembersOf<EntryParameter>)
            count += numberOfEntryParametersOfKind(kind);
        return count;
    }
    std::optional<unsigned> thisParameterIndex() const;
    std::optional<unsigned> calleeParameterIndex() const;
    std::optional<unsigned> newTargetParameterIndex() const;
    std::optional<unsigned> argumentCountParameterIndex() const;
    ALWAYS_INLINE unsigned firstParameterIndex() const { return numberOfEntryParametersBefore(EntryParameter::Parameter); }
    std::optional<unsigned> hiddenRestParameterIndex() const;
    ALWAYS_INLINE unsigned numberOfPositionalParameters() const
    {
        unsigned numberOfRestParameters = has(FunctionFlags::HasRestParameter);
        RELEASE_ASSERT(parameters.size() >= numberOfRestParameters);
        return parameters.size() - numberOfRestParameters;
    }

    ALWAYS_INLINE EntryParameter entryParameterKind(unsigned entryParameterIndex) const
    {
        unsigned end = 0;
        for (EntryParameter kind : allMembersOf<EntryParameter>) {
            end += numberOfEntryParametersOfKind(kind);
            if (entryParameterIndex < end)
                return kind;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    ALWAYS_INLINE const DeclaredType* declaredEntryParameter(unsigned entryParameterIndex) const LIFETIME_BOUND
    {
        unsigned first = 0;
        for (EntryParameter kind : allMembersOf<EntryParameter>) {
            unsigned end = first + numberOfEntryParametersOfKind(kind);
            if (entryParameterIndex >= end) {
                first = end;
                continue;
            }
            switch (kind) {
            case EntryParameter::Capture:
                return &captures[entryParameterIndex - first];
            case EntryParameter::This:
                return &thisType;
            case EntryParameter::Parameter:
                return &parameters[entryParameterIndex - first];
            case EntryParameter::Callee:
            case EntryParameter::NewTarget:
            case EntryParameter::ArgumentCount:
            case EntryParameter::HiddenRest:
                return nullptr;
            }
            RELEASE_ASSERT_NOT_REACHED();
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
};

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
