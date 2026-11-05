/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperatorEmitter.h"

#if ENABLE(AOT)

#include "AOTConversionEmitter.h"
#include "AOTRepresentationRules.h"
#include "AOTRuntimeCallEmitter.h"
#include "AOTStringEmitter.h"
#include "AOTStubsValues.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "JSCJSValueInlines.h"
#include "JSType.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint64_t bitsOfQuietNaN = 0x7ffull << 52 | 1ull << 51;
static_assert((undefinedDouble & bitsOfQuietNaN) == bitsOfQuietNaN && (nullDouble & bitsOfQuietNaN) == bitsOfQuietNaN);

namespace {

class EqualityEmitter final : public OperationEmitter {
public:
    explicit EqualityEmitter(CodeGenerator& generator)
        : OperationEmitter(generator)
    {
    }

    Condition callEqualStub(const OperationContext& context, bool isStrict, Boxed left, Boxed right, const CallOptions& options = { })
    {
        return Condition(m_out.castToInt32(callStub(context, AOT::equal(isStrict), { left.value(), right.value() }, options).first));
    }

    bool comparesOnlyNumbers(const ValueContext& value)
    {
        Vector<TypeID, 2> provenTypes;
        Vector<Representation, 2> representations;
        for (ValueID operand : value->operands()) {
            provenTypes.append(typeIDOf(operand));
            representations.append(representationOf(operand));
        }
        RepresentationContext context { m_program, m_plan, types(), m_unit };
        return isEqualityOfNumbers(context, value.value(), OperandForms { provenTypes.span(), representations.span() });
    }

    void noteFormAgainstWrittenText(const ValueContext& value, StringID literal)
    {
        constexpr StateOfKnowledge inPlace = StateOfKnowledge::LiteralComparedInPlace;
        static_assert(formByLoopAlone(Opcode::StrictEq, inPlace, true).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, true).form);
        static_assert(formByLoopAlone(Opcode::StrictEq, inPlace, false).form == formByLoopAlone(Opcode::ValueEqualsLiteral, StateOfKnowledge::Always, false).form);
        static_assert(formByLoopAlone(Opcode::Eq, inPlace, true).form == formByLoopAlone(Opcode::StrictEq, inPlace, true).form && formByLoopAlone(Opcode::Eq, inPlace, false).form == formByLoopAlone(Opcode::StrictEq, inPlace, false).form);
        if (!m_records.formsSaid.areNoted())
            return;
        StringEmitter& strings = emitter<StringEmitter>();
        String text = strings.textOf(literal);
        if (text.isEmpty()) {
            m_out.noteForm(Form::Open, StateOfKnowledge::EmptyLiteral);
            return;
        }
        if (!text.is8Bit() || !StringEmitter::canCompareInline(text)) {
            m_out.noteForm(Form::Closed, StateOfKnowledge::LiteralComparedByStub);
            return;
        }
        FormAtPlace named = formByLoopAlone(value.opcode(), inPlace, generator().isInLoop(value.block()));
        m_out.noteForm(named.form, inPlace, named.place);
    }

    Condition looseEqualityOnBoxed(const ValueContext& value)
    {
        m_out.noteForm(Form::Closed, stateOf(value->form()));
        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        Boxed x = conversions.box(value, get(left), typeOf(left));
        Boxed y = conversions.box(value, get(right), typeOf(right));
        return callEqualStub(value, false, x, y);
    }

    Condition equalityOnIntegers(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (formOfOperands(value)) {
        case Representation::Int32: {
            Word32 x = in<Word32>(left);
            return equal(x, in<Word32>(right));
        }
        case Representation::Int64: {
            Integer x = in<Integer>(left);
            return equal(x, in<Integer>(right));
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnNumbers(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if (isStrict && get(left).representation == Representation::OptionalInt64 && get(right).representation == Representation::OptionalInt64) {
            ConversionEmitter& conversions = emitter<ConversionEmitter>();
            Integer leftBits = conversions.bitsOf(get(left));
            return equal(leftBits, conversions.bitsOf(get(right)));
        }
        uint16_t leftPrimitives = typeOf(left).primitives;
        uint16_t rightPrimitives = typeOf(right).primitives;
        bool mayBothBeNumbers = (leftPrimitives & Primitive::Number) && (rightPrimitives & Primitive::Number);
        bool mayBothBeNullish = (leftPrimitives & Primitive::UndefinedOrNull) && (rightPrimitives & Primitive::UndefinedOrNull);
        bool mayBeSameNullish = leftPrimitives & rightPrimitives & Primitive::UndefinedOrNull;
        bool mayBeEqualNullish = isStrict ? mayBeSameNullish : mayBothBeNullish;

        Condition areEqualNumbers;
        if (mayBothBeNumbers)
            areEqualNumbers = equal(numberOrNaN(value, left), numberOrNaN(value, right));
        if (!mayBeEqualNullish)
            return areEqualNumbers ? areEqualNumbers : boolean(false);

        ConversionEmitter& conversions = emitter<ConversionEmitter>();
        Condition areEqualNullish;
        if (isStrict) {
            if (get(left).representation != get(right).representation)
                return { };
            Integer leftBits = conversions.bitsOf(get(left));
            Integer rightBits = conversions.bitsOf(get(right));
            areEqualNullish = equal(leftBits, rightBits);
            if (mayBothBeNumbers)
                areEqualNullish = logicalAnd(areEqualNullish, conversions.isMarker(get(left)));
            return logicalOr(areEqualNumbers, areEqualNullish);
        }

        for (ValueID id : { left, right }) {
            if (!(typeOf(id).primitives & Primitive::Number))
                continue;
            Condition isNullish = conversions.isMarker(get(id));
            areEqualNullish = areEqualNullish ? logicalAnd(areEqualNullish, isNullish) : isNullish;
        }
        if (!areEqualNullish)
            return boolean(true);
        return logicalOr(areEqualNumbers, areEqualNullish);
    }

    Condition equalityOnBits(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (formOfOperands(value)) {
        case Representation::Cell: {
            CellPointer x = in<CellPointer>(left);
            return equal(x, in<CellPointer>(right));
        }
        case Representation::JSValue: {
            Boxed x = in<Boxed>(left);
            return equalityOnBoxedBits(value, x, in<Boxed>(right), isStrict);
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell32:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnStrings(const ValueContext& value, bool isStrict)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        switch (formOfOperands(value)) {
        case Representation::Cell:
        case Representation::JSValue: {
            if (!typeOf(left).isAnyString() || !typeOf(right).isAnyString())
                return equalityOnNullableStrings(value, isStrict);
            for (auto [written, other] : { std::pair { right, left }, std::pair { left, right } }) {
                const Operation& definition = operationAt(definitionOf(written));
                if (definition.opcode == Opcode::String) {
                    noteFormAgainstWrittenText(value, definition.string());
                    return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::String, in<CellPointer>(other).value(), definition.string(), std::nullopt, true);
                }
            }
            m_out.noteForm(Form::Closed, stateOf(value->form()));
            CellPointer x = in<CellPointer>(left);
            return emitter<StringEmitter>().areEqual(value, x, in<CellPointer>(right));
        }
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::Double:
        case Representation::OptionalInt64:
        case Representation::OptionalDouble:
        case Representation::Cell32:
        case Representation::RawWord:
            break;
        }
        return { };
    }

    Condition equalityOnNumberAndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isNumber);
        if (!operands)
            return { };
        m_out.noteForm(Form::Open, StateOfKnowledge::NumberAndValue);
        Number number = in<Number>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        Condition otherIsNumber = isNumber(other);
        return logicalAnd(otherIsNumber, equal(number, numberToDouble(other)));
    }

    bool branchOnEqualityOfInt32AndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = value.isBranchCondition() ? mixedOperandsOf(value, &Type::isInt32) : std::nullopt;
        if (!operands)
            return false;
        m_out.noteForm(Form::Open, StateOfKnowledge::Int32AndValue);
        BranchTargets targets = value.branchTargets();
        Word32 expected = in<Word32>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        LBasicBlock isInt32OfSomeValue = newBlock();
        LBasicBlock isNotInt32 = newBlock();
        branch(isInt32(other), { normalEdge(isInt32OfSomeValue), rareEdge(isNotInt32) });
        m_out.appendTo(isInt32OfSomeValue);
        branch(equal(unboxInt32(other), expected), targets);
        m_out.appendTo(isNotInt32);
        if (expected.value()->hasInt32() && expected.value()->asInt32())
            branch(equal(other, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(expected.value()->asInt32())))), targets);
        else {
            Condition otherIsNumber = isNumber(other);
            Number otherNumber = unboxDouble(other);
            branch(logicalAnd(otherIsNumber, equal(otherNumber, toNumber(expected))), targets);
        }
        return true;
    }

    Condition equalityOnInt32AndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isInt32);
        if (!operands)
            return { };
        m_out.noteForm(Form::Open, StateOfKnowledge::Int32AndValue);
        Word32 expected = in<Word32>(operands->typed);
        Boxed other = in<Boxed>(operands->boxed);
        LBasicBlock isSameInt32 = newBlock();
        LBasicBlock isOtherInt32 = newBlock();
        LBasicBlock isInt32OfSomeValue = newBlock();
        LBasicBlock isNotInt32 = newBlock();
        Join<Condition> join(m_out);
        branch(isInt32(other), { normalEdge(isInt32OfSomeValue), rareEdge(isNotInt32) });
        m_out.appendTo(isInt32OfSomeValue);
        branch(equal(unboxInt32(other), expected), { unlikelyEdge(isSameInt32), normalEdge(isOtherInt32) });
        m_out.appendTo(isSameInt32);
        join.add(boolean(true));
        m_out.appendTo(isOtherInt32);
        join.add(boolean(false));
        m_out.appendTo(isNotInt32);
        if (expected.value()->hasInt32() && expected.value()->asInt32())
            join.add(equal(other, boxedConstant(JSValue(JSValue::EncodeAsDouble, static_cast<double>(expected.value()->asInt32())))));
        else {
            Condition otherIsNumber = isNumber(other);
            Number otherNumber = unboxDouble(other);
            join.add(logicalAnd(otherIsNumber, equal(otherNumber, toNumber(expected))));
        }
        return join.finish();
    }

    Condition equalityOnStringAndValue(const ValueContext& value)
    {
        std::optional<MixedOperands> operands = mixedOperandsOf(value, &Type::isAnyString);
        if (!operands)
            return { };
        auto [typed, boxed] = *operands;
        if (const Operation& definition = operationAt(definitionOf(typed)); definition.opcode == Opcode::String) {
            const Type& type = typeOf(boxed);
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
            noteFormAgainstWrittenText(value, definition.string());
            return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::Value, in<Boxed>(boxed).value(), definition.string(), std::nullopt, everyCellIsString);
        }
        m_out.noteForm(Form::Closed, StateOfKnowledge::StringAndValue);
        Boxed string = boxCell(in<CellPointer>(typed));
        return callEqualStub(value, true, string, in<Boxed>(boxed));
    }

    Condition equalityOnValues(const ValueContext& value, bool isStrict)
    {
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
        return callEqualStub(value, isStrict, x, y);
    }

    void generateStrictEqualityOnValues(const ValueContext& value)
    {
        Boxed x = in<Boxed>(value->leftOperand());
        Boxed y = in<Boxed>(value->rightOperand());
        if (value->leftOperand() == value->rightOperand()) {
            m_out.noteForm(Form::Open, StateOfKnowledge::ValueAndItself);
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock isNoInt32 = newBlock();
                LBasicBlock isDouble = newBlock();
                m_out.branch(isInt32(x).value(), targets.taken, normalEdge(isNoInt32));
                m_out.appendTo(isNoInt32);
                m_out.branch(isNumber(x).value(), normalEdge(isDouble), targets.taken);
                m_out.appendTo(isDouble);
                Number number = unboxDouble(x);
                branch(equal(number, number), targets);
            });
            return;
        }
        bool asksBothTagsInLine = value->expectsInt32s() && generator().code().at(value.block()).frequency != BlockFrequency::Rare;
        if (asksBothTagsInLine && !bitsDecide(kindsComparedBy(value)) && !generator().takesOpenForm(value, StateOfKnowledge::Generic)) {
            m_out.noteForm(Form::Closed, StateOfKnowledge::Generic);
            defineTestByBranching(value, [&](BranchTargets targets) {
                LBasicBlock areInt32s = newBlock();
                LBasicBlock callsStub = newBlock();
                m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), normalEdge(areInt32s), rareEdge(callsStub));
                m_out.appendTo(areInt32s);
                branch(equal(x, y), targets);
                m_out.appendTo(callsStub);
                LValue operands[] = { x.value(), y.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, targets);
            });
            return;
        }
        if (!bitsDecide(kindsComparedBy(value)) && !generator().takesOpenForm(value, StateOfKnowledge::Generic)) {
            if (value.isBranchCondition()) {
                LValue operands[] = { x.value(), y.value() };
                emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, value.branchTargets());
                return;
            }
            defineTest(value, callEqualStub(value, true, x, y));
            return;
        }
        if (value.isBranchCondition()) {
            branchOnEqualityOfValues(value, x, y, value.branchTargets());
            return;
        }
        defineTest(value, areValuesEqual(value, x, y));
    }

private:
    struct MixedOperands {
        ValueID typed;
        ValueID boxed;
    };

    std::optional<MixedOperands> mixedOperandsOf(const ValueContext& value, bool (Type::*isTyped)() const)
    {
        ValueID left = value->leftOperand();
        ValueID right = value->rightOperand();
        if ((typeOf(left).*isTyped)())
            return MixedOperands { left, right };
        if ((typeOf(right).*isTyped)())
            return MixedOperands { right, left };
        return std::nullopt;
    }

    Representation formOfOperands(const ValueContext& value)
    {
        ValueID left = value->leftOperand();
        return representationOf(constantsInFormOfUses.contains(opcodeOf(left)) ? value->rightOperand() : left);
    }

    Number numberOrNaN(const ValueContext& value, ValueID id)
    {
        switch (representationOf(id)) {
        case Representation::Double:
            return in<Number>(id);
        case Representation::OptionalDouble:
            return emitter<ConversionEmitter>().withoutMarkers(get(id)).as<Number>();
        case Representation::Unchosen:
        case Representation::Void:
        case Representation::Boolean:
        case Representation::Int32:
        case Representation::Int64:
        case Representation::OptionalInt64:
        case Representation::Cell:
        case Representation::Cell32:
        case Representation::JSValue:
        case Representation::RawWord:
            break;
        }
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        return number(0);
    }

    Condition equalityOnBoxedBits(const ValueContext& value, Boxed x, Boxed y, bool isStrict)
    {
        Condition haveSameBits = equal(x, y);
        if (isStrict)
            return haveSameBits;
        Condition isLeftNullish = isUndefinedOrNull(x, typeOf(value->leftOperand()).primitives);
        Condition isRightNullish = isUndefinedOrNull(y, typeOf(value->rightOperand()).primitives);
        return logicalOr(haveSameBits, logicalAnd(isLeftNullish, isRightNullish));
    }

    Condition equalityOnNullableStrings(const ValueContext& value, bool isStrict)
    {
        for (auto [written, other] : { std::pair { value->rightOperand(), value->leftOperand() }, std::pair { value->leftOperand(), value->rightOperand() } }) {
            const Operation& definition = operationAt(definitionOf(written));
            if (definition.opcode == Opcode::String) {
                noteFormAgainstWrittenText(value, definition.string());
                return emitter<StringEmitter>().equalsLiteral(value, ComparedOperand::Value, in<Boxed>(other).value(), definition.string(), std::nullopt, true);
            }
        }
        m_out.noteForm(Form::Closed, stateOf(value->form()));
        Boxed x = in<Boxed>(value->leftOperand());
        return callEqualStub(value, isStrict, x, in<Boxed>(value->rightOperand()));
    }

    bool mayBeEmpty(ValueID id) { return typeOf(id).primitives & Primitive::Absent; }

    struct ComparedKinds {
        bool typesAreExhaustive;
        bool mayBothBeNaN;
        bool mayBothBeNumbers;
        bool mayBothBeStrings;
        bool mayBeEqualByContents;
    };

    ComparedKinds kindsComparedBy(const ValueContext& value)
    {
        Type left = typeOf(value->leftOperand());
        Type right = typeOf(value->rightOperand());
        bool typesAreExhaustive = left.isJavaScriptValue() && right.isJavaScriptValue();
        auto mayBothBe = [&](uint16_t primitives) {
            return !typesAreExhaustive || (left.primitives & primitives && right.primitives & primitives);
        };
        bool mayBothBeStrings = mayBothBe(Primitive::AnyString);
        return { typesAreExhaustive, mayBothBe(Primitive::WholeNumber), mayBothBe(Primitive::Number), mayBothBeStrings, mayBothBeStrings || mayBothBe(Primitive::BigInt) };
    }

    static bool bitsDecide(const ComparedKinds& kinds) { return !kinds.mayBothBeNumbers && !kinds.mayBeEqualByContents; }

    void decideWithoutContents(const ValueContext& value, Boxed x, Boxed y, BranchTargets targets, NOESCAPE const Invocable<void()> auto& answerByStub, NOESCAPE const Invocable<void()> auto& answerForStrings)
    {
        static_assert(formDependsOnLoopOf(Opcode::StrictEq));
        Type left = typeOf(value->leftOperand());
        Type right = typeOf(value->rightOperand());
        ComparedKinds kinds = kindsComparedBy(value);
        auto [typesAreExhaustive, mayBothBeNaN, mayBothBeNumbers, mayBothBeStrings, mayBeEqualByContents] = kinds;
        auto mayBeComparedByIdentity = [&, typesAreExhaustive = typesAreExhaustive](const Type& type) {
            return !typesAreExhaustive || type.hasObjects() || type.primitives & Primitive::AnySymbol;
        };
        if (bitsDecide(kinds)) {
            m_out.noteForm(Form::Open, StateOfKnowledge::Bits);
            branch(equal(x, y), targets);
            return;
        }

        bool comparesStringsHere = generator().takesFormOfLoops(value, StateOfKnowledge::Generic) && mayBothBeStrings;

        LBasicBlock hasOtherBits = newBlock();
        LBasicBlock callsStub = newBlock();
        bool stubIsRare = !mayBeEqualByContents || comparesStringsHere;
        auto edgeToStub = [&] {
            return stubIsRare ? rareEdge(callsStub) : unlikelyEdge(callsStub);
        };
        if (mayBothBeNaN) {
            LBasicBlock hasSameBits = newBlock();
            LBasicBlock isSameNumber = newBlock();
            m_out.branch(equal(x, y).value(), unlikelyEdge(hasSameBits), normalEdge(hasOtherBits));
            m_out.appendTo(hasSameBits);
            m_out.branch(isNumber(x).value(), normalEdge(isSameNumber), targets.taken);
            m_out.appendTo(isSameNumber);
            m_out.branch(isInt32(x).value(), targets.taken, edgeToStub());
        } else
            m_out.branch(equal(x, y).value(), targets.taken, normalEdge(hasOtherBits));

        m_out.appendTo(hasOtherBits);
        LBasicBlock areCells = mayBeEqualByContents ? newBlock() : nullptr;
        LBasicBlock areNotBothCells = mayBothBeNumbers ? newBlock() : nullptr;
        m_out.branch(areBothCells(x, y).value(), areCells ? normalEdge(areCells) : targets.notTaken, areNotBothCells ? normalEdge(areNotBothCells) : targets.notTaken);
        if (areNotBothCells) {
            m_out.appendTo(areNotBothCells);
            m_out.branch(isInt32(Boxed(m_out.bitAnd(x.value(), y.value()))).value(), targets.notTaken, edgeToStub());
        }
        if (areCells) {
            LBasicBlock comparesContents = comparesStringsHere ? newBlock() : callsStub;
            m_out.appendTo(areCells);
            for (auto [id, word] : { std::pair { value->leftOperand(), x }, std::pair { value->rightOperand(), y } }) {
                if (!mayBeEmpty(id))
                    continue;
                LBasicBlock isPresent = newBlock();
                m_out.branch(isEmpty(word).value(), targets.notTaken, normalEdge(isPresent));
                m_out.appendTo(isPresent);
            }
            if (mayBeComparedByIdentity(left) || mayBeComparedByIdentity(right)) {
                CellPointer tested = unboxCell(mayBeComparedByIdentity(left) ? x : y);
                m_out.branch(hasCellTypeAbove(tested, static_cast<JSType>(LastValueCompareCellType)).value(), targets.notTaken, unlikelyEdge(comparesContents));
            } else
                m_out.jump(comparesContents);
            if (comparesStringsHere) {
                LBasicBlock firstIsString = newBlock();
                LBasicBlock areStrings = newBlock();
                m_out.appendTo(comparesContents);
                m_out.branch(hasCellType(unboxCell(x), StringType).value(), normalEdge(firstIsString), rareEdge(callsStub));
                m_out.appendTo(firstIsString);
                m_out.branch(hasCellType(unboxCell(y), StringType).value(), normalEdge(areStrings), targets.notTaken);
                m_out.appendTo(areStrings);
                answerForStrings();
            }
        }
        m_out.appendTo(callsStub);
        answerByStub();
    }

    void branchOnEqualityOfValues(const ValueContext& value, Boxed x, Boxed y, BranchTargets targets)
    {
        auto branchInStub = [&] {
            LValue operands[] = { x.value(), y.value() };
            emitter<RuntimeCallEmitter>().callStubAndBranch(value, strictEqualInFlags(), operands, { }, targets);
        };
        decideWithoutContents(value, x, y, targets, branchInStub, [&] {
            branch(emitter<StringEmitter>().areEqual(value, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct), targets);
        });
    }

    Condition areValuesEqual(const ValueContext& value, Boxed x, Boxed y)
    {
        LBasicBlock isSame = newBlock();
        LBasicBlock isOther = newBlock();
        Join<Condition> join(m_out);
        decideWithoutContents(value, x, y, { normalEdge(isSame), normalEdge(isOther) }, [&] {
            join.add(callEqualStub(value, true, x, y));
        }, [&] {
            join.add(emitter<StringEmitter>().areEqual(value, unboxCell(x), unboxCell(y), StringEmitter::StringIdentity::Distinct));
        });
        m_out.appendTo(isSame);
        join.add(boolean(true));
        m_out.appendTo(isOther);
        join.add(boolean(false));
        return join.finish();
    }

    Condition areBothCells(Boxed x, Boxed y) { return isCell(Boxed(m_out.bitOr(x.value(), y.value()))); }
};

} // anonymous namespace

static ValueID writtenTextComparedWithValue(const Code& code, const Operation& equality)
{
    for (ValueID operand : { equality.rightOperand(), equality.leftOperand() }) {
        if (code.at(operand).opcode == Opcode::String)
            return operand;
    }
    return { };
}

static_assert(static_cast<uint64_t>(-JSValue::DoubleEncodeOffset) >> 52 == 0xfff && static_cast<uint64_t>(-JSValue::DoubleEncodeOffset) << 12);

void OperatorEmitter::generateNumberOrNaN(const ValueContext& value)
{
    define(value, numberToDouble(in<Boxed>(value->valueOperand())));
}

void OperatorEmitter::generateEquality(const ValueContext& value, bool isStrict)
{
    EqualityEmitter equality(generator());
    ValueID left = value->leftOperand();
    ValueID right = value->rightOperand();
    Condition result;
    switch (value->form()) {
    case OperatorForm::Generic:
        if (ValueID written = isStrict ? writtenTextComparedWithValue(m_code, value.value()) : ValueID()) {
            ValueID other = written == left ? right : left;
            const Type& type = typeOf(other);
            bool everyCellIsString = !type.hasObjects() && !(type.primitives & cellPrimitives & ~Primitive::AnyString);
            equality.noteFormAgainstWrittenText(value, operationAt(written).string());
            defineTestByBranching(value, [&](BranchTargets targets) {
                emitter<StringEmitter>().branchOnEqualsLiteral(value, ComparedOperand::Value, in<Boxed>(other).value(), operationAt(written).string(), targets, std::nullopt, everyCellIsString);
            });
            return;
        }
        if (isStrict) {
            equality.generateStrictEqualityOnValues(value);
            return;
        }
        result = equality.equalityOnValues(value, isStrict);
        break;
    case OperatorForm::Integers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Integers);
        result = equality.equalityOnIntegers(value);
        break;
    case OperatorForm::Numbers:
        m_out.noteForm(Form::Open, StateOfKnowledge::Numbers);
        if (equality.comparesOnlyNumbers(value)) {
            generateComparisonOfNumbers(value, std::nullopt);
            return;
        }
        result = equality.equalityOnNumbers(value, isStrict);
        break;
    case OperatorForm::Bits:
        m_out.noteForm(Form::Open, StateOfKnowledge::Bits);
        result = equality.equalityOnBits(value, isStrict);
        break;
    case OperatorForm::Booleans: {
        m_out.noteForm(Form::Open, StateOfKnowledge::Booleans);
        Condition x = in<Condition>(left);
        result = equal(x, in<Condition>(right));
        break;
    }
    case OperatorForm::BigInts: {
        m_out.noteForm(Form::Closed, StateOfKnowledge::BigInts);
        CellPointer x = in<CellPointer>(left);
        result = equality.callEqualStub(value, isStrict, boxCell(x), boxCell(in<CellPointer>(right)));
        break;
    }
    case OperatorForm::Strings:
        result = equality.equalityOnStrings(value, true);
        break;
    case OperatorForm::NullableStrings:
        result = equality.equalityOnStrings(value, isStrict);
        break;
    case OperatorForm::NumberAndValue:
        result = isStrict ? equality.equalityOnNumberAndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::StringAndValue:
        result = isStrict ? equality.equalityOnStringAndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::Int32AndValue:
        if (isStrict && equality.branchOnEqualityOfInt32AndValue(value))
            return;
        result = isStrict ? equality.equalityOnInt32AndValue(value) : equality.looseEqualityOnBoxed(value);
        break;
    case OperatorForm::Unselected:
    case OperatorForm::CheckedIntegers:
    case OperatorForm::StringAndPrimitive:
    case OperatorForm::NullableStringAndPrimitive:
    case OperatorForm::String:
    case OperatorForm::Number:
    case OperatorForm::NumberByOperation:
    case OperatorForm::NullableString:
    case OperatorForm::NullableNumber:
    case OperatorForm::MayBeString:
    case OperatorForm::MayBeStringOrNumber:
        break;
    }
    if (!result) {
        fail(value, InternalErrorKind::OpcodeWithoutCode);
        result = boolean(false);
    }
    defineTest(value, result);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
