/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTJSONVerification.h"

#if ENABLE(AOT)

#include "AOTForwardDataflow.h"
#include "AOTJSONVerificationCapacity.h"
#include "AOTSchemaWalkers.h"
#include "AOTStringBuilderCursor.h"
#include <algorithm>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

using OperationFamilySet = EnumeratorBitSet<OperationFamily, numberOfEnumerators<OperationFamily>>;

static constexpr OperationFamilySet familiesWithTypedDataRules {
    OperationFamily::JSONParse,
    OperationFamily::JSONWrite,
    OperationFamily::Validation,
    OperationFamily::StringBuilder,
};
static constexpr OpcodeSet cursorInvalidatingOpcodes { Opcode::JSONSourceClose, Opcode::JSONSourceAbandon, Opcode::JSONSourceOpen };

static bool mayMoveBuilderBuffer(const EffectsOnEdges& effects)
{
    Effects onAnyEdge = effects.onAnyEdge();
    return onAnyEdge.mayRunOtherCode() || onAnyEdge.writes.contains(AbstractHeap::StringBuilderContents);
}

static ValueID findStaleValue(const Terminator& terminator, const BitVector& stale)
{
    ValueID found;
    terminator.forEachChild([&](ValueID child) {
        if (!found && stale.get(child.index()))
            found = child;
    });
    return found;
}

namespace {

enum class KeyOrderOrigin : uint8_t { Unknown, SameLayout, Other };

struct PointerSummary {
    PointerSummary(OpaqueKind kind, const Function& function)
        : kind(kind)
        , pointers(function.values.size())
        , sessions(function.values.size())
        , invalidations(function.values.size())
        , invalidatingTerminators(function.blocks.size())
    {
    }

    bool canBecomeStale() const
    {
        bool hasTrackedValue = !pointers.isEmpty() || !sessions.isEmpty();
        return hasTrackedValue && (!invalidations.isEmpty() || !invalidatingTerminators.isEmpty());
    }

    // Every tracked value, unless the operation works on one object and leaves the pointers into the others as they are.
    const BitVector* pointersInvalidatedBy(ValueID id) const
    {
        if (groupInvalidatedBy.isEmpty() || groupInvalidatedBy[id.index()] == everyGroup)
            return nullptr;
        return &pointersOfGroup[groupInvalidatedBy[id.index()]];
    }

    static constexpr uint32_t everyGroup = UINT32_MAX;

    OpaqueKind kind;
    BitVector pointers;
    BitVector sessions;
    BitVector invalidations;
    BitVector invalidatingTerminators;
    // By operation that invalidates. Empty where every such operation invalidates every pointer.
    Vector<uint32_t> groupInvalidatedBy;
    Vector<BitVector> pointersOfGroup;
};

class StalePointerProblem {
    WTF_MAKE_NONCOPYABLE(StalePointerProblem);
public:
    using State = BitVector;

    StalePointerProblem(const Function& function, const PointerSummary& summary)
        : m_function(function)
        , m_summary(summary)
        , m_tracked(summary.pointers)
    {
        m_tracked.merge(summary.sessions);
    }

    State stateAtEntry() const { return { }; }

    void execute(State& stale, ValueID id) const
    {
        if (m_summary.invalidations.get(id.index())) {
            if (const BitVector* invalidated = m_summary.pointersInvalidatedBy(id))
                stale.merge(*invalidated);
            else
                stale = m_tracked;
        }
        stale.clear(id.index());
    }

    void execute(State& stale, const DataflowStep& step) const { execute(stale, step.id); }

    void executeTerminator(State& stale, const DataflowTerminatorStep& step) const
    {
        if (m_summary.invalidatingTerminators.get(step.block.index()))
            stale = m_tracked;
    }

    void enterEdge(State& stale, Edge edge) const
    {
        const Target& target = targetOf(m_function, edge);
        BitVector staleArguments;
        for (uint32_t index = 0; index < target.arguments.size(); ++index) {
            if (stale.get(target.arguments[index].index()))
                staleArguments.set(index);
        }
        auto& parameters = m_function.at(target.block).parameters;
        for (uint32_t index = 0; index < parameters.size(); ++index)
            stale.set(parameters[index].index(), staleArguments.get(index));
    }

    bool merge(State& into, const State& from) const
    {
        size_t previousCount = into.bitCount();
        into.merge(from);
        return into.bitCount() != previousCount;
    }

private:
    const Function& m_function;
    const PointerSummary& m_summary;
    BitVector m_tracked;
};

class TypedDataVerifier {
    WTF_MAKE_NONCOPYABLE(TypedDataVerifier);
public:
    TypedDataVerifier(const Program&, FunctionID, const Function&, FunctionAnalyses&, Vector<InternalError>&);

    void run(IRForm);

private:
    std::optional<OpaqueKind> kindOf(ValueID id) const { return m_program.typeOf(m_function, id).opaque; }
    void report(InternalErrorKind, BlockID, ValueID, uint32_t count = 0);

    bool reportFirstViolation(InternalErrorKind kind, NOESCAPE const Invocable<bool(BlockID, ValueID)> auto& isViolation)
    {
        for (BlockID block : m_cfg.reversePostOrder()) {
            for (ValueID id : m_function.at(block).values) {
                if (isViolation(block, id)) {
                    report(kind, block, id);
                    return true;
                }
            }
        }
        return false;
    }

    void collectSummary();
    void addIfPointer(ValueID);
    void collectSourceGuards(std::span<const ValueID> guards);
    void collectBuilderInvalidations();
    void groupCursorsByBuilder();
    bool invalidatesBuilderCursors(ValueID);

    bool isReturnedCall(BlockID, ValueID) const;
    bool isUnderMatchingTest(const Value& advance);
    KeyOrderOrigin originOfKeyOrder(LayoutID, ValueID order);
    bool usesStalePointer(const PointerSummary&, ValueID, const BitVector& stale) const;

    void verifyFunctionReferences();
    void verifyCursorGuards();
    void verifyPointersAreNotStale(const PointerSummary&, InternalErrorKind);
    void verifyKeyOrderObjects();
    void verifyBuilderCapacity();

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses& m_analyses;
    const CFG& m_cfg;
    Vector<InternalError>& m_errors;
    PointerSummary m_cursors;
    PointerSummary m_builderCursors;
    OpcodeSet m_usedOpcodes;
};

TypedDataVerifier::TypedDataVerifier(const Program& program, FunctionID id, const Function& function, FunctionAnalyses& analyses, Vector<InternalError>& errors)
    : m_program(program)
    , m_id(id)
    , m_function(function)
    , m_analyses(analyses)
    , m_cfg(analyses.cfg())
    , m_errors(errors)
    , m_cursors(OpaqueKind::Cursor, function)
    , m_builderCursors(OpaqueKind::BuilderCursor, function)
{
}

void TypedDataVerifier::run(IRForm form)
{
    collectSummary();
    verifyFunctionReferences();
    verifyCursorGuards();
    verifyPointersAreNotStale(m_cursors, InternalErrorKind::CursorUsedAfterClose);
    verifyKeyOrderObjects();
    if (form < IRForm::Prepared || m_builderCursors.pointers.isEmpty())
        return;
    collectBuilderInvalidations();
    verifyBuilderCapacity();
    verifyPointersAreNotStale(m_builderCursors, InternalErrorKind::BuilderCursorUsedAfterMove);
}

void TypedDataVerifier::report(InternalErrorKind kind, BlockID block, ValueID value, uint32_t count)
{
    m_errors.append(InternalError {
        .kind = kind,
        .function = m_id,
        .block = block,
        .value = value,
        .count = count,
        .location = value ? m_function.locationOf(value) : noSourceLocation,
    });
}

void TypedDataVerifier::collectSummary()
{
    Vector<ValueID, 8> guards;
    auto add = [&](ValueID id) {
        if (isGuardValue(m_function, id))
            guards.append(id);
        else
            addIfPointer(id);
    };

    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = m_function.at(blockID);
        for (ValueID parameter : block.parameters)
            add(parameter);
        for (ValueID id : block.values) {
            Opcode opcode = m_function.at(id).opcode;
            m_usedOpcodes.add(opcode);
            add(id);
            if (cursorInvalidatingOpcodes.contains(opcode))
                m_cursors.invalidations.quickSet(id.index());
        }
    }
    collectSourceGuards(guards.span());
}

void TypedDataVerifier::addIfPointer(ValueID id)
{
    std::optional<OpaqueKind> kind = kindOf(id);
    if (kind != m_cursors.kind && kind != m_builderCursors.kind)
        return;
    PointerSummary& summary = kind == m_cursors.kind ? m_cursors : m_builderCursors;
    const Value& value = m_function.at(id);
    ValueID producer = value.opcode == Opcode::ResultAt ? value.resultsOperand() : id;
    for (ValueID tracked : { id, producer })
        summary.pointers.quickSet(tracked.index());
}

void TypedDataVerifier::collectSourceGuards(std::span<const ValueID> guards)
{
    if (!m_usedOpcodes.contains(Opcode::JSONSourceOpen))
        return;
    for (ValueID guard : guards) {
        if (m_analyses.guardFacts(guard).find(Proof::SourceOpen))
            m_cursors.sessions.quickSet(guard.index());
    }
}

void TypedDataVerifier::collectBuilderInvalidations()
{
    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (invalidatesBuilderCursors(id))
                m_builderCursors.invalidations.quickSet(id.index());
        }
        if (mayMoveBuilderBuffer(m_analyses.effectsOfTerminator(block)))
            m_builderCursors.invalidatingTerminators.quickSet(block.index());
    }
    groupCursorsByBuilder();
}

// Each builder has a buffer of its own, so what is done to one builder moves no other's. Builders that meet in a parameter of a block count as one, and a cursor belongs to the builder that it was reserved in.
void TypedDataVerifier::groupCursorsByBuilder()
{
    uint32_t numberOfValues = m_function.values.size();
    Vector<uint32_t> parent(numberOfValues, [](size_t index) {
        return static_cast<uint32_t>(index);
    });
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
    auto isBuilderOrCursor = [&](ValueID id) {
        std::optional<OpaqueKind> kind = kindOf(id);
        return kind == OpaqueKind::StringBuilder || kind == OpaqueKind::BuilderCursor || m_builderCursors.pointers.get(id.index());
    };

    for (BlockID block : m_cfg.reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            ValueID first = isBuilderOrCursor(id) ? id : ValueID();
            m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                if (!isBuilderOrCursor(operand))
                    return;
                if (first)
                    unite(first, operand);
                first = operand;
            });
        }
        forEachOutgoingEdge(m_function, block, [&](Edge, const Target& target) {
            auto& parameters = m_function.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                if (isBuilderOrCursor(parameters[index]))
                    unite(parameters[index], target.arguments[index]);
            }
        });
    }

    Vector<uint32_t> numberOfGroup(FillWith { }, numberOfValues, PointerSummary::everyGroup);
    auto groupOf = [&](ValueID id) {
        uint32_t& number = numberOfGroup[find(id.index())];
        if (number == PointerSummary::everyGroup) {
            number = m_builderCursors.pointersOfGroup.size();
            m_builderCursors.pointersOfGroup.append(BitVector(numberOfValues));
        }
        return number;
    };
    for (size_t pointer : m_builderCursors.pointers)
        m_builderCursors.pointersOfGroup[groupOf(ValueID(static_cast<uint32_t>(pointer)))].quickSet(pointer);

    m_builderCursors.groupInvalidatedBy.fill(PointerSummary::everyGroup, numberOfValues);
    for (size_t index : m_builderCursors.invalidations) {
        ValueID id(static_cast<uint32_t>(index));
        if (m_analyses.mayRunOtherCodeOnAnyEdge(id))
            continue;
        ValueID builder = kindOf(id) == OpaqueKind::StringBuilder ? id : ValueID();
        m_function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
            if (kindOf(operand) == OpaqueKind::StringBuilder)
                builder = operand;
        });
        if (builder)
            m_builderCursors.groupInvalidatedBy[index] = groupOf(builder);
    }
}

bool TypedDataVerifier::invalidatesBuilderCursors(ValueID id)
{
    Opcode opcode = m_function.at(id).opcode;
    if (familyOf(opcode) == OperationFamily::JSONWrite)
        return renewsReservation(opcode) || opcode == Opcode::StringBuilderCommit;
    return mayMoveBuilderBuffer(m_analyses.effectsOf(id));
}

bool TypedDataVerifier::isReturnedCall(BlockID block, ValueID id) const
{
    const Terminator& terminator = m_function.at(block).terminator;
    return terminator.kind == TerminatorKind::ReturnCall && terminator.callOperand() == id;
}

void TypedDataVerifier::verifyFunctionReferences()
{
    bool didReport = reportFirstViolation(InternalErrorKind::CursorInFunctionValue, [&](BlockID block, ValueID id) {
        const Value& value = m_function.at(id);
        bool isViolation = false;
        forEachFunctionReference(value, [&](FunctionID referenced, FunctionReferenceKind kind) {
            if (!hasCursorInSignature(m_program, m_program.at(referenced)))
                return;
            bool isReturningDirectCall = kind == FunctionReferenceKind::Call && value.opcode == Opcode::CallDirect && !isReturnedCall(block, id);
            isViolation |= !isReturningDirectCall;
        });
        return isViolation;
    });
    if (didReport || !hasCursorInSignature(m_program, m_function))
        return;
    if (!m_function.hasOnlyDirectCallers())
        report(InternalErrorKind::CursorInFunctionValue, { }, { });
}

bool TypedDataVerifier::isUnderMatchingTest(const Value& advance)
{
    ValueID cursor = m_analyses.identityOf(advance.cursorOperand());
    return std::ranges::any_of(m_analyses.guardFacts(advance.guardOperand()).facts, [&](const GuardFact& fact) {
        if (!fact.proves(Proof::TextMatches) || fact.subject != cursor)
            return false;
        return std::ranges::all_of(fact.origins, [&](const GuardOrigin& origin) {
            if (origin.source != GuardSource::Test)
                return false;
            const Value& test = m_function.at(origin.test);
            return test.opcode == Opcode::JSONStartsWith && test.string() == advance.string();
        });
    });
}

void TypedDataVerifier::verifyCursorGuards()
{
    reportFirstViolation(InternalErrorKind::CursorGuardMismatch, [&](BlockID, ValueID id) {
        const Value& value = m_function.at(id);
        ProofSet relied = reliesOf(value.opcode);
        if (relied.contains(Proof::TextMatches))
            return !isUnderMatchingTest(value);
        if (relied.contains(Proof::SourceOpen))
            return !m_analyses.guardFacts(value.guardOperand()).find(Proof::SourceOpen);
        return false;
    });
}

bool TypedDataVerifier::usesStalePointer(const PointerSummary& summary, ValueID id, const BitVector& stale) const
{
    const Value& value = m_function.at(id);
    if (value.opcode == Opcode::ResultAt && kindOf(id) != summary.kind)
        return false;
    bool result = false;
    value.forEachOperand([&](ValueID operand, uint32_t) {
        result |= stale.get(operand.index());
    });
    return result;
}

void TypedDataVerifier::verifyPointersAreNotStale(const PointerSummary& summary, InternalErrorKind violation)
{
    if (!summary.canBecomeStale())
        return;

    StalePointerProblem problem(m_function, summary);
    Vector<std::optional<BitVector>> staleAtHead = m_analyses.solveForward(problem);
    for (BlockID blockID : m_cfg.reversePostOrder()) {
        const Block& block = m_function.at(blockID);
        BitVector& stale = *staleAtHead[blockID.index()];
        for (ValueID id : block.values) {
            if (usesStalePointer(summary, id, stale)) {
                report(violation, blockID, id);
                return;
            }
            problem.execute(stale, id);
        }
        if (ValueID leaving = findStaleValue(block.terminator, stale)) {
            report(violation, blockID, leaving);
            return;
        }
    }
}

KeyOrderOrigin TypedDataVerifier::originOfKeyOrder(LayoutID layout, ValueID order)
{
    const UseLists& useLists = m_analyses.useLists();
    KeyOrderOrigin origin = KeyOrderOrigin::SameLayout;
    BitVector isVisited;
    Vector<ValueID, 8> worklist { order };
    while (!worklist.isEmpty()) {
        ValueID id = m_analyses.identityOf(worklist.takeLast());
        if (isVisited.set(id.index()))
            continue;

        ValueLocation location = useLists.locationOf(id);
        switch (location.placement) {
        case ValuePlacement::Detached:
            return KeyOrderOrigin::Other;
        case ValuePlacement::Parameter:
            if (location.block == entryBlock) {
                origin = KeyOrderOrigin::Unknown;
                break;
            }
            for (Edge edge : m_cfg.incomingEdges(location.block)) {
                IncomingValue incoming = incomingValueOf(m_function, edge, location.index);
                if (incoming.kind != IncomingValue::Kind::Argument)
                    return KeyOrderOrigin::Other;
                worklist.append(incoming.argument);
            }
            break;
        case ValuePlacement::InBlock: {
            const Value& value = m_function.at(id);
            const Value& row = value.opcode == Opcode::ResultAt ? m_function.at(value.resultsOperand()) : value;
            if (row.opcode == Opcode::CallDirect) {
                origin = KeyOrderOrigin::Unknown;
                break;
            }
            if (familyOf(row.opcode) != OperationFamily::JSONParse || !Value::opcodesWithLayout.contains(row.opcode) || row.layout() != layout)
                return KeyOrderOrigin::Other;
            if (OperandPosition::order(row.opcode))
                worklist.append(row.orderOperand());
            break;
        }
        }
    }
    return origin;
}

void TypedDataVerifier::verifyKeyOrderObjects()
{
    if (!m_usedOpcodes.contains(Opcode::JSONObjectFinish))
        return;
    reportFirstViolation(InternalErrorKind::KeyOrderOfOtherObject, [&](BlockID, ValueID id) {
        const Value& finish = m_function.at(id);
        if (finish.opcode != Opcode::JSONObjectFinish)
            return false;
        const Value& object = m_function.at(m_analyses.sameObjectThroughCasts(finish.objectOperand()));
        if (object.opcode != Opcode::NewObject || object.layout() != finish.layout())
            return true;
        return originOfKeyOrder(finish.layout(), finish.orderOperand()) == KeyOrderOrigin::Other;
    });
}

void TypedDataVerifier::verifyBuilderCapacity()
{
    if (auto violation = findBuilderCapacityViolation(m_program, m_function, m_cfg))
        report(InternalErrorKind::BuilderCapacityExceeded, violation->block, violation->value, violation->requiredBytes);
}

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
    return std::ranges::any_of(function.captures, allowsOnlyDirectCalls, &DeclaredType::type)
        || std::ranges::any_of(function.parameters, allowsOnlyDirectCalls, &DeclaredType::type)
        || std::ranges::any_of(function.results, allowsOnlyDirectCalls, &DeclaredType::type);
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
    TypedDataVerifier(program, id, function, analyses, errors).run(form);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
