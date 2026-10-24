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
#include <algorithm>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

PassOutcome fuseConcatenations(FunctionPassContext& context)
{
    const Program& program = context.program;
    Function& function = context.function;
    FunctionAnalyses& analyses = context.analyses;
    IRBuilder builder(analyses.context(), function, context.types, &analyses);
    Vector<std::pair<BlockID, BitVector>, 2> absorbedValues;
    bool didMoveStrings = false;
    bool didDropParts = false;

    auto typeOf = [&](ValueID id) -> const Type& { return program.typeOf(function, id); };
    auto isText = [&](ValueID id) { return function.at(id).opcode == Opcode::String; };
    auto hasObjectOperand = [&](const Value& value) { return std::ranges::any_of(value.operands(), [&](ValueID operand) { return typeOf(operand).hasObjects(); }); };
    auto hasSymbolOperand = [&](const Value& value) { return std::ranges::any_of(value.operands(), [&](ValueID operand) { return !!(typeOf(operand).primitives & Primitive::AnySymbol); }); };
    auto isConcatenation = [&](const Value& value) {
        if (!concatenationOpcodes.contains(value.opcode) || !program.at(value.type).isString() || hasObjectOperand(value) || hasSymbolOperand(value))
            return false;
        return value.opcode == Opcode::Strcat || typeOf(value.leftOperand()).isAnyString() || typeOf(value.rightOperand()).isAnyString();
    };
    // A concatenation converts each of its parts as ToString does.
    auto isConversionOfPrimitive = [&](const Value& value) {
        return value.opcode == Opcode::ToString && (!hasObjectOperand(value) || context.build.isSabotaged(Sabotage::FusedConversionsRunCode)) && !hasSymbolOperand(value);
    };
    // No string is too long to be made of such parts, so the concatenation throws nothing and may run at any later time.
    auto hasBoundedLength = [&](const Value& value) {
        constexpr uint16_t bounded = Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::LiteralString;
        return std::ranges::all_of(value.operands(), [&](ValueID operand) { return typeOf(operand).hasOnlyPrimitives(bounded); });
    };
    // A key is looked up by its parts, and no string is made of a number among them.
    auto mayBecomeKey = [&](ValueID string) {
        const Value& value = function.at(string);
        if (Value::opcodesWithIsLikelyKey.contains(value.opcode) && value.isLikelyKey())
            return true;
        std::span<const Use> uses = analyses.useLists().usesOf(string);
        return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
            if (use.kind == Use::Kind::TerminatorOperand)
                return function.at(use.block).terminator.kind == TerminatorKind::Return;
            return isKeyOperand(program, function, use);
        });
    };
    // A concatenation fails for lack of memory alone. Which of two such operations fails first, nobody can tell: `a[i] + a[j] + a[k]` reads a character, which may have to be made, between two concatenations.
    auto hasObservableEffects = [&](ValueID id) { return !analyses.isRemovableWhenUnused(id); };
    auto hasObservableEffectsBetween = [&](BlockID id, uint32_t begin, uint32_t end) { return std::ranges::any_of(function.at(id).values.subspan(begin, end - begin), hasObservableEffects); };

    auto becomeConcatenationOf = [&](ValueID id, std::span<const ValueID> parts) {
        bool isLikelyKey = Value::opcodesWithIsLikelyKey.contains(function.at(id).opcode) && function.at(id).isLikelyKey();
        Value& concatenation = function.edit(id, ValueEdit::Definition);
        concatenation.become(Opcode::Strcat);
        concatenation.setVariadicOperands(parts);
        concatenation.setIsLikelyKey(isLikelyKey);
    };
    auto markAbsorbed = [&](ValueLocation location) {
        size_t index = absorbedValues.findIf([&](const auto& absorbed) { return absorbed.first == location.block; });
        if (index == notFound) {
            index = absorbedValues.size();
            absorbedValues.append({ location.block, BitVector() });
        }
        absorbedValues[index].second.set(location.index);
    };
    auto eraseAbsorbedValues = [&] {
        size_t numberOfAbsorbedValues = 0;
        for (const auto& [block, indexes] : std::exchange(absorbedValues, { })) {
            builder.eraseValues(block, indexes);
            numberOfAbsorbedValues += indexes.bitCount();
        }
        // The builder knows what it has erased, not the operands that went to whoever absorbed it, nor the copies that its other readers were handed.
        if (numberOfAbsorbedValues)
            analyses.didChangeValues();
        return numberOfAbsorbedValues;
    };

    // An access that fails its test reads the property by name and comes back, so the start of a key, `a.id + ","`, reaches the rest of the expression as a parameter of the block where the ways meet.
    auto makeStringsBehindMerges = [&] {
        // A parameter of a block that is a concatenation of one form on every edge into the block.
        struct MergedString {
            BlockID block;
            uint32_t parameterIndex { 0 };
            ValueID parameter;
            ValueID pattern;
            bool isValid { true };
            bool isWanted { false };
            Vector<std::pair<Edge, ValueID>, 4> incoming { };
            Vector<ValueID, 4> partParameters { };
        };
        const CFG& cfg = analyses.cfg();
        const UseLists& uses = analyses.useLists();
        Vector<MergedString, 4> merged;
        auto find = [&](ValueID parameter) -> MergedString* {
            size_t index = merged.findIf([&](const MergedString& known) { return known.parameter == parameter; });
            return index == notFound ? nullptr : &merged[index];
        };
        auto parameterAt = [&](const Use& use) { return function.at(targetOf(function, use.edge()).block).parameters[use.operandIndex]; };
        // Each part is a number or a string of the source text, so the concatenation gives the same string wherever it runs, and throws nothing. Nobody reads it where it is made.
        auto isMadeOnlyForEdges = [&](ValueID id) {
            const Value& value = function.at(id);
            if (!isConcatenation(value) || (value.opcode == Opcode::Strcat && value.isLikelyKey()))
                return false;
            if (!std::ranges::all_of(value.operands(), [&](ValueID part) { return isText(part) || typeOf(part).isNumber(); }) || uses.locationOf(id).placement != ValuePlacement::InBlock || !uses.useCount(id))
                return false;
            return std::ranges::all_of(uses.usesOf(id), [](const Use& use) { return use.kind == Use::Kind::EdgeArgument; });
        };
        auto haveSameForm = [&](ValueID first, ValueID second) {
            return std::ranges::equal(function.at(first).operands(), function.at(second).operands(), [&](ValueID a, ValueID b) {
                return isText(a) == isText(b) && (!isText(a) || function.at(a).string() == function.at(b).string());
            });
        };
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID id : function.at(block).values) {
                if (!isMadeOnlyForEdges(id))
                    continue;
                for (const Use& use : uses.usesOf(id)) {
                    if (!find(parameterAt(use)))
                        merged.append(MergedString { targetOf(function, use.edge()).block, use.operandIndex, parameterAt(use), id });
                }
            }
        }
        if (merged.isEmpty())
            return;

        // A block that nothing reaches any more stays until the blocks are compacted. What it passes to a merge has no form, and its edge takes arguments like any other.
        BitVector isEnteredFromDeadBlock(function.blocks.size());
        for (uint32_t index = 0; index < function.blocks.size(); ++index) {
            if (!cfg.isReachable(BlockID(index)))
                function.at(BlockID(index)).forEachTarget([&](const Target& target) { isEnteredFromDeadBlock.quickSet(target.block.index()); });
        }
        auto comesAsOneForm = [&](const MergedString& string) {
            const Block& block = function.at(string.block);
            if (block.role != BlockRole::Ordinary || isEnteredFromDeadBlock.quickGet(string.block.index()))
                return false;
            auto isReadByOwnHandler = [&](const Use& use) {
                if (use.block == string.block)
                    return use.kind == Use::Kind::HandlerArgument;
                return !analyses.dominators().terminatorDominates(string.block, use.block);
            };
            if (block.handler && std::ranges::any_of(uses.usesOf(string.parameter), isReadByOwnHandler))
                return false;
            for (Edge edge : cfg.incomingEdges(string.block)) {
                const Target& target = targetOf(function, edge);
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
                    return isConcatenation(function.at(use.user));
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
        merged.removeAllMatching([](const MergedString& string) { return !string.isValid; });
        if (merged.isEmpty())
            return;

        Vector<ValueID, 8> perhapsUnused;
        for (MergedString& string : merged) {
            for (Edge edge : cfg.incomingEdges(string.block)) {
                ValueID argument = targetOf(function, edge).arguments[string.parameterIndex];
                string.incoming.append({ edge, argument });
                if (function.at(argument).opcode != Opcode::Parameter)
                    perhapsUnused.appendIfNotContains(argument);
            }
        }
        for (MergedString& string : merged) {
            Value pattern(function.at(string.pattern));
            builder.setOrigin(pattern.origin());
            for (ValueID part : pattern.operands())
                string.partParameters.append(isText(part) ? ValueID() : builder.addParameter(string.block, TypeTable::number));
        }
        for (const MergedString& string : merged) {
            for (auto [edge, argument] : string.incoming) {
                const MergedString* other = find(argument);
                for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
                    if (string.partParameters[partIndex])
                        builder.appendArgument(edge, other ? other->partParameters[partIndex] : function.at(argument).operand(partIndex));
                }
            }
        }

        ValueSubstitution substitution(function);
        for (const MergedString& string : merged) {
            Value concatenation(function.at(string.pattern));
            builder.setOrigin(concatenation.origin());
            Vector<ValueID, 4> parts;
            for (uint32_t partIndex = 0; partIndex < string.partParameters.size(); ++partIndex) {
                if (ValueID parameter = string.partParameters[partIndex])
                    parts.append(parameter);
                else
                    parts.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), Value(function.at(concatenation.operand(partIndex)))));
            }
            concatenation.become(Opcode::Strcat);
            concatenation.setVariadicOperands(parts.span());
            perhapsUnused.append(builder.insertBefore(ProgramPoint::atHeadOf(string.block), WTF::move(concatenation)));
            substitution.replace(string.parameter, perhapsUnused.last());
        }
        builder.applyInsertions();
        substitution.apply(function, &analyses);
        for (size_t index = 0; index < merged.size(); ++index) {
            BlockID block = merged[index].block;
            if (merged.findIf([&](const MergedString& string) { return string.block == block; }) != index)
                continue;
            BitVector parameterIndexes;
            for (const MergedString& string : merged) {
                if (string.block == block)
                    parameterIndexes.set(string.parameterIndex);
            }
            builder.removeParameters(block, parameterIndexes);
        }

        // First the concatenations that nothing is passed any more, then the texts that only they held.
        for (unsigned round = 0; round < 2; ++round) {
            const UseLists& usesNow = analyses.useLists();
            Vector<ValueID, 8> parts;
            for (ValueID id : perhapsUnused) {
                if (usesNow.useCount(id))
                    continue;
                for (ValueID part : function.at(id).operands()) {
                    if (isText(part))
                        parts.appendIfNotContains(part);
                }
                markAbsorbed(usesNow.locationOf(id));
            }
            eraseAbsorbedValues();
            perhapsUnused = WTF::move(parts);
        }
        didMoveStrings = true;
    };
    makeStringsBehindMerges();

    // `${a.id},${b.id}`: an access that fails its test reads the property by name and comes back, so each number reaches the template converted, as a parameter of the block where the ways meet.
    struct Passed {
        Edge edge;
        ValueLocation parameter;
        ValueID conversion;
    };
    Vector<Passed, 8> passed;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            // An Add of what may be two numbers would add them.
            if (function.at(id).opcode != Opcode::Strcat)
                continue;
            for (ValueID operand : function.at(id).operands()) {
                if (function.at(operand).opcode != Opcode::Parameter)
                    continue;
                const UseLists& uses = analyses.useLists();
                const CFG& cfg = analyses.cfg();
                ValueLocation location = uses.locationOf(operand);
                if (location.placement != ValuePlacement::Parameter || uses.useCount(operand) != 1 || function.at(location.block).role != BlockRole::Ordinary || !mayBecomeKey(id))
                    continue;
                size_t numberBefore = passed.size();
                for (Edge edge : cfg.incomingEdges(location.block)) {
                    if (edge.isHandlerEdge() || incomingValueOf(function, edge, location.index).kind != IncomingValue::Kind::Argument) {
                        passed.shrink(numberBefore);
                        break;
                    }
                    ValueID argument = targetOf(function, edge).arguments[location.index];
                    if (isConversionOfPrimitive(function.at(argument)) && uses.useCount(argument) == 1)
                        passed.append({ edge, location, argument });
                }
            }
        }
    }
    if (!passed.isEmpty()) {
        const UseLists& uses = analyses.useLists();
        for (const Passed& change : passed) {
            ValueID primitive = function.at(change.conversion).operand(0);
            editTarget(function, change.edge, BlockEdit::Arguments).arguments[change.parameter.index] = primitive;
            Value& parameter = function.edit(function.at(change.parameter.block).parameters[change.parameter.index], ValueEdit::Type);
            parameter.type = context.types.unionOf(parameter.type, function.at(primitive).type);
            markAbsorbed(uses.locationOf(change.conversion));
        }
        eraseAbsorbedValues();
        didMoveStrings = true;
    }

    auto findAbsorbableOperand = [&](ProgramPoint outerPoint, ValueID operand, uint32_t limit, const BitVector& absorbed, size_t numberOfOtherParts) -> std::optional<uint32_t> {
        const Block& block = function.at(outerPoint.block);
        const Value& inner = function.at(operand);
        // Nobody can tell when a primitive is converted, so what runs between the conversion and the concatenation does not matter.
        bool isConversion = isConversionOfPrimitive(inner) && mayBecomeKey(block.values[outerPoint.index]);
        if (!isConcatenation(inner) && !isConversion)
            return std::nullopt;
        bool mayRunAtAnyLaterTime = isConversion || hasBoundedLength(inner);

        const UseLists& uses = analyses.useLists();
        ValueLocation location = uses.locationOf(operand);
        if (uses.useCount(operand) != 1 || location.block != outerPoint.block || location.index >= (mayRunAtAnyLaterTime ? outerPoint.index : limit))
            return std::nullopt;
        for (uint32_t index = limit; !mayRunAtAnyLaterTime && --index > location.index;) {
            if (!absorbed.get(index) && hasObservableEffects(block.values[index]))
                return std::nullopt;
        }
        if (context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), block.values[outerPoint.index]))
            return std::nullopt;
        return location.index;
    };
    for (size_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector absorbed;
        for (uint32_t outerIndex = 0; outerIndex < values.size(); ++outerIndex) {
            const Value& outer = function.at(values[outerIndex]);
            if (!isConcatenation(outer))
                continue;
            Vector<ValueID, 16> parts;
            uint32_t limit = outerIndex;
            bool didAbsorb = false;
            for (uint32_t operandIndex = outer.numberOfOperands(); operandIndex--;) {
                ValueID operand = outer.operand(operandIndex);
                std::optional<uint32_t> found = findAbsorbableOperand({ BlockID(blockIndex), outerIndex }, operand, limit, absorbed, parts.size() + operandIndex);
                if (!found) {
                    parts.append(operand);
                    continue;
                }
                const Value& inner = function.at(operand);
                for (uint32_t partIndex = inner.numberOfOperands(); partIndex--;)
                    parts.append(inner.operand(partIndex));
                absorbed.set(*found);
                didAbsorb = true;
                if (inner.opcode != Opcode::ToString && !hasBoundedLength(inner))
                    limit = *found;
            }
            parts.reverse();
            if (didAbsorb)
                becomeConcatenationOf(values[outerIndex], parts.span());

            // A concatenation converts each of its parts by itself, so the constant without characters adds nothing to it: "" + x is x as a string.
            size_t numberOfEmptyParts = parts.removeAllMatching([&](ValueID part) {
                KnownValue known = knownValueOf(program, function, part);
                return known.kind() == KnownValue::Kind::String && known.asString() == program.strings.emptyString();
            });
            if (numberOfEmptyParts) {
                becomeConcatenationOf(values[outerIndex], parts.span());
                didDropParts = true;
            }
        }
        if (!absorbed.isEmpty())
            absorbedValues.append({ BlockID(blockIndex), WTF::move(absorbed) });
    }
    size_t numberOfAbsorbedValues = eraseAbsorbedValues();
    if (didDropParts)
        analyses.didChangeValues();

    // An edge that leaves the way from an absorbed concatenation to the one that absorbed it, and passes the absorbed one on.
    Vector<std::pair<Edge, ValueID>, 4> copiesOnEdges;
    // A reader of the absorbed concatenation in a block beside that way.
    Vector<std::pair<Use, ValueID>, 4> copiesForReaders;
    BitVector absorbedFromEarlierBlocks;
    // Tests of the parts stand between the concatenations of one expression, each with an edge to code for the part it did not expect.
    auto canAbsorbFromEarlierBlock = [&](ProgramPoint outerPoint, ValueID operand, size_t numberOfOtherParts) {
        const Value& inner = function.at(operand);
        if (!isConcatenation(inner))
            return false;
        const UseLists& uses = analyses.useLists();
        ValueLocation location = uses.locationOf(operand);
        if (location.placement != ValuePlacement::InBlock || location.block == outerPoint.block)
            return false;
        ValueID outer = function.at(outerPoint.block).values[outerPoint.index];
        if (context.exceeds(LimitID::MaxFusedConcatenationParts, numberOfOtherParts + inner.numberOfOperands(), outer))
            return false;
        if (std::ranges::any_of(inner.operands(), [&](ValueID part) { return absorbedFromEarlierBlocks.get(part.index()); }))
            return false;

        const CFG& cfg = analyses.cfg();
        bool mayThrow = !hasBoundedLength(inner);
        Vector<Edge, 8> way;
        for (BlockID block = outerPoint.block; block != location.block; block = way.last().from) {
            std::span<const Edge> incoming = cfg.incomingEdges(block);
            if (incoming.size() != 1 || incoming[0].isHandlerEdge() || function.at(block).handler || way.size() >= context.limit(LimitID::MaxBlocksBetweenFusedConcatenations))
                return false;
            if (mayThrow && hasObservableEffectsBetween(block, 0, block == outerPoint.block ? outerPoint.index : function.at(block).values.size()))
                return false;
            way.append(incoming[0]);
        }
        if (function.at(location.block).handler || (mayThrow && hasObservableEffectsBetween(location.block, location.index + 1, function.at(location.block).values.size())))
            return false;
        constexpr TerminatorKindSet terminatorsOnTheWay { TerminatorKind::Jump, TerminatorKind::Branch, TerminatorKind::TestAndBranch };
        if (!std::ranges::all_of(way, [&](Edge edge) { return terminatorsOnTheWay.contains(function.at(edge.from).terminator.kind); }))
            return false;

        size_t numberOfCopiesOnEdges = copiesOnEdges.size();
        size_t numberOfCopiesForReaders = copiesForReaders.size();
        bool isUsedByOuter = false;
        bool canCopyForOthers = std::ranges::all_of(uses.usesOf(operand), [&](const Use& use) {
            if (use.kind == Use::Kind::Operand && absorbedFromEarlierBlocks.get(use.user.index()))
                return false;
            if (use.kind == Use::Kind::Operand && use.user == outer)
                return !std::exchange(isUsedByOuter, true);
            bool isOnTheWay = use.block == outerPoint.block || way.containsIf([&](Edge edge) { return edge.from == use.block; });
            if (!isOnTheWay) {
                const Block& block = function.at(use.block);
                if (mayThrow || block.handler || block.role != BlockRole::Ordinary || use.kind == Use::Kind::HandlerArgument || use.kind == Use::Kind::BlockSubject)
                    return false;
                copiesForReaders.append({ use, operand });
                return true;
            }
            if (use.kind != Use::Kind::EdgeArgument || use.block == outerPoint.block || way.contains(use.edge()))
                return false;
            copiesOnEdges.append({ use.edge(), operand });
            return true;
        });
        if (canCopyForOthers && isUsedByOuter)
            return true;
        copiesOnEdges.shrink(numberOfCopiesOnEdges);
        copiesForReaders.shrink(numberOfCopiesForReaders);
        return false;
    };
    // An inner concatenation has absorbed what it can before another absorbs it.
    for (BlockID block : analyses.cfg().reversePostOrder()) {
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& outer = function.at(values[index]);
            if (!isConcatenation(outer))
                continue;
            Vector<ValueID, 16> parts;
            bool didAbsorb = false;
            uint32_t numberOfOperands = outer.numberOfOperands();
            for (uint32_t operandIndex = 0; operandIndex < numberOfOperands; ++operandIndex) {
                ValueID operand = outer.operand(operandIndex);
                if (!canAbsorbFromEarlierBlock({ block, index }, operand, parts.size() + numberOfOperands - operandIndex - 1)) {
                    parts.append(operand);
                    continue;
                }
                parts.append(function.at(operand).operands());
                markAbsorbed(analyses.useLists().locationOf(operand));
                absorbedFromEarlierBlocks.set(operand.index());
                didAbsorb = true;
            }
            if (didAbsorb)
                becomeConcatenationOf(values[index], parts.span());
        }
    }

    if (!copiesForReaders.isEmpty()) {
        // A reader that has absorbed a concatenation since holds the original at another place.
        Vector<std::pair<Use, ValueID>, 4> readers;
        for (auto [reader, original] : copiesForReaders) {
            if (reader.kind != Use::Kind::Operand) {
                readers.append({ reader, original });
                continue;
            }
            std::span<const ValueID> operands = function.at(reader.user).operands();
            for (reader.operandIndex = 0; reader.operandIndex < operands.size(); ++reader.operandIndex) {
                if (operands[reader.operandIndex] == original)
                    readers.append({ reader, original });
            }
        }
        BitVector isDone;
        for (size_t first = 0; first < readers.size(); ++first) {
            if (isDone.get(first))
                continue;
            auto [reader, original] = readers[first];
            ValueID copy = builder.insertBefore(ProgramPoint::atHeadOf(reader.block), Value(function.at(original)));
            for (size_t index = first; index < readers.size(); ++index) {
                if (isDone.get(index) || readers[index].second != original || readers[index].first.block != reader.block)
                    continue;
                isDone.set(index);
                replaceUse(function, readers[index].first, copy);
            }
        }
        builder.applyInsertions();
    }

    Vector<Value, 4> originals = copiesOnEdges.map<Vector<Value, 4>>([&](const auto& copy) { return Value(function.at(copy.second)); });
    BitVector isDone;
    for (size_t first = 0; first < copiesOnEdges.size(); ++first) {
        if (isDone.get(first))
            continue;
        Edge edge = copiesOnEdges[first].first;
        BlockID added = builder.splitEdge(edge);
        for (size_t index = first; index < copiesOnEdges.size(); ++index) {
            ValueID original = copiesOnEdges[index].second;
            if (isDone.get(index) || copiesOnEdges[index].first != edge)
                continue;
            isDone.set(index);
            if (!function.at(added).terminator.targets[0].arguments.contains(original))
                continue;
            Arguments& arguments = function.edit(added, BlockEdit::Arguments).terminator.targets[0].arguments;
            std::ranges::replace(arguments, original, builder.insertBefore(ProgramPoint::atHeadOf(added), Value(originals[index])));
        }
        builder.applyInsertions();
    }

    // A copy for a reader stands at the head of the reader's block, and moves all that lies in that block.
    for (auto& [block, indexes] : absorbedValues) {
        indexes.clearAll();
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (absorbedFromEarlierBlocks.get(values[index].index()))
                indexes.set(index);
        }
    }
    numberOfAbsorbedValues += eraseAbsorbedValues();
    if (numberOfAbsorbedValues)
        context.build.statistics().count(StatisticID::ConcatenationsFused, numberOfAbsorbedValues);
    else if (!didMoveStrings && !didDropParts)
        return PassOutcome::Unchanged;
    // The analyses were told of every edit, so whatever they hold was made behind the last one.
    context.keptAnalyses = allAnalyses;
    return PassOutcome::Changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
