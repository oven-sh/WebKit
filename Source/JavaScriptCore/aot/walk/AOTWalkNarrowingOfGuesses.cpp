/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLayoutsByNames.h"
#include "AOTLinkTables.h"
#include "AOTOpcodeSets.h"
#include "AOTPlan.h"
#include "AOTResultRules.h"
#include "AOTSweep.h"
#include "AOTTypeNarrowing.h"
#include <algorithm>
#include <array>
#include <optional>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet readsOfPlacesInNarrowingOfGuesses { Opcode::PropGet, Opcode::PrivateGet, Opcode::GetModuleVariable, Opcode::GetEnvironmentField, Opcode::GetCapture };

static bool saysNothingInNarrowingOfGuesses(const TypeArena& types, TypeID type)
{
    if (!type || isNeverType(type))
        return true;
    return !holdsOnlyPrimitives(type) && types.at(type).hasUnboundedUnknown();
}

static TypeID typeSeenToArriveInNarrowingOfGuesses(const Walker& walker, uint32_t entryParameter)
{
    const LinkTables& held = walker.facts().held();
    FunctionID function = walker.currentFunction();
    if (function.index() >= held.functions.size())
        return { };
    std::span<const LinkedParameter> parameters = held.parametersOf(function);
    return entryParameter < parameters.size() ? parameters[entryParameter].proven : TypeID();
}

static void meetWithWhatWasSeenToArriveInNarrowingOfGuesses(Walker& walker, uint32_t entryParameter, TypeID& found)
{
    TypeArena& types = walker.types();
    Type written = types.at(found);
    if (!written.hasOnlyLayouts() || written.hasOnlyClosures() || holdsOneMember(written.layouts))
        return;
    TypeID seen = typeSeenToArriveInNarrowingOfGuesses(walker, entryParameter);
    if (saysNothingInNarrowingOfGuesses(types, seen))
        return;
    TypeID met = types.intersectionOf(found, seen);
    if (!isNeverType(met) && types.at(met).hasObjects())
        found = met;
}

static void meetWithWhatUnitExpectsOfItsOwnParameterInNarrowingOfGuesses(Walker& walker, ValueID input, TypeID& found)
{
    if (!walker.depthOfCopies())
        return;
    std::optional<uint32_t> ofUnit = walker.entryParameterOfUnitBoundTo(walker.referenceOf(input));
    if (!ofUnit)
        return;
    const LinkTables& link = walker.facts().linked();
    FunctionID unit = walker.unit().function;
    if (unit.index() >= link.functions.size())
        return;
    std::span<const LinkedParameter> parameters = link.parametersOf(unit);
    if (*ofUnit >= parameters.size() || !parameters[*ofUnit].expected)
        return;
    TypeArena& types = walker.types();
    TypeID met = types.intersectionOf(found, parameters[*ofUnit].expected);
    if (!isNeverType(met) && types.at(met).hasObjects())
        found = met;
}

static TypeID typeHeldOfReadInNarrowingOfGuesses(Walker& walker, const BodyValue& read, TypeID receiver)
{
    Opcode opcode = read.opcode();
    TypingContext typing { walker.program(), walker.types(), walker.heldPositions(), TypeLevel::Proven };
    Draft draft(opcode);
    draft.flags = static_cast<uint8_t>(read.flags());
    copyFields(read, draft, [](ValueID operand) {
        return Reference::value(operand);
    });

    std::optional<uint8_t> placeOfReceiver = receiverOf(opcode);
    Vector<TypeID, 8> operandTypes;
    operandTypes.fill(TypeID(), numberOfNamedOperandsOf(opcode) + read.variadicOperands().size());
    read.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        Reference built = walker.referenceOf(operand);
        bool isReceiver = placeOfReceiver && *placeOfReceiver == operandIndex;
        operandTypes[operandIndex] = receiver && isReceiver ? receiver : built ? walker.typeOf(built) : TypeID(TypeTable::anything);
    });
    return resultTypeOf(typing, draft, OperandTypes { operandTypes.span() });
}

static TypeID objectsBesideWrittenNullInNarrowingOfGuesses(Walker& walker, ValueID input, const BodyValue& definition, bool& isDeclared)
{
    TypeArena& types = walker.types();
    TypeID held;
    if (definition.isParameter()) {
        if (walker.currentBodyIndex().blockOf(input) == entryBlock)
            held = typeSeenToArriveInNarrowingOfGuesses(walker, input.index());
    } else if (readsOfPlacesInNarrowingOfGuesses.contains(definition.opcode())) {
        TypeID receiver;
        if (std::optional<uint8_t> place = receiverOf(definition.opcode())) {
            ValueID object = definition.operand(*place);
            Reference built = object ? walker.referenceOf(object) : Reference();
            receiver = built ? walker.typeOf(built) : TypeID();
            if (saysNothingInNarrowingOfGuesses(types, receiver))
                receiver = object && object < input ? findExpectation(walker, object).type : TypeID();
            if (!receiver)
                return { };
        }
        held = typeHeldOfReadInNarrowingOfGuesses(walker, definition, receiver);
    }
    if (!saysNothingInNarrowingOfGuesses(types, held) && types.at(held).hasObjects())
        return types.withOnlyObjects(held);

    LayoutID only = walker.facts().layoutsByNames().findOnlyLayoutWithAll(walker.namesUsedOff(input));
    if (!only)
        return { };
    isDeclared = false;
    return types.forLayout(only);
}

static bool nameSpeaksForLayoutInNarrowingOfGuesses(const Walker& walker, PropertyKeyID key)
{
    const Program& program = walker.program();
    if (liesInThread(key))
        return false;
    if (program.at(key).kind == PropertyKeyKind::Text && program.nameClassOf(key) != NameClass::Ordinary)
        return false;
    BuiltinName name = program.builtinNameOf(key);
    if (!name)
        return true;
    const BuiltinTable& builtins = program.builtins();
    for (BuiltinClassID builtinClass : builtins.classesWithInstanceMember(name)) {
        BuiltinMemberLookup member = builtins.findForInstance(builtinClass, name);
        if (member.outcome != BuiltinMemberLookup::Outcome::Found)
            return false;
        if (findFactRow(walker.facts().builtinUses(), builtins.canonicalID(member.member).index(), &BuiltinUseRecord::builtin))
            return false;
    }
    return true;
}

static bool someCastStatesLayoutInNarrowingOfGuesses(Walker& walker, ValueID input, LayoutID expected)
{
    TypeArena& types = walker.types();
    const BodyIndex& index = walker.currentBodyIndex();
    return std::ranges::any_of(walker.castsOf(input), [&](uint64_t cast) {
        TypeID written = index.value(ValueID(static_cast<uint32_t>(cast))).as<Opcode::Convert>().declared().written();
        return written && !holdsOnlyPrimitives(written) && singleLayoutOf(types, types.at(types.withoutNullish(written))) == expected;
    });
}

static bool oneBuiltinClassAnswersAllNamesInNarrowingOfGuesses(const Walker& walker, std::span<const PropertyKeyID> names)
{
    const Program& program = walker.program();
    const BuiltinTable& builtins = program.builtins();
    auto builtinNameOf = [&](PropertyKeyID key) {
        return liesInThread(key) ? BuiltinName() : program.builtinNameOf(key);
    };
    auto named = std::ranges::find_if(names, [&](PropertyKeyID key) { return !!builtinNameOf(key); });
    if (named == names.end())
        return true;
    return std::ranges::any_of(builtins.classesWithInstanceMember(builtinNameOf(*named)), [&](BuiltinClassID builtinClass) {
        return std::ranges::all_of(names, [&](PropertyKeyID key) {
            BuiltinName name = builtinNameOf(key);
            return !name || std::ranges::contains(builtins.classesWithInstanceMember(name), builtinClass);
        });
    });
}

static bool restsOnNamesThatBuiltinsAnswerInNarrowingOfGuesses(Walker& walker, ValueID input, const BodyValue& definition, TypeID found)
{
    TypeArena& types = walker.types();
    LayoutID expected = singleLayoutOf(types, types.at(found));
    if (!expected || (!definition.isParameter() && inputOperationsThatHandOn.contains(definition.opcode())))
        return false;
    if (definition.isParameter() && walker.currentBodyIndex().blockOf(input) == entryBlock && !saysNothingInNarrowingOfGuesses(types, typeSeenToArriveInNarrowingOfGuesses(walker, input.index())))
        return false;
    std::span<const PropertyKeyID> names = walker.namesUsedOff(input);
    if (names.empty() || walker.facts().layoutsByNames().findOnlyLayoutWithAll(names) != expected)
        return false;
    if (someCastStatesLayoutInNarrowingOfGuesses(walker, input, expected))
        return false;
    return std::ranges::none_of(names, [&](PropertyKeyID name) {
        return nameSpeaksForLayoutInNarrowingOfGuesses(walker, name);
    }) && oneBuiltinClassAnswersAllNamesInNarrowingOfGuesses(walker, names);
}

namespace {

struct EdgeThatFillsParameterInNarrowingOfExpectations {
    BodyIncomingEdge edge;
    BodyEdgeResult result;
};

} // anonymous namespace

static bool saysNothingInNarrowingOfExpectations(const TypeArena& types, TypeID type)
{
    if (!type || isNeverType(type))
        return true;
    return !holdsOnlyPrimitives(type) && types.at(type).hasUnboundedUnknown();
}

static std::optional<EdgeThatFillsParameterInNarrowingOfExpectations> findEdgeThatFillsInNarrowingOfExpectations(const BodyIndex& index, ValueID parameter)
{
    if (!parameter || !index.isParameter(parameter))
        return std::nullopt;
    std::optional<BodyIncomingEdge> edge = index.onlyIncomingEdge(index.blockOf(parameter));
    if (!edge)
        return std::nullopt;
    uint32_t numberOfProofs = static_cast<uint32_t>(edge->target.proofs().size());
    for (uint32_t proofIndex = 0; proofIndex < numberOfProofs; ++proofIndex) {
        std::optional<BodyEdgeResult> result = index.edgeResult(edge->from, edge->targetIndex, edge->target, proofIndex);
        if (result && result->parameter == parameter)
            return EdgeThatFillsParameterInNarrowingOfExpectations { *edge, *result };
    }
    return std::nullopt;
}

static TypeID tagWrittenInMemberInNarrowingOfExpectations(Walker& walker, LayoutID member, PropertyKeyID key, bool asWritten)
{
    const Program& program = walker.program();
    std::optional<FieldReference> reference = findField(program, member, key);
    if (!reference)
        return { };
    const Field& field = fieldAt(program, *reference);
    if (field.form.contains(FieldForm::IsAccessor) || !field.flags.contains(FieldFlags::IsRequired))
        return { };
    TypeID written = field.declared.expected().predicted();
    if (saysNothingInNarrowingOfExpectations(walker.types(), written))
        return { };
    if (asWritten)
        return written;
    uint16_t added = 0;
    if (field.declared.has(PositionFlags::OmitsUndefined))
        added |= Primitive::Undefined;
    if (field.declared.has(PositionFlags::OmitsNull))
        added |= Primitive::Null;
    return added ? walker.types().withoutPrimitives(written, added) : written;
}

static void cutExpectationByTestedField(Walker& walker, ValueID input, TypeID& found, bool& isDeclared)
{
    const BodyIndex& index = walker.currentBodyIndex();
    BodyValue definition = index.value(input);
    if (definition.isParameter() || definition.opcode() != Opcode::NarrowByField)
        return;
    auto narrowing = definition.as<Opcode::NarrowByField>();
    std::optional<EdgeThatFillsParameterInNarrowingOfExpectations> filling = findEdgeThatFillsInNarrowingOfExpectations(index, narrowing.readOperand());
    if (!filling)
        return;

    TypeArena& types = walker.types();
    Reference builtRead = walker.referenceOf(narrowing.readOperand());
    TypeID provenOfRead = builtRead && builtRead.isRecord() ? walker.typeOf(builtRead) : TypeID();
    if (!provenOfRead || isNeverType(provenOfRead))
        return;

    TypingContext expecting { walker.program(), types, walker.positionsKnowingNothing(), TypeLevel::Expected };
    const BodyEdgeResult& result = filling->result;
    BodyValue test = result.test ? index.value(result.test) : BodyValue();
    Vector<TypeID, 8> typesOfTestOperands;
    if (result.test) {
        if (test.isParameter())
            return;
        typesOfTestOperands.fill(TypeID(), numberOfNamedOperandsOf(test.opcode()) + test.variadicOperands().size());
        test.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            Reference built = walker.referenceOf(operand);
            typesOfTestOperands[operandIndex] = built ? walker.typeOf(built) : TypeID(TypeTable::anything);
        });
        if (result.operandIndex >= typesOfTestOperands.size())
            return;
    }
    auto whatEdgeLeavesOf = [&](TypeID tag) {
        if (!result.test)
            return typeOfCaseResult(expecting, index.terminator(filling->edge.from), tag, filling->edge.targetIndex);
        typesOfTestOperands[result.operandIndex] = tag;
        return typeOfEdgeResult(expecting, test, OperandTypes { typesOfTestOperands.span() }, result.operandIndex, result.isOnTakenEdge ? EdgeSide::Taken : EdgeSide::NotTaken);
    };
    auto edgeLeavesSomethingOf = [&](TypeID tag) {
        TypeID left = whatEdgeLeavesOf(tag);
        return !left || (!isNeverType(left) && types.overlap(left, provenOfRead));
    };
    TypeID selected = whatEdgeLeavesOf(provenOfRead);
    bool selects = selected && !isNeverType(selected) && holdsOnlyPrimitives(selected) && !(types.at(selected).primitives & Primitive::UndefinedOrNull) && isSingleValue(types, types.at(selected));

    PropertyKeyID key = narrowing.key();
    BodyValue holder = index.value(narrowing.valueOperand());
    Reference builtHolder = walker.referenceOf(narrowing.valueOperand());
    std::array<TypeID, 3> whatHolderMayBe { holder.isParameter() ? holder.parameterType() : TypeID(), found, builtHolder ? walker.typeOf(builtHolder) : TypeID() };
    for (TypeID inFront : whatHolderMayBe) {
        if (saysNothingInNarrowingOfExpectations(types, inFront) || !types.at(inFront).hasOnlyLayouts() || types.at(inFront).hasOnlyClosures() || holdsOneMember(types.at(inFront).layouts))
            continue;
        if (!selects && Walker::keepsRoundsOver(types, types.at(inFront).layouts))
            continue;
        Vector<LayoutID, 16> members;
        members.append(membersOf(types, types.at(inFront).layouts));
        size_t numberOfMembers = members.size();
        members.removeAllMatching([&](LayoutID member) {
            TypeID tag = tagWrittenInMemberInNarrowingOfExpectations(walker, member, key, selects);
            return tag && !(selects ? types.overlap(tag, selected) : edgeLeavesSomethingOf(tag));
        });
        if (members.isEmpty() || members.size() == numberOfMembers)
            continue;
        if (inFront != found)
            isDeclared = true;
        found = types.forLayouts(members.span());
        return;
    }
}

void refineExpectationOfValue(Walker& walker, ValueID input, TypeID& found, bool& isDeclared)
{
    const BodyIndex& index = walker.currentBodyIndex();
    TypeArena& types = walker.types();
    BodyValue definition = index.value(input);
    if (!definition.isParameter() && definition.opcode() == Opcode::NarrowByField) {
        cutExpectationByTestedField(walker, input, found, isDeclared);
        return;
    }
    if (!found)
        return;

    if (types.at(found).hasOnlyPrimitives(Primitive::UndefinedOrNull)) {
        if (!isDeclared || isNeverType(found))
            return;
        if (TypeID objects = objectsBesideWrittenNullInNarrowingOfGuesses(walker, input, definition, isDeclared))
            found = types.unionOf(found, objects);
        return;
    }
    if (walker.depthOfCopies()) {
        bool statesType = !definition.isParameter() && opcodesThatStateTypes.contains(definition.opcode());
        if (ValueID behindCasts = statesType ? index.valueBehindCasts(input) : input; index.isParameter(behindCasts) && index.blockOf(behindCasts) == entryBlock)
            meetWithWhatUnitExpectsOfItsOwnParameterInNarrowingOfGuesses(walker, behindCasts, found);
    }
    if (isDeclared) {
        if (definition.isParameter() && index.blockOf(input) == entryBlock)
            meetWithWhatWasSeenToArriveInNarrowingOfGuesses(walker, input.index(), found);
        return;
    }
    if (restsOnNamesThatBuiltinsAnswerInNarrowingOfGuesses(walker, input, definition, found))
        found = { };
}

void bindNamesBehindFailedTest(Walker& walker, Reference value, Reference test, uint32_t operandIndexOfTest, std::span<const ValueID> names, Origin origin)
{
    if (!walker.has(Mechanism::TypedOperations) || walker.isUnreachable() || names.empty())
        return;
    if (!test || !test.isValue() || !value || !value.isValue())
        return;
    const Code& code = walker.code();
    ValueID root = rootOfCellInSweptCode(code, value.id());
    if (walker.state().isFresh(code.isConstant(root) ? Reference::constant(root) : Reference::value(root)))
        return;

    Draft narrowed(Opcode::Narrowed, origin);
    narrowed.setValueOperand(value);
    narrowed.setTestOperand(test);
    narrowed.setOperandIndex(operandIndexOfTest);
    narrowed.setIsOnTakenEdge(false);
    TypeID left = walker.resultTypeOf(narrowed);
    if (!left || left == code.at(value.id()).type)
        return;
    Reference refused = walker.build(WTF::move(narrowed));
    if (!refused)
        return;
    for (ValueID name : names)
        walker.bind(name, refused);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
