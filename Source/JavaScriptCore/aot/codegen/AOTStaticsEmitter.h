/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTOperationEmitter.h"

namespace JSC { namespace AOT { namespace Typed {

class StaticsEmitter final : public OperationEmitter {
public:
    explicit StaticsEmitter(CodeGenerator& generator) : OperationEmitter(generator) { }

    // Image lays the statics out behind Compile, so each word is reached through a mention: PrimitiveEmitter::instanceWord().
    TypedPointer addressOf(VariableID);
    Operand loadVariable(VariableID);
    Operand loadEncodedVariable(VariableID);
    void storeVariable(VariableID, Operand, const Type& storedType);
    // The variable of a static field: a boxed word, empty where the field does not lie in it. It is a property of the class object, and the run time reads and writes it under the name.
    TypedPointer addressOfStaticField(VariableID);
    bool holdsStaticField(VariableID) const;
    void storeStaticField(VariableID, Boxed);
    // AOTClassGuards.h. What Image knows a guard by. Of a class with a record.
    ClassGuardKey keyOfGuard(LayoutID classLayout, ClassSide);
    ClassGuardKey keyOfGuard(LayoutID classLayout, ClassSide, PropertyKeyID name, bool isAskedByStoresAlone = false);
    // One guard for two names or more, each a key of the program, ascending and none twice.
    ClassGuardKey keyOfGuardOfNames(LayoutID classLayout, ClassSide, std::span<const PropertyKeyID> names);
    // The guard is the lowest bit of the byte.
    TypedPointer guardByte(const ClassGuardKey&);
    // A word of 32 bits: how many of the guards do not hold. Two guards or more, none twice, of whatever classes and sides.
    TypedPointer countOfGuards(std::span<const ClassGuardKey>);
    // Of a layout with PlannedLayout::isEscapedOffset. Zero, or a cell of the layout that nothing holds: the next allocation takes it. Every collection clears the word.
    TypedPointer wordOfEnvironmentHandedBack(LayoutID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
