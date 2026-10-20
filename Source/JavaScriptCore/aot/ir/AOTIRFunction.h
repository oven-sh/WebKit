/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBlock.h"
#include "AOTSchemaRecords.h"
#include "AOTTypeTable.h"
#include "AOTWire.h"
#include <optional>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {

class FunctionRemap;
class StringRemap;
class TypeRemap;

enum class ConstructorForm : uint8_t { NotConstructor, Base, Derived };

struct Function : FunctionRecord {
    Vector<Block, 0, CrashOnOverflow, 2> blocks;
    Vector<Value, 0, CrashOnOverflow, 4> values;
    bool representationsAreChosen { false };

    bool has(FunctionFlags flag) const { return flags.contains(flag); }
    bool hasAny(OptionSet<FunctionFlags> set) const { return flags.containsAny(set); }
    bool has(FunctionCapabilities capability) const { return capabilities.contains(capability); }
    bool hasBody() const { return !blocks.isEmpty(); }

    Value& at(ValueID id) { ASSERT(id.index() < values.size()); return values.mutableSpan().data()[id.index()]; }
    const Value& at(ValueID id) const { ASSERT(id.index() < values.size()); return values.span().data()[id.index()]; }
    Block& at(BlockID id) { ASSERT(id.index() < blocks.size()); return blocks.mutableSpan().data()[id.index()]; }
    const Block& at(BlockID id) const { ASSERT(id.index() < blocks.size()); return blocks.span().data()[id.index()]; }

    const DeclaredType& result() const;

    bool isModuleBody() const;
    bool isUnsplitCoroutine() const;
    ConstructorForm constructorForm() const;

    bool mayChangeSignature() const { return has(FunctionCapabilities::MayChangeSignature); }
    bool mayBeCloned() const { return has(FunctionCapabilities::MayBeCloned); }
    bool mayBeInlined() const { return has(FunctionCapabilities::MayBeInlined); }
    bool hasOrdinaryFrame() const { return has(FunctionCapabilities::HasOrdinaryFrame); }
    bool hasOnlyDirectCallers() const { return has(FunctionCapabilities::HasOnlyDirectCallers); }
    bool mayBeCalledByHost() const { return !has(FunctionCapabilities::IsNeverCalledByHost); }
    bool isHiddenInStackTraces() const { return has(FunctionCapabilities::IsHiddenInStackTraces) || has(FunctionFlags::IsHiddenInStackTraces); }

    Wire::SourceLocation locationOf(ValueID) const;
    Wire::SourceLocation locationOf(Origin) const;
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
    DeclaredType* declaredEntryParameter(unsigned entryParameterIndex) LIFETIME_BOUND;
};

OptionSet<FunctionCapabilities> initialCapabilitiesOf(const Function&);

// Answers whether a block or a value moved.
bool compactFunction(Function&);
void remapIDs(Function&, const TypeRemap&, const StringRemap&, const FunctionRemap&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
