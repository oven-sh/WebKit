/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinTypes.h"
#include "AOTBuiltinUses.h"
#include "AOTCodeShapes.h"
#include "AOTResultRules.h"
#include "AOTSelectionContext.h"
#include "AOTTypePropagation.h"

namespace JSC { namespace AOT { namespace Typed {

static ExpectedElements expectedElementsOf(const Program& program, const Type& receiver)
{
    switch (typedArrayTypeOf(program.builtins(), receiver)) {
    case TypeInt32:
        return ExpectedElements::Int32Array;
    case TypeUint8:
        return ExpectedElements::Uint8Array;
    default:
        // An array of numbers says little: what compiled code makes has doubles, what a built-in hands out has int32 as often, and a wrong guess costs every access.
        return ExpectedElements::Anything;
    }
}

// An access by key that is still here calls a stub, whatever arrives. What its receiver is expected to be picks the stub that comes to that soonest. Nobody knows it behind this pass.
static PassOutcome noteExpectedElements(const Program& program, Function& function)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (Value& value : function.editAllValues()) {
        if (value.opcode != Opcode::ElemGet && value.opcode != Opcode::ElemSet)
            continue;
        ExpectedElements expected = expectedElementsOf(program, program.expectedOrTypeOf(function, value.objectOperand()));
        if (expected == value.expectedElements())
            continue;
        value.setExpectedElements(expected);
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

static PassOutcome clearExpectedTypes(Function& function)
{
    PassOutcome outcome = PassOutcome::Unchanged;
    for (Value& value : function.editAllValues()) {
        if (!value.expected())
            continue;
        value.setExpected(TypeID());
        outcome = PassOutcome::Changed;
    }
    return outcome;
}

// A call is matched with an overload by the types of its operands. Where an operand no longer has the type that the overload takes, no form of the overload may be
// compiled: each of them reads the operand as what it is declared to be. The built-in is called there as a function is, with whatever arrives.
static PassOutcome callWithAnyReceiverWhereTypesAreLost(FunctionPassContext& pass)
{
    Function& function = pass.function;
    const BuiltinTable& table = pass.program.builtins();
    TypingContext typing { pass.program, function, pass.types };
    bool didChange = false;
    bool didChangeType = false;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Value& call = function.edit(id);
            if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || overloadTakesOperands(typing, call))
                continue;
            if (isCalledOnlyDirectly(table, table.at(call.overload()).builtin))
                continue;
            call.setBuiltinCallForm(BuiltinCallForm::AnyReceiver);
            call.setNeedsPristineReceiver(false);
            TypeID computed = resultTypeOf(pass.program, function, call, pass.types);
            didChangeType |= computed != call.type;
            call.type = computed;
            didChange = true;
        }
    }
    if (!didChange)
        return PassOutcome::Unchanged;

    pass.analyses.didChangeValues();
    if (didChangeType) {
        TypePropagation propagation(TypePropagationInputs { pass.program, pass.id, function, pass.analyses, pass.types, pass.build, &pass.typingMemo });
        propagation.restartAllValues();
        propagation.solveTypes();
        propagation.finish();
    }
    return PassOutcome::Changed;
}

static void restoreExactTypes(FunctionPassContext& pass, SelectionContext& context)
{
    std::span<const ValueID> pending = context.narrowedValues();
    if (pending.empty())
        return;

    TypePropagationInputs inputs { pass.program, pass.id, pass.function, pass.analyses, pass.types, pass.build, &pass.typingMemo };
    TypePropagation propagation(inputs);
    if (context.hasWidenedValue())
        propagation.restartAllValues();
    else
        propagation.restartDependentsOf(pending);
    propagation.solveTypes();
    TypePropagationOutcome outcome = propagation.finish();

    context.didRestoreTypes();
    if (outcome.didChangeControlFlow())
        context.didRenumberValues();
}

static bool returnsWhatTestHandsOver(const Function& function, const Block& from, const Target& target)
{
    const Block& to = function.at(target.block);
    return target.arguments.isEmpty() && target.proofs.size() == 1 && to.parameters.size() == 1 && to.values.isEmpty() && to.handler == from.handler
        && to.terminator.kind == TerminatorKind::Return && to.terminator.valueOperand() == to.parameters[0];
}

// A result is tested where it is returned, and the value is returned whatever the test says: the test tells nobody anything.
static bool returnTestedValues(Function& function)
{
    bool didChange = false;
    for (Block& block : function.editAllBlocks()) {
        const Terminator& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::TestAndBranch || block.values.isEmpty() || block.values.last() != terminator.conditionOperand())
            continue;
        const Value& test = function.at(terminator.conditionOperand());
        bool returnsEitherWay = test.opcode == Opcode::TypeTest && std::ranges::all_of(terminator.targets, [&](const Target& target) {
            return returnsWhatTestHandsOver(function, block, target);
        });
        if (!returnsEitherWay)
            continue;
        Terminator replacement = function.at(terminator.targets[0].block).terminator;
        replacement.setValueOperand(test.valueOperand());
        block.terminator = WTF::move(replacement);
        block.values.removeLast();
        didChange = true;
    }
    return didChange;
}

PassOutcome select(FunctionPassContext& pass)
{
    PassOutcome outcome = noteExpectedElements(pass.program, pass.function);
    outcome |= clearExpectedTypes(pass.function);
    outcome |= callWithAnyReceiverWhereTypesAreLost(pass);
    outcome |= chooseCodeShapes(pass, ChosenShapes::ChooseAgain);
    outcome |= makeDescriptorLiteralsBehindJoins(pass);

    SelectionContext context(pass);
    for (const SelectionRule& rule : selectionRules()) {
        if (!context.isEnabled(rule.id))
            continue;
        rule.apply(context);
        restoreExactTypes(pass, context);
    }
    if (context.didRewrite())
        outcome = PassOutcome::Changed;

    bool didReturnTestedValues = returnTestedValues(pass.function);
    if (didReturnTestedValues)
        outcome = PassOutcome::Changed;

    if (context.needsCompaction() || didReturnTestedValues) {
        compactFunction(pass.function);
        pass.analyses.didChangeControlFlow();
    }
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
