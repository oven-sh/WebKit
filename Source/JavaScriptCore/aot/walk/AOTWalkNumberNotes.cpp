/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// What the rules of numbers keep beside Code: two records that say one thing for good, and what was built in the one block that is appended to. No entry of WalkState.
// At a suspension: what was built in the block is forgotten, so nothing that no value of the body stands for lives across it. At a throw: nothing, a handler begins another block.
// Withheld: nobody notes, so nothing is found.

#include "config.h"
#include "AOTWalkNumberForms.h"

#if ENABLE(AOT)

#include "AOTFamilies.h"
#include "AOTOpcodeSets.h"
#include "AOTOperatorForms.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <wtf/HashFunctions.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

// AOTWalkTypedOperators.cpp
OperatorForm formAllowedForTypedOperator(Walker&, const Draft&);
// AOTWalkNumbers.cpp
Reference boxedUnlessNumbersAreUnboxed(Walker&, Reference, Origin);
void prepareOperandsForFormNumbers(Walker&, Draft&);

// A Narrowed stands where its test has passed, and a part of several results says something only beside the others.
// The row of a native function says what the function reads, and nobody checks what the row leaves out: two calls of one stay two calls.
static constexpr OpcodeSet opcodesBuiltAnewEachTimeInNumberNotes { Opcode::Parameter, Opcode::Narrowed, Opcode::ResultAt, Opcode::CallBuiltin, Opcode::CountExecution, Opcode::CheckProof };

NotesOfNumbers& notesOfNumbers(Walker& walker)
{
    std::unique_ptr<Walker::KeptForBuild>& kept = walker.keptByNumbers();
    if (!kept)
        kept = makeUniqueWithoutFastMallocCheck<NotesOfNumbers>();
    return static_cast<NotesOfNumbers&>(*kept);
}

void NotesOfNumbers::clear()
{
    if (!m_integersBesideRecords.isEmpty())
        m_integersBesideRecords.clear();
    if (!m_lengthsOfPieces.isEmpty())
        m_lengthsOfPieces.clear();
    forgetBlock();
    callBeingTyped = nullptr;
}

void NotesOfNumbers::noteIntegerBeside(ValueID record, Reference integer)
{
    if (record && integer.isRecord())
        m_integersBesideRecords.set(record.index(), integer.bits());
}

Reference NotesOfNumbers::integerBeside(ValueID record) const
{
    if (!record || m_integersBesideRecords.isEmpty())
        return { };
    auto found = m_integersBesideRecords.find(record.index());
    return found == m_integersBesideRecords.end() ? Reference() : Reference::fromBits(found->value);
}

void NotesOfNumbers::noteLengthOfPiece(ValueID piece, Reference numberOfCodeUnits)
{
    if (piece && numberOfCodeUnits.isRecord())
        m_lengthsOfPieces.set(piece.index(), numberOfCodeUnits.bits());
}

Reference NotesOfNumbers::lengthOfPiece(ValueID piece) const
{
    if (!piece || m_lengthsOfPieces.isEmpty())
        return { };
    auto found = m_lengthsOfPieces.find(piece.index());
    return found == m_lengthsOfPieces.end() ? Reference() : Reference::fromBits(found->value);
}

// ---- The one block that is appended to

void NotesOfNumbers::forgetBlock()
{
    m_block = { };
    if (!m_conversions.isEmpty())
        m_conversions.clear();
    m_operations.shrink(0);
    m_words.shrink(0);
    m_placesOfOperations.shrink(0);
    m_readsOfOperations = Effects::none().reads;
    m_timesReadsWereEnded = 1;
}

void NotesOfNumbers::standIn(BlockID block)
{
    if (block == m_block)
        return;
    forgetBlock();
    m_block = block;
}

Reference NotesOfNumbers::findConversion(BlockID block, Reference value, Representation wanted) const
{
    if (!block || block != m_block || m_conversions.isEmpty() || !value.isRecord())
        return { };
    auto found = m_conversions.find(keyOfConversion(value, wanted));
    return found == m_conversions.end() ? Reference() : Reference::fromBits(found->value);
}

void NotesOfNumbers::noteConversion(BlockID block, Reference value, Representation wanted, Reference converted)
{
    if (!block || !value.isRecord() || !converted.isRecord())
        return;
    standIn(block);
    m_conversions.set(keyOfConversion(value, wanted), converted.bits());
}

// The operands keep their order and their names: a Narrowed names an operand of its test by index, and an operation has the type that the names of its operands give it.
void NotesOfNumbers::collectWords(const Draft& draft, Vector<uint32_t, 24>& words)
{
    std::span<const uint32_t> payload = draft.payload();
    words.append(static_cast<uint32_t>(draft.opcode) | static_cast<uint32_t>(draft.representation) << 16 | static_cast<uint32_t>(draft.flags) << 24);
    words.append(static_cast<uint32_t>(payload.size()));
    words.append(payload);
    for (Reference operand : draft.operands())
        words.append(operand.bits());
}

unsigned NotesOfNumbers::hashOf(std::span<const uint32_t> words)
{
    unsigned hash = static_cast<unsigned>(words.size());
    for (uint32_t word : words)
        hash = WTF::pairIntHash(hash, word);
    return hash;
}

void NotesOfNumbers::growPlacesOfOperations()
{
    if ((m_operations.size() + 1) * 2 <= m_placesOfOperations.size())
        return;
    m_placesOfOperations.fill(noOperation, std::max<size_t>(32, m_placesOfOperations.size() * 2));
    size_t mask = m_placesOfOperations.size() - 1;
    for (uint32_t index = 0; index < m_operations.size(); ++index) {
        size_t place = m_operations[index].hash & mask;
        while (m_placesOfOperations[place] != noOperation)
            place = (place + 1) & mask;
        m_placesOfOperations[place] = index;
    }
}

Reference NotesOfNumbers::findOperation(BlockID block, const Draft& draft) const
{
    if (!block || block != m_block || m_operations.isEmpty())
        return { };

    Vector<uint32_t, 24> words;
    collectWords(draft, words);
    unsigned hash = hashOf(words.span());
    size_t mask = m_placesOfOperations.size() - 1;
    for (size_t place = hash & mask; m_placesOfOperations[place] != noOperation; place = (place + 1) & mask) {
        const OperationBuilt& operation = m_operations[m_placesOfOperations[place]];
        if (operation.hash != hash || operation.numberOfWords != words.size())
            continue;
        if (operation.readsAsOf && operation.readsAsOf != m_timesReadsWereEnded)
            continue;
        if (std::ranges::equal(m_words.span().subspan(operation.firstWord, operation.numberOfWords), words.span()))
            return operation.built;
    }
    return { };
}

// It changes nothing, so a second one answers as the first did while nothing that it reads is written. An allocation reads and writes the allocator's state, so two of them are never one.
void NotesOfNumbers::noteOperation(BlockID block, const Draft& draft, const Effects& effects, Reference built)
{
    Opcode opcode = draft.opcode;
    if (!block || !built.isValue() || opcodesBuiltAnewEachTimeInNumberNotes.contains(opcode) || boundToOf(opcode) != BoundTo::Nothing)
        return;
    if (resultRuleOf(opcode) == ResultRule::None || resultRuleOf(opcode) == ResultRule::Allocation || interpretedOf(opcode) == Interpretation::Allocation)
        return;
    if (!effects.writes.isEmpty() || effects.mayThrow || !effects.alwaysCompletes() || effects.reads.containsAny(bookkeepingHeaps))
        return;

    standIn(block);
    growPlacesOfOperations();

    Vector<uint32_t, 24> words;
    collectWords(draft, words);
    OperationBuilt operation;
    operation.hash = hashOf(words.span());
    operation.firstWord = m_words.size();
    operation.numberOfWords = words.size();
    operation.built = built;
    if (!effects.reads.isEmpty()) {
        operation.readsAsOf = m_timesReadsWereEnded;
        m_readsOfOperations.merge(effects.reads);
    }
    m_words.appendVector(words);

    size_t mask = m_placesOfOperations.size() - 1;
    size_t place = operation.hash & mask;
    while (m_placesOfOperations[place] != noOperation)
        place = (place + 1) & mask;
    m_placesOfOperations[place] = m_operations.size();
    m_operations.append(operation);
}

// One count for all that read: a write that meets what any of them reads ends them all, so a write costs no look at the list.
void NotesOfNumbers::endWhatIsWritten(const Effects& effects)
{
    if (!m_block)
        return;
    if (effects.maySuspend) {
        forgetBlock();
        return;
    }
    if (m_readsOfOperations.isEmpty() || (!effects.mayRunOtherCode() && !effects.writes.overlaps(m_readsOfOperations)))
        return;
    ++m_timesReadsWereEnded;
    m_readsOfOperations = Effects::none().reads;
}

// ---- What the files of the package share

bool hasMechanismsOfNumberForms(const Walker& walker)
{
    return walker.has(Mechanism::TypedOperations) && walker.has(Mechanism::UnboxedNumbers);
}

// No type tells two doubles apart, and none names a made text: those two are read off the constant.
PrimitiveKnownToFolding primitiveKnownInNumberForms(const Walker& walker, Reference reference)
{
    if (!reference || !reference.isRecord())
        return { };
    const Operation& record = walker.code().at(reference.id());
    if (record.opcode == Opcode::Double)
        return PrimitiveKnownToFolding::number(record.number());
    if (record.opcode == Opcode::String)
        return PrimitiveKnownToFolding::text(record.string());

    TypeID id = walker.typeOf(reference);
    if (!id)
        return { };
    Type type = walker.types().at(id);
    if (!type.isJavaScriptValue() || type.hasObjects())
        return { };
    switch (type.primitives) {
    case Primitive::Undefined:
        return PrimitiveKnownToFolding::undefined();
    case Primitive::Null:
        return PrimitiveKnownToFolding::null();
    case Primitive::False:
        return PrimitiveKnownToFolding::boolean(false);
    case Primitive::True:
        return PrimitiveKnownToFolding::boolean(true);
    case Primitive::Integer:
        return type.integers.isSingleValue() ? PrimitiveKnownToFolding::integer(type.integers.minimum) : PrimitiveKnownToFolding();
    case Primitive::LiteralString: {
        StringID literal = singleStringLiteralOf(type);
        return literal ? PrimitiveKnownToFolding::text(literal) : PrimitiveKnownToFolding();
    }
    default:
        return { };
    }
}

std::optional<int64_t> integerKnownInNumberForms(const Walker& walker, Reference reference)
{
    TypeID id = reference && reference.isRecord() ? walker.typeOf(reference) : TypeID();
    if (!id)
        return std::nullopt;
    Type type = walker.types().at(id);
    if (!type.isInteger() || !type.integers.isSingleValue())
        return std::nullopt;
    return type.integers.minimum;
}

// A number has one constant: an integer that integer types hold is an Integer, any other number a Double.
Reference constantInNumberForms(Walker& walker, PrimitiveKnownToFolding primitive)
{
    Draft constant(Opcode::Undefined);
    switch (primitive.kind()) {
    case PrimitiveKnownToFolding::Kind::Undefined:
        break;
    case PrimitiveKnownToFolding::Kind::Null:
        constant.become(Opcode::Null);
        break;
    case PrimitiveKnownToFolding::Kind::Boolean:
        constant.become(Opcode::Boolean);
        constant.setBoolean(primitive.asBoolean());
        break;
    case PrimitiveKnownToFolding::Kind::Number:
        if (isTrackedInteger(primitive.asNumber())) {
            constant.become(Opcode::Integer);
            constant.setInteger(static_cast<int64_t>(primitive.asNumber()));
        } else {
            constant.become(Opcode::Double);
            constant.setNumber(primitive.asNumber());
        }
        break;
    case PrimitiveKnownToFolding::Kind::Text:
        constant.become(Opcode::String);
        constant.setString(primitive.asText());
        break;
    case PrimitiveKnownToFolding::Kind::Unknown:
        return { };
    }
    return walker.constant(WTF::move(constant));
}

Reference integerInNumberForms(Walker& walker, int64_t integer)
{
    Draft constant(Opcode::Integer);
    constant.setInteger(integer);
    return walker.constant(WTF::move(constant));
}

Reference booleanInNumberForms(Walker& walker, bool boolean)
{
    Draft constant(Opcode::Boolean);
    constant.setBoolean(boolean);
    return walker.constant(WTF::move(constant));
}

Reference buildInAllowedFormInNumberForms(Walker& walker, Draft&& draft)
{
    // Whoever was to build an operand stood nowhere.
    for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
        if (!draft.operand(operandIndex))
            return { };
    }
    // `h & done` in a loop, where the conversion of h is an operation of its own and the operator is built anew beside a boolean. ToNumber of a boolean, undefined or null runs no code.
    // What an edge behind a test narrows is an operand of the test, so a test keeps its operands.
    constexpr OpcodeSet convertsOperandsToNumbers = operatorsOfKinds({ OperatorKind::Additive, OperatorKind::Multiplicative, OperatorKind::Bitwise });
    if (convertsOperandsToNumbers.contains(draft.opcode)) {
        bool isEachNumberLike = true;
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
            Type type = walker.types().at(walker.typeOf(draft.operand(operandIndex)));
            isEachNumberLike &= !type.isNever() && type.isNumberLike();
        }
        if (isEachNumberLike)
            prepareOperandsForFormNumbers(walker, draft);
        for (uint32_t operandIndex = 0; operandIndex < draft.numberOfNamedOperands(); ++operandIndex) {
            if (!draft.operand(operandIndex))
                return { };
        }
    }
    walker.chooseConservativeFields(draft);
    draft.setForm(formAllowedForTypedOperator(walker, draft));
    Origin origin = draft.origin();
    return boxedUnlessNumbersAreUnboxed(walker, walker.build(WTF::move(draft)), origin);
}

Reference buildOperatorInNumberForms(Walker& walker, Opcode opcode, Reference left, Reference right, Origin origin)
{
    Draft draft(opcode, origin);
    draft.setOperand(0, left);
    if (right)
        draft.setOperand(1, right);
    return buildInAllowedFormInNumberForms(walker, WTF::move(draft));
}

// Only the condition of a Branch itself has such edges.
std::optional<uint32_t> operandsNarrowedBehindTestInNumberForms(const Walker& walker, ValueID test)
{
    const BodyIndex& index = walker.currentBodyIndex();
    if (index.isUnused(test))
        return 0;
    std::optional<BodyUser> user = index.onlyUser(test);
    if (!user)
        return std::nullopt;
    if (user->kind != BodyUseKind::TerminatorOperand)
        return 0;

    uint32_t operands = 0;
    for (BodyTarget target : index.terminator(user->block).targets()) {
        for (uint32_t operandIndex : target.proofs())
            operands |= 1u << (operandIndex & 31);
    }
    return operands;
}

void goOnWhereTestPassesInNumberForms(Walker& walker, Reference passes, BlockID otherwise)
{
    BlockID next = walker.addBlock();
    walker.branch(passes, next, otherwise);
    walker.continueIn(next);
}

Reference buildNarrowedInNumberForms(Walker& walker, Reference value, Reference test, uint32_t operandIndex, bool isOnTakenEdge, Origin origin)
{
    Draft proof(Opcode::Narrowed, origin);
    walker.chooseConservativeFields(proof);
    proof.setValueOperand(value);
    proof.setTestOperand(test);
    proof.setOperandIndex(operandIndex);
    proof.setIsOnTakenEdge(isOnTakenEdge);
    return walker.build(WTF::move(proof));
}

static Reference addParameterForWaysInNumberForms(Walker& walker, BlockID meeting, std::span<const WayInNumberForms> ways, Reference WayInNumberForms::* brought)
{
    TypeID type;
    Vector<Representation, 4> forms;
    for (const WayInNumberForms& way : ways) {
        if (!way.end || !(way.*brought))
            continue;
        TypeID arriving = walker.code().at((way.*brought).id()).type;
        type = type ? joinOf(walker.types(), walker.facts().families(), type, arriving) : arriving;
        forms.append(walker.representationOf(way.*brought));
    }
    return type ? walker.addParameter(meeting, type, walker.formOfParameter(type, forms.span())) : Reference();
}

Reference meetInNumberForms(Walker& walker, BlockID meeting, std::span<const WayInNumberForms> ways, Reference* metBeside)
{
    bool someWayArrives = std::ranges::any_of(ways, [](const WayInNumberForms& way) {
        return !!way.end;
    });
    if (!someWayArrives)
        return { };

    Reference met = addParameterForWaysInNumberForms(walker, meeting, ways, &WayInNumberForms::brought);
    Reference beside = met ? addParameterForWaysInNumberForms(walker, meeting, ways, &WayInNumberForms::broughtBeside) : Reference();
    for (const WayInNumberForms& way : ways) {
        if (!way.end)
            continue;
        walker.continueIn(way.end);
        std::array<Reference, 2> arguments { way.brought, way.broughtBeside };
        walker.jumpTo(meeting, std::span<const Reference>(arguments).first(beside ? 2 : met ? 1 : 0));
    }
    walker.continueIn(meeting);
    if (metBeside)
        *metBeside = beside;
    return met;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
