/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTIRProgram.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTTypeArena.h"
#include "AOTTypeTable.h"
#include "AOTWalkResultsInParts.h"
#include "AOTWalkStringFolds.h"
#include <optional>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

OperatorForm operatorFormFor(Walker&, const Draft&);
Reference boxNumberIfNeeded(Walker&, Reference, Origin);

static constexpr OpcodeSet accessesUnderKeysInWalkStrings { Opcode::ElemSet, Opcode::ElemHas, Opcode::ElemDelete, Opcode::ElemDefine };
static constexpr OpcodeSet conversionsToNumbersInWalkStrings { Opcode::ToNumber, Opcode::ToNumeric };

static_assert(opcodesThatBuildTypedOperationOnStringsTakes == (accessesUnderKeysInWalkStrings | conversionsToNumbersInWalkStrings | allEqualities | allRelations | OpcodeSet { Opcode::ElemGet, Opcode::PropGet, Opcode::ToBoolean }));

bool buildTypedOperationOnStringsOfItsOpcodes(Walker& walker, const BodyValue& value, const InputsOfTypedOperation& inputs)
{
    Opcode opcode = value.opcode();
    if (opcode == Opcode::PropGet)
        return buildLengthOfPieceInSight(walker, value, inputs);
    if (opcode == Opcode::ElemGet) {
        return buildElementOfMatchInFrame(walker, value, inputs) || buildReadByPartsOfKey(walker, value, inputs) || buildCharacterByIndex(walker, value, inputs) || buildLookupUnderAnyKeyOnPlainRecords(walker, value, inputs);
    }
    if (inputs.areOnlyTypes())
        return false;
    if (!accessesUnderKeysInWalkStrings.contains(opcode)) {
        bool somethingIsProven = false;
        for (uint32_t operandIndex = 0; operandIndex < numberOfNamedOperandsOf(opcode) && !somethingIsProven; ++operandIndex) {
            if (ValueID operand = value.operand(operandIndex))
                somethingIsProven |= !walker.hasUninformativeType(walker.referenceOf(operand));
        }
        if (!somethingIsProven)
            return false;
    }
    if (accessesUnderKeysInWalkStrings.contains(opcode))
        return buildLookupUnderAnyKeyOnPlainRecords(walker, value, inputs);
    if (allEqualities.contains(opcode))
        return buildEqualityOnStrings(walker, value, inputs);
    if (allRelations.contains(opcode))
        return buildRelationOnStrings(walker, value, inputs);
    if (opcode == Opcode::ToBoolean)
        return buildTruthinessOfPieceInSight(walker, value, inputs) || buildTruthinessOfLengthOfString(walker, value, inputs);
    if (conversionsToNumbersInWalkStrings.contains(opcode))
        return buildNumberOfPiece(walker, value, inputs);
    return false;
}

bool buildConstructionOnAnyOperandsForStrings(Walker& walker, const CallByOneRow& call)
{
    return buildURLCallByScan(walker, call);
}

bool buildCallByOneRowForStrings(Walker& walker, const CallByOneRow& call)
{
    if (walker.isUnreachable())
        return false;
    if (foldCallByOneRowOnKnownOperands(walker, call) || buildThrowOfNewError(walker, call) || buildFormatterCallByPattern(walker, call) || buildURLCallByScan(walker, call))
        return true;
    if (call.mode == BuiltinCallMode::Call && walker.program().builtins().canonicalID(call.builtin) == walker.program().builtins().canonicalID(Builtins::StringPrototypeMatchAll))
        return openMatchIteration(walker, call);
    if (!call.codeInLine || call.mode != BuiltinCallMode::Call)
        return false;

    switch (*call.codeInLine) {
    case InlineBuiltin::StringCharCodeAt:
    case InlineBuiltin::StringCodePointAt:
    case InlineBuiltin::StringCharAt:
    case InlineBuiltin::StringAt:
        return buildCharacterReadByMethod(walker, call);
    case InlineBuiltin::StringSlice:
    case InlineBuiltin::StringSubstring:
    case InlineBuiltin::StringSubstr:
        return buildPieceByMethod(walker, call);
    case InlineBuiltin::StringStartsWith:
    case InlineBuiltin::StringEndsWith:
        return buildTestOfEndOfString(walker, call);
    case InlineBuiltin::StringIndexOf:
    case InlineBuiltin::StringLastIndexOf:
    case InlineBuiltin::StringIncludes:
        return buildSearchInString(walker, call);
    case InlineBuiltin::StringSplit:
        return buildSplitInParts(walker, call);
    case InlineBuiltin::CallNumberConstructor:
    case InlineBuiltin::ParseInt:
        return buildNumberOfPieceByCall(walker, call);
    case InlineBuiltin::MapGet:
        return buildLookupOfPieceInMap(walker, call);
    case InlineBuiltin::MapHas:
        return buildTestForKeyLookedUpBehindIt(walker, call);
    case InlineBuiltin::MapSet:
        return buildStoreUnderPieceInMap(walker, call);
    default:
        return false;
    }
}

NotesOfStrings& notesOfStrings(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByStrings();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<NotesOfStrings>();
    return static_cast<NotesOfStrings&>(*kept);
}

Reference referenceToRecordForStrings(const Walker& walker, ValueID record)
{
    if (!record)
        return { };
    return walker.code().isConstant(record) ? Reference::constant(record) : Reference::value(record);
}

Reference rootForStrings(const Walker& walker, Reference reference)
{
    if (!reference.isValue())
        return reference;
    ValueID root = walker.code().valueBehindNames<cellPreservingOpcodes>(reference.id());
    return root == reference.id() ? reference : referenceToRecordForStrings(walker, root);
}

Reference stringBehindFlatString(const Walker& walker, Reference reference)
{
    Reference root = rootForStrings(walker, reference);
    if (!root.isValue() || walker.code().at(root.id()).opcode != Opcode::FlattenString)
        return { };
    return referenceToRecordForStrings(walker, walker.code().at(root.id()).stringOperand());
}

Reference rootOfStringForStrings(const Walker& walker, Reference string)
{
    Reference root = rootForStrings(walker, string);
    while (Reference behind = stringBehindFlatString(walker, root))
        root = rootForStrings(walker, behind);
    return root;
}

bool isLengthOfStringForStrings(const Walker& walker, Reference length, Reference string)
{
    Reference root = rootForStrings(walker, length);
    if (!root.isValue() || walker.code().at(root.id()).opcode != Opcode::GetStringLength)
        return false;
    Reference measured = referenceToRecordForStrings(walker, walker.code().at(root.id()).stringOperand());
    return rootOfStringForStrings(walker, measured) == rootOfStringForStrings(walker, string);
}

static PrimitiveKnownToFolding onlyValueOfTypeInWalkStrings(const TypeArena& types, TypeID id)
{
    if (!id)
        return { };
    const Type* record = holdsOnlyPrimitives(id) ? nullptr : &types.recordOf(id);
    if (record && (!record->isJavaScriptValue() || record->hasObjects()))
        return { };

    switch (record ? record->primitives : primitivesInID(id)) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer: {
        Range integers = record ? record->integers : integersInID(id);
        return integers.isSingleValue() ? PrimitiveKnownToFolding::integer(integers.minimum) : PrimitiveKnownToFolding();
    }
    case Primitive::LiteralString: {
        StringID literal = record ? singleStringLiteralOf(*record) : StringID();
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

PrimitiveKnownToFolding primitiveKnownForStrings(const Walker& walker, Reference reference)
{
    if (!reference)
        return { };
    if (reference.isRecord()) {
        const Operation& record = walker.code().at(reference.id());
        if (record.opcode == Opcode::Double)
            return PrimitiveKnownToFolding::number(record.number());
        if (record.opcode == Opcode::String)
            return PrimitiveKnownToFolding::text(record.string());
    }
    return onlyValueOfTypeInWalkStrings(walker.types(), walker.typeOf(reference));
}

StringID textKnownForStrings(const Walker& walker, Reference reference)
{
    PrimitiveKnownToFolding known = primitiveKnownForStrings(walker, reference);
    return known.isText() ? known.asText() : StringID();
}

unsigned lengthOfTextForStrings(const Walker& walker, StringID text)
{
    Vector<char16_t> buffer;
    return walker.types().viewOf(text, buffer).length();
}

std::optional<char16_t> onlyCodeUnitOfText(const Walker& walker, StringID text)
{
    Vector<char16_t> buffer;
    StringView view = walker.types().viewOf(text, buffer);
    if (view.length() != 1)
        return std::nullopt;
    return view[0];
}

Reference integerForStrings(Walker& walker, int64_t integer)
{
    Draft constant(Opcode::Integer);
    constant.setInteger(integer);
    return walker.constant(WTF::move(constant));
}

bool isProvenString(const Walker& walker, TypeID id)
{
    if (!id)
        return false;
    Type type = walker.types().at(id);
    return !type.isNever() && type.isAnyString();
}

namespace {

struct SumInWalkStrings {
    struct Term {
        ValueID record;
        int64_t factor { 0 };
        int64_t least { std::numeric_limits<int64_t>::min() };
        int64_t greatest { std::numeric_limits<int64_t>::max() };
    };
    struct PassedTest {
        ValueID relation;
        bool holds { false };
    };
    Vector<Term, 8> terms;
    Vector<PassedTest, 4> passedTests;
    int64_t constant { 0 };
    bool holdsWhatIsTooGreat { false };
};

} // anonymous namespace

static constexpr unsigned deepestSumReadInWalkStrings = 6;
static constexpr int64_t greatestMagnitudeAddedInWalkStrings = static_cast<int64_t>(1) << 53;

static void addToSumInWalkStrings(const Walker& walker, SumInWalkStrings& sum, ValueID value, int64_t factor, unsigned depth = 0)
{
    const Code& code = walker.code();
    const Operation* record = &code.at(value);
    SumInWalkStrings::Term added { { }, factor };
    auto holdToTypeOfName = [&] {
        Type type = walker.types().at(record->type);
        if (!type.isInteger())
            return;
        added.least = std::max(added.least, type.integers.minimum);
        added.greatest = std::min(added.greatest, type.integers.maximum);
    };
    holdToTypeOfName();
    while (valuePreservingOpcodes.contains(record->opcode)) {
        if (record->opcode == Opcode::Narrowed && record->testOperand() && !record->narrowsHolderOfOperand() && !record->narrowsOtherResultOfOperand() && !record->narrowsTermOfOperand() && allRelations.contains(code.at(record->testOperand()).opcode))
            sum.passedTests.append({ record->testOperand(), record->isOnTakenEdge() });
        value = record->operand(0);
        record = &code.at(value);
        holdToTypeOfName();
    }
    if (record->opcode == Opcode::Integer) {
        if (record->integer() < -greatestMagnitudeAddedInWalkStrings || record->integer() > greatestMagnitudeAddedInWalkStrings)
            sum.holdsWhatIsTooGreat = true;
        else
            sum.constant += factor * record->integer();
        return;
    }
    if (depth < deepestSumReadInWalkStrings && hasOperatorForm(record->opcode) && record->form() == OperatorForm::Integers) {
        switch (record->opcode) {
        case Opcode::Add:
        case Opcode::Sub:
            addToSumInWalkStrings(walker, sum, record->operand(0), factor, depth + 1);
            addToSumInWalkStrings(walker, sum, record->operand(1), record->opcode == Opcode::Add ? factor : -factor, depth + 1);
            return;
        case Opcode::Increment:
        case Opcode::Decrement:
            sum.constant += record->opcode == Opcode::Increment ? factor : -factor;
            addToSumInWalkStrings(walker, sum, record->operand(0), factor, depth + 1);
            return;
        default:
            break;
        }
    }
    for (SumInWalkStrings::Term& term : sum.terms) {
        if (term.record == value) {
            term.factor += factor;
            term.least = std::max(term.least, added.least);
            term.greatest = std::min(term.greatest, added.greatest);
            return;
        }
    }
    added.record = value;
    sum.terms.append(added);
}

static bool sumIsAtMostInWalkStrings(const SumInWalkStrings& sum, int64_t most)
{
    constexpr int64_t greatestMagnitudeAdded = greatestMagnitudeAddedInWalkStrings;
    if (sum.holdsWhatIsTooGreat)
        return false;
    int64_t greatest = sum.constant;
    for (const SumInWalkStrings::Term& term : sum.terms) {
        if (!term.factor)
            continue;
        int64_t bound = term.factor > 0 ? term.greatest : term.least;
        if (std::abs(term.factor) > 2 || bound < -greatestMagnitudeAdded || bound > greatestMagnitudeAdded)
            return false;
        greatest += term.factor * bound;
    }
    return greatest <= most;
}

bool relationIsShownBySumsForStrings(const Walker& walker, Opcode relation, Reference left, Reference right)
{
    RELEASE_ASSERT(relation == Opcode::Less || relation == Opcode::LessEq);
    if (!left || !right || !left.isRecord() || !right.isRecord() || !walker.has(Mechanism::Folding))
        return false;
    const Code& code = walker.code();
    int64_t most = relation == Opcode::LessEq ? 0 : -1;
    SumInWalkStrings difference;
    addToSumInWalkStrings(walker, difference, left.id(), 1);
    addToSumInWalkStrings(walker, difference, right.id(), -1);
    if (sumIsAtMostInWalkStrings(difference, most))
        return true;

    for (SumInWalkStrings::PassedTest passed : Vector<SumInWalkStrings::PassedTest, 4>(difference.passedTests)) {
        const Operation& test = code.at(passed.relation);
        if (test.form() != OperatorForm::Integers)
            continue;
        bool saysLess = test.opcode == Opcode::Less || test.opcode == Opcode::LessEq;
        bool admitsEqual = test.opcode == Opcode::LessEq || test.opcode == Opcode::GreaterEq;
        uint32_t lesser = saysLess == passed.holds ? 0 : 1;
        int64_t mostOfTest = admitsEqual == passed.holds ? 0 : -1;
        SumInWalkStrings rest = difference;
        addToSumInWalkStrings(walker, rest, test.operand(lesser), -1);
        addToSumInWalkStrings(walker, rest, test.operand(1 - lesser), 1);
        if (sumIsAtMostInWalkStrings(rest, most - mostOfTest))
            return true;
    }
    return false;
}

std::optional<bool> relationDecidedForStrings(const Walker& walker, Opcode relation, Reference left, Reference right)
{
    RELEASE_ASSERT(relation == Opcode::Less || relation == Opcode::LessEq);
    if (!left || !right)
        return std::nullopt;
    Type leftType = walker.types().at(walker.typeOf(left));
    Type rightType = walker.types().at(walker.typeOf(right));
    if (!leftType.isInteger() || !rightType.isInteger())
        return std::nullopt;

    bool holdsOfEqual = relation == Opcode::LessEq;
    if (rootForStrings(walker, left) == rootForStrings(walker, right))
        return holdsOfEqual;
    if (holdsOfEqual ? leftType.integers.maximum <= rightType.integers.minimum : leftType.integers.maximum < rightType.integers.minimum)
        return true;
    if (holdsOfEqual ? leftType.integers.minimum > rightType.integers.maximum : leftType.integers.minimum >= rightType.integers.maximum)
        return false;
    return std::nullopt;
}

Opcode relationWithOperandsSwappedForStrings(Opcode relation)
{
    switch (relation) {
    case Opcode::Less:
        return Opcode::Greater;
    case Opcode::LessEq:
        return Opcode::GreaterEq;
    case Opcode::Greater:
        return Opcode::Less;
    case Opcode::GreaterEq:
        return Opcode::LessEq;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

std::optional<uint32_t> operandsNarrowedBehindTestForStrings(const Walker& walker, ValueID test)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isUnused(test))
        return 0;
    std::optional<BodyUser> user = index.onlyUser(test);
    if (!user) {
        bool hasUnsharedOperands = true;
        index.value(test).forEachOperand([&](ValueID operand, uint32_t) {
            hasUnsharedOperands &= index.hasOneUse(operand);
        });
        if (hasUnsharedOperands)
            return 0;
        std::optional<BodyUser> first = index.firstUser(test);
        ValueID truth = first && first->kind == BodyUseKind::Operand && index.opcodeOf(first->value) == Opcode::ToBoolean ? first->value : ValueID();
        std::optional<BodyUser> asker = truth ? index.onlyUser(truth) : std::nullopt;
        if (!asker || asker->kind != BodyUseKind::TerminatorOperand)
            return std::nullopt;
        uint32_t numberOfUsesSeen = 1;
        for (BodyTarget target : index.terminator(asker->block).targets())
            numberOfUsesSeen += target.proofs().size();
        return index.useCount(test) == numberOfUsesSeen ? std::optional<uint32_t>(0) : std::nullopt;
    }
    if (user->kind != BodyUseKind::TerminatorOperand)
        return 0;

    uint32_t operands = 0;
    for (BodyTarget target : index.terminator(user->block).targets()) {
        for (uint32_t operandIndex : target.proofs())
            operands |= 1u << (operandIndex & 31);
    }
    return operands;
}

static bool branchOnRecordEntersCurrentBlockInWalkStrings(const Walker& walker, Reference test, bool holds)
{
    if (!test.isValue() || walker.isUnreachable())
        return false;
    const Code& code = walker.code();
    EdgeID entering = code.at(walker.currentBlock()).firstEdgeIn();
    if (!entering)
        return false;
    const Terminator& end = code.at(code.at(entering).from).terminator;
    if (!end.isSet || !end.isConditionalBranch() || (entering != end.taken() && entering != end.notTaken()))
        return false;

    bool hasComeOutTrue = entering == end.taken();
    for (ValueID condition = end.conditionOperand(); condition != test.id(); condition = code.at(condition).valueOperand()) {
        Opcode opcode = code.at(condition).opcode;
        if (opcode == Opcode::LogicalNot)
            hasComeOutTrue = !hasComeOutTrue;
        else if (opcode != Opcode::ToBoolean && !conversionsOfForm.contains(opcode))
            return false;
    }
    return hasComeOutTrue == holds;
}

std::optional<TestThatLedHere> testThatLedToBlockOf(Walker& walker, const BodyValue& value)
{
    const BodyIndex& index = walker.currentBodyIndex();
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(value.id()));
    if (!edge)
        return std::nullopt;
    BodyTerminator terminator = index.terminator(edge->from);
    if (terminator.kind() != TerminatorKind::Branch)
        return std::nullopt;

    TestThatLedHere led { terminator.operand(0), !edge->targetIndex };
    while (led.test && !index.isParameter(led.test)) {
        Opcode opcode = index.opcodeOf(led.test);
        if (opcode == Opcode::LogicalNot)
            led.holds = !led.holds;
        else if (opcode != Opcode::ToBoolean)
            break;
        led.test = index.value(led.test).operand(0);
    }
    if (!led.test || !branchOnRecordEntersCurrentBlockInWalkStrings(walker, walker.referenceBoundTo(led.test), led.holds))
        return std::nullopt;
    return led;
}

static bool relationSaysPositionBelowLengthInWalkStrings(const Walker& walker, const Operation& relation, bool holds, uint32_t& placeOfPosition, Reference string)
{
    switch (relation.opcode) {
    case Opcode::Less:
    case Opcode::GreaterEq:
        placeOfPosition = 0;
        break;
    case Opcode::Greater:
    case Opcode::LessEq:
        placeOfPosition = 1;
        break;
    default:
        return false;
    }
    bool saysSoWhereItHolds = relation.opcode == Opcode::Less || relation.opcode == Opcode::Greater;
    if (holds != saysSoWhereItHolds)
        return false;
    return isLengthOfStringForStrings(walker, referenceToRecordForStrings(walker, relation.operand(1 - placeOfPosition)), string);
}

bool nameProvesPositionBelowLengthForStrings(const Walker& walker, Reference position, Reference string)
{
    const Code& code = walker.code();
    while (position.isValue() && cellPreservingOpcodes.contains(code.at(position.id()).opcode)) {
        const Operation& name = code.at(position.id());
        if (name.opcode == Opcode::Narrowed && name.testOperand() && !name.narrowsTermOfOperand()) {
            uint32_t placeOfPosition = 0;
            if (relationSaysPositionBelowLengthInWalkStrings(walker, code.at(name.testOperand()), name.isOnTakenEdge(), placeOfPosition, string) && placeOfPosition == name.operandIndex())
                return true;
        }
        position = referenceToRecordForStrings(walker, name.operand(0));
    }
    return false;
}

void bindForStrings(Walker& walker, ValueID input, Reference built, Origin origin)
{
    walker.bind(input, built ? boxNumberIfNeeded(walker, built, origin) : built);
}

Reference buildOperatorForStrings(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    if (walker.isUnreachable() || !left)
        return { };
    Draft operation(opcode, origin);
    walker.chooseConservativeFields(operation);
    operation.setOperand(0, left);
    if (right)
        operation.setOperand(1, right);
    if (hasOperatorForm(opcode))
        operation.setForm(operatorFormFor(walker, operation));
    Reference built = walker.build(WTF::move(operation));
    return built ? boxNumberIfNeeded(walker, built, origin) : built;
}

Reference buildNegationForStrings(Walker& walker, Reference boolean, Origin origin)
{
    PrimitiveKnownToFolding known = primitiveKnownForStrings(walker, boolean);
    if (known.kind() == PrimitiveKnownToFolding::Kind::Boolean)
        return walker.constant(PrimitiveKnownToFolding::boolean(!known.asBoolean()));
    return buildOperatorForStrings(walker, Opcode::LogicalNot, boolean, { }, origin);
}

Reference buildLengthOfString(Walker& walker, Reference string, Origin origin)
{
    if (StringID text = textKnownForStrings(walker, string))
        return integerForStrings(walker, lengthOfTextForStrings(walker, text));
    if (walker.isUnreachable() || !string)
        return { };
    Draft length(Opcode::GetStringLength, origin);
    length.setStringOperand(string);
    return walker.build(WTF::move(length));
}

Reference buildCodeUnitOfString(Walker& walker, Reference string, Reference position, Origin origin)
{
    if (walker.isUnreachable() || !string || !position)
        return { };
    Reference read = string;
    if (string.isConstant() && !position.isConstant()) {
        if (Reference flat = flatLiteralBeforeLoop(walker, string, origin))
            read = flat;
    } else if (nameProvesPositionBelowLengthForStrings(walker, position, string)) {
        if (Reference flat = flatStringForReaderOfCharacters(walker, string, origin, true))
            read = flat;
    }
    if (walker.isUnreachable())
        return { };
    Draft codeUnit(Opcode::GetStringCodeUnit, origin);
    codeUnit.setStringOperand(read);
    codeUnit.setPositionOperand(position);
    return walker.build(WTF::move(codeUnit));
}

Reference buildPieceOfString(Walker& walker, Reference string, Reference start, Reference end, Origin origin)
{
    if (walker.isUnreachable() || !string || !start || !end)
        return { };
    Draft piece(Opcode::Substring, origin);
    piece.setStringOperand(string);
    piece.setStartOperand(start);
    piece.setEndOperand(end);
    return walker.build(WTF::move(piece));
}

Reference buildTypeTestForStrings(Walker& walker, Reference value, TypeID tested, Origin origin)
{
    if (walker.isUnreachable() || !value)
        return { };
    Draft test(Opcode::TypeTest, origin);
    test.setValueOperand(value);
    test.setTestedType(tested);
    return walker.build(WTF::move(test));
}

Reference buildResultOfRangeForStrings(Walker& walker, Reference range, uint32_t index, Origin origin)
{
    if (walker.isUnreachable() || !range)
        return { };
    Draft result(Opcode::ResultAt, origin);
    result.setResultsOperand(range);
    result.setIndex(index);
    return walker.build(WTF::move(result));
}

static bool isDecidedInWalkStrings(const Walker& walker, Reference test)
{
    return primitiveKnownForStrings(walker, test).kind() == PrimitiveKnownToFolding::Kind::Boolean;
}

Reference buildProofBehindTestForStrings(Walker& walker, Reference value, Reference test, uint32_t operandIndex, Origin origin)
{
    if (!value.isValue() || !test.isValue() || isDecidedInWalkStrings(walker, test))
        return value;
    if (walker.isUnreachable())
        return { };
    Draft proof(Opcode::Narrowed, origin);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(true);
    return walker.build(WTF::move(proof));
}

bool buildArmsForStrings(Walker& walker, Reference test, BlockFrequency ofFailing, const ArmForStrings& passing, const ArmForStrings& failing, std::span<Reference> met)
{
    if (!test || walker.isUnreachable())
        return false;

    auto areAllBuilt = [](std::span<const Reference> values) {
        return std::ranges::all_of(values, [](Reference value) {
            return !!value;
        });
    };
    if (isDecidedInWalkStrings(walker, test)) {
        (primitiveKnownForStrings(walker, test).asBoolean() ? passing : failing)(met);
        return !walker.isUnreachable() && areAllBuilt(met);
    }

    BlockID passingBlock = walker.addBlock();
    BlockID failingBlock = walker.addBlock(ofFailing);
    BlockID meetingBlock = walker.addBlock();
    walker.branch(test, passingBlock, failingBlock);

    Vector<Walker::JoinArm, 2> arms;
    auto buildArm = [&](BlockID block, const ArmForStrings& arm) {
        walker.continueIn(block);
        Walker::JoinArm built;
        built.values.fill(Reference(), met.size());
        arm(built.values.mutableSpan());
        if (walker.isUnreachable() || walker.hasFailed())
            return;
        if (!areAllBuilt(built.values.span()))
            return walker.fail("an arm of an operation on strings has no value for what meets behind it"_s);
        built.last = walker.currentBlock();
        arms.append(WTF::move(built));
    };
    buildArm(passingBlock, passing);
    if (!walker.hasFailed())
        buildArm(failingBlock, failing);
    if (walker.hasFailed() || arms.isEmpty())
        return false;
    return walker.joinArms(arms.span(), meetingBlock, met);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
