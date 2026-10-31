/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRecordUsers.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfOperands.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTThreshold.h"
#include <atomic>
#include <numeric>
#include <wtf/Atomics.h>
#include "AOTBitVector.h"

namespace JSC { namespace AOT { namespace Typed {

namespace {

using Mark = uint64_t;

constexpr unsigned numberOfKeyBits = 61;
constexpr Mark seenByAll = ~static_cast<Mark>(0);
constexpr Mark seenWithoutKey = static_cast<Mark>(1) << 63;
constexpr Mark seenInEnvironment = static_cast<Mark>(1) << 62;
constexpr Mark seenByCodeOfOperands = static_cast<Mark>(1) << 61;

constexpr Mark bitOfKey(PropertyKeyID key) { return static_cast<Mark>(1) << (key.index() % numberOfKeyBits); }

struct RecordNodes {
    uint32_t of(LayoutID id) const { return within(0, id.index(), firstLayoutSet); }
    uint32_t of(LayoutSetID id) const { return within(firstLayoutSet, id.index(), firstType); }
    uint32_t of(TypeID id) const { return within(firstType, id.index(), firstVariable); }
    uint32_t of(VariableID id) const { return firstVariable + id.index(); }

    static uint32_t within(uint32_t first, uint32_t index, uint32_t end)
    {
        RELEASE_ASSERT(index < end - first);
        return first + index;
    }

    uint32_t firstLayoutSet;
    uint32_t firstType;
    uint32_t firstVariable;
};

} // anonymous namespace

Vector<FunctionID> findFunctionsSeeing(const Program& program, const ChangedRecords& changed, std::span<const uint8_t> followsFieldsOfOperands, std::span<const uint8_t> isAsked, const ParallelLoops& loops)
{
    RELEASE_ASSERT(isAsked.size() == program.functions.size());
    if (std::ranges::none_of(isAsked, std::identity { }))
        return { };

    using Node = uint32_t;
    const Node firstLayoutSet = program.layouts.size();
    const Node firstType = firstLayoutSet + program.types.numberOfLayoutSets();
    const Node firstVariable = firstType + program.types.size();
    const RecordNodes nodes { firstLayoutSet, firstType, firstVariable };
    Vector<Mark> marks = createVectorInParallel<Mark>(loops, firstVariable + program.variables.size(), 0);
    Vector<uint8_t> isKeyOfMarkedField;
    Vector<FieldReference> fieldsOfEnvironments;

    auto markAt = [&](Node node) { return WTF::atomicLoad(&marks[node], std::memory_order_relaxed); };
    auto addAt = [&](Node node, Mark mark) { WTF::atomicStore(&marks[node], markAt(node) | mark, std::memory_order_relaxed); };
    auto searchInto = [&](Mark& found, bool looksAtTypes = true) {
        return [&, looksAtTypes](const auto& id) {
            if constexpr (requires { nodes.of(id); }) {
                if (looksAtTypes || !std::same_as<std::remove_cvref_t<decltype(id)>, TypeID>)
                    found |= markAt(nodes.of(id));
            }
        };
    };

    for (LayoutID id : changed.layouts)
        addAt(nodes.of(id), seenByAll);
    if (!changed.fields.empty()) {
        unsigned numberOfKeys = program.types.numberOfPropertyKeys();
        isKeyOfMarkedField.fill(0, numberOfKeys);
        Vector<uint8_t> isReadFromOperands(FillWith { }, numberOfKeys, static_cast<uint8_t>(0));
        for (BuiltinName name : namesReadFromOperands(program.builtins()))
            isReadFromOperands[program.keyOf(name).index()] = 1;

        for (FieldReference reference : changed.fields) {
            if (program.at(reference.layout).kind == LayoutKind::Environment) {
                fieldsOfEnvironments.append(reference);
                addAt(nodes.of(reference.layout), seenInEnvironment);
                continue;
            }
            PropertyKeyID key = fieldAt(program, reference).key;
            if (!key) {
                addAt(nodes.of(reference.layout), seenByAll);
                continue;
            }
            isKeyOfMarkedField[key.index()] = 1;
            addAt(nodes.of(reference.layout), isReadFromOperands[key.index()] ? bitOfKey(key) | seenByCodeOfOperands : bitOfKey(key));
        }
        AOT::sort(fieldsOfEnvironments);
    }
    for (VariableID id : changed.variables)
        addAt(nodes.of(id), seenByAll);

    auto forEachSourceOfSet = [&](const LayoutSet& set, const auto& functor) {
        for (LayoutID member : set.written())
            functor(nodes.of(member));
    };
    auto forEachSourceOfType = [&](const Type& type, const auto& functor) {
        if (type.environment)
            functor(nodes.of(type.environment));
        if (type.layouts)
            functor(nodes.of(type.layouts));
        for (const ClassPart& part : type.classes) {
            for (TypeID contents : part.writtenContents) {
                if (contents)
                    functor(nodes.of(contents));
            }
        }
    };
    auto forEachInheritedLayout = [&](LayoutID id, const auto& functor) {
        const Layout& layout = program.at(id);
        auto visitIfSet = [&](LayoutID other) {
            if (other)
                functor(nodes.of(other));
        };
        visitIfSet(layout.base);
        visitIfSet(layout.flow.base);
        visitIfSet(layout.classFlow.prototypeBase);
        for (LayoutID supertype : layout.supertypes)
            functor(nodes.of(supertype));
        for (const Field& field : layout.fields)
            visitIfSet(field.originLayout);
    };
    auto forEachLayoutStoodFor = [&](LayoutID id, const auto& functor) {
        for (LayoutID member : program.at(id).flow.members)
            functor(nodes.of(member));
        if (LayoutSetID members = program.declaredMembersOf(id))
            functor(nodes.of(members));
        for (LayoutID below : program.layoutsBelow(id))
            functor(nodes.of(below));
    };

    auto markOfTypesOf = [&](const Value& value) {
        Mark mark = 0;
        if (value.type)
            mark |= markAt(nodes.of(value.type));
        if (TypeID expected = value.expected().predicted())
            mark |= markAt(nodes.of(expected));
        return mark;
    };
    auto namesIn = [&](FunctionID id) {
        BodyIndex* index = program.bodyIndex();
        return index ? index->namesIn(program, id) : std::nullopt;
    };

    auto markOfAllNamedBy = [&](FunctionID id) {
        Mark found = 0;
        if (std::optional<NamesInBody> names = namesIn(id)) {
            auto search = searchInto(found);
            std::ranges::for_each(names->types, search);
            std::ranges::for_each(names->layouts, search);
            std::ranges::for_each(names->layoutSets, search);
            std::ranges::for_each(names->variables, search);
            return found;
        }

        const Function& function = program.functions[id.index()];
        forEachReference(static_cast<const FunctionRecord&>(function), searchInto(found));
        for (const Value& value : function.values) {
            if (found & seenWithoutKey)
                return found;
            found |= markOfTypesOf(value);
            forEachReference(value, searchInto(found));
        }
        for (const Block& block : function.blocks)
            forEachReference(block.terminator, searchInto(found));
        return found;
    };

    auto namesMarkedVariable = [&](FunctionID id) {
        bool isFound = false;
        auto search = [&](VariableID variable) {
            isFound |= !!markAt(nodes.of(variable));
        };
        if (std::optional<NamesInBody> names = namesIn(id)) {
            return std::ranges::any_of(names->variables, [&](VariableID variable) {
                return !!markAt(nodes.of(variable));
            });
        }

        const Function& function = program.functions[id.index()];
        forEachVariable(static_cast<const FunctionRecord&>(function), search);
        for (const Value& value : function.values) {
            if (isFound)
                return true;
            forEachVariable(value, search);
        }
        for (const Block& block : function.blocks)
            forEachVariable(block.terminator, search);
        return isFound;
    };

    auto markOfOperandsWhere = [&](const Function& function, const auto& instruction, const auto& runsCodeOfOperand) {
        Mark mark = 0;
        instruction.forEachOperand([&](ValueID operand, uint32_t index) {
            if (operand && runsCodeOfOperand(index))
                mark |= markOfTypesOf(function.at(operand));
        });
        return mark;
    };
    auto markOfOperandsWhoseCodeRuns = [&](const Function& function, const Value& value) -> Mark {
        switch (effectRuleOf(value.opcode)) {
        case EffectRule::Declared:
        case EffectRule::Operator:
        case EffectRule::Access:
            return markOfOperandsWhere(function, value, [&](uint32_t index) {
                return !protocolsOf(value.opcode, index).isEmpty();
            });
        case EffectRule::Call:
            if (!OperandPosition::newTarget(value.opcode) && calleesOfCall(program, value).listed().builtins.empty())
                return 0;
            [[fallthrough]];
        case EffectRule::Builtin:
        case EffectRule::Inline:
            return markOfOperandsWhere(function, value, [](uint32_t) {
                return true;
            });
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    auto markOfOperandsWhoseCodeRunsAtEnd = [&](const Function& function, const Terminator& terminator) {
        bool isInline = effectRuleOf(terminator.kind) == EffectRule::Inline;
        return markOfOperandsWhere(function, terminator, [&](uint32_t index) {
            return isInline || !protocolsOf(terminator.kind, index).isEmpty();
        });
    };

    auto valueLooksForMarkedField = [&](const Function& function, const Value& value, bool looksAtCodeOfOperands) {
        constexpr OpcodeSet accessesToOneFieldOfEnvironment { Opcode::GetEnvironmentField, Opcode::PutEnvironmentField };
        if (accessesToOneFieldOfEnvironment.contains(value.opcode))
            return std::ranges::binary_search(fieldsOfEnvironments, FieldReference { value.layout(), value.field() });

        bool hasKey = false;
        Mark markedKeys = 0;
        forEachReference(value, [&](const auto& id) {
            if constexpr (std::same_as<std::remove_cvref_t<decltype(id)>, PropertyKeyID>) {
                hasKey = true;
                if (isKeyOfMarkedField[id.index()])
                    markedKeys |= bitOfKey(id);
            }
        });
        bool goesByAnyField = Value::opcodesWithLayout.contains(value.opcode) || Value::opcodesWithFields.contains(value.opcode)
            || (effectRuleOf(value.opcode) == EffectRule::Access && !hasKey);
        if (looksAtCodeOfOperands && (markOfOperandsWhoseCodeRuns(function, value) & seenByCodeOfOperands))
            return true;
        Mark wanted = goesByAnyField ? seenByAll : markedKeys;
        if (!wanted)
            return false;

        Mark found = markOfTypesOf(value);
        forEachReference(value, searchInto(found));
        value.forEachOperand([&](ValueID operand, uint32_t) {
            if (operand)
                found |= markOfTypesOf(function.at(operand));
        });
        return !!(found & wanted);
    };

    auto looksForMarkedField = [&](FunctionID id, Mark markOfAllNamed) {
        const Function& function = program.functions[id.index()];
        Mark foundInRecord = 0;
        forEachReference(static_cast<const FunctionRecord&>(function), searchInto(foundInRecord, false));
        if (foundInRecord)
            return true;

        bool looksAtCodeOfOperands = !!(markOfAllNamed & seenByCodeOfOperands);
        if (looksAtCodeOfOperands) {
            for (const Value& value : function.values) {
                if (valueLooksForMarkedField(function, value, true))
                    return true;
            }
        } else {
            static const OpcodeSet opcodesThatMayLook = [] {
                OpcodeSet opcodes = findOpcodesNaming<PropertyKeyID>() | Value::opcodesWithLayout | Value::opcodesWithFields;
                for (Opcode opcode : allMembersOf<Opcode>) {
                    if (effectRuleOf(opcode) == EffectRule::Access)
                        opcodes.add(opcode);
                }
                return opcodes;
            }();
            bool looks = anyValueListedOrNot(program, id, opcodesThatMayLook, [&](ValueID, const Value& value) {
                return valueLooksForMarkedField(function, value, false);
            });
            if (looks)
                return true;
            static const TerminatorKindSet kindsThatMayName = findTerminatorKindsNaming<LayoutID, VariableID, LayoutSetID>();
            if (!mayEndBlockWith(program, id, kindsThatMayName))
                return false;
        }
        for (const Block& block : function.blocks) {
            Mark found = 0;
            forEachReference(block.terminator, searchInto(found, false));
            if (found || (looksAtCodeOfOperands && (markOfOperandsWhoseCodeRunsAtEnd(function, block.terminator) & seenByCodeOfOperands)))
                return true;
        }
        return false;
    };

    auto seesMarkedRecord = [&](FunctionID id) {
        Mark mark = markOfAllNamedBy(id);
        if (!mark)
            return false;
        return (mark & seenWithoutKey) || looksForMarkedField(id, mark);
    };

    auto addMarksOfSources = [&](Node count, const auto& forEachSourceOf) {
        constexpr unsigned nodesInShare = 512;
        // Marks are only added, so the result does not depend on the order in which nodes take marks from their sources. Most marks arrive within a few steps. Long chains are followed from source to target below.
        AOT_THRESHOLD(unsigned, maxRoundsOverAllNodes, 3);
        bool isFinished = false;
        for (unsigned round = 0; round < maxRoundsOverAllNodes && !isFinished; ++round) {
            std::atomic<bool> didAdd { false };
            forEachIndexInParallel(loops, (count + nodesInShare - 1) / nodesInShare, [&](unsigned share) {
                bool didAddInShare = false;
                for (Node target = share * nodesInShare; target < std::min(count, (share + 1) * nodesInShare); ++target) {
                    Mark known = markAt(target);
                    if (known == seenByAll)
                        continue;
                    Mark mark = known;
                    forEachSourceOf(target, [&](Node source) {
                        mark |= markAt(source);
                    });
                    if (mark == known)
                        continue;
                    addAt(target, mark);
                    didAddInShare = true;
                }
                if (didAddInShare)
                    didAdd.store(true, std::memory_order_relaxed);
            });
            isFinished = !didAdd.load(std::memory_order_relaxed);
        }
        if (isFinished)
            return;

        auto forEachEdge = [&](const auto& functor) {
            forEachIndexInParallel(loops, (count + nodesInShare - 1) / nodesInShare, [&](unsigned share) {
                for (Node target = share * nodesInShare; target < std::min(count, (share + 1) * nodesInShare); ++target) {
                    if (markAt(target) == seenByAll)
                        continue;
                    forEachSourceOf(target, [&](Node source) {
                        functor(source, target);
                    });
                }
            });
        };

        Vector<uint32_t> firstTarget(FillWith { }, count + 1, 0u);
        forEachEdge([&](Node source, Node) {
            WTF::atomicExchangeAdd(&firstTarget[source + 1], 1u, std::memory_order_relaxed);
        });
        std::partial_sum(firstTarget.begin(), firstTarget.end(), firstTarget.begin());
        Vector<Node> targets;
        targets.grow(firstTarget[count]);
        Vector<uint32_t> nextTarget(firstTarget.span().first(count));
        forEachEdge([&](Node source, Node target) {
            targets[WTF::atomicExchangeAdd(&nextTarget[source], 1u, std::memory_order_relaxed)] = target;
        });

        Vector<Node> worklist = findIndicesInParallel(loops, count, [&](unsigned node) {
            return markAt(node) && firstTarget[node] != firstTarget[node + 1];
        });
        BitVector isInWorklist(count);
        for (Node node : worklist)
            isInWorklist.quickSet(node);
        while (!worklist.isEmpty()) {
            Node source = worklist.takeLast();
            isInWorklist.quickClear(source);
            Mark mark = markAt(source);
            for (Node target : targets.span().subspan(firstTarget[source], firstTarget[source + 1] - firstTarget[source])) {
                if (!(mark & ~markAt(target)))
                    continue;
                addAt(target, mark);
                if (firstTarget[target] != firstTarget[target + 1] && !isInWorklist.quickSet(target))
                    worklist.append(target);
            }
        }
    };

    auto findFunctionsWhere = [&](const auto& isFound) {
        return collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            if (isFound(index))
                found.append(FunctionID(index));
        });
    };
    auto followsFields = [&](unsigned index) {
        return index < followsFieldsOfOperands.size() && followsFieldsOfOperands[index] && isAsked[index];
    };

    if (changed.layouts.empty() && changed.fields.empty()) {
        return findFunctionsWhere([&](unsigned index) {
            return isAsked[index] && namesMarkedVariable(FunctionID(index));
        });
    }
    bool someFollowFields = false;
    for (unsigned index = 0; index < followsFieldsOfOperands.size() && !someFollowFields; ++index)
        someFollowFields = followsFields(index);

    addMarksOfSources(firstLayoutSet, [&](Node node, const auto& functor) {
        forEachInheritedLayout(LayoutID(node), functor);
    });
    addMarksOfSources(firstType, [&](Node node, const auto& functor) {
        if (node < firstLayoutSet)
            forEachLayoutStoodFor(LayoutID(node), functor);
        else
            forEachSourceOfSet(program.at(LayoutSetID(node - firstLayoutSet)), functor);
    });

    auto addMarksOfSourcesTo = [&](unsigned index, bool& hasContents) {
        Mark known = markAt(firstType + index);
        Mark mark = known;
        forEachSourceOfType(program.at(TypeID(index)), [&](Node source) {
            mark |= markAt(source);
            hasContents |= source >= firstType;
        });
        if (mark == known)
            return false;
        addAt(firstType + index, mark);
        return true;
    };
    Vector<uint32_t> typesWithContents = collectInParallel<uint32_t>(loops, firstVariable - firstType, [&](unsigned index, Vector<uint32_t>& found) {
        bool hasContents = false;
        addMarksOfSourcesTo(index, hasContents);
        if (hasContents)
            found.append(index);
    });
    for (std::atomic<bool> didAdd = true; didAdd.exchange(false, std::memory_order_relaxed);) {
        forEachIndexInParallel(loops, typesWithContents.size(), [&](unsigned position) {
            bool hasContents = false;
            if (addMarksOfSourcesTo(typesWithContents[position], hasContents))
                didAdd.store(true, std::memory_order_relaxed);
        });
    }

    Vector<FunctionID> functions = findFunctionsWhere([&](unsigned index) {
        return isAsked[index] && (!someFollowFields || !followsFields(index)) && seesMarkedRecord(FunctionID(index));
    });
    if (!someFollowFields)
        return functions;

    forEachIndexInParallel(loops, firstLayoutSet, [&](unsigned index) {
        Mark mark = 0;
        forEachVariable(static_cast<const LayoutRecord&>(program.at(LayoutID(index))), [&](VariableID id) {
            mark |= markAt(nodes.of(id));
        });
        if (mark)
            addAt(index, mark);
    });
    addMarksOfSources(firstVariable, [&](Node node, const auto& functor) {
        if (node >= firstType) {
            forEachSourceOfType(program.at(TypeID(node - firstType)), functor);
            return;
        }
        if (node >= firstLayoutSet) {
            forEachSourceOfSet(program.at(LayoutSetID(node - firstLayoutSet)), functor);
            return;
        }
        forEachReference(static_cast<const LayoutRecord&>(program.at(LayoutID(node))), [&](const auto& id) {
            if constexpr (requires { nodes.of(id); } && !std::same_as<std::remove_cvref_t<decltype(id)>, VariableID>)
                functor(nodes.of(id));
        });
        forEachLayoutStoodFor(LayoutID(node), functor);
    });

    functions.appendVector(findFunctionsWhere([&](unsigned index) {
        return followsFields(index) && !!markOfAllNamedBy(FunctionID(index));
    }));
    AOT::sort(functions);
    return functions;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
