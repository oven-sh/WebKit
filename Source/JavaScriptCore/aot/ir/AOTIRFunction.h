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

// The record of a function as the reader made it, and its body where the front end wrote it. Nothing changes either once the program is read.
struct Function : FunctionRecord {
    Body body;

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool hasAny(OptionSet<FunctionFlags> set) const { return flags.containsAny(set); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasBody() const { return !!body; }

    const DeclaredType& result() const;

    bool isModuleBody() const;
    bool isCoroutine() const { return hasAny({ FunctionFlags::IsAsync, FunctionFlags::IsGenerator }); }
    // A stack frame shows the property `name` of the function object from the first look for it on. Before that it shows the displayName, and the name of an accessor without "get " or "set ".
    bool framesTellWhetherNameWasLookedFor() const { return has(FunctionCapabilities::NotesLooksForName) || kind == FunctionKind::Getter || kind == FunctionKind::Setter; }
    ConstructorForm constructorForm() const;
    // The construct call allocates `this` for a plain function. A class constructor allocates its own.
    bool takesThisFromConstruction() const { return kind == FunctionKind::Plain && constructorForm() == ConstructorForm::Base; }

    bool hasOnlyDirectCallers() const { return has(FunctionCapabilities::HasOnlyDirectCallers); }
    bool mayBeCalledByHost() const { return !has(FunctionCapabilities::IsNeverCalledByHost); }
    bool isHiddenInStackTraces() const { return has(FunctionCapabilities::IsHiddenInStackTraces) || has(FunctionFlags::IsHiddenInStackTraces); }

    // Where the source text has the value: its written position where it has one, else where it reports errors.
    Wire::SourceLocation locationOf(const BodyValue&) const;
    Wire::SourceLocation locationOf(const BodyTerminator&) const;
    // `position` is one of this function's own body. Code::locationOf() answers for an origin in an inline frame.
    Wire::SourceLocation locationAt(uint32_t position) const;
    Wire::SourceLocation declarationLocation() const;

    // The parameters of the entry block, in the order of EntryParameter.
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
