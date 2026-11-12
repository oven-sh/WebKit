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

    TypedPointer addressOf(VariableID);
    Operand loadVariable(VariableID);
    void storeVariable(VariableID, Operand, const Type& storedType, bool isUndoneAtRestart);
    TypedPointer addressOfStaticField(VariableID);
    bool holdsStaticField(VariableID) const;
    void storeStaticField(VariableID, Boxed);
    Operand loadFieldOfEnvironment(VariableID, LayoutID, uint32_t field);
    void storeFieldOfEnvironment(VariableID, LayoutID, uint32_t field, Operand, const Type& storedType, bool isUndoneAtRestart);
    ClassGuardKey keyOfGuard(LayoutID classLayout, ClassSide);
    ClassGuardKey keyOfGuard(LayoutID classLayout, ClassSide, PropertyKeyID name, AskerOfClassGuardOfName = AskerOfClassGuardOfName::Reads);
    ClassGuardKey keyOfGuardOfNames(LayoutID classLayout, ClassSide, std::span<const PropertyKeyID> names);
    TypedPointer guardByte(const ClassGuardKey&);
    TypedPointer countOfGuards(std::span<const ClassGuardKey>);
    TypedPointer rowOfCountsOfGuards(std::span<const Vector<ClassGuardKey, 4>> slots);
    TypedPointer wordOfEnvironmentHandedBack(LayoutID);
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
