/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPass.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTFunctionAnalyses.h"
#include "AOTIRBuilder.h"
#include "AOTKnownValue.h"
#include "AOTStringBuilderAnalysis.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct AbsorbedValues {
    BlockID block;
    BitVector indexes;
};

// An edge that leaves the way from an absorbed concatenation to the one that absorbed it, and passes the absorbed one on.
struct CopyOnEdge {
    Edge edge;
    ValueID original;
};

// A reader of the absorbed concatenation in a block beside that way.
struct CopyForReader {
    Use reader;
    ValueID original;
};

struct Copies {
    Vector<CopyOnEdge, 4> onEdges;
    Vector<CopyForReader, 4> forReaders;
};

// A parameter of a block that is a concatenation of one form on every edge into the block.
struct MergedString {
    BlockID block;
    uint32_t parameterIndex { 0 };
    ValueID parameter;
    ValueID pattern;
    bool isValid { true };
    bool isWanted { false };
    Vector<ValueID, 4> partParameters;
};

class ConcatenationFuser {
public:
    explicit ConcatenationFuser(FunctionPassContext& context)
        : m_context(context)
        , m_program(context.program)
        , m_function(context.function)
    {
    }

    PassOutcome run();

private:
    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    bool hasObjectOperand(const Value&) const;
    bool hasSymbolOperand(const Value&) const;
    bool isConcatenation(const Value&) const;
    bool isConversionOfPrimitive(const Value&) const;
    bool mayBecomeKey(ValueID) const;
    bool hasObservableEffects(ValueID) const;

    std::optional<uint32_t> findAbsorbableOperand(ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts);
    void fuseOperands(ProgramPoint outerPoint, BitVector& absorbed);
    bool dropEmptyParts(ValueID);
    void fuseInBlock(BlockID);
    size_t eraseAbsorbedValues(IRBuilder&);

    bool hasBoundedLength(const Value&) const;
    bool hasObservableEffectsBetween(BlockID, uint32_t begin, uint32_t end) const;
    bool canAbsorbFromEarlierBlock(ProgramPoint outerPoint, ValueID operand, size_t numberOfOtherParts, Copies&);
    void fuseOperandsOfEarlierBlocks(ProgramPoint outerPoint);
    void markAbsorbed(ValueLocation);
    void findAbsorbedValuesWhereTheyLieNow();
    void copyOntoEdges(IRBuilder&);
    void findReadersAsTheyAreNow();
    void copyForReaders(IRBuilder&);

    bool isMadeOnlyForEdges(ValueID) const;
    bool haveSameForm(ValueID, ValueID) const;
    Vector<MergedString, 4> findMergedStrings();
    bool makeStringsBehindMerges(IRBuilder&);
    bool passPrimitivesToConcatenations(IRBuilder&);

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    Vector<AbsorbedValues, 2> m_absorbedValues;
    Vector<CopyOnEdge, 4> m_copiesOnEdges;
    Vector<CopyForReader, 4> m_copiesForReaders;
    BitVector m_absorbedFromEarlierBlocks;
    bool m_didDropParts { false };
};

bool ConcatenationFuser::hasObjectOperand(const Value& value) const
{
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (typeOf(value.operand(operandIndex)).hasObjects())
            return true;
    }
    return false;
}

bool ConcatenationFuser::hasSymbolOperand(const Value& value) const
{
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (typeOf(value.operand(operandIndex)).primitives & Primitive::AnySymbol)
            return true;
    }
    return false;
}

bool ConcatenationFuser::isConcatenation(const Value& value) const
{
    if (!concatenationOpcodes.contains(value.opcode) || !m_program.at(value.type).isString())
        return false;
    if (hasObjectOperand(value) || hasSymbolOperand(value))
        return false;
    return value.opcode == Opcode::Strcat || typeOf(value.leftOperand()).isAnyString() || typeOf(value.rightOperand()).isAnyString();
}

// A concatenation converts each of its parts as ToString does.
bool ConcatenationFuser::isConversionOfPrimitive(const Value& value) const
{
    return value.opcode == Opcode::ToString && !hasObjectOperand(value) && !hasSymbolOperand(value);
}

// A key is looked up by its parts, and no string is made of a number among them.
bool ConcatenationFuser::mayBecomeKey(ValueID string) const
{
    const Value& value = m_function.at(string);
    if (Value::opcodesWithIsLikelyKey.contains(value.opcode) && value.isLikelyKey())
        return true;
    std::span<const Use> uses = m_context.analyses.useLists().usesOf(string);
    return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
        if (use.kind == Use::Kind::TerminatorOperand)
            return m_function.at(use.block).terminator.kind == TerminatorKind::Return;
        return isKeyOperand(m_program, m_function, use);
    });
}

// A concatenation fails for lack of memory alone. Which of two such operations fails first, nobody can tell: `a[i] + a[j] + a[k]` reads a character, which may have to be made, between two concatenations.
bool ConcatenationFuser::hasObservableEffects(ValueID id) const
{
    return !m_context.analyses.isRemovableWhenUnused(id);
}

std::optional<uint32_t> ConcatenationFuser::findAbsorbableOperand(ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts)
{
    const Value& inner = m_function.at(operand);
    // Nobody can tell when a primitive is converted, so what runs between the conversion and the concatenation does not matter.
    bool isConversion = isConversionOfPrimitive(inner) && mayBecomeKey(m_function.at(outerPoint.block).values[outerPoint.index]);
    if (!isConcatenation(inner) && !isConversion)
        return std::nullopt;

    const UseLists& uses = m_context.analyses.useLists();
    if (uses.useCount(operand) != 1)
        return std::nullopt;

    ValueLocation location = uses.locationOf(operand);
    if (location.block != outerPoint.block || location.index >= (isConversion ? outerPoint.index : limit))
        return std::nullopt;

    const Block& block = m_function.at(outerPoint.block);
    for (uint32_t index = limit; !isConversion && --index > location.index;) {
        if (!absorbed.get(index) && hasObservableEffects(block.values[index]))
            return std::nullopt;
    }
    if (m_context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), block.values[outerPoint.index]))
        return std::nullopt;
    return location.index;
}

void ConcatenationFuser::fuseOperands(ProgramPoint outerPoint, BitVector& absorbed)
{
    ValueID outerID = m_function.at(outerPoint.block).values[outerPoint.index];
    const Value& outer = m_function.at(outerID);
    Vector<ValueID, 16> reversedParts;
    uint32_t limit = outerPoint.index;
    bool didAbsorb = false;
    for (uint32_t operandIndex = outer.numberOfOperands(); operandIndex--;) {
        ValueID operand = outer.operand(operandIndex);
        std::optional<uint32_t> found = findAbsorbableOperand(outerPoint, operand, limit, absorbed, reversedParts.size() + operandIndex);
        if (!found) {
            reversedParts.append(operand);
            continue;
        }
        const Value& inner = m_function.at(operand);
        for (uint32_t partIndex = inner.numberOfOperands(); partIndex--;)
            reversedParts.append(inner.operand(partIndex));
        absorbed.set(*found);
        didAbsorb = true;
        if (inner.opcode != Opcode::ToString)
            limit = *found;
    }
    if (!didAbsorb)
        return;

    reversedParts.reverse();
    bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(outer.opcode) && outer.isLikelyKey();
    Value& fused = m_function.edit(outerID, ValueEdit::Definition);
    fused.become(Opcode::Strcat);
    fused.setVariadicOperands(reversedParts.span());
    fused.setIsLikelyKey(isLikelyKey);
}

// A concatenation converts each of its parts by itself, so the constant without characters adds nothing to it: "" + x is x as a string.
bool ConcatenationFuser::dropEmptyParts(ValueID id)
{
    const Value& value = m_function.at(id);
    Vector<ValueID, 16> parts;
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID operand = value.operand(operandIndex);
        KnownValue known = knownValueOf(m_program, m_function, operand);
        if (known.kind() != KnownValue::Kind::String || known.asString() != m_program.strings.emptyString())
            parts.append(operand);
    }
    if (parts.size() == numberOfOperands)
        return false;

    bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(value.opcode) && value.isLikelyKey();
    Value& shortened = m_function.edit(id, ValueEdit::Definition);
    shortened.become(Opcode::Strcat);
    shortened.setVariadicOperands(parts.span());
    shortened.setIsLikelyKey(isLikelyKey);
    return true;
}

void ConcatenationFuser::fuseInBlock(BlockID id)
{
    const BlockValues& values = m_function.at(id).values;
    BitVector absorbed;
    for (uint32_t index = 0; index < values.size(); ++index) {
        if (!isConcatenation(m_function.at(values[index])))
            continue;
        fuseOperands({ id, index }, absorbed);
        m_didDropParts |= dropEmptyParts(values[index]);
    }
    if (!absorbed.isEmpty())
        m_absorbedValues.append(AbsorbedValues { id, WTF::move(absorbed) });
}

size_t ConcatenationFuser::eraseAbsorbedValues(IRBuilder& builder)
{
    size_t numberOfAbsorbedValues = 0;
    for (const AbsorbedValues& absorbed : std::exchange(m_absorbedValues, { })) {
        builder.eraseValues(absorbed.block, absorbed.indexes);
        numberOfAbsorbedValues += absorbed.indexes.bitCount();
    }
    // The builder knows what it has erased, not the operands that went to whoever absorbed it, nor the copies that its other readers were handed.
    if (numberOfAbsorbedValues)
        m_context.analyses.didChangeValues();
    return numberOfAbsorbedValues;
}

// No string is too long to be made of such parts, so the concatenation throws nothing and may run at any later time.
bool ConcatenationFuser::hasBoundedLength(const Value& value) const
{
    constexpr uint16_t bounded = Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::LiteralString;
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        if (!typeOf(value.operand(operandIndex)).hasOnlyPrimitives(bounded))
            return false;
    }
    return true;
}

bool ConcatenationFuser::hasObservableEffectsBetween(BlockID id, uint32_t begin, uint32_t end) const
{
    const BlockValues& values = m_function.at(id).values;
    for (uint32_t index = begin; index < end; ++index) {
        if (hasObservableEffects(values[index]))
            return true;
    }
    return false;
}

// Tests of the parts stand between the concatenations of one expression, each with an edge to code for the part it did not expect.
bool ConcatenationFuser::canAbsorbFromEarlierBlock(ProgramPoint outerPoint, ValueID operand, size_t numberOfOtherParts, Copies& copies)
{
    const Value& inner = m_function.at(operand);
    if (!isConcatenation(inner))
        return false;
    const UseLists& uses = m_context.analyses.useLists();
    ValueLocation location = uses.locationOf(operand);
    if (location.placement != ValuePlacement::InBlock || location.block == outerPoint.block)
        return false;
    ValueID outer = m_function.at(outerPoint.block).values[outerPoint.index];
    if (m_context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), outer))
        return false;
    uint32_t numberOfParts = inner.numberOfOperands();
    for (uint32_t partIndex = 0; partIndex < numberOfParts; ++partIndex) {
        if (m_absorbedFromEarlierBlocks.get(inner.operand(partIndex).index()))
            return false;
    }

    const CFG& cfg = m_context.analyses.cfg();
    bool mayThrow = !hasBoundedLength(inner);
    Vector<Edge, 8> way;
    for (BlockID block = outerPoint.block; block != location.block; block = way.last().from) {
        std::span<const Edge> incoming = cfg.incomingEdges(block);
        if (incoming.size() != 1 || incoming[0].isHandlerEdge() || m_function.at(block).handler)
            return false;
        if (way.size() >= m_context.limit(LimitID::MaxBlocksBetweenFusedConcatenations))
            return false;
        if (mayThrow && hasObservableEffectsBetween(block, 0, block == outerPoint.block ? outerPoint.index : m_function.at(block).values.size()))
            return false;
        way.append(incoming[0]);
    }
    if (m_function.at(location.block).handler)
        return false;
    if (mayThrow && hasObservableEffectsBetween(location.block, location.index + 1, m_function.at(location.block).values.size()))
        return false;
    for (Edge edge : way) {
        TerminatorKind kind = m_function.at(edge.from).terminator.kind;
        if (kind != TerminatorKind::Jump && kind != TerminatorKind::Branch && kind != TerminatorKind::TestAndBranch)
            return false;
    }

    bool isUsedByOuter = false;
    for (const Use& use : uses.usesOf(operand)) {
        if (use.kind == Use::Kind::Operand && m_absorbedFromEarlierBlocks.get(use.user.index()))
            return false;
        if (use.kind == Use::Kind::Operand && use.user == outer) {
            if (std::exchange(isUsedByOuter, true))
                return false;
            continue;
        }
        bool isOnTheWay = use.block == outerPoint.block || way.containsIf([&](Edge edge) { return edge.from == use.block; });
        if (!isOnTheWay) {
            const Block& block = m_function.at(use.block);
            if (mayThrow || block.handler || block.role != BlockRole::Ordinary || use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
                return false;
            copies.forReaders.append(CopyForReader { use, operand });
            continue;
        }
        if (use.kind != Use::Kind::EdgeArgument || use.block == outerPoint.block || way.contains(use.edge()))
            return false;
        copies.onEdges.append(CopyOnEdge { use.edge(), operand });
    }
    return isUsedByOuter;
}

void ConcatenationFuser::markAbsorbed(ValueLocation location)
{
    for (AbsorbedValues& absorbed : m_absorbedValues) {
        if (absorbed.block == location.block) {
            absorbed.indexes.set(location.index);
            return;
        }
    }
    m_absorbedValues.append(AbsorbedValues { location.block, { } });
    m_absorbedValues.last().indexes.set(location.index);
}

// A copy for a reader stands at the head of the reader's block, and moves all that lies in that block.
void ConcatenationFuser::findAbsorbedValuesWhereTheyLieNow()
{
    for (AbsorbedValues& absorbed : m_absorbedValues) {
        absorbed.indexes.clearAll();
        const BlockValues& values = m_function.at(absorbed.block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_absorbedFromEarlierBlocks.get(values[index].index()))
                absorbed.indexes.set(index);
        }
    }
}

void ConcatenationFuser::fuseOperandsOfEarlierBlocks(ProgramPoint outerPoint)
{
    ValueID outerID = m_function.at(outerPoint.block).values[outerPoint.index];
    const Value& outer = m_function.at(outerID);
    Vector<ValueID, 16> parts;
    bool didAbsorb = false;
    uint32_t numberOfOperands = outer.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID operand = outer.operand(operandIndex);
        Copies copies;
        if (!canAbsorbFromEarlierBlock(outerPoint, operand, parts.size() + numberOfOperands - operandIndex - 1, copies)) {
            parts.append(operand);
            continue;
        }
        const Value& inner = m_function.at(operand);
        for (uint32_t partIndex = 0; partIndex < inner.numberOfOperands(); ++partIndex)
            parts.append(inner.operand(partIndex));
        m_copiesOnEdges.appendVector(copies.onEdges);
        m_copiesForReaders.appendVector(copies.forReaders);
        markAbsorbed(m_context.analyses.useLists().locationOf(operand));
        m_absorbedFromEarlierBlocks.set(operand.index());
        didAbsorb = true;
    }
    if (!didAbsorb)
        return;

    bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(outer.opcode) && outer.isLikelyKey();
    Value& fused = m_function.edit(outerID, ValueEdit::Definition);
    fused.become(Opcode::Strcat);
    fused.setVariadicOperands(parts.span());
    fused.setIsLikelyKey(isLikelyKey);
}

void ConcatenationFuser::findReadersAsTheyAreNow()
{
    Vector<CopyForReader, 4> readers;
    for (const CopyForReader& listed : m_copiesForReaders) {
        if (listed.reader.kind != Use::Kind::Operand) {
            readers.append(listed);
            continue;
        }
        const Value& user = m_function.at(listed.reader.user);
        uint32_t numberOfOperands = user.numberOfOperands();
        for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
            if (user.operand(operandIndex) != listed.original)
                continue;
            Use reader = listed.reader;
            reader.operandIndex = operandIndex;
            readers.append(CopyForReader { reader, listed.original });
        }
    }
    m_copiesForReaders = WTF::move(readers);
}

void ConcatenationFuser::copyForReaders(IRBuilder& builder)
{
    if (m_copiesForReaders.isEmpty())
        return;
    findReadersAsTheyAreNow();
    BitVector isDone;
    for (size_t first = 0; first < m_copiesForReaders.size(); ++first) {
        if (isDone.get(first))
            continue;
        auto [reader, original] = m_copiesForReaders[first];
        ValueID copy = builder.insertBefore(ProgramPoint::atHeadOf(reader.block), Value(m_function.at(original)));
        for (size_t index = first; index < m_copiesForReaders.size(); ++index) {
            const CopyForReader& other = m_copiesForReaders[index];
            if (isDone.get(index) || other.original != original || other.reader.block != reader.block)
                continue;
            isDone.set(index);
            replaceUse(m_function, other.reader, copy);
        }
    }
    builder.applyInsertions();
}

void ConcatenationFuser::copyOntoEdges(IRBuilder& builder)
{
    Vector<Value, 4> originals = m_copiesOnEdges.map<Vector<Value, 4>>([&](const CopyOnEdge& copy) {
        return Value(m_function.at(copy.original));
    });
    BitVector isDone;
    for (size_t first = 0; first < m_copiesOnEdges.size(); ++first) {
        if (isDone.get(first))
            continue;
        Edge edge = m_copiesOnEdges[first].edge;
        BlockID added = builder.splitEdge(edge);
        for (size_t index = first; index < m_copiesOnEdges.size(); ++index) {
            if (isDone.get(index) || m_copiesOnEdges[index].edge != edge)
                continue;
            isDone.set(index);
            if (!std::ranges::contains(m_function.at(added).terminator.targets[0].arguments.span(), m_copiesOnEdges[index].original))
                continue;
            Arguments& arguments = m_function.edit(added, BlockEdit::Arguments).terminator.targets[0].arguments;
            ValueID copy = builder.insertBefore(ProgramPoint::atHeadOf(added), Value(originals[index]));
            for (uint32_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex) {
                if (arguments[argumentIndex] == m_copiesOnEdges[index].original)
                    arguments[argumentIndex] = copy;
            }
        }
        builder.applyInsertions();
    }
}

// Each part is a number or a string of the source text, so the concatenation gives the same string wherever it runs, and throws nothing. Nobody reads it where it is made.
bool ConcatenationFuser::isMadeOnlyForEdges(ValueID id) const
{
    const Value& value = m_function.at(id);
    if (!isConcatenation(value) || (value.opcode == Opcode::Strcat && value.isLikelyKey()))
        return false;
    uint32_t numberOfOperands = value.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        ValueID part = value.operand(operandIndex);
        if (m_function.at(part).opcode != Opcode::String && !typeOf(part).isNumber())
            return false;
    }
    const UseLists& uses = m_context.analyses.useLists();
    if (uses.locationOf(id).placement != ValuePlacement::InBlock || !uses.useCount(id))
        return false;
    return std::ranges::all_of(uses.usesOf(id), [](const Use& use) {
        return use.kind == Use::Kind::EdgeArgument;
    });
}

bool ConcatenationFuser::haveSameForm(ValueID first, ValueID second) const
{
    const Value& a = m_function.at(first);
    const Value& b = m_function.at(second);
    if (a.numberOfOperands() != b.numberOfOperands())
        return false;
    uint32_t numberOfOperands = a.numberOfOperands();
    for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
        const Value& partOfA = m_function.at(a.operand(operandIndex));
        const Value& partOfB = m_function.at(b.operand(operandIndex));
        if ((partOfA.opcode == Opcode::String) != (partOfB.opcode == Opcode::String))
            return false;
        if (partOfA.opcode == Opcode::String && partOfA.string() != partOfB.string())
            return false;
    }
    return true;
}

Vector<MergedString, 4> ConcatenationFuser::findMergedStrings()
{
    const CFG& cfg = m_context.analyses.cfg();
    const UseLists& uses = m_context.analyses.useLists();
    Vector<MergedString, 4> merged;
    auto find = [&](ValueID parameter) -> MergedString* {
        size_t index = merged.findIf([&](const MergedString& known) {
            return known.parameter == parameter;
        });
        return index == notFound ? nullptr : &merged[index];
    };
    auto parameterAt = [&](const Use& use) {
        return m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex];
    };

    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (!isMadeOnlyForEdges(id))
                continue;
            for (const Use& use : uses.usesOf(id)) {
                if (!find(parameterAt(use)))
                    merged.append(MergedString { targetOf(m_function, use.edge()).block, use.operandIndex, parameterAt(use), id, true, false, { } });
            }
        }
    }
    if (merged.isEmpty())
        return merged;

    // A block that nothing reaches any more stays until the blocks are compacted. What it passes to a merge has no form, and its edge takes arguments like any other.
    BitVector isEnteredFromDeadBlock(m_function.blocks.size());
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        if (cfg.isReachable(BlockID(index)))
            continue;
        forEachOutgoingEdge(m_function, BlockID(index), [&](Edge, const Target& target) {
            isEnteredFromDeadBlock.quickSet(target.block.index());
        });
    }

    auto comesAsOneForm = [&](const MergedString& string) {
        const Block& block = m_function.at(string.block);
        if (block.role != BlockRole::Ordinary || isEnteredFromDeadBlock.quickGet(string.block.index()))
            return false;
        for (Edge edge : cfg.incomingEdges(string.block)) {
            const Target& target = targetOf(m_function, edge);
            if (edge.isHandlerEdge() || string.parameterIndex >= target.arguments.size())
                return false;
            ValueID argument = target.arguments[string.parameterIndex];
            if (const MergedString* other = find(argument)) {
                if (!other->isValid || !haveSameForm(other->pattern, string.pattern))
                    return false;
                continue;
            }
            if (!isMadeOnlyForEdges(argument) || !haveSameForm(argument, string.pattern))
                return false;
            // It is made behind every merge that it reaches, or it stays where it is.
            for (const Use& use : uses.usesOf(argument)) {
                const MergedString* reached = find(parameterAt(use));
                if (!reached || !reached->isValid)
                    return false;
            }
        }
        return true;
    };
    // Behind the merge another concatenation takes the string in, there or behind a further merge. Otherwise nothing is won.
    auto isTakenIn = [&](const MergedString& string) {
        return std::ranges::any_of(uses.usesOf(string.parameter), [&](const Use& use) {
            if (use.kind == Use::Kind::Operand)
                return isConcatenation(m_function.at(use.user));
            if (use.kind != Use::Kind::EdgeArgument)
                return false;
            const MergedString* next = find(parameterAt(use));
            return next && next->isValid && next->isWanted;
        });
    };

    for (bool didChange = true; didChange;) {
        didChange = false;
        for (MergedString& string : merged)
            string.isWanted = false;
        for (bool didGrow = true; didGrow;) {
            didGrow = false;
            for (MergedString& string : merged) {
                if (string.isValid && !string.isWanted && isTakenIn(string)) {
                    string.isWanted = true;
                    didGrow = true;
                }
            }
        }
        for (MergedString& string : merged) {
            if (string.isValid && (!string.isWanted || !comesAsOneForm(string))) {
                string.isValid = false;
                didChange = true;
            }
        }
    }
    merged.removeAllMatching([](const MergedString& string) {
        return !string.isValid;
    });
    return merged;
}

// An access that fails its test reads the property by name and comes back, so the start of a key, `a.id + ","`, reaches the rest of the expression as a parameter of the block where the ways meet.
bool ConcatenationFuser::makeStringsBehindMerges(IRBuilder& builder)
{
    Vector<MergedString, 4> merged = findMergedStrings();
    if (merged.isEmpty())
        return false;

    struct Incoming {
        Edge edge;
        ValueID argument;
    };
    const CFG& cfg = m_context.analyses.cfg();
    Vector<Vector<Incoming, 4>, 4> incoming;
    Vector<ValueID, 8> perhapsUnused;
    for (const MergedString& string : merged) {
        incoming.append({ });
        for (Edge edge : cfg.incomingEdges(string.block)) {
            ValueID argument = targetOf(m_function, edge).arguments[string.parameterIndex];
            incoming.last().append({ edge, argument });
            if (m_function.at(argument).opcode != Opcode::Parameter && !perhapsUnused.contains(argument))
                perhapsUnused.append(argument);
        }
    }
    auto find = [&](ValueID parameter) -> const MergedString* {
        size_t index = merged.findIf([&](const MergedString& known) {
            return known.parameter == parameter;
        });
        return index == notFound ? nullptr : &merged[index];
    };

    for (MergedString& string : merged) {
        Value pattern(m_function.at(string.pattern));
        builder.setOrigin(pattern.origin());
        for (uint32_t partIndex = 0; partIndex < pattern.numberOfOperands(); ++partIndex) {
            bool isText = m_function.at(pattern.operand(partIndex)).opcode == Opcode::String;
            string.partParameters.append(isText ? ValueID() : builder.addParameter(string.block, TypeTable::number));
        }
    }
    for (size_t index = 0; index < merged.size(); ++index) {
        const MergedString& string = merged[index];
        for (const Incoming& from : incoming[index]) {
            const MergedString* other = find(from.argument);
            for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
                if (string.partParameters[partIndex])
                    builder.appendArgument(from.edge, other ? other->partParameters[partIndex] : m_function.at(from.argument).operand(partIndex));
            }
        }
    }

    ValueSubstitution substitution(m_function);
    for (const MergedString& string : merged) {
        Value concatenation(m_function.at(string.pattern));
        builder.setOrigin(concatenation.origin());
        Vector<ValueID, 4> parts;
        for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
            if (ValueID parameter = string.partParameters[partIndex])
                parts.append(parameter);
            else
                parts.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), Value(m_function.at(concatenation.operand(partIndex)))));
        }
        concatenation.become(Opcode::Strcat);
        concatenation.setVariadicOperands(parts.span());
        perhapsUnused.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), WTF::move(concatenation)));
        substitution.replace(string.parameter, perhapsUnused.last());
    }
    builder.applyInsertions();
    substitution.apply(m_function, &m_context.analyses);
    for (size_t index = 0; index < merged.size(); ++index) {
        BitVector parameterIndexes;
        for (const MergedString& string : merged) {
            if (string.block == merged[index].block)
                parameterIndexes.set(string.parameterIndex);
        }
        bool isFirstOfBlock = merged.findIf([&](const MergedString& string) { return string.block == merged[index].block; }) == index;
        if (isFirstOfBlock)
            builder.removeParameters(merged[index].block, parameterIndexes);
    }

    // First the concatenations that nothing is passed any more, then the texts that only they held.
    for (unsigned round = 0; round < 2; ++round) {
        const UseLists& uses = m_context.analyses.useLists();
        Vector<ValueID, 8> parts;
        for (ValueID id : perhapsUnused) {
            if (uses.useCount(id))
                continue;
            const Value& unused = m_function.at(id);
            for (uint32_t partIndex = 0; partIndex < unused.numberOfOperands(); ++partIndex) {
                ValueID part = unused.operand(partIndex);
                if (m_function.at(part).opcode == Opcode::String && !parts.contains(part))
                    parts.append(part);
            }
            markAbsorbed(uses.locationOf(id));
        }
        eraseAbsorbedValues(builder);
        perhapsUnused = WTF::move(parts);
    }
    return true;
}

// `${a.id},${b.id}`: an access that fails its test reads the property by name and comes back, so each number reaches the template converted, as a parameter of the block where the ways meet.
bool ConcatenationFuser::passPrimitivesToConcatenations(IRBuilder& builder)
{
    struct Passed {
        Edge edge;
        ValueLocation parameter;
        ValueID conversion;
    };
    Vector<Passed, 8> passed;
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values) {
            const Value& outer = m_function.at(id);
            // An Add of what may be two numbers would add them.
            if (outer.opcode != Opcode::Strcat)
                continue;
            for (uint32_t operandIndex = 0; operandIndex < outer.numberOfOperands(); ++operandIndex) {
                ValueID operand = outer.operand(operandIndex);
                if (m_function.at(operand).opcode != Opcode::Parameter)
                    continue;
                const UseLists& uses = m_context.analyses.useLists();
                const CFG& cfg = m_context.analyses.cfg();
                ValueLocation location = uses.locationOf(operand);
                if (location.placement != ValuePlacement::Parameter || uses.useCount(operand) != 1 || m_function.at(location.block).role != BlockRole::Ordinary || !mayBecomeKey(id))
                    continue;
                size_t numberBefore = passed.size();
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    if (edge.isHandlerEdge() || incomingValueOf(m_function, edge, location.index).kind != IncomingValue::Kind::Argument) {
                        passed.shrink(numberBefore);
                        break;
                    }
                    ValueID argument = targetOf(m_function, edge).arguments[location.index];
                    if (isConversionOfPrimitive(m_function.at(argument)) && uses.useCount(argument) == 1)
                        passed.append({ edge, location, argument });
                }
            }
        }
    }
    if (passed.isEmpty())
        return false;

    const UseLists& uses = m_context.analyses.useLists();
    for (const Passed& change : passed) {
        ValueID primitive = m_function.at(change.conversion).operand(0);
        editTarget(m_function, change.edge, BlockEdit::Arguments).arguments[change.parameter.index] = primitive;
        Value& parameter = m_function.edit(m_function.at(change.parameter.block).parameters[change.parameter.index], ValueEdit::Type);
        parameter.type = m_context.types.unionOf(parameter.type, m_function.at(primitive).type);
        markAbsorbed(uses.locationOf(change.conversion));
    }
    eraseAbsorbedValues(builder);
    return true;
}

PassOutcome ConcatenationFuser::run()
{
    IRBuilder builder(m_context.analyses.context(), m_function, m_context.types, &m_context.analyses);
    bool didMoveStrings = makeStringsBehindMerges(builder);
    didMoveStrings |= passPrimitivesToConcatenations(builder);

    for (size_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex)
        fuseInBlock(BlockID(blockIndex));

    size_t numberOfAbsorbedValues = eraseAbsorbedValues(builder);
    if (m_didDropParts)
        m_context.analyses.didChangeValues();

    // An inner concatenation has absorbed what it can before another absorbs it.
    for (BlockID block : m_context.analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (isConcatenation(m_function.at(values[index])))
                fuseOperandsOfEarlierBlocks({ block, index });
        }
    }
    copyForReaders(builder);
    copyOntoEdges(builder);
    findAbsorbedValuesWhereTheyLieNow();
    numberOfAbsorbedValues += eraseAbsorbedValues(builder);
    if (!numberOfAbsorbedValues)
        return didMoveStrings || m_didDropParts ? PassOutcome::Changed : PassOutcome::Unchanged;
    m_context.build.statistics().count(StatisticID::ConcatenationsFused, numberOfAbsorbedValues);
    return PassOutcome::Changed;
}

} // anonymous namespace

PassOutcome fuseConcatenations(FunctionPassContext& context)
{
    PassOutcome outcome = ConcatenationFuser(context).run();
    // The analyses were told of every edit, so whatever they hold was made behind the last one.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
