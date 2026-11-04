/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// A BINDING IS NO OBJECT. Nothing outside the scope of its declaration can name a variable of a module or a scope slot, so the front end's binder sees every store into one.
// What it has found stands on the wire: IsImmutable (a `const`, a declared function, a `let` that nothing assigns again), and the established type of a declared function. Link is asked nothing here.
//   `function outer() { function a() { return b() + 1 } function b() { .. } }`: whoever calls `b` out of the scope slot has the scope that `b` captured at hand.
//   `const cfg = load(); function f() { a(cfg.x); g(); b(cfg.x) }`: what a read has found in such a binding is what every later read finds.
// At a suspension: what a read has found is forgotten. The lowering of a decorated class stores into the name of the class twice, and between the two only code runs that the storing body waits for.

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

// ---- The scope at hand

// A declared function is made where its scope is entered, over that scope, and is stored into it: BodyVerifier holds every store into such a field to that.
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
    const Operation& read = code.at(rootOfCellInSweptCode(code, closure.id()));
    if (read.opcode != Opcode::GetEnvironmentField || read.layout() != scope)
        return { };
    if (functionDeclaredInScopeSlot(program, scope, read.field()) != callee)
        return { };
    ValueID holder = read.environmentOperand();
    return code.isConstant(holder) ? Reference::constant(holder) : Reference::value(holder);
}

// ---- What a read has found

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
    const Module& module = program.at(variable.module);
    if (!isEvaluatedAtMostOnceOf(module.kind) || module.flags.containsAny({ ModuleFlags::RunsAgain, ModuleFlags::MayBeEvicted, ModuleFlags::MayBeMocked }))
        return false;
    // No operation stands for such a store, or what lies in the word is a mark.
    const Plan& plan = walker.plan();
    if (plan.runTimeMayFill(id) || plan.deferredDefinitions().objectBeside(id))
        return false;
    return !!plan.deferredDefinitions().findDeferred(id) == (definition == DefinitionOfStableVariable::IsPutOff);
}

static bool scopeSlotStaysAsFilled(const Walker& walker, FieldReference field)
{
    const Layout& layout = walker.program().at(field.layout);
    return layout.isEnvironment() && field.field < layout.fields.size() && layout.fields[field.field].flags.contains(FieldFlags::IsImmutable);
}

static bool keepsNotesOfStableBindings(const Walker& walker)
{
    // A build that checks proofs builds each read, so that what it finds can be held against them.
    return walker.has(Mechanism::Folding) && !walker.checksProofs();
}

// A binding with a dead zone shows by itself that its store has run. A `var`, and whatever else is made with undefined in it, reads as undefined until then:
// the body that declares it may be a generator, which whoever holds it steps between two reads, or its store may be put off.
static bool isValueOfFilledBinding(Walker& walker, Reference value, bool hasDeadZone)
{
    TypeID type = value ? walker.typeOf(value) : TypeID();
    uint16_t saysNothing = Primitive::Absent | Primitive::Unspecified | (hasDeadZone ? 0 : Primitive::Undefined);
    return type && !(walker.types().at(type).primitives & saysNothing);
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

// `const table = { .. }` at the top of a module, made by whoever reads it first. The word holds a mark until then, so only what such a read has found behind its test, or made, is noted.
void noteObjectOfVariablePutOff(Walker& walker, VariableID variable, Reference object)
{
    if (keepsNotesOfStableBindings(walker) && variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff) && isValueOfFilledBinding(walker, object, hasDeadZone(walker, variable)))
        noteKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index(), LocationMasks(), object.bits(), true);
}

Reference objectOfVariablePutOff(Walker& walker, VariableID variable)
{
    if (!keepsNotesOfStableBindings(walker) || !variableStaysAsFilled(walker, variable, DefinitionOfStableVariable::IsPutOff))
        return { };
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, variable.index());
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

// Null: no read, store or check on every way to here has seen the binding filled.
Reference valueOfStableBinding(Walker& walker, const BodyValue& read)
{
    if (!keepsNotesOfStableBindings(walker))
        return { };
    std::optional<uint64_t> noted;
    if (read.opcode() == Opcode::GetModuleVariable) {
        if (variableStaysAsFilled(walker, read.variable()))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, { }, { }, read.variable().index());
    } else if (read.opcode() == Opcode::GetEnvironmentField) {
        Reference environment = walker.referenceOf(read.environmentOperand());
        if (environment && scopeSlotStaysAsFilled(walker, { read.layout(), read.field() }))
            noted = findKnown(walker, KnownNote::ValueKeptByPlace, environment, { }, read.field());
    }
    if (!noted)
        return { };
    // What stands for the read claims no more than the read would.
    Reference kept = Reference::fromBits(static_cast<uint32_t>(*noted));
    TypeID widest = widestTypeOfOperation(walker, read);
    TypeID type = walker.typeOf(kept);
    return widest && type && walker.types().isSubset(type, widest) ? kept : Reference();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
