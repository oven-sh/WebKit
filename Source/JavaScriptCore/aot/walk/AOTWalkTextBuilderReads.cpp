/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTWalkStrings.h"
#include <array>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet readersOfLengthInTextBuilderReads { Opcode::PropGet, Opcode::ToBoolean, Opcode::StrictEq, Opcode::Eq };
static_assert(OperandIndex::StrictEq::left == OperandIndex::Eq::left && OperandIndex::StrictEq::right == OperandIndex::Eq::right);

static bool isEmptyTextInTextBuilderReads(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isConstant())
        return false;
    const Operation& constant = walker.code().at(reference.id());
    return constant.opcode == Opcode::String && constant.string() == walker.program().strings.emptyString();
}

static Reference lengthOfTextInTextBuilderReads(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    if (text.blockOfState != walker.currentBlock()) {
        text.stringOfState = { };
        text.lengthOfState = { };
        text.blockOfState = walker.currentBlock();
    }
    if (text.lengthOfState)
        return text.lengthOfState;

    Draft length(text.stringOfState ? Opcode::GetStringLength : Opcode::StringBuilderLength, origin);
    walker.chooseConservativeFields(length);
    if (text.stringOfState)
        length.setStringOperand(text.stringOfState);
    else {
        length.setBuilderOperand(name);
        becomeOperationOfStringBuilderInFrame(walker, text, name, length, Opcode::StackStringBuilderLength);
        if (walker.isUnreachable())
            return { };
    }
    Reference numberOfCodeUnits = walker.build(WTF::move(length));
    if (!numberOfCodeUnits)
        return { };
    text.lengthOfState = boxNumberIfNeeded(walker, numberOfCodeUnits, origin);
    return text.lengthOfState;
}

static Reference buildTestOfLengthInTextBuilderReads(Walker& walker, Draft&& test)
{
    walker.chooseConservativeFields(test);
    test.setForm(operatorFormFor(walker, test));
    return walker.build(WTF::move(test));
}

Reference buildTruthinessOfStringInBuilder(Walker& walker, StringInBuilder& text, Reference name, Origin origin)
{
    origin.isTailCall = false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return { };
    Draft hasCodeUnits(Opcode::ToBoolean, origin);
    hasCodeUnits.setValueOperand(length);
    return buildTestOfLengthInTextBuilderReads(walker, WTF::move(hasCodeUnits));
}

static bool buildEqualityAfterLengthCheck(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name, Reference written, Origin origin)
{
    StringID known = written ? textKnownForStrings(walker, written) : StringID();
    if (!known || operandsNarrowedBehindTestForStrings(walker, reader.id()) != std::optional<uint32_t>(0))
        return false;
    Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
    if (!length)
        return true;
    Draft hasLength(Opcode::StrictEq, origin);
    hasLength.setLeftOperand(length);
    hasLength.setRightOperand(integerForStrings(walker, lengthOfTextForStrings(walker, known)));
    Reference isOfSameLength = buildTestOfLengthInTextBuilderReads(walker, WTF::move(hasLength));
    std::array<Reference, 1> isEqual;
    auto compare = [&](std::span<Reference> results) {
        Reference string = materializeStringInBuilder(walker, text, name, origin);
        results[0] = string ? buildOperatorForStrings(walker, Opcode::StrictEq, string, written, origin) : Reference();
    };
    auto giveNo = [&](std::span<Reference> results) {
        results[0] = walker.constant(PrimitiveKnownToFolding::boolean(false));
    };
    bool isBuilt = buildArmsForStrings(walker, isOfSameLength, BlockFrequency::Normal, ArmForStrings(compare), ArmForStrings(giveNo), isEqual);
    text.stringOfState = { };
    text.lengthOfState = { };
    if (isBuilt)
        walker.bind(reader.id(), isEqual[0]);
    return true;
}

bool readsTextWithoutString(Walker& walker, const BodyValue& reader, StringInBuilder& text, Reference name)
{
    Opcode opcode = reader.opcode();
    if (!readersOfLengthInTextBuilderReads.contains(opcode))
        return false;
    auto namesText = [&](ValueID operand) {
        return operand && walker.nameLastBoundTo(operand) == name;
    };

    Origin origin = walker.originOf(reader);
    switch (opcode) {
    case Opcode::PropGet: {
        if (reader.isFreeName() || reader.key() != walker.program().keyOf(BuiltinNames::length) || !namesText(reader.objectOperand()))
            return false;
        if (Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin))
            walker.bind(reader.id(), length);
        return true;
    }
    case Opcode::ToBoolean: {
        if (!namesText(reader.valueOperand()))
            return false;
        if (Reference truth = buildTruthinessOfStringInBuilder(walker, text, name, origin))
            walker.bind(reader.id(), truth);
        return true;
    }
    case Opcode::StrictEq:
    case Opcode::Eq: {
        ValueID left = reader.operand(OperandIndex::StrictEq::left);
        ValueID right = reader.operand(OperandIndex::StrictEq::right);
        bool comparesWithEmptyText = (namesText(left) && isEmptyTextInTextBuilderReads(walker, walker.referenceOf(right))) || (namesText(right) && isEmptyTextInTextBuilderReads(walker, walker.referenceOf(left)));
        if (!comparesWithEmptyText) {
            if (namesText(left) == namesText(right))
                return false;
            return buildEqualityAfterLengthCheck(walker, reader, text, name, walker.referenceOf(namesText(left) ? right : left), origin);
        }
        Reference length = lengthOfTextInTextBuilderReads(walker, text, name, origin);
        if (!length)
            return true;
        Draft isEmpty(Opcode::StrictEq, origin);
        isEmpty.inputValueGiven = reader.id();
        isEmpty.setLeftOperand(length);
        isEmpty.setRightOperand(walker.constant(Draft(Opcode::Integer, origin)));
        if (Reference answer = buildTestOfLengthInTextBuilderReads(walker, WTF::move(isEmpty)))
            walker.bind(reader.id(), answer);
        return true;
    }
    default:
        return false;
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
