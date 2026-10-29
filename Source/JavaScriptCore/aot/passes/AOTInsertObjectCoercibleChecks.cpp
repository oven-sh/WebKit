/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTProgramPasses.h"

#if ENABLE(AOT)

#include "AOTRewrite.h"
#include "AOTTypePropagation.h"
#include <algorithm>
#include <optional>
#include <utility>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readAccesses { Opcode::PropGet, Opcode::ElemGet };
static constexpr OpcodeSet writeAccesses { Opcode::PropSet, Opcode::ElemSet };
static constexpr OpcodeSet iterationAccesses { Opcode::IteratorOpen };

static_assert((readAccesses | writeAccesses | iterationAccesses | OpcodeSet { Opcode::RequireObjectCoercible }) == objectCoercibleCheckOpcodes);

static constexpr OpcodeSet checkedNamedAccesses { Opcode::PropGet, Opcode::PropSet };
static constexpr OpcodeSet checkedKeyedAccesses { Opcode::ElemGet, Opcode::ElemSet };

static constexpr uint32_t checkedReceiverOperand = OperandIndex::PropGet::object;
static constexpr uint32_t keyOperand = OperandIndex::ElemGet::property;

static_assert(OperandIndex::PropSet::object == checkedReceiverOperand);
static_assert(OperandIndex::ElemGet::object == checkedReceiverOperand);
static_assert(OperandIndex::ElemSet::object == checkedReceiverOperand);
static_assert(OperandIndex::IteratorOpen::iterable == checkedReceiverOperand);
static_assert(OperandIndex::ElemSet::property == keyOperand);

static std::optional<NullishAccess> nullishAccessOf(Opcode opcode)
{
    if (readAccesses.contains(opcode))
        return NullishAccess::Read;
    if (writeAccesses.contains(opcode))
        return NullishAccess::Write;
    if (iterationAccesses.contains(opcode))
        return NullishAccess::Iterate;
    return std::nullopt;
}

static bool isNarrowedByCheck(const Type& type)
{
    return type.mayBeUndefinedOrNull() && !type.isUndefinedOrNull();
}

// Another pass puts such a check where a read it resolved threw.
static bool isNarrowingCheck(Rewrite& rewrite, const Value& value)
{
    return value.opcode == Opcode::RequireObjectCoercible && isNarrowedByCheck(rewrite.typeOf(value.valueOperand()));
}

static bool looksAtReceiverFirst(Rewrite& rewrite, const Value& access)
{
    if (checkedNamedAccesses.contains(access.opcode) && access.isFreeName())
        return false;
    return !checkedKeyedAccesses.contains(access.opcode) || !rewrite.typeOf(access.operand(keyOperand)).hasObjects();
}

// `Object.getOwnPropertyDescriptor(o, k).value`: the descriptor is some object or undefined. Behind the check SinkAllocations has an object whose fields the call can deliver by themselves.
static bool mayBeTakenApart(Rewrite& rewrite, ValueID receiver)
{
    const Value& definition = rewrite.function.at(sameObjectThroughCasts(rewrite.function, receiver));
    if (definition.opcode != Opcode::CallBuiltin || definition.builtinCallForm() != BuiltinCallForm::Whole || !isNarrowedByCheck(rewrite.typeOf(receiver)))
        return false;
    return rewrite.program.builtins().at(definition.overload()).code.fieldsForm.has_value();
}

static bool requiresCheck(Rewrite& rewrite, const Value& access)
{
    if (!nullishAccessOf(access.opcode))
        return false;
    ValueID receiver = access.operand(checkedReceiverOperand);
    bool needsCheck = isNarrowedByCheck(rewrite.typeOf(receiver)) && !rewrite.typeOf(receiver).anyObject;
    return (needsCheck || mayBeTakenApart(rewrite, receiver)) && looksAtReceiverFirst(rewrite, access);
}

static Value makeCheckBefore(Rewrite& rewrite, const Value& access, ValueID receiver)
{
    Value check = IRBuilder::make(Opcode::RequireObjectCoercible, { receiver });
    check.setNullishAccess(*nullishAccessOf(access.opcode));
    const PropertyKey* key = checkedNamedAccesses.contains(access.opcode) ? &rewrite.program.at(access.key()) : nullptr;
    check.setName(key && key->kind == PropertyKeyKind::Text ? key->text : StringID());
    return check;
}

// The first thing that somebody could see on the way on which the test fails, where that is an access to the tested value.
static ValueID findAccessBehindFailedTest(Rewrite& rewrite, const Terminator& branch)
{
    constexpr unsigned maxBlocksOnTheWay = 4;
    const Function& function = rewrite.function;
    Vector<ValueID, 4> namesOfSubject { sameObjectThroughCasts(function, function.at(branch.conditionOperand()).valueOperand()).valueInFrontOfFacts() };
    Vector<BlockID, maxBlocksOnTheWay> way;
    const Target* target = &branch.notTaken();
    while (way.size() < maxBlocksOnTheWay && !way.contains(target->block)) {
        way.append(target->block);
        const Block& block = function.at(target->block);
        if (block.handler)
            return { };
        for (uint32_t index = 0; index < block.parameters.size(); ++index) {
            if (index >= target->arguments.size() || namesOfSubject.contains(sameObjectThroughCasts(function, target->arguments[index]).valueInFrontOfFacts()))
                namesOfSubject.append(block.parameters[index]);
        }
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            bool throwsForNullishReceiver = (readAccesses.contains(value.opcode) || writeAccesses.contains(value.opcode)) && looksAtReceiverFirst(rewrite, value);
            if (throwsForNullishReceiver && namesOfSubject.contains(sameObjectThroughCasts(function, value.operand(checkedReceiverOperand)).valueInFrontOfFacts()))
                return id;
            bool isInvisible = isCastWithoutTest(value.opcode) || (effectRuleOf(value.opcode) == EffectRule::Declared && declaredBoundOf(value.opcode).isRemovableWhenUnused());
            if (!isInvisible)
                return { };
        }
        if (block.terminator.kind != TerminatorKind::Jump)
            return { };
        target = &block.terminator.next();
    }
    return { };
}

// `p.name.length`: the field holds a string, undefined or null, and the read throws for the last two. The test that tells them from the string is the check, which costs nothing where the read goes through the value.
static bool replaceTestsThatOnlyNullishValuesFail(FunctionPassContext& context)
{
    Rewrite rewrite(context);
    Vector<std::pair<BlockID, ValueID>, 4> replacedTests;
    for (BlockID id : rewrite.blocks()) {
        const Block& block = rewrite.at(id);
        if (block.handler || !block.terminator.isConditionalBranch())
            continue;
        const Value& test = rewrite.at(block.terminator.conditionOperand());
        if (test.opcode != Opcode::TypeTest || rewrite.program.at(test.testedType()).mayBeUndefinedOrNull())
            continue;
        if (!rewrite.typeOf(test.valueOperand()).without(rewrite.program.at(test.testedType()), context.types).isUndefinedOrNull())
            continue;
        if (ValueID access = findAccessBehindFailedTest(rewrite, block.terminator))
            replacedTests.append({ id, access });
    }
    if (replacedTests.isEmpty() || waitsForGenericVersion(rewrite.program, rewrite.function))
        return false;

    IRBuilder& builder = rewrite.builder();
    for (auto [block, accessID] : replacedTests) {
        const Value& access = rewrite.at(accessID);
        Terminator& terminator = rewrite.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        Value check = makeCheckBefore(rewrite, access, rewrite.at(terminator.conditionOperand()).valueOperand());
        Target taken = terminator.taken();
        Arguments arguments = WTF::move(taken.arguments);

        builder.setOrigin(access.origin());
        terminator = Terminator { };
        builder.moveToEndOf(block);
        ValueID passed = builder.append(WTF::move(check));
        for (size_t result = 0; result < taken.proofs.size(); ++result)
            arguments.append(passed);
        builder.emitJump(taken.block, WTF::move(arguments));
    }
    builder.emptyUnreachableBlocks();
    return true;
}

static PassOutcome insertChecksBeforeAccesses(FunctionPassContext& context)
{
    using CheckID = ID<struct CheckIDTag>;
    struct Check {
        ProgramPoint point;
        ValueID access;
        ValueID result;
    };

    Rewrite rewrite(context);
    bool requiresAnyCheck = std::ranges::any_of(rewrite.function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            const Value& value = rewrite.at(id);
            return objectCoercibleCheckOpcodes.contains(value.opcode) && (requiresCheck(rewrite, value) || isNarrowingCheck(rewrite, value));
        });
    });
    if (!requiresAnyCheck)
        return PassOutcome::Unchanged;

    const Dominators& dominators = rewrite.analyses.dominators();
    Vector<Check, 8> checks;
    Vector<std::pair<Use, CheckID>, 16> replacedUses;
    Vector<CheckID> activeChecks(rewrite.function.values.size());
    Vector<std::pair<ValueID, CheckID>, 8> checkedReceivers;
    Vector<size_t, 16> scopes;

    auto activeCheckOf = [&](ValueID value, BlockID block) -> CheckID {
        CheckID check = activeChecks[value.index()];
        if (!check)
            return { };
        // A covered block dominates its handler and whatever the handler leads to, and its checks do not.
        BlockID checkingBlock = checks[check.index()].point.block;
        return checkingBlock == block || dominators.terminatorDominates(checkingBlock, block) ? check : CheckID();
    };
    // Does nothing unless some check is active.
    auto visitUse = [&](ValueID used, const Use& use) {
        if (CheckID check = activeCheckOf(used, use.block))
            replacedUses.append({ use, check });
    };
    auto visitArguments = [&](Use::Kind kind, BlockID block, const Target& target, uint32_t targetIndex) {
        for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
            if (ValueID argument = target.arguments[argumentIndex])
                visitUse(argument, Use { kind, block, { }, argumentIndex, targetIndex });
        }
    };

    dominators.walkTree([&](BlockID blockID) {
        scopes.append(checkedReceivers.size());
        const Block& block = rewrite.at(blockID);
        if (!checkedReceivers.isEmpty()) {
            if (block.handler)
                visitArguments(Use::Kind::HandlerArgument, blockID, *block.handler, 0);
            if (block.subject)
                visitUse(block.subject, Use { Use::Kind::BlockSubject, blockID, { }, 0, 0 });
        }

        for (uint32_t index = 0; index < block.values.size(); ++index) {
            ValueID id = block.values[index];
            const Value& value = rewrite.at(id);
            if (!checkedReceivers.isEmpty()) {
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    visitUse(operand, Use { Use::Kind::Operand, blockID, id, operandIndex, 0 });
                });
            }
            if (!objectCoercibleCheckOpcodes.contains(value.opcode))
                continue;
            bool isCheck = isNarrowingCheck(rewrite, value);
            if (!isCheck && !requiresCheck(rewrite, value))
                continue;
            ValueID receiver = isCheck ? value.valueOperand() : value.operand(checkedReceiverOperand);
            if (activeCheckOf(receiver, blockID))
                continue;
            CheckID check(checks.size());
            checks.append({ { blockID, index }, id, isCheck ? id : ValueID() });
            checkedReceivers.append({ receiver, std::exchange(activeChecks[receiver.index()], check) });
            if (!isCheck)
                replacedUses.append({ Use { Use::Kind::Operand, blockID, id, checkedReceiverOperand, 0 }, check });
        }

        if (checkedReceivers.isEmpty())
            return;
        block.terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            visitUse(operand, Use { Use::Kind::TerminatorOperand, blockID, { }, operandIndex, 0 });
        });
        for (uint32_t targetIndex = 0; targetIndex < block.terminator.targets.size(); ++targetIndex)
            visitArguments(Use::Kind::EdgeArgument, blockID, block.terminator.targets[targetIndex], targetIndex);
    }, [&](BlockID) {
        size_t numberOfOuterReceivers = scopes.takeLast();
        while (checkedReceivers.size() > numberOfOuterReceivers) {
            auto [receiver, replacedCheck] = checkedReceivers.takeLast();
            activeChecks[receiver.index()] = replacedCheck;
        }
    });
    if (replacedUses.isEmpty())
        return PassOutcome::Unchanged;

    for (Check& check : checks) {
        const Value& access = rewrite.at(check.access);
        if (!check.result)
            check.result = rewrite.insertBefore(check.point, access.origin(), makeCheckBefore(rewrite, access, access.operand(checkedReceiverOperand)));
        rewrite.retype(check.result);
    }
    for (const auto& [use, check] : replacedUses)
        rewrite.replaceUse(use, checks[check.index()].result);
    return rewrite.commit();
}

PassOutcome insertObjectCoercibleChecks(FunctionPassContext& context)
{
    if (context.function.role == FunctionRole::GenericVersion && waitsForGenericVersion(context.program, context.function))
        return PassOutcome::Unchanged;
    bool replacedTests = replaceTestsThatOnlyNullishValuesFail(context);
    PassOutcome outcome = insertChecksBeforeAccesses(context);
    if (!replacedTests)
        return outcome;
    context.keptAnalyses = { };
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
