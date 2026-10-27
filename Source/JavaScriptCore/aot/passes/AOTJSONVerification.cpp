/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONVerification.h"

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringBuilderCursor.h"
#include "AOTThreshold.h"
#include "AOTValueIdentity.h"
#include <algorithm>
#include <optional>
#include <wtf/BitVector.h>

namespace JSC { namespace AOT { namespace Typed {

AOT_THRESHOLD(unsigned, maxCapacityRounds, 4);

static constexpr EnumeratorBitSet<OperationFamily, numberOfEnumerators<OperationFamily>> familiesWithTypedDataRules {
    OperationFamily::JSONParse,
    OperationFamily::JSONWrite,
    OperationFamily::StringBuilder,
};
static constexpr OpcodeSet cursorInvalidatingOpcodes { Opcode::JSONSourceClose, Opcode::JSONSourceAbandon, Opcode::JSONSourceOpen };

static bool mayMoveBuilderBuffer(const EffectsOnEdges& effects)
{
    Effects onAnyEdge = effects.onAnyEdge();
    return onAnyEdge.mayRunOtherCode() || onAnyEdge.writes.contains(AbstractHeap::StringBuilderContents);
}

namespace {

// The pointers of one kind and what invalidates them. To the solver it is the problem of which of them are stale.
struct PointerSummary {
    using State = BitVector;
    static constexpr uint32_t everyGroup = UINT32_MAX;

    State stateAtEntry() const { return { }; }

    void execute(State& stale, ValueID id) const
    {
        if (invalidations.get(id.index())) {
            // Every tracked value, unless the operation works on one object and leaves the pointers into the others as they are.
            if (groupInvalidatedBy.isEmpty() || groupInvalidatedBy[id.index()] == everyGroup)
                stale = tracked;
            else
                stale.merge(pointersOfGroup[groupInvalidatedBy[id.index()]]);
        }
        stale.clear(id.index());
    }

    void execute(State& stale, const DataflowStep& step) const { execute(stale, step.id); }

    void executeTerminator(State& stale, const DataflowTerminatorStep& step) const
    {
        if (invalidatingTerminators.get(step.block.index()))
            stale = tracked;
    }

    void enterEdge(State& stale, Edge edge) const
    {
        const Target& target = targetOf(function, edge);
        BitVector staleArguments;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (stale.get(target.arguments[index].index()))
                staleArguments.set(index);
        }
        auto& parameters = function.at(target.block).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index)
            stale.set(parameters[index].index(), staleArguments.get(index));
    }

    bool merge(State& into, const State& from) const
    {
        size_t previousCount = into.bitCount();
        into.merge(from);
        return into.bitCount() != previousCount;
    }

    const Function& function;
    OpaqueKind kind;
    BitVector pointers { function.values.size() };
    BitVector sessions { function.values.size() };
    BitVector tracked { };
    BitVector invalidations { function.values.size() };
    BitVector invalidatingTerminators { function.blocks.size() };
    // By operation that invalidates. Empty where every such operation invalidates every pointer.
    Vector<uint32_t> groupInvalidatedBy { };
    Vector<BitVector> pointersOfGroup { };
};

struct TypedDataVerifier {
    std::optional<OpaqueKind> kindOf(ValueID value) const { return program.typeOf(function, value).opaque; }

    void report(InternalErrorKind kind, BlockID block, ValueID value, uint32_t count = 0)
    {
        errors.append(InternalError {
            .kind = kind,
            .function = id,
            .block = block,
            .value = value,
            .count = count,
            .location = value ? function.locationOf(value) : noSourceLocation,
        });
    }

    bool reportFirstViolation(InternalErrorKind kind, NOESCAPE const Invocable<bool(BlockID, ValueID)> auto& isViolation)
    {
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID value : function.at(block).values) {
                if (isViolation(block, value)) {
                    report(kind, block, value);
                    return true;
                }
            }
        }
        return false;
    }

    void run(IRForm form)
    {
        collectSummary();
        verifyFunctionReferences();
        verifyCursorGuards();
        verifyPointersAreNotStale(cursors, InternalErrorKind::CursorUsedAfterClose);
        verifyKeyOrderObjects();
        if (form < IRForm::Prepared || builderCursors.pointers.isEmpty())
            return;
        collectBuilderInvalidations();
        verifyBuilderCapacity();
        verifyPointersAreNotStale(builderCursors, InternalErrorKind::BuilderCursorUsedAfterMove);
    }

    void collectSummary()
    {
        Vector<ValueID, 8> guards;
        auto add = [&](ValueID value) {
            if (isGuardValue(function, value)) {
                guards.append(value);
                return;
            }
            std::optional<OpaqueKind> kind = kindOf(value);
            if (kind != cursors.kind && kind != builderCursors.kind)
                return;
            BitVector& pointers = (kind == cursors.kind ? cursors : builderCursors).pointers;
            pointers.quickSet(value.index());
            if (function.at(value).opcode == Opcode::ResultAt)
                pointers.quickSet(function.at(value).resultsOperand().index());
        };

        for (BlockID blockID : cfg.reversePostOrder()) {
            const Block& block = function.at(blockID);
            for (ValueID parameter : block.parameters)
                add(parameter);
            for (ValueID value : block.values) {
                Opcode opcode = function.at(value).opcode;
                usedOpcodes.add(opcode);
                add(value);
                if (cursorInvalidatingOpcodes.contains(opcode))
                    cursors.invalidations.quickSet(value.index());
            }
        }
        if (!usedOpcodes.contains(Opcode::JSONSourceOpen))
            return;
        for (ValueID guard : guards) {
            if (analyses.guardFacts(guard).find(Proof::SourceOpen))
                cursors.sessions.quickSet(guard.index());
        }
    }

    void collectBuilderInvalidations()
    {
        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID value : function.at(block).values) {
                Opcode opcode = function.at(value).opcode;
                bool invalidates = familyOf(opcode) == OperationFamily::JSONWrite ? renewsReservation(opcode) || opcode == Opcode::StringBuilderCommit : mayMoveBuilderBuffer(analyses.effectsOf(value));
                if (invalidates)
                    builderCursors.invalidations.quickSet(value.index());
            }
            if (mayMoveBuilderBuffer(analyses.effectsOfTerminator(block)))
                builderCursors.invalidatingTerminators.quickSet(block.index());
        }
        groupCursorsByBuilder();
    }

    // Each builder has a buffer of its own, so what is done to one builder moves no other's. Builders that meet in a parameter of a block count as one, and a cursor belongs to the builder that it was reserved in.
    void groupCursorsByBuilder()
    {
        uint32_t numberOfValues = function.values.size();
        Vector<uint32_t> parent(numberOfValues, [](size_t index) { return static_cast<uint32_t>(index); });
        auto find = [&](uint32_t value) {
            while (parent[value] != value)
                value = parent[value] = parent[parent[value]];
            return value;
        };
        auto unite = [&](ValueID first, ValueID second) {
            uint32_t a = find(first.index());
            uint32_t b = find(second.index());
            parent[std::max(a, b)] = std::min(a, b);
        };
        auto isBuilderOrCursor = [&](ValueID value) {
            std::optional<OpaqueKind> kind = kindOf(value);
            return kind == OpaqueKind::StringBuilder || kind == OpaqueKind::BuilderCursor || builderCursors.pointers.get(value.index());
        };

        for (BlockID block : cfg.reversePostOrder()) {
            for (ValueID value : function.at(block).values) {
                ValueID first = isBuilderOrCursor(value) ? value : ValueID();
                function.at(value).forEachOperand([&](ValueID operand, uint32_t) {
                    if (!isBuilderOrCursor(operand))
                        return;
                    if (first)
                        unite(first, operand);
                    first = operand;
                });
            }
            forEachOutgoingEdge(function, block, [&](Edge, const Target& target) {
                auto& parameters = function.at(target.block).parameters;
                for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                    if (isBuilderOrCursor(parameters[index]))
                        unite(parameters[index], target.arguments[index]);
                }
            });
        }

        Vector<uint32_t> numberOfGroup(FillWith { }, numberOfValues, PointerSummary::everyGroup);
        auto groupOf = [&](ValueID value) {
            uint32_t& number = numberOfGroup[find(value.index())];
            if (number == PointerSummary::everyGroup) {
                number = builderCursors.pointersOfGroup.size();
                builderCursors.pointersOfGroup.append(BitVector(numberOfValues));
            }
            return number;
        };
        for (size_t pointer : builderCursors.pointers)
            builderCursors.pointersOfGroup[groupOf(ValueID(static_cast<uint32_t>(pointer)))].quickSet(pointer);

        builderCursors.groupInvalidatedBy.fill(PointerSummary::everyGroup, numberOfValues);
        for (size_t index : builderCursors.invalidations) {
            ValueID value(static_cast<uint32_t>(index));
            if (analyses.mayRunOtherCodeOnAnyEdge(value))
                continue;
            ValueID builder = kindOf(value) == OpaqueKind::StringBuilder ? value : ValueID();
            function.at(value).forEachOperand([&](ValueID operand, uint32_t) {
                if (kindOf(operand) == OpaqueKind::StringBuilder)
                    builder = operand;
            });
            if (builder)
                builderCursors.groupInvalidatedBy[index] = groupOf(builder);
        }
    }

    void verifyFunctionReferences()
    {
        bool didReport = reportFirstViolation(InternalErrorKind::CursorInFunctionValue, [&](BlockID block, ValueID valueID) {
            const Value& value = function.at(valueID);
            bool isViolation = false;
            forEachFunctionReference(value, [&](FunctionID referenced, FunctionReferenceKind kind) {
                if (!hasCursorInSignature(program, program.at(referenced)))
                    return;
                const Terminator& terminator = function.at(block).terminator;
                bool isReturnedCall = terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == valueID;
                isViolation |= kind != FunctionReferenceKind::Call || value.opcode != Opcode::CallDirect || isReturnedCall;
            });
            return isViolation;
        });
        if (!didReport && hasCursorInSignature(program, function) && !function.hasOnlyDirectCallers())
            report(InternalErrorKind::CursorInFunctionValue, { }, { });
    }

    bool isUnderMatchingTest(const Value& advance)
    {
        ValueID cursor = analyses.identityOf(advance.cursorOperand());
        return std::ranges::any_of(analyses.guardFacts(advance.guardOperand()).facts, [&](const GuardFact& fact) {
            if (!fact.proves(Proof::TextMatches) || fact.subject != cursor)
                return false;
            return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
                if (origin.source != GuardSource::Test)
                    return false;
                const Value& test = function.at(origin.test);
                return test.opcode == Opcode::JSONStartsWith && test.string() == advance.string();
            });
        });
    }

    void verifyCursorGuards()
    {
        reportFirstViolation(InternalErrorKind::CursorGuardMismatch, [&](BlockID, ValueID valueID) {
            const Value& value = function.at(valueID);
            ProofSet relied = reliesOf(value.opcode);
            if (relied.contains(Proof::TextMatches))
                return !isUnderMatchingTest(value);
            return relied.contains(Proof::SourceOpen) && !analyses.guardFacts(value.guardOperand()).find(Proof::SourceOpen);
        });
    }

    void verifyPointersAreNotStale(PointerSummary& summary, InternalErrorKind violation)
    {
        bool canBecomeStale = (!summary.pointers.isEmpty() || !summary.sessions.isEmpty()) && (!summary.invalidations.isEmpty() || !summary.invalidatingTerminators.isEmpty());
        if (!canBecomeStale)
            return;

        summary.tracked = summary.pointers;
        summary.tracked.merge(summary.sessions);
        Vector<std::optional<BitVector>> staleAtHead = analyses.solveForward(summary);
        for (BlockID blockID : cfg.reversePostOrder()) {
            const Block& block = function.at(blockID);
            BitVector& stale = *staleAtHead[blockID.index()];
            ValueID user;
            for (ValueID valueID : block.values) {
                const Value& value = function.at(valueID);
                if (value.opcode != Opcode::ResultAt || kindOf(valueID) == summary.kind) {
                    value.forEachOperand([&](ValueID operand, uint32_t) {
                        if (stale.get(operand.index()))
                            user = valueID;
                    });
                }
                if (user)
                    break;
                summary.execute(stale, valueID);
            }
            block.terminator.forEachChild([&](ValueID child) {
                if (!user && stale.get(child.index()))
                    user = child;
            });
            if (user) {
                report(violation, blockID, user);
                return;
            }
        }
    }

    // Whether all that may arrive as the value passes: an operation is shown to the functor, which may name more values, and a parameter of a block stands for the arguments of the edges into the block.
    bool holdsForOriginsOf(ValueID start, bool holdsForParametersOfFunction, const auto& canonicalOf, const auto& holdsFor)
    {
        const UseLists& useLists = analyses.useLists();
        BitVector isVisited;
        Vector<ValueID, 8> worklist { start };
        while (!worklist.isEmpty()) {
            ValueID value = canonicalOf(worklist.takeLast());
            if (isVisited.set(value.index()))
                continue;

            ValueLocation location = useLists.locationOf(value);
            if (location.placement == ValuePlacement::InBlock) {
                if (!holdsFor(function.at(value), worklist))
                    return false;
                continue;
            }
            if (location.placement != ValuePlacement::Parameter)
                return false;
            if (location.block == entryBlock) {
                if (!holdsForParametersOfFunction)
                    return false;
                continue;
            }
            for (Edge edge : cfg.incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(function, edge, location.index);
                if (incoming.kind != IncomingValue::Kind::Argument)
                    return false;
                worklist.append(incoming.argument);
            }
        }
        return true;
    }

    // What a caller hands in, or a callee answers, is not followed.
    enum class KeyOrderOrigin : uint8_t { SameLayoutOrUnknown, Other };
    KeyOrderOrigin originOfKeyOrder(LayoutID layout, ValueID order)
    {
        bool isOfLayoutWhereFollowed = holdsForOriginsOf(order, true, [&](ValueID value) { return analyses.identityOf(value); }, [&](const Value& value, auto& worklist) {
            const Value& row = value.opcode == Opcode::ResultAt ? function.at(value.resultsOperand()) : value;
            if (row.opcode == Opcode::CallDirect)
                return true;
            if (familyOf(row.opcode) != OperationFamily::JSONParse || !Value::opcodesWithLayout.contains(row.opcode) || row.layout() != layout)
                return false;
            if (OperandPosition::order(row.opcode))
                worklist.append(row.orderOperand());
            return true;
        });
        return isOfLayoutWhereFollowed ? KeyOrderOrigin::SameLayoutOrUnknown : KeyOrderOrigin::Other;
    }

    // An allocation that is made again on several ways arrives as the parameter of the block where they meet.
    bool isNewObjectOf(LayoutID layout, ValueID object)
    {
        return holdsForOriginsOf(object, false, [&](ValueID value) { return analyses.sameObjectThroughCasts(value); }, [&](const Value& value, auto&) {
            return value.opcode == Opcode::NewObject && value.layout() == layout;
        });
    }

    void verifyKeyOrderObjects()
    {
        if (!usedOpcodes.contains(Opcode::JSONObjectFinish))
            return;
        reportFirstViolation(InternalErrorKind::KeyOrderOfOtherObject, [&](BlockID, ValueID value) {
            const Value& finish = function.at(value);
            return finish.opcode == Opcode::JSONObjectFinish && (!isNewObjectOf(finish.layout(), finish.objectOperand()) || originOfKeyOrder(finish.layout(), finish.orderOperand()) == KeyOrderOrigin::Other);
        });
    }

    // No row takes more bytes than the rows before it have left of the reservation of its cursor.
    void verifyBuilderCapacity()
    {
        Vector<std::optional<uint32_t>> reservedBytes(function.values.size());
        auto isCursor = [&](ValueID value) { return kindOf(value) == OpaqueKind::BuilderCursor; };
        auto reservedBytesOf = [&](ValueID value) { return reservedBytes[value.index()].value_or(0); };
        struct {
            BlockID block;
            ValueID value;
            uint32_t requiredBytes { 0 };
        } violation;

        auto mergeParameters = [&](BlockID block, bool shouldWiden) {
            bool didChange = false;
            auto& parameters = function.at(block).parameters;
            for (uint32_t index = 0; index < parameters.size(); ++index) {
                if (!isCursor(parameters[index]))
                    continue;
                std::optional<uint32_t>& reserved = reservedBytes[parameters[index].index()];
                std::optional<uint32_t> merged = reserved;
                for (Edge edge : cfg.incomingEdges(block)) {
                    IncomingValue incoming = incomingValueOf(function, edge, index);
                    if (std::optional<uint32_t> onEdge = incoming.kind == IncomingValue::Kind::Argument ? reservedBytes[incoming.argument.index()] : std::optional<uint32_t>(0))
                        merged = std::min(merged.value_or(*onEdge), *onEdge);
                }
                if (merged == reserved)
                    continue;
                if (shouldWiden)
                    merged = 0;
                reserved = merged;
                didChange = true;
            }
            return didChange;
        };
        auto reservedBytesBehind = [&](BlockID block, ValueID value) -> uint32_t {
            const Value& row = function.at(value);
            if (row.opcode == Opcode::StringBuilderReserve)
                return row.size();
            if (!OperandPosition::cursor(row.opcode))
                return 0;

            uint32_t taken = bytesTakenFromReservation(program, row);
            uint32_t reserved = reservedBytesOf(row.cursorOperand());
            if (taken > reserved && !violation.value)
                violation = { block, value, taken };
            if (renewsReservation(row.opcode))
                return maxStringBuilderReservation;
            return taken > reserved ? 0 : reserved - taken;
        };

        bool didChange = true;
        for (unsigned round = 1; didChange; ++round) {
            didChange = false;
            violation = { };
            for (BlockID block : cfg.reversePostOrder()) {
                didChange |= mergeParameters(block, round >= maxCapacityRounds);
                for (ValueID valueID : function.at(block).values) {
                    const Value& value = function.at(valueID);
                    if (familyOf(value.opcode) == OperationFamily::JSONWrite)
                        reservedBytes[valueID.index()] = reservedBytesBehind(block, valueID);
                    else if (isCursor(valueID)) {
                        ValueID forwarded = value.opcode == Opcode::ResultAt || isCast(value.opcode) ? value.operand(0) : ValueID();
                        reservedBytes[valueID.index()] = forwarded ? reservedBytesOf(forwarded) : 0;
                    }
                }
            }
        }
        if (violation.value)
            report(InternalErrorKind::BuilderCapacityExceeded, violation.block, violation.value, violation.requiredBytes);
    }

    const Program& program;
    FunctionID id;
    const Function& function;
    FunctionAnalyses& analyses;
    Vector<InternalError>& errors;
    const CFG& cfg { analyses.cfg() };
    PointerSummary cursors { function, OpaqueKind::Cursor };
    PointerSummary builderCursors { function, OpaqueKind::BuilderCursor };
    OpcodeSet usedOpcodes { };
};

} // anonymous namespace

bool hasTypedDataRules(Opcode opcode)
{
    return familiesWithTypedDataRules.contains(familyOf(opcode));
}

bool hasCursorInSignature(const Program& program, const Function& function)
{
    auto allowsOnlyDirectCalls = [&](TypeID type) {
        std::optional<OpaqueKind> kind = program.at(type).opaque;
        return kind && allowsDirectCallsOnly(*kind);
    };
    return std::ranges::any_of(function.captures, allowsOnlyDirectCalls, &DeclaredType::written)
        || std::ranges::any_of(function.parameters, allowsOnlyDirectCalls, &DeclaredType::written)
        || std::ranges::any_of(function.results, allowsOnlyDirectCalls, &DeclaredType::written);
}

bool referencesFunctionWithCursor(const Program& program, const Value& value)
{
    bool result = false;
    forEachFunctionReference(value, [&](FunctionID referenced, FunctionReferenceKind) {
        result |= hasCursorInSignature(program, program.at(referenced));
    });
    return result;
}

void verifyTypedDataRules(const Program& program, FunctionID id, const Function& function, FunctionAnalyses& analyses, IRForm form, Vector<InternalError>& errors)
{
    TypedDataVerifier { program, id, function, analyses, errors }.run(form);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
