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

    bool isModuleBody() const;
    bool isCoroutine() const { return hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }); }
    ConstructorForm constructorForm() const;
    bool takesThisFromConstruction() const { return kind == FunctionKind::Plain && constructorForm() == ConstructorForm::Base; }

    bool mayBeCalledByHost() const { return !has(FunctionCapabilities::IsNeverCalledByHost); }
    bool isHiddenInStackTraces() const { return has(FunctionCapabilities::IsHiddenInStackTraces) || has(FunctionFlags::IsHiddenInStackTraces); }

    Wire::SourceLocation locationOf(const BodyValue&) const;
    Wire::SourceLocation locationOf(const BodyTerminator&) const;
    Wire::SourceLocation locationAt(uint32_t position) const;
    Wire::SourceLocation declarationLocation() const;

    unsigned numberOfEntryParameters() const;
    std::optional<unsigned> thisParameterIndex() const;
    std::optional<unsigned> calleeParameterIndex() const;
    std::optional<unsigned> newTargetParameterIndex() const;
    std::optional<unsigned> argumentCountParameterIndex() const;
    unsigned firstParameterIndex() const;
    std::optional<unsigned> hiddenRestParameterIndex() const;
    unsigned numberOfPositionalParameters() const;

    EntryParameter entryParameterKind(unsigned entryParameterIndex) const;
    const DeclaredType* declaredEntryParameter(unsigned entryParameterIndex) const LIFETIME_BOUND;
};

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
