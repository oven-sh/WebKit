/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// TypedOperations, a copy and what is written behind it: `{ ...source, name: value }` and `{ ...first, ...second }`, where no type says which names the object will have, are one operation each.
// The getters of the source run in front of every definition, as written: the operation copies first. Nobody has the copy until it is built, so nobody can tell when a name was defined on it.
// It keeps nothing from one operation to the next: the copy looks ahead in its block and walks the row to its end. At a suspension and at a throw: nothing that has an effect stands between two of the row.
// Withheld: the copy and each definition are copied in their closed forms.

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

// Of the primitives only a string has own properties.
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

// It is walked in front of the copy, which may run any code: so it has no effect at all, and nothing is built for the copy yet.
static bool mayStandBehindCopyInCopiesFused(const BodyValue& operation, ValueID copy)
{
    Opcode opcode = operation.opcode();
    if (namesValueInCopiesFused(operation, copy))
        return false;
    if (constantsInFormOfUses.contains(opcode))
        return true;
    // A value that enters a written position may be tested there.
    if (opcodesThatStateTypes.contains(opcode))
        return opcode == Opcode::Upcast;
    return declaredBoundInTable(opcode).isPure() && !operandsMayRunCode(opcode);
}

// The run time takes the number of an identifier. An index is an element, and `__proto__` is a name like any other only for a definition by itself.
static bool keyMayBeDefinedWithCopyInCopiesFused(const Walker& walker, PropertyKeyID key)
{
    if (!key || liesInThread(key) || walker.types().at(key).kind != PropertyKeyKind::Text)
        return false;
    return !walker.program().isIndexLike(key) && walker.program().nameClassOf(key) != NameClass::Proto;
}

// By the body alone: what the operation would hand to the copy.
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

// The stub and the run time look at each word as it comes.
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
    // The code of the operation keeps the copy and the definitions by name as one arm and adds the arm by operation: it pays where the code comes again.
    FunctionID function = walker.unit().function;
    if (!walker.isInLoop() && (walker.facts().runsAtMostOnce(function) || walker.program().at(function).isModuleBody()))
        return false;
    // Only the copy of an ordinary object gets its names from the stub, or is made together with the copy of a second source.
    // An object of the program's layouts is copied by the stub of the plain copy, and each definition behind it has a cache of its own.
    Reference source = walker.referenceOf(operation.sourceOperand());
    if (!isValueOfProgramInCopiesFused(walker, source) || walker.types().at(walker.typeOf(source)).hasOnlyLayouts())
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BodyBlock block = index.block(index.blockOf(copy.id()));
    uint32_t firstValue = block.firstValue().index();
    uint32_t end = walker.endOfStretchBeingWalked();
    uint32_t placeOfCopy = copy.id().index() - firstValue;

    // Nothing is built until the body shows that something is taken.
    TakenByCopyInCopiesFused firstTaken = TakenByCopyInCopiesFused::Nothing;
    for (uint32_t place = placeOfCopy + 1; place < end && firstTaken == TakenByCopyInCopiesFused::Nothing; ++place) {
        BodyValue next = index.value(ValueID(firstValue + place));
        firstTaken = whatCopyTakesOfInCopiesFused(walker, next, copy.id());
        if (firstTaken == TakenByCopyInCopiesFused::Nothing && !mayStandBehindCopyInCopiesFused(next, copy.id()))
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
    // Among the operations of the block: those in front of it are walked, or are part of the one operation.
    uint32_t walkedUpTo = placeOfCopy + 1;
    for (uint32_t place = placeOfCopy + 1; place < end && !secondSource; ++place) {
        BodyValue next = index.value(ValueID(firstValue + place));
        TakenByCopyInCopiesFused whatIsTaken = whatCopyTakesOfInCopiesFused(walker, next, copy.id());
        if (whatIsTaken == TakenByCopyInCopiesFused::Nothing) {
            if (!mayStandBehindCopyInCopiesFused(next, copy.id()))
                break;
            continue;
        }
        // `{ ...source, a: 1, a: 2 }`: the table of the run time has one place for a name. `{ ...first, a, ...second }`: the names come in front of the keys of the second.
        if (whatIsTaken == TakenByCopyInCopiesFused::Name ? keys.contains(next.as<Opcode::PropDefine>().key()) : !keys.isEmpty())
            break;

        // The type of a value that stands in between is known only when it is built.
        walker.walkStretch(block.id(), walkedUpTo, place);
        walkedUpTo = place;
        if (walker.isUnreachable() || walker.hasFailed())
            return true;
        // `{ ...source, text: line }`, where `line` grows in a loop: what is taken is not the operation being walked, and the walk stands where it is written. It gets a string as any reader does.
        std::optional<StringsInPlaceOfTextsInBuilders> strings;
        if (walker.hasTextsInStringBuilders()) {
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
            // `{ ...first, ...undefined, ...second }`: such a spread copies nothing, and the copy goes on to what is written behind it.
            if (!walker.types().at(walker.typeOf(second)).hasOnlyPrimitives(primitivesWithoutOwnPropertiesInCopiesFused)) {
                // A source whose keys are known by name is read field by field into the copy as it is built.
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

    // A call that Link lists at an operation of the row is told as Walker::walkOperation() tells it of an operation that it walks.
    for (ValueID part : taken) {
        if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), part))
            walker.arrivals().leaveAnything(callee);
    }
    // The row is walked.
    walker.walkStretch(block.id(), walkedUpTo, walkedUpTo);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
