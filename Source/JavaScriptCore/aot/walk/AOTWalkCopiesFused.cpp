/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTNameClass.h"
#include "AOTOpcodeSets.h"
#include "AOTRepresentationRules.h"
#include "AOTTypeArena.h"
#include "AOTWalkTextBuilders.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t primitivesWithoutOwnPropertiesInCopiesFused = Primitive::UndefinedOrNull | Primitive::Boolean | Primitive::Number | Primitive::AnySymbol | Primitive::BigInt;

namespace {

enum class TakenByCopyInCopiesFused : uint8_t { Nothing, Name, SecondSource };

} // anonymous namespace

static bool namesValueInCopiesFused(const BodyValue& operation, ValueID named)
{
    bool names = false;
    operation.forEachOperand([&](ValueID operand, uint32_t) {
        names |= operand == named;
    });
    return names;
}

static bool isTakenByDefinitionOnCopyAloneInCopiesFused(const BodyIndex& index, ValueID made, ValueID copy)
{
    ValueID user = index.onlyUsingValue(made);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        made = user;
        user = index.onlyUsingValue(made);
    }
    if (!user || index.opcodeOf(user) != Opcode::PropDefine)
        return false;
    auto definition = index.value(user).as<Opcode::PropDefine>();
    return definition.objectOperand() == copy && definition.valueOperand() == made;
}

bool mayStandBehindCopy(const Walker& walker, const BodyIndex& index, const BodyValue& operation, ValueID copy)
{
    Opcode opcode = operation.opcode();
    if (namesValueInCopiesFused(operation, copy))
        return false;
    if (constantsInFormOfUses.contains(opcode))
        return true;
    if (opcode == Opcode::NewClosure)
        return walker.everyClosureStandsBehindCopies() || isTakenByDefinitionOnCopyAloneInCopiesFused(index, operation.id(), copy);
    if (opcode == Opcode::Convert) {
        DeclaredType declared = operation.as<Opcode::Convert>().declared();
        return !declared.has(PositionFlags::IsClaim) && !declared.has(PositionFlags::IsInt32Promise);
    }
    if (opcodesThatStateTypes.contains(opcode))
        return opcode == Opcode::Upcast;
    return declaredBoundInTable(opcode).isPure() && !operandsMayRunCode(opcode);
}

static bool keyMayBeDefinedWithCopyInCopiesFused(const Walker& walker, PropertyKeyID key)
{
    if (!key || liesInThread(key) || walker.types().at(key).kind != PropertyKeyKind::Text)
        return false;
    return !walker.program().isIndexLike(key) && walker.program().nameClassOf(key) != NameClass::Proto;
}

static TakenByCopyInCopiesFused whatCopyTakesOfInCopiesFused(const Walker& walker, const BodyValue& operation, ValueID copy)
{
    if (operation.opcode() == Opcode::PropDefine) {
        auto definition = operation.as<Opcode::PropDefine>();
        bool isTaken = definition.objectOperand() == copy && definition.valueOperand() != copy && keyMayBeDefinedWithCopyInCopiesFused(walker, definition.key());
        return isTaken ? TakenByCopyInCopiesFused::Name : TakenByCopyInCopiesFused::Nothing;
    }
    if (operation.opcode() == Opcode::CopyDataProperties) {
        auto spread = operation.as<Opcode::CopyDataProperties>();
        bool isTaken = spread.targetOperand() == copy && spread.sourceOperand() != copy && spread.excludedKeys().isEmpty() && spread.excludedOperands().empty();
        return isTaken ? TakenByCopyInCopiesFused::SecondSource : TakenByCopyInCopiesFused::Nothing;
    }
    return TakenByCopyInCopiesFused::Nothing;
}

static bool isValueOfProgramInCopiesFused(const Walker& walker, Reference value)
{
    if (!value || !value.isRecord())
        return false;
    Type type = walker.types().at(walker.typeOf(value));
    return type.isJavaScriptValue() && !(type.primitives & (Primitive::Absent | Primitive::Unspecified));
}

bool buildCopyWithWhatFollowsOfItsOpcodes(Walker& walker, const BodyValue& copy)
{
    if (copy.opcode() != Opcode::CopyObject || !walker.has(Mechanism::TypedOperations))
        return false;
    auto operation = copy.as<Opcode::CopyObject>();
    if (operation.assigns() || !operation.excludedKeys().isEmpty() || !operation.excludedOperands().empty())
        return false;
    FunctionID function = walker.unit().function;
    if (!walker.isInLoop() && walker.program().at(function).isModuleBody())
        return false;
    Reference source = walker.referenceOf(operation.sourceOperand());
    if (!isValueOfProgramInCopiesFused(walker, source) || walker.types().at(walker.typeOf(source)).hasOnlyLayouts())
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(copy.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t end = walker.endOfStretchBeingWalked();
    uint32_t placeOfCopy = copy.id().index() - firstValue;

    TakenByCopyInCopiesFused firstTaken = TakenByCopyInCopiesFused::Nothing;
    for (uint32_t place = placeOfCopy + 1; place < end && firstTaken == TakenByCopyInCopiesFused::Nothing; ++place) {
        BodyValue next = index.value(ValueID(firstValue + place));
        firstTaken = whatCopyTakesOfInCopiesFused(walker, next, copy.id());
        if (firstTaken == TakenByCopyInCopiesFused::Nothing && !mayStandBehindCopy(walker, index, next, copy.id()))
            return false;
    }
    if (firstTaken == TakenByCopyInCopiesFused::Nothing)
        return false;
    Draft written = walker.draftOf(copy);
    if (walker.hasFailed())
        return true;

    Vector<PropertyKeyID, 8> keys;
    Vector<Reference, 8> values;
    Reference secondSource;
    Origin originOfSecondSource { };
    Vector<ValueID, 8> taken;
    uint32_t walkedUpTo = placeOfCopy + 1;
    for (uint32_t place = placeOfCopy + 1; place < end && !secondSource; ++place) {
        BodyValue next = index.value(ValueID(firstValue + place));
        TakenByCopyInCopiesFused whatIsTaken = whatCopyTakesOfInCopiesFused(walker, next, copy.id());
        if (whatIsTaken == TakenByCopyInCopiesFused::Nothing) {
            if (!mayStandBehindCopy(walker, index, next, copy.id()))
                break;
            continue;
        }
        if (whatIsTaken == TakenByCopyInCopiesFused::Name ? keys.contains(next.as<Opcode::PropDefine>().key()) : !keys.isEmpty())
            break;

        walker.walkStretch(block.id(), walkedUpTo, place);
        walkedUpTo = place;
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        std::optional<MaterializedStringsScope> strings;
        if (walker.hasStringsInBuilders()) {
            strings.emplace(walker, next);
            if (walker.isUnreachable())
                return true;
        }

        if (whatIsTaken == TakenByCopyInCopiesFused::Name) {
            auto definition = next.as<Opcode::PropDefine>();
            Reference value = walker.referenceOf(definition.valueOperand());
            if (!isValueOfProgramInCopiesFused(walker, value))
                break;
            keys.append(definition.key());
            values.append(value);
        } else {
            auto spread = next.as<Opcode::CopyDataProperties>();
            Reference second = walker.referenceOf(spread.sourceOperand());
            if (!isValueOfProgramInCopiesFused(walker, second))
                break;
            if (!walker.types().at(walker.typeOf(second)).hasOnlyPrimitives(primitivesWithoutOwnPropertiesInCopiesFused)) {
                if (ownKeysInSight(walker, second) || planOwnKeyWalk(walker, spread.sourceOperand(), second))
                    break;
                secondSource = second;
                originOfSecondSource = walker.originOf(next);
            }
        }
        taken.append(next.id());
        walkedUpTo = place + 1;
    }

    Draft fused(keys.isEmpty() ? (secondSource ? Opcode::CopyTwoObjects : Opcode::CopyObject) : Opcode::CopyObjectAndDefine, written.origin());
    fused.inputValueGiven = copy.id();
    fused.expected = written.expected;
    walker.chooseConservativeFields(fused);
    fused.setSourceOperand(source);
    if (!keys.isEmpty()) {
        fused.setDefinedKeys(keys.span());
        fused.setValuesOperands(values.span());
    } else if (secondSource) {
        fused.setSecondSourceOperand(secondSource);
        fused.setSecondOrigin(originOfSecondSource);
    }
    if (Reference made = walker.build(WTF::move(fused)))
        walker.bind(copy.id(), made);

    for (ValueID part : taken) {
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), part))
            walker.leaveAnythingAt(callee);
    }
    walker.walkStretch(block.id(), walkedUpTo, walkedUpTo);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
