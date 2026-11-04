/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Strings: `path.startsWith(dir + "/")`, `name.endsWith("." + extension)`, a template in the place of the `+`: a join that is written for one test of an end of a string is never made.
// The join walks the row to its end, as the first of a row of joins does: the call is walked on the part that lies at the end of the string, and the one code unit that is written is read beside it.
// Only the length says whether the two fit in one string. Where they do not, the row is walked as it is written in a Rare block, where the join throws.
// It keeps nothing, and no row goes past a suspension. Withheld (TypedOperations, BuiltinCalls), and in a closed arm: the join is made and the call takes it.

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

// What runs once gains nothing by a test in front of it.
static bool comesAgainForStringJoins(const Walker& walker)
{
    if (walker.isInLoop())
        return true;
    FunctionID function = walker.unit().function;
    return !walker.facts().runsAtMostOnce(function) && !walker.program().at(function).isModuleBody();
}

// The call that alone takes the text, behind whatever only hands it on, further on in the stretch that is being walked. `taken`: the name under which it takes it.
// Null: there is none, or something stands between the two that the program can see.
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

// Nothing was built where it answers nothing.
static std::optional<TestAgainstJoinOfTexts> findTestAgainstJoinForStringJoins(Walker& walker, const BodyValue& join, Reference takenToHaveText = { })
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    ValueID taken;
    ValueID user = findCallThatAloneTakesJoinForStringJoins(walker, index, index.block(index.blockOf(join.id())), join.id(), taken);
    if (!user)
        return std::nullopt;

    // The method is read and the receiver is there before the argument is made.
    BodyValue call = index.value(user);
    ValueID receiver = call.thisValueOperand();
    if (call.argumentsOperands().size() != 1 || call.argumentsOperands()[0] != taken || !receiver || !(receiver < join.id()) || !(call.calleeOperand() < join.id()))
        return std::nullopt;
    std::optional<InlineBuiltin> codeInLine = testOfEndCalledForStringJoins(walker, call);
    Reference string = walker.referenceOf(receiver);
    // `if (line.endsWith("," + tail)) ..; line += ..`: the receiver is an operand of the call, which is not the operation being walked. So it is still bound to the builder, of which typeOf() says `string`.
    if (string && walker.hasTextsInStringBuilders() && isTextInStringBuilder(walker, string))
        return std::nullopt;
    if (!codeInLine || !string || !isProvenString(walker, walker.typeOf(string)))
        return std::nullopt;

    // With a text on one side `+` joins, and a part that has text by itself runs no code to give it.
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
    Reference no = constantForStrings(walker, PrimitiveKnownToFolding::boolean(false));
    std::array<Reference, 1> answer;
    bool isBuilt = buildArmsForStrings(walker, holdsOfPart, BlockFrequency::Normal, [&](std::span<Reference> results) {
        Reference length = buildLengthOfString(walker, string, origin);
        Reference position = lengthOfPart;
        if (!testsStart) {
            // In front of the start of the string it is a number that no length reaches.
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
    uint32_t behindCall = test->call.index() + 1 - firstValue;
    BodyTerminator end = index.terminator(inputBlock);
    bool isReturnedAtOnce = end.kind() == TerminatorKind::ReturnCall && end.operand(0) == test->call;

    // No part runs code, so a call that Link lists at the join for a conversion enters nothing. It is told all the same: the calls that are walked below leave their own word behind.
    if (FunctionID callee = walker.facts().linked().calleeAt(walker.currentFunction(), join.id()))
        walker.arrivals().leaveAnything(callee);

    Origin origin = walker.originOf(join);
    Reference part = test->partAtEnd;
    if (!isProvenString(walker, walker.typeOf(part)))
        part = buildOperatorForStrings(walker, Opcode::ToString, part, { }, origin);
    Reference lengthOfPart = buildLengthOfString(walker, part, origin);
    Reference fitsWithUnit = buildOperatorForStrings(walker, Opcode::Less, lengthOfPart, integerForStrings(walker, Range::length().maximum), origin);

    std::array<Reference, 1> answer;
    bool isBuilt = buildArmsForStrings(walker, fitsWithUnit, BlockFrequency::Rare, [&](std::span<Reference> results) {
        walker.bind(join.id(), part);
        walker.walkStretch(inputBlock, placeOfJoin + 1, behindCall);
        Reference holdsOfPart = walker.isUnreachable() ? Reference() : walker.referenceOf(test->call);
        results[0] = buildTestOfCodeUnitBesidePartOfJoin(walker, test->string, lengthOfPart, holdsOfPart, test->unitBeside, test->testsStart, origin);
    }, [&](std::span<Reference> results) {
        Walker::ClosedArm closedArm(walker);
        walker.walkStretch(inputBlock, placeOfJoin, behindCall);
        if (isReturnedAtOnce)
            walker.endArmWithReturnOf(inputBlock);
        if (!walker.isUnreachable())
            results[0] = walker.referenceOf(test->call);
    }, answer);
    if (isBuilt)
        walker.bind(test->call, answer[0]);
    return true;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
