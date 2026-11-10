/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"
#include "AOTWalkTextBuilders.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTypes.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsOfOperations.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct TestAgainstJoinOfTexts {
    ValueID call;
    bool testsStart { false };
    Reference string;
    Reference partAtEnd;
    char16_t unitBeside { 0 };
};

} // anonymous namespace

static bool comesAgainForStringJoins(const Walker& walker)
{
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.program().at(function).isModuleBody();
}

static ValueID findCallThatAloneTakesJoinForStringJoins(const Walker& walker, const BodyIndex& index, const BodyBlock& block, ValueID join, ValueID& taken)
{
    taken = join;
    ValueID user = index.onlyUsingValue(taken);
    while (user && opcodesThatStateTypes.contains(index.opcodeOf(user))) {
        taken = user;
        user = index.onlyUsingValue(taken);
    }
    if (!user || user.index() >= block.firstValue().index() + walker.endOfStretchBeingWalked() || index.opcodeOf(user) != Opcode::Call)
        return { };
    for (uint32_t between = join.index() + 1; between < user.index(); ++between) {
        Opcode opcode = index.opcodeOf(ValueID(between));
        if (!opcodesThatStateTypes.contains(opcode) && !declaredBoundInTable(opcode).isUnobservable())
            return { };
    }
    return user;
}

static std::optional<InlineBuiltin> testOfEndCalledForStringJoins(Walker& walker, const BodyValue& call)
{
    TypeArena& types = walker.types();
    Reference callee = walker.referenceOf(call.calleeOperand());
    if (!callee)
        return std::nullopt;
    Type typeOfCallee = types.at(walker.typeOf(callee));
    BuiltinID builtin = typeOfCallee.isObject() ? singleBuiltinOf(types, typeOfCallee) : BuiltinID();
    if (!builtin)
        return std::nullopt;

    const BuiltinTable& table = walker.program().builtins();
    for (const BuiltinOverload& overload : table.overloadsOf(table.canonicalID(builtin))) {
        std::optional<InlineBuiltin> codeInLine = overload.code.implementation.inlineBuiltin;
        if (overload.mode == BuiltinCallMode::Call && (codeInLine == InlineBuiltin::StringStartsWith || codeInLine == InlineBuiltin::StringEndsWith))
            return codeInLine;
    }
    return std::nullopt;
}

static std::optional<TestAgainstJoinOfTexts> findTestAgainstJoinForStringJoins(Walker& walker, const BodyValue& join, Reference takenToHaveText = { })
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ValueID taken;
    ValueID user = findCallThatAloneTakesJoinForStringJoins(walker, index, index.block(index.blockOf(join.id())), join.id(), taken);
    if (!user)
        return std::nullopt;

    BodyValue call = index.value(user);
    ValueID receiver = call.thisValueOperand();
    if (call.argumentsOperands().size() != 1 || call.argumentsOperands()[0] != taken || !receiver || !(receiver < join.id()) || !(call.calleeOperand() < join.id()))
        return std::nullopt;
    std::optional<InlineBuiltin> codeInLine = testOfEndCalledForStringJoins(walker, call);
    Reference string = walker.referenceOf(receiver);
    if (string && walker.hasStringsInBuilders() && isStringInBuilder(walker, string))
        return std::nullopt;
    if (!codeInLine || !string || !isProvenString(walker, walker.typeOf(string)))
        return std::nullopt;

    Vector<Reference, 2> parts;
    bool eachHasText = true;
    join.forEachOperand([&](ValueID operand, uint32_t) {
        Reference part = walker.referenceOf(operand);
        eachHasText &= part && (part == takenToHaveText || hasTextByItself(types.at(walker.typeOf(part))));
        parts.append(part);
    });
    if (!eachHasText || parts.size() != 2)
        return std::nullopt;

    bool testsStart = codeInLine == InlineBuiltin::StringStartsWith;
    Reference partAtEnd = parts[testsStart ? 0 : 1];
    StringID textBeside = textKnownForStrings(walker, parts[testsStart ? 1 : 0]);
    std::optional<char16_t> unitBeside = textBeside ? onlyCodeUnitOfText(walker, textBeside) : std::nullopt;
    if (!unitBeside || textKnownForStrings(walker, partAtEnd))
        return std::nullopt;

    auto typeOfInput = [&](ValueID input) {
        if (input == taken)
            return TypeID(TypeTable::string);
        Reference reference = walker.referenceOf(input);
        return reference ? walker.typeOf(reference) : TypeID();
    };
    if (wouldCallBuiltin(walker, call, TypesOfInputs(typeOfInput)) != BuiltinCallAskedAbout::ByOneRow)
        return std::nullopt;
    return TestAgainstJoinOfTexts { user, testsStart, string, partAtEnd, *unitBeside };
}

Reference buildTestOfCodeUnitBesidePartOfJoin(Walker& walker, Reference string, Reference lengthOfPart, Reference holdsOfPart, char16_t unit, bool testsStart, Origin origin)
{
    Reference no = walker.constant(PrimitiveKnownToFolding::boolean(false));
    std::array<Reference, 1> answer;
    bool isBuilt = buildArmsForStrings(walker, holdsOfPart, BlockFrequency::Normal, [&](std::span<Reference> results) {
        Reference length = buildLengthOfString(walker, string, origin);
        Reference position = lengthOfPart;
        if (!testsStart) {
            Reference startOfPart = buildOperatorForStrings(walker, Opcode::Sub, length, lengthOfPart, origin);
            Reference inFront = buildOperatorForStrings(walker, Opcode::Sub, startOfPart, integerForStrings(walker, 1), origin);
            position = buildOperatorForStrings(walker, Opcode::URShift, inFront, integerForStrings(walker, 0), origin);
        }
        Reference isInside = buildOperatorForStrings(walker, Opcode::Less, position, length, origin);
        std::array<Reference, 1> holds;
        bool isRead = buildArmsForStrings(walker, isInside, BlockFrequency::Normal, [&](std::span<Reference> inner) {
            Reference inside = buildProofBehindTestForStrings(walker, position, isInside, OperandIndex::Less::left, origin);
            inner[0] = buildOperatorForStrings(walker, Opcode::StrictEq, buildCodeUnitOfString(walker, string, inside, origin), integerForStrings(walker, unit), origin);
        }, [&](std::span<Reference> inner) {
            inner[0] = no;
        }, holds);
        results[0] = isRead ? holds[0] : Reference();
    }, [&](std::span<Reference> results) {
        results[0] = no;
    }, answer);
    return isBuilt ? answer[0] : Reference();
}

ValueID findTestOfEndThatTakesJoinApart(Walker& walker, const BodyValue& join, Reference takenToHaveText)
{
    if (!operationsThatJoinTexts.contains(join.opcode()) || !walker.has(Mechanism::BuiltinCalls) || !walker.opensGuesses() || walker.isUnreachable() || !comesAgainForStringJoins(walker))
        return { };
    std::optional<TestAgainstJoinOfTexts> test = findTestAgainstJoinForStringJoins(walker, join, takenToHaveText);
    return test ? test->call : ValueID();
}

static_assert(opcodesThatBuildTestAgainstJoinOfTextsTakes == (operationsThatJoinTexts));

bool buildTestAgainstJoinOfTextsOfItsOpcodes(Walker& walker, const BodyValue& join)
{
    if (!operationsThatJoinTexts.contains(join.opcode()) || !walker.has(Mechanism::BuiltinCalls) || !walker.opensGuesses() || walker.isUnreachable() || !comesAgainForStringJoins(walker))
        return false;
    std::optional<TestAgainstJoinOfTexts> test = findTestAgainstJoinForStringJoins(walker, join);
    if (!test)
        return false;

    const BodyIndex& index = walker.currentBodyIndex();
    BlockID inputBlock = index.blockOf(join.id());
    uint32_t firstValue = index.block(inputBlock).firstValue().index();
    uint32_t placeOfJoin = join.id().index() - firstValue;
    uint32_t afterCall = test->call.index() + 1 - firstValue;
    Walker::StretchInArms stretch = walker.stretchInArms(inputBlock, placeOfJoin, afterCall);

    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), join.id()))
        walker.leaveAnythingAt(callee);

    Origin origin = walker.originOf(join);
    Reference part = test->partAtEnd;
    if (!isProvenString(walker, walker.typeOf(part)))
        part = buildOperatorForStrings(walker, Opcode::ToString, part, { }, origin);
    Reference lengthOfPart = buildLengthOfString(walker, part, origin);
    Reference fitsWithUnit = buildOperatorForStrings(walker, Opcode::Less, lengthOfPart, integerForStrings(walker, Range::length().maximum), origin);

    auto bringWhatIsReadBehind = [&](std::span<Reference> results) {
        for (size_t position = 0; position < results.size() && !walker.isUnreachable(); ++position)
            results[position] = walker.referenceOf(stretch.mentionedBehind[position]);
    };
    Vector<Reference, 8> met(stretch.mentionedBehind.size());
    bool isBuilt = buildArmsForStrings(walker, fitsWithUnit, BlockFrequency::Rare, [&](std::span<Reference> results) {
        walker.bind(join.id(), part);
        walker.walkStretch(inputBlock, placeOfJoin + 1, afterCall);
        Reference holdsOfPart = walker.isUnreachable() ? Reference() : walker.referenceOf(test->call);
        walker.bind(test->call, buildTestOfCodeUnitBesidePartOfJoin(walker, test->string, lengthOfPart, holdsOfPart, test->unitBeside, test->testsStart, origin));
        bringWhatIsReadBehind(results);
    }, [&](std::span<Reference> results) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, placeOfJoin, afterCall);
        if (stretch.isReturnedAtOnce)
            walker.threadReturnOf(inputBlock);
        bringWhatIsReadBehind(results);
    }, met.mutableSpan());
    for (uint32_t place = placeOfJoin; place < afterCall; ++place)
        walker.bind(ValueID(firstValue + place), Reference());
    for (size_t position = 0; position < met.size() && isBuilt; ++position)
        walker.bind(stretch.mentionedBehind[position], met[position]);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
