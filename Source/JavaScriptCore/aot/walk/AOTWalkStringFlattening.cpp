/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalkStrings.h"

#if ENABLE(AOT)

#include "AOTBuiltinIDs.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsDeclared.h"
#include "AOTIRProgram.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <optional>
#include <span>

#include "AOTWalkKnown.h"

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint32_t flatStringAmongNotesOfFlattening = 0;
static constexpr uint32_t readerHasBeenHereAmongNotesOfFlattening = 1;

static Reference flatStringNotedForFlattening(Walker& walker, Reference string)
{
    std::optional<uint64_t> noted = findKnown(walker, KnownNote::FlatString, string, { }, flatStringAmongNotesOfFlattening);
    return noted ? Reference::fromBits(static_cast<uint32_t>(*noted)) : Reference();
}

static void noteFlatStringForFlattening(Walker& walker, Reference string, Reference flat)
{
    noteKnown(walker, KnownNote::FlatString, string, { }, flatStringAmongNotesOfFlattening, { }, flat.bits(), true);
}

static bool readerHasBeenHereForFlattening(Walker& walker, Reference string)
{
    if (findKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening))
        return true;
    noteKnown(walker, KnownNote::FlatString, string, { }, readerHasBeenHereAmongNotesOfFlattening, { }, 1);
    return false;
}

static Draft draftOfFlatStringForFlattening(Reference string, Origin origin)
{
    origin.isTailCall = false;
    Draft flatten(Opcode::FlattenString, origin);
    flatten.setStringOperand(string);
    return flatten;
}

static std::optional<uint32_t> loopForHoistedFlattening(const Walker& walker, Reference string)
{
    const auto& here = walker.code().at(walker.currentBlock());
    if (!walker.has(Mechanism::LoopInvariants) || here.frequency == BlockFrequency::Rare || here.handler)
        return std::nullopt;
    return walker.outermostLoopToMakeTextInFrontOf(singleElementSpan(string), declaredBoundOf(Opcode::FlattenString));
}

static std::optional<uint32_t> loopForUnseenFlattening(Walker& walker, Reference string)
{
    std::optional<uint32_t> outermost = loopForHoistedFlattening(walker, string);
    return outermost ? walker.outermostLoopWithoutVisibleWork(*outermost) : std::nullopt;
}

static Reference flatStringOfLoopAround(Walker& walker, Reference string)
{
    std::optional<uint32_t> outermost = loopForHoistedFlattening(walker, string);
    std::optional<uint32_t> innermost = outermost ? walker.innermostOpenLoop() : std::nullopt;
    for (uint32_t index = outermost.value_or(0); innermost && index <= *innermost && !walker.hasFailed(); ++index) {
        if (Reference flat = walker.flatStringBeforeLoop(index, string))
            return flat;
    }
    return { };
}

static Reference flattenForSpecializedVersion(Walker& walker, Reference string, Origin origin)
{
    constexpr Walker::IterationPurity purity = Walker::IterationPurity::KnownEffectsOnly;
    std::optional<uint32_t> loop = loopForHoistedFlattening(walker, string) ? walker.innermostOpenLoop() : std::nullopt;
    Vector<Reference, 2> conditions;
    if (!loop || !walker.isSpecializedLoop(*loop, purity) || !walker.isReachedByEachIteration(*loop))
        return { };
    walker.tryBuildFirstArrivalConditions(*loop, conditions);
    if (walker.hasFailed())
        return { };
    origin.isTailCall = false;
    Draft resolve(Opcode::TryResolveRope, origin);
    resolve.setStringOperand(string);
    resolve.setIsReadOperands(conditions.span());
    Reference isFlat = walker.buildInFrontOfOpenLoop(*loop, WTF::move(resolve));
    if (!isFlat || walker.hasFailed() || !walker.tryBranchInPreheader(isFlat, true, purity))
        return { };
    walker.recordSpeculation();
    walker.count(StatisticID::StringsFlattenedForVersions);
    Draft flatten = draftOfFlatStringForFlattening(string, origin);
    flatten.setIsReadOperands(conditions.span());
    return walker.buildInFrontOfOpenLoop(*loop, WTF::move(flatten));
}

static Reference flattenInFrontOfLoopForFlattening(Walker& walker, Reference string, Origin origin)
{
    std::optional<uint32_t> loop = loopForUnseenFlattening(walker, string);
    if (!loop || loop != loopForHoistedFlattening(walker, string)) {
        if (Reference flat = flatStringOfLoopAround(walker, string))
            return flat;
    }
    if (walker.hasFailed())
        return { };
    if (!loop)
        return flattenForSpecializedVersion(walker, string, origin);
    Draft flatten = draftOfFlatStringForFlattening(string, origin);
    Vector<Reference, 2> conditions;
    if (!walker.tryBuildFirstArrivalConditions(*loop, conditions) && walker.innermostOpenLoop() == loop && !walker.isFlattenedBeforeLoop(*loop, string, conditions.span()))
        flatten.setMayStayRope(true);
    if (walker.hasFailed())
        return { };
    flatten.setIsReadOperands(conditions.span());
    if (flatten.mayStayRope())
        return walker.buildInFrontOfOpenLoop(*loop, WTF::move(flatten));
    if (Reference flat = walker.flatStringOfOtherVersion(string, conditions.span())) {
        walker.count(StatisticID::FlatStringsShared);
        return flat;
    }
    Reference flat = walker.buildInFrontOfOpenLoop(*loop, WTF::move(flatten));
    if (flat && flat.isValue())
        walker.noteHoistedFlatString(flat);
    return flat;
}

Reference flatLiteralBeforeLoop(Walker& walker, Reference literal, Origin origin)
{
    std::optional<uint32_t> loop = walker.isUnreachable() ? std::nullopt : loopForHoistedFlattening(walker, literal);
    Reference flat = loop ? walker.buildInFrontOfOpenLoop(*loop, draftOfFlatStringForFlattening(literal, origin)) : Reference();
    if (flat)
        walker.count(StatisticID::LiteralCharactersHoisted);
    return flat;
}

Reference flatStringInSight(Walker& walker, Reference string)
{
    if (!string.isValue())
        return { };
    if (walker.types().at(walker.typeOf(string)).isOpaque(OpaqueKind::FlatString))
        return string;
    Reference flat = flatStringNotedForFlattening(walker, string);
    return flat && walker.isInSightWhereWalkBuilds(flat) ? flat : Reference();
}

bool isOrCanBeFlattenedBeforeLoop(Walker& walker, Reference string)
{
    if (walker.isUnreachable() || !string.isValue())
        return false;
    return flatStringInSight(walker, string) || loopForUnseenFlattening(walker, string);
}

Reference flatStringMadeWhereWalkStands(Walker& walker, Reference string, Origin origin)
{
    if (walker.isUnreachable() || !string.isValue() || !isProvenString(walker, walker.typeOf(string)))
        return { };
    if (Reference flat = flatStringInSight(walker, string))
        return flat;
    Reference flat = walker.build(draftOfFlatStringForFlattening(string, origin));
    if (flat)
        noteFlatStringForFlattening(walker, string, flat);
    return flat;
}

static Reference openBeforeLoopForReads(Walker& walker, Reference string, Origin origin)
{
    std::optional<uint32_t> loop = walker.unit().role != CodeRole::CoroutineBody ? loopForHoistedFlattening(walker, string) : std::nullopt;
    if (!loop)
        return { };
    origin.isTailCall = false;
    Draft open(Opcode::OpenString, origin);
    open.setStringOperand(string);
    Reference opened = walker.buildInFrontOfOpenLoop(*loop, WTF::move(open));
    if (opened)
        walker.count(StatisticID::StringsOpenedBeforeLoops);
    return opened;
}

Reference flatStringForReaderOfCharacters(Walker& walker, Reference string, Origin origin, bool readsCodeUnitsAlone)
{
    if (walker.isUnreachable() || !string.isValue())
        return { };
    if (walker.code().at(string.id()).opcode == Opcode::OpenString)
        return readsCodeUnitsAlone ? string : Reference();
    if (Reference flat = flatStringInSight(walker, string))
        return flat;
    if (!isProvenString(walker, walker.typeOf(string)))
        return { };

    Reference flat = flattenInFrontOfLoopForFlattening(walker, string, origin);
    if (!flat && readsCodeUnitsAlone) {
        if (Reference opened = openBeforeLoopForReads(walker, string, origin); opened || walker.hasFailed())
            return opened;
    }
    if (!flat && readerHasBeenHereForFlattening(walker, string))
        flat = walker.build(draftOfFlatStringForFlattening(string, origin));
    if (flat)
        noteFlatStringForFlattening(walker, string, flat);
    return flat;
}

static bool isCallOfMathFunction(const Walker& walker, const BodyIndex& index, const BodyValue& call)
{
    if (call.opcode() != Opcode::Call || index.isParameter(call.calleeOperand()) || index.opcodeOf(call.calleeOperand()) != Opcode::Builtin)
        return false;
    const BuiltinTable& table = walker.program().builtins();
    BuiltinID function = table.canonicalID(index.value(call.calleeOperand()).as<Opcode::Builtin>().builtin());
    return table.at(function).holder == Builtins::Math && std::ranges::all_of(table.overloadsOf(function), [](const BuiltinOverload& overload) {
        return overload.mode != BuiltinCallMode::Call || (!overload.receiver && !overload.parameters.empty() && overload.effects.capability == BuiltinCapability::None);
    });
}

void flattenStringsReadByLoop(Walker& walker, BlockID inputHead, std::span<const Reference> entering, Origin origin)
{
    const BodyIndex& index = walker.currentBodyIndex();
    const Program& program = walker.program();
    LoopID loop = index.loopOf(inputHead);
    BodyBlock head = index.block(inputHead);
    BodyTerminator end = head.terminator();
    if (end.kind() != TerminatorKind::Branch || !head.numberOfParameters())
        return;
    ValueID condition = end.as<TerminatorKind::Branch>().conditionOperand();
    if (!condition || index.isParameter(condition) || index.opcodeOf(condition) != Opcode::Less)
        return;
    BlockID body = (*end.targets().begin()).block();
    if (!index.isInLoop(body, loop))
        return;

    uint32_t firstValueOfLoop = head.firstParameter().index();
    ValueID counter = index.stripCasts(index.value(condition).operand(0));
    ValueID length = index.stripCasts(index.value(condition).operand(1));
    bool countsInHead = counter.index() >= firstValueOfLoop && counter.index() - firstValueOfLoop < head.numberOfParameters();
    if (!countsInHead || index.isParameter(length) || index.opcodeOf(length) != Opcode::PropGet)
        return;
    BodyValue measured = index.value(length);
    if (measured.isFreeName() || program.nameClassOf(measured.key()) != NameClass::Length)
        return;
    ValueID string = index.stripCasts(measured.objectOperand());
    PrimitiveKnownToFolding start = primitiveKnownForStrings(walker, entering[counter.index() - firstValueOfLoop]);
    if (measured.objectOperand().index() >= firstValueOfLoop || start.kind() != PrimitiveKnownToFolding::Kind::Number || start.asNumber())
        return;

    Vector<ValueID, 8> numbers;
    auto isNumberAtFirst = [&](ValueID given) {
        ValueID operand = index.stripCasts(given);
        if (!index.isParameter(operand) && (index.opcodeOf(operand) == Opcode::Integer || index.opcodeOf(operand) == Opcode::Double))
            return true;
        if (numbers.contains(operand))
            return true;
        Reference known;
        if (operand.index() < firstValueOfLoop)
            known = walker.referenceOf(operand);
        else if (operand.index() - firstValueOfLoop < head.numberOfParameters())
            known = entering[operand.index() - firstValueOfLoop];
        return known && walker.typeOf(known) && walker.types().isSubset(walker.typeOf(known), TypeTable::number);
    };
    auto readsCharacter = [&](BodyValue value) {
        if (value.opcode() == Opcode::ElemGet)
            return index.stripCasts(value.objectOperand()) == string;
        if (value.opcode() != Opcode::Call || !value.thisValueOperand() || index.stripCasts(value.thisValueOperand()) != string)
            return false;
        PropertyKeyID key = value.as<Opcode::Call>().calleeKey();
        return key == program.keyOf(BuiltinNames::charCodeAt) || key == program.keyOf(BuiltinNames::charAt) || key == program.keyOf(BuiltinNames::codePointAt);
    };
    static constexpr OpcodeSet unseenOpcodes = opcodesThatStateTypes | OpcodeSet { Opcode::Integer, Opcode::Double, Opcode::Builtin };
    auto isUnseen = [&](BodyValue value) {
        if (unseenOpcodes.contains(value.opcode()))
            return true;
        if (value.opcode() == Opcode::PropGet)
            return !value.isFreeName() && index.stripCasts(value.objectOperand()) == string;
        ValueID user = index.onlyUsingValue(value.id());
        bool computes = numberProducingOperators.contains(value.opcode()) || isCallOfMathFunction(walker, index, value);
        if (!computes || !user || index.isParameter(user) || index.blockOf(user) != body)
            return false;
        bool takesNumbers = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            takesNumbers &= isNumberAtFirst(operand) || (!index.isParameter(operand) && index.opcodeOf(operand) == Opcode::Builtin);
        });
        if (takesNumbers)
            numbers.append(value.id());
        return takesNumbers;
    };
    bool headIsUnseen = std::ranges::all_of(head.values(), [&](BodyValue value) {
        return value.opcode() == Opcode::LoopHint || value.id() == condition || isUnseen(value);
    });
    bool isRead = false;
    for (BodyValue value : index.block(body).values()) {
        isRead = headIsUnseen && readsCharacter(value);
        if (isRead || !headIsUnseen || !isUnseen(value))
            break;
    }
    Reference name = isRead ? walker.referenceOf(measured.objectOperand()) : Reference();
    if (name && flatStringMadeWhereWalkStands(walker, name, origin))
        walker.count(StatisticID::StringsFlattenedBeforeGuardedLoops);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
