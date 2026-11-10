/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTDeclaredFunctionsInScopes.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTOpcodeSets.h"
#include "AOTSweep.h"
#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

Reference findCaptureAtHand(Walker& walker, FunctionID callee, uint32_t capture, Reference closure)
{
    if (!closure || !closure.isValue() || !walker.has(Mechanism::DirectCalls))
        return { };
    const Program& program = walker.program();
    if (capture >= program.at(callee).captures.size())
        return { };
    LayoutID scope = scopeThatCaptureTakes(program, callee, capture);
    if (!scope)
        return { };

    const Code& code = walker.code();
    const Operation& read = code.at(underlyingCellOf(code, closure.id()));
    if (read.opcode != Opcode::GetEnvironmentField || read.layout() != scope)
        return { };
    if (functionDeclaredInScopeSlot(program, scope, read.field()) != callee)
        return { };
    ValueID holder = read.environmentOperand();
    return code.isConstant(holder) ? Reference::constant(holder) : Reference::value(holder);
}

enum class DefinitionOfStableVariable : uint8_t { IsRunByModule, IsPutOff };

static bool variableStaysAsFilled(const Walker& walker, VariableID id, DefinitionOfStableVariable definition = DefinitionOfStableVariable::IsRunByModule)
{
    const Program& program = walker.program();
    if (id.index() >= program.variables.size())
        return false;
    const Variable& variable = program.at(id);
    if (variable.owner || !variable.module || !variable.flags.contains(VariableFlags::IsImmutable))
        return false;
    if (variable.flags.containsAny({ VariableFlags::IsReplaceable, VariableFlags::HoldsMock, VariableFlags::OriginalMayRun }))
        return false;
    if (!loaderEntersBodyAtMostOnce(program.at(variable.module)))
        return false;
    const Plan& plan = walker.plan();
    if (plan.runTimeMayFill(id))
        return false;
    return !!plan.deferredDefinitions().findDeferred(id) == (definition == DefinitionOfStableVariable::IsPutOff);
}

bool numberReadOutOfVariableStays(const Walker& walker, VariableID variable)
{
    return variableStaysAsFilled(walker, variable);
}

static bool scopeSlotStaysAsFilled(const Walker& walker, FieldReference field)
{
    const Layout& layout = walker.program().at(field.layout);
    return layout.isEnvironment() && field.field < layout.fields.size() && layout.fields[field.field].flags.contains(FieldFlags::IsImmutable);
}

static bool keepsNotesOfStableBindings(const Walker& walker)
{
    return walker.has(Mechanism::Folding) && walker.state().remembersPlaces();
}

static ALWAYS_INLINE bool isValueOfFilledBinding(Walker& walker, Reference value, bool hasDeadZone)
{
    TypeID type = value ? walker.typeOf(value) : TypeID();
    uint16_t unfilledPrimitives = Primitive::Absent | Primitive::Unspecified | (hasDeadZone ? 0 : Primitive::Undefined);
    return type && !(walker.types().at(type).primitives & unfilledPrimitives);
}

static bool hasDeadZone(const Walker& walker, VariableID variable)
{
    return walker.program().at(variable).initialization == VariableInitialization::Uninitialized;
}

static bool hasDeadZone(const Walker& walker, FieldReference field)
{
    TypeID written = walker.program().at(field.layout).fields[field.field].declared.written();
    return written && (walker.program().at(written).primitives & Primitive::Absent);
}

void noteValueOfStableBinding(Walker& walker, VariableID variable, Reference value)
{
    if (keepsNotesOfStableBindings(walker) && variableStaysAsFilled(walker, variable) && isValueOfFilledBinding(walker, value, hasDeadZone(walker, variable)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index(), LocationMasks(), value.bits(), true);
}

void noteValueOfStableBinding(Walker& walker, Reference environment, FieldReference field, Reference value)
{
    if (environment && keepsNotesOfStableBindings(walker) && scopeSlotStaysAsFilled(walker, field) && isValueOfFilledBinding(walker, value, hasDeadZone(walker, field)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, field.field, LocationMasks(), value.bits(), true);
}

void noteObjectOfVariablePutOff(Walker& walker, VariableID variable, Reference object)
{
    if (keepsNotesOfStableBindings(walker) && variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff) && isValueOfFilledBinding(walker, object, hasDeadZone(walker, variable)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index(), LocationMasks(), object.bits(), true);
}

bool objectOfVariablePutOffStays(const Walker& walker, VariableID variable)
{
    return keepsNotesOfStableBindings(walker) && isVariableFilledAtFirstRead(walker.plan(), variable) && variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff);
}

Reference objectOfVariablePutOff(Walker& walker, VariableID variable)
{
    if (!keepsNotesOfStableBindings(walker) || !variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff))
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index());
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

Reference valueOfStableVariable(Walker& walker, VariableID variable, TypeID widest)
{
    if (!keepsNotesOfStableBindings(walker) || !variableStaysAsFilled(walker, variable))
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index());
    if (!noted)
        return { };
    Reference kept = Reference::fromBits(static_cast<uint32_t>(*noted));
    TypeID type = walker.typeOf(kept);
    return widest && type && walker.types().isSubset(type, widest) ? kept : Reference();
}

Reference valueOfStableBinding(Walker& walker, const BodyValue& read)
{
    if (!keepsNotesOfStableBindings(walker))
        return { };
    std::optional<uint64_t> noted;
    bool readsScopeThatUnitMakes = false;
    if (read.opcode() == Opcode::GetModuleVariable) {
        if (variableStaysAsFilled(walker, read.variable()))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, read.variable().index());
    } else if (read.opcode() == Opcode::GetEnvironmentField) {
        Reference environment = walker.referenceOf(read.environmentOperand());
        if (environment && scopeSlotStaysAsFilled(walker, { read.layout(), read.field() }))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, read.field());
        readsScopeThatUnitMakes = walker.unitHasMadeScopes() && noted && environment.isValue() && walker.code().at(environment.id()).opcode == Opcode::NewObject;
    }
    if (!noted)
        return { };
    Reference kept = Reference::fromBits(static_cast<uint32_t>(*noted));
    TypeID type = walker.typeOf(kept);
    if (type && readsScopeThatUnitMakes && !walker.isInLoop())
        return kept;
    TypeID widest = widestTypeOfOperation(walker, read);
    return widest && type && walker.types().isSubset(type, widest) ? kept : Reference();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
