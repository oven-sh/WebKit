/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Narrowing for Guesses: what is expected of a value is cut by what the body has tested and by what was seen to arrive, a test of shapes leaves out the layouts that have none, and the side on which the one test of a guess fails is typed by the refusal.
// At a suspension and at a throw: nothing. An expectation claims nothing, and the name behind a failed test is a Narrowed like any other.
// Withheld: nobody asks. Each expectation is tested at its use, so any answer here is safe.

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

// ---- Two guesses at one value

// `function visit(node: Node)` where only an Identifier was seen to arrive: what both say. Two guesses that name objects and share none disagree, and what is written stays.
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

// The rule of the row of a read over what Link holds of the places, with `receiver` in the place of the receiver.
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

// `this.next = null` in a constructor function: what is written of the field names no object, so of the objects that come to lie there it says nothing.
// What is held of the place names them, or the names that are asked of the value do. Null: nobody names any.
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

// ---- Names that built-ins answer

// `e.then`, `x.constructor`, `list.length`, `entry.value`: a member of one of the engine's classes that reached code may come by answers to the name, so the name speaks for no layout of the program. It still tells layouts apart.
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

// The weakest guess: one layout in use has all the names that the body asks of the value. True: that is all that speaks for `found`, and none of the names speaks for a layout.
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
    return std::ranges::none_of(names, [&](PropertyKeyID name) {
        return nameSpeaksForLayoutInNarrowingOfGuesses(walker, name);
    });
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

// Of a parameter that a test or a case fills, in a block that one edge enters: BodyIndex::valueNarrowedInto(), with the edge.
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

// `kind: "circle"`: what the source writes of the field in the member, whichever kind of position the front end makes of it. The undefined and null that it adds to every reference are none of that.
// Null: the member may lack the property, or answers by a getter. Such a member is told from nobody.
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

// `function area(s: Circle | Square) { if (s.kind === "circle") return s.radius; return s.side; }`, `switch (s.kind) { case "circle": .. }`: on each edge the members of whose written tag the test or the case leaves something.
// Each tag is asked by itself: the refusal of `=== "circle"` leaves nothing of "circle", though it takes nothing away from what is proven of the value that was read, which is anything.
// What is written of the holder is asked first: what is expected of it may have been cut to a stranger that has the names of all arms.
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

    // Whoever asks ahead of the walk finds nothing built for the value that was read.
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
    // `=== "circle"` has passed, `case "circle":`: of any tag at all the edge leaves ONE value. It SELECTS: a member stays where its written tag may be that value, and no type is made to ask.
    // Any other edge takes one value away. Of a wide union that leaves a wide union, at the price of a round with a new type for each member: it is left as it is.
    TypeID selected = whatEdgeLeavesOf(provenOfRead);
    bool selects = selected && !isNeverType(selected) && holdsOnlyPrimitives(selected) && !(types.at(selected).primitives & Primitive::UndefinedOrNull) && isSingleValue(types, types.at(selected));

    BodyValue holder = index.value(narrowing.valueOperand());
    Reference builtHolder = walker.referenceOf(narrowing.valueOperand());
    std::array<TypeID, 3> whatHolderMayBe { holder.isParameter() ? holder.parameterType() : TypeID(), found, builtHolder ? walker.typeOf(builtHolder) : TypeID() };
    for (TypeID inFront : whatHolderMayBe) {
        if (saysNothingInNarrowingOfExpectations(types, inFront) || !types.at(inFront).hasOnlyLayouts() || types.at(inFront).hasOnlyClosures() || holdsOneMember(types.at(inFront).layouts))
            continue;
        if (!selects && Walker::keepsRoundsOver(types, types.at(inFront).layouts))
            continue;
        // Asking about a member may make types and sets, so no set is read meanwhile.
        Vector<LayoutID, 16> members;
        members.append(membersOf(types, types.at(inFront).layouts));
        size_t numberOfMembers = members.size();
        members.removeAllMatching([&](LayoutID member) {
            TypeID tag = tagWrittenInMemberInNarrowingOfExpectations(walker, member, narrowing.key(), selects);
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
    if (isDeclared) {
        if (definition.isParameter() && index.blockOf(input) == entryBlock)
            meetWithWhatWasSeenToArriveInNarrowingOfGuesses(walker, input.index(), found);
        return;
    }
    if (restsOnNamesThatBuiltinsAnswerInNarrowingOfGuesses(walker, input, definition, found))
        found = { };
}

// ---- The test of a guess

// `function size(x: string | string[]) { return x.length }`, proven so: the guess tests for the lists. Where that is its only test the closed block has one edge, and what the refusal leaves of the value is a text.
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
