/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTAddGenericVersion.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowApply.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTGenericCopy.h"
#include "AOTHeat.h"
#include "AOTIRBuilder.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTOperatorForms.h"
#include "AOTPass.h"
#include "AOTProgramCaches.h"
#include "AOTRegionCloner.h"
#include "AOTResultRules.h"
#include "AOTSSARepair.h"
#include "AOTSimplifyBuiltinCallsInternal.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypeArena.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <atomic>
#include <compare>
#include <limits>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <wtf/Atomics.h>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr OpcodeSet typeValidations { Opcode::ValidateType, Opcode::ValidateCallee };

enum class ExpectedTypes : uint8_t { Dropped, Kept };

struct CheckSite {
    ValueID check;
    ProgramPoint point;
    bool hasExit { true };
    BlockID block;
    BlockID continuation;
};

struct GenericVersionCut {
    ProgramPoint point;
    BlockID blockBefore;
    BlockID blockAfter;
};

struct OperandOfOtherKind {
    ProgramPoint point;
    // Null: the terminator of the block.
    ValueID user;
    uint32_t operandIndex;
    TypeID castType;
};

struct Position {
    enum class Kind : uint8_t { Variable, Field, EntryParameter, Result };

    Kind kind { Kind::Variable };
    uint32_t owner { 0 };
    uint32_t index { 0 };

    friend bool operator==(const Position&, const Position&) = default;
    friend auto operator<=>(const Position&, const Position&) = default;
};

struct TypedStore {
    ValueID stored;
    TypeID required;
    const Value* operation { nullptr };
    uint32_t operandIndex { 0 };
};

struct GuardedRead {
    ValueID value;
    ProgramPoint insertionPoint;
    DeclaredType declared;
    Vector<Use, 4> uses;
};

// What the operations of a body read, by their own words and the types of their operands. It holds until the body is edited.
struct ReadsOfBody {
    bool isListed { false };
    // Ascending, each once.
    Vector<Position> positions;
    Vector<ValueID> typedReadsByName;
};

class GenericVersionBuilder {
    WTF_MAKE_NONCOPYABLE(GenericVersionBuilder);
public:
    explicit GenericVersionBuilder(FunctionPassContext&);

    PassOutcome run();

private:
    const Type& typeOf(ValueID value) const LIFETIME_BOUND { return m_program.typeOf(m_function, value); }
    TypePropagationInputs typingInputs() { return { m_program, m_context.id, m_function, m_analyses, m_context.types, m_context.build, &m_context.typingMemo }; }

    bool removeChecksInUnreachableBlocks();
    void collectChecks();
    bool removeRedundantChecks();
    bool isReliedOn(ValueID check, Vector<ValueID, 8>& casts);
    bool isOnlyComparedByBits(ValueID check) const;
    bool dropChecksNobodyReliesOn();
    bool dropChecksThatTurnAwayWhatArrives();
    bool guardElementsThatOneOperatorTakes();
    bool keepsMoreAliveOverAwaitThanFrameMapHolds();

    bool tryCreateGenericVersion();
    void splitBlocks(std::span<const ProgramPoint> rejoinPoints);
    const GenericVersionCut* findCut(ProgramPoint) const;
    void mergeBlocksCutForNothing();
    void findUnmergeableExits();
    void eraseProof(ValueID, ExpectedTypes);
    void didChangeCastsInPlace();
    void eraseProofsInCopy(const GenericCopy&, const BitVector& isTainted, size_t numberOfTypedBlocks);
    void lowerCheck(GenericCopy&, const CheckSite&);

    Value createMismatchReport(ValueID, TypeID expectedType, TypeSiteID) const;
    void replaceChecksWithGuards(std::span<const ValueID> checks, ExpectedTypes = ExpectedTypes::Kept);
    CalleeSetID withCalleesNobodyHasSeen(CalleeSetID);
    LayoutSetID withReceiversNobodyHasSeen(LayoutSetID);
    void fallBackToGuards();
    bool assumeOperandKinds();
    bool assumeTypesOfProvenPositions();
    void insertAssumedTypes(std::span<const OperandOfOtherKind>);
    void finishTyping();

    FunctionPassContext& m_context;
    const Program& m_program;
    Function& m_function;
    FunctionAnalyses& m_analyses;
    IRBuilder m_builder;
    TypeID m_anyValue;
    bool m_reportsMismatches;
    Vector<CheckSite, 8> m_sites;
    Vector<GenericVersionCut, 16> m_cuts;
    // Empty where the function has no generic version, and every block runs with what a check would have turned away.
    BitVector m_isGenericBlock;
    // A check without an exit has become a guard at each use, so the typed blocks behind it run with what it would have turned away too.
    bool m_hasGuardsInTypedBlocks { false };
    Vector<ValueID, 8> m_valuesTurnedAway;
    Vector<std::pair<ValueID, TypeID>, 4> m_erasedProofsOfLiterals;
};

} // anonymous namespace

static BlockFrequency genericBlockFrequency(BlockFrequency frequency)
{
    return frequency == BlockFrequency::Normal ? BlockFrequency::Unlikely : BlockFrequency::Rare;
}

static TypeID anyValueType(TypeArena& types)
{
    return types.withPrimitives(TypeTable::anything, Primitive::Absent);
}

static void convertToUpcast(Value& cast, ValueID operand, TypeID castType)
{
    cast.become(Opcode::Upcast);
    cast.setValueOperand(operand);
    cast.setCastType(castType);
}

static bool isAfterTailCall(const Function& function, ProgramPoint point)
{
    const Block& block = function.at(point.block);
    if (block.terminator.kind != TerminatorKind::ReturnCall)
        return false;
    size_t callIndex = block.values.find(block.terminator.callOperand());
    return callIndex != notFound && point.index > callIndex;
}

static ProgramPoint pointAfter(ProgramPoint point)
{
    return { point.block, point.index + 1 };
}

static bool isBefore(ProgramPoint a, ProgramPoint b)
{
    return std::tie(a.block, a.index) < std::tie(b.block, b.index);
}

BitVector computeTaintedValues(const Function& function, const UseLists& useLists, std::span<const ValueID> checks)
{
    BitVector isTainted(function.values.size());
    Vector<ValueID, 16> worklist;
    auto taint = [&](ValueID value) {
        if (isTainted.add(value.index()))
            worklist.append(value);
    };
    auto taintEdgeResults = [&](BlockID from) {
        const Terminator& terminator = function.at(from).terminator;
        for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
            const CompactList<ValueID>& parameters = function.at(terminator.targets[targetIndex].block).parameters;
            uint32_t first = firstEdgeResultParameter(function, from, targetIndex);
            uint32_t end = first + numberOfEdgeResults(function, from, targetIndex);
            for (uint32_t index = first; index < end && index < parameters.size(); ++index)
                taint(parameters[index]);
        }
    };

    for (ValueID check : checks)
        taint(check);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            switch (use.kind) {
            case Use::Kind::Operand:
                taint(use.user);
                break;
            case Use::Kind::TerminatorOperand:
                taintEdgeResults(use.block);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument: {
                const CompactList<ValueID>& parameters = function.at(targetOf(function, use.edge()).block).parameters;
                if (use.operandIndex < parameters.size())
                    taint(parameters[use.operandIndex]);
                break;
            }
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
    return isTainted;
}

BitVector computeTaintedValues(const Function& function, const CFG& cfg, std::span<const ValueID> checks)
{
    BitVector isTainted(function.values.size());
    for (ValueID check : checks)
        isTainted.quickSet(check.index());
    auto isMarked = [&](ValueID value) {
        return value && isTainted.quickGet(value.index());
    };
    auto hasMarkedOperand = [&](const auto& user) {
        bool hasOne = false;
        user.forEachOperand([&](ValueID operand, uint32_t) {
            hasOne |= isMarked(operand);
        });
        return hasOne;
    };

    // Where no edge leads back, whatever a value takes the mark from stands in front of it in this order.
    bool hasChanged;
    do {
        hasChanged = false;
        auto taint = [&](ValueID value) {
            hasChanged |= isTainted.add(value.index());
        };
        auto handOn = [&](const Target& target) {
            const CompactList<ValueID>& parameters = function.at(target.block).parameters;
            for (uint32_t index = 0; index < target.arguments.size() && index < parameters.size(); ++index) {
                if (isMarked(target.arguments[index]))
                    taint(parameters[index]);
            }
        };
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (ValueID value : block.values) {
                if (!isTainted.quickGet(value.index()) && hasMarkedOperand(function.at(value)))
                    taint(value);
            }
            const Terminator& terminator = block.terminator;
            std::ranges::for_each(terminator.targets, handOn);
            if (block.handler)
                handOn(*block.handler);
            if (terminator.targets.isEmpty() || !hasMarkedOperand(terminator))
                continue;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                const CompactList<ValueID>& parameters = function.at(terminator.targets[targetIndex].block).parameters;
                uint32_t first = firstEdgeResultParameter(function, id, targetIndex);
                uint32_t end = first + numberOfEdgeResults(function, id, targetIndex);
                for (uint32_t index = first; index < end && index < parameters.size(); ++index)
                    taint(parameters[index]);
            }
        }
    } while (hasChanged && cfg.hasCycle());
    return isTainted;
}

// The values that may be the very value that a check has turned away: the check, the casts of it, and the parameters that take it.
static BitVector computeValuesTurnedAway(const Function& function, const CFG& cfg, std::span<const ValueID> checks)
{
    BitVector mayBeTurnedAway(function.values.size());
    for (ValueID check : checks)
        mayBeTurnedAway.set(check.index());
    auto isMarked = [&](ValueID value) {
        return value && mayBeTurnedAway.get(value.index());
    };
    auto takesMarkedValue = [&](BlockID block, uint32_t parameterIndex) {
        return std::ranges::any_of(cfg.incomingEdges(block), [&](Edge edge) {
            IncomingValue incoming = incomingValueOf(function, edge, parameterIndex);
            return isMarked(incoming.kind == IncomingValue::Kind::Argument ? incoming.argument : edgeResultAt(function, incoming.result).subject);
        });
    };

    // Where no edge leads back, whatever a value takes the mark from stands in front of it in this order.
    bool hasChanged;
    do {
        hasChanged = false;
        for (BlockID id : cfg.reversePostOrder()) {
            const Block& block = function.at(id);
            for (uint32_t index = 0; id != entryBlock && index < block.parameters.size(); ++index) {
                if (!isMarked(block.parameters[index]) && takesMarkedValue(id, index))
                    hasChanged |= mayBeTurnedAway.add(block.parameters[index].index());
            }
            for (ValueID value : block.values) {
                if (isCast(function.at(value).opcode) && !isMarked(value) && isMarked(function.at(value).operand(0)))
                    hasChanged |= mayBeTurnedAway.add(value.index());
            }
        }
    } while (hasChanged && cfg.hasCycle());
    return mayBeTurnedAway;
}

GenericVersionBuilder::GenericVersionBuilder(FunctionPassContext& context)
    : m_context(context)
    , m_program(context.program)
    , m_function(context.function)
    , m_analyses(context.analyses)
    , m_builder(context.analyses.context(), context.function, context.types, &context.analyses)
    , m_anyValue(anyValueType(context.types))
    , m_reportsMismatches(context.build.recordedOptions().policy() == MismatchPolicy::Report)
{
}

PassOutcome GenericVersionBuilder::run()
{
    // Everything here starts from a check.
    if (!m_analyses.opcodesOfValues().contains(Opcode::Check))
        return PassOutcome::Unchanged;

    bool changed = removeChecksInUnreachableBlocks();
    collectChecks();
    changed |= removeRedundantChecks();
    bool hasDroppedChecks = dropChecksNobodyReliesOn();
    hasDroppedChecks |= dropChecksThatTurnAwayWhatArrives();
    hasDroppedChecks |= guardElementsThatOneOperatorTakes();
    if (m_sites.isEmpty()) {
        if (hasDroppedChecks)
            finishTyping();
        return changed || hasDroppedChecks ? PassOutcome::Changed : PassOutcome::Unchanged;
    }

    // The first suspension of a generator stands once in its body.
    bool isUnsplitGenerator = m_function.isUnsplitCoroutine() && m_function.has(FunctionFlags::IsGenerator);
    bool runsSeldom = isInColderPartOfProgram(m_context.inputs.heat, m_program, m_context.id);
    if (runsSeldom || isUnsplitGenerator || m_function.role == FunctionRole::GenericVersion || keepsMoreAliveOverAwaitThanFrameMapHolds() || !tryCreateGenericVersion())
        fallBackToGuards();
    return PassOutcome::Changed;
}

// Such a function builds only where its allocations sink past the await, and a second version of what uses them stands in the way of that.
bool GenericVersionBuilder::keepsMoreAliveOverAwaitThanFrameMapHolds()
{
    if (!m_function.isUnsplitCoroutine())
        return false;
    uint64_t maxRoots = m_context.limit(LimitID::MaxFrameMapRoots);
    if (m_function.values.size() <= maxRoots)
        return false;

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        bool awaits = std::ranges::any_of(values, [&](ValueID value) {
            return m_function.at(value).opcode == Opcode::Await;
        });
        if (!awaits)
            continue;
        Liveness::LocalCalc live(m_analyses.liveness(), block);
        for (uint32_t index = values.size(); index--;) {
            if (m_function.at(values[index]).opcode == Opcode::Await) {
                uint64_t numberOfLiveValues = 0;
                live.forEachLive([&](ValueID) {
                    ++numberOfLiveValues;
                });
                if (numberOfLiveValues > maxRoots)
                    return true;
            }
            live.execute(index);
        }
    }
    return false;
}

// Whether an operation looks at the checked value, or a position of some type takes it. Appends the check and the casts of its value.
bool GenericVersionBuilder::isReliedOn(ValueID check, Vector<ValueID, 8>& casts)
{
    const UseLists& useLists = m_analyses.useLists();
    TypingContext typing { m_program, m_function, m_context.types };
    // The type of an array literal follows its elements, and a store into a typed field tests a value that is not proven. So only calls count among the operations that take any value.
    auto takesAnyValue = [&](const Value& user, uint32_t operandIndex) {
        if (user.operandUses(operandIndex) != OperandUses { OperandUse::Retains })
            return false;
        switch (user.operandKind(operandIndex)) {
        case OperandKind::Any:
            return effectRuleOf(user.opcode) == EffectRule::Call;
        case OperandKind::Declared: {
            TypeID required = requiredTypeOfOperand(typing, user, operandIndex);
            return !required || required == TypeTable::anything;
        }
        case OperandKind::Boolean:
        case OperandKind::Integer:
        case OperandKind::Number:
        case OperandKind::String:
        case OperandKind::PropertyKey:
        case OperandKind::Object:
        case OperandKind::Callable:
        case OperandKind::Array:
        case OperandKind::Layout:
        case OperandKind::Environment:
        case OperandKind::EvalOperand:
        case OperandKind::Results:
        case OperandKind::IteratorState:
        case OperandKind::Enumerator:
        case OperandKind::Exception:
        case OperandKind::ClassToken:
        case OperandKind::Guard:
        case OperandKind::Convertible:
        case OperandKind::StringBuilder:
        case OperandKind::Cursor:
        case OperandKind::KeyOrder:
        case OperandKind::FlatString:
        case OperandKind::CodeAddress:
        case OperandKind::BuilderCursor:
            return false;
        }
    };

    BitVector isSameValue(m_function.values.size());
    Vector<ValueID, 8> worklist;
    auto add = [&](ValueID value) {
        if (isSameValue.add(value.index()))
            worklist.append(value);
    };
    add(check);
    casts.append(check);
    while (!worklist.isEmpty()) {
        for (const Use& use : useLists.usesOf(worklist.takeLast())) {
            switch (use.kind) {
            case Use::Kind::Operand: {
                const Value& user = m_function.at(use.user);
                if (user.opcode == Opcode::Upcast || user.opcode == Opcode::AssumeType) {
                    if (!isSameValue.get(use.user.index()))
                        casts.append(use.user);
                    add(use.user);
                } else if (!takesAnyValue(user, use.operandIndex))
                    return true;
                break;
            }
            case Use::Kind::TerminatorOperand: {
                const Terminator& terminator = m_function.at(use.block).terminator;
                if (terminator.kind != TerminatorKind::Return)
                    return true;
                TypeID required = requiredTypeOfOperand(typing, terminator, use.operandIndex);
                if (required && required != TypeTable::anything)
                    return true;
                break;
            }
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument: {
                const CompactList<ValueID>& parameters = m_function.at(targetOf(m_function, use.edge()).block).parameters;
                if (use.operandIndex >= parameters.size())
                    return true;
                add(parameters[use.operandIndex]);
                break;
            }
            case Use::Kind::BlockSubject:
                return true;
            }
        }
    }
    return false;
}

// `fiber === cursor.current`: beside what is no number, no string and no bigint the bits are compared, whatever the checked value is.
bool GenericVersionBuilder::isOnlyComparedByBits(ValueID check) const
{
    const Type& anything = m_context.types.at(TypeTable::anything);
    std::span<const Use> uses = m_analyses.useLists().usesOf(check);
    return !uses.empty() && std::ranges::all_of(uses, [&](const Use& use) {
        if (use.kind != Use::Kind::Operand || m_function.at(use.user).opcode != Opcode::StrictEq)
            return false;
        // The other side must not owe its type to a check that goes for the same reason.
        ValueID other = m_function.at(use.user).operand(1 - use.operandIndex);
        return m_function.at(other).opcode != Opcode::Check && canCompareBits(typeOf(other), anything);
    });
}

// A value that is only handed on, as an argument of a call by value or into a position of any type, is the same value in both versions of the function.
bool GenericVersionBuilder::dropChecksNobodyReliesOn()
{
    // A build that reports mismatches reports them where the check stands.
    if (m_reportsMismatches)
        return false;

    Vector<ValueID, 8> dropped;
    Vector<ValueID, 8> erasedCasts;
    const CompactList<ValueID>& entryParameters = m_function.at(entryBlock).parameters;
    for (const CheckSite& site : m_sites) {
        // Only the check of a parameter.
        Vector<ValueID, 8> casts;
        if (isOnlyComparedByBits(site.check))
            casts.append(site.check);
        else if (!entryParameters.contains(sameObjectThroughCasts(m_function, m_function.at(site.check).valueOperand())) || isReliedOn(site.check, casts))
            continue;
        dropped.append(site.check);
        erasedCasts.appendVector(casts);
    }
    if (dropped.isEmpty())
        return false;

    // What an operation makes of the value does not depend on its type, so nothing but the casts changes.
    AOT::sort(erasedCasts);
    removeRepeatedElements(erasedCasts);
    for (ValueID cast : erasedCasts)
        eraseProof(cast, ExpectedTypes::Kept);
    didChangeCastsInPlace();
    m_valuesTurnedAway.appendVector(dropped);
    m_context.build.statistics().count(StatisticID::ChecksReplacedByGuards, dropped.size());
    typeValuesWithoutCutting(typingInputs());
    m_analyses.didChangeTypes();
    m_sites.removeAllMatching([&](const CheckSite& site) {
        return dropped.contains(site.check);
    });
    return true;
}

// Flow has seen an ordinary object arrive that the program only calls by the type. The test would fail in ordinary runs and send them into the generic version, so the value goes on as one that has no type.
bool GenericVersionBuilder::dropChecksThatTurnAwayWhatArrives()
{
    Vector<ValueID, 8> dropped;
    Vector<ValueID, 8> primitivesStillExpected;
    for (const CheckSite& site : m_sites) {
        const Value& check = m_function.at(site.check);
        if (check.cause() == CheckCause::MayFail)
            (m_context.types.at(check.castType()).hasObjects() ? dropped : primitivesStillExpected).append(site.check);
    }
    if (dropped.isEmpty() && primitivesStillExpected.isEmpty())
        return false;
    // What is read from such an object has its type from the type that does not hold.
    BitVector isTainted = computeTaintedValues(m_function, m_analyses.useLists(), dropped.span());
    for (const CheckSite& site : m_sites) {
        if (isTainted.get(site.check.index()) && !dropped.contains(site.check))
            dropped.append(site.check);
    }
    primitivesStillExpected.removeAllMatching([&](ValueID check) {
        return dropped.contains(check);
    });
    replaceChecksWithGuards(dropped.span(), ExpectedTypes::Dropped);
    replaceChecksWithGuards(primitivesStillExpected.span(), ExpectedTypes::Kept);
    // A report that was put in front of a check has moved what stands behind it.
    m_sites.shrink(0);
    collectChecks();
    return true;
}

// `sum += list[index]`: an array and an iterator hand out undefined whatever their elements are declared to be, and the operator has a form for it. An exit would make all that the generic version reads.
bool GenericVersionBuilder::guardElementsThatOneOperatorTakes()
{
    Vector<ValueID, 8> guarded;
    for (const CheckSite& site : m_sites) {
        const Value& check = m_function.at(site.check);
        Opcode source = m_function.at(sameObjectThroughCasts(m_function, check.valueOperand())).opcode;
        std::span<const Use> uses = m_analyses.useLists().usesOf(site.check);
        if ((source != Opcode::ElemGet && source != Opcode::ResultAt) || uses.size() != 1)
            continue;
        // A switch over texts tells them apart itself.
        if (uses[0].kind == Use::Kind::TerminatorOperand && m_function.at(uses[0].block).terminator.kind == TerminatorKind::SwitchString && !typeOf(check.valueOperand()).hasObjects()) {
            guarded.append(site.check);
            continue;
        }
        if (uses[0].kind != Use::Kind::Operand)
            continue;
        const Value& user = m_function.at(uses[0].user);
        // What is compared with another value is mostly read again behind the comparison, and that read is checked.
        bool isOnlyLookedAt = user.operandKind(uses[0].operandIndex) == OperandKind::Any && !user.operandUses(uses[0].operandIndex).contains(OperandUse::Retains) && (user.numberOfOperands() == 1 || !typeOf(uses[0].user).isBoolean());
        if (isOnlyLookedAt && !m_context.types.at(check.castType()).hasObjects() && !typeOf(check.valueOperand()).hasObjects())
            guarded.append(site.check);
    }
    if (guarded.isEmpty())
        return false;
    replaceChecksWithGuards(guarded.span());
    m_sites.shrink(0);
    collectChecks();
    return true;
}

bool GenericVersionBuilder::removeChecksInUnreachableBlocks()
{
    const CFG& cfg = m_analyses.cfg();
    bool changed = false;
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        if (cfg.isReachable(BlockID(index)))
            continue;
        for (ValueID id : m_function.blocks[index].values) {
            if (m_function.at(id).opcode != Opcode::Check)
                continue;
            becomeUpcast(m_function, id, m_function.at(id).valueOperand());
            changed = true;
        }
    }
    if (changed)
        m_analyses.didChangeValues();
    return changed;
}

void GenericVersionBuilder::collectChecks()
{
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const BlockValues& values = m_function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (m_function.at(values[index]).opcode == Opcode::Check)
                m_sites.append(CheckSite { .check = values[index], .point = { block, index } });
        }
    }
}

bool GenericVersionBuilder::removeRedundantChecks()
{
    const UseLists& useLists = m_analyses.useLists();
    size_t numberOfChecks = m_sites.size();
    m_sites.removeAllMatching([&](const CheckSite& site) {
        ValueID operand = m_function.at(site.check).valueOperand();
        TypeID castType = m_function.at(site.check).castType();
        if (typeOf(operand).isSubsetOf(m_context.types.at(castType), m_program)) {
            convertToUpcast(m_function.edit(site.check, ValueEdit::Definition), operand, castType);
            return true;
        }
        if (useLists.useCount(site.check))
            return false;
        Value& check = m_function.edit(site.check);
        convertToUpcast(check, operand, m_anyValue);
        check.type = m_function.at(operand).type;
        check.setExpected({ });
        return true;
    });
    if (m_sites.size() == numberOfChecks)
        return false;
    didChangeCastsInPlace();
    return true;
}

const GenericVersionCut* GenericVersionBuilder::findCut(ProgramPoint point) const
{
    auto found = std::ranges::lower_bound(m_cuts, point, isBefore, &GenericVersionCut::point);
    return found != m_cuts.end() && found->point == point ? &*found : nullptr;
}

void GenericVersionBuilder::splitBlocks(std::span<const ProgramPoint> rejoinPoints)
{
    for (CheckSite& site : m_sites) {
        site.hasExit = !isAfterTailCall(m_function, pointAfter(site.point));
        if (site.hasExit)
            m_cuts.append(GenericVersionCut { pointAfter(site.point), { }, { } });
    }
    for (ProgramPoint point : rejoinPoints) {
        if (point.index && !isAfterTailCall(m_function, point))
            m_cuts.append(GenericVersionCut { point, { }, { } });
    }
    AOT::sort(m_cuts, isBefore, &GenericVersionCut::point);
    removeRepeatedElements(m_cuts, [](const GenericVersionCut& a, const GenericVersionCut& b) {
        return a.point == b.point;
    });

    Vector<uint32_t, 8> indexes;
    for (size_t next = 0; next < m_cuts.size();) {
        BlockID block = m_cuts[next].point.block;
        indexes.shrink(0);
        for (size_t index = next; index < m_cuts.size() && m_cuts[index].point.block == block; ++index)
            indexes.append(m_cuts[index].point.index);

        m_builder.setOrigin(m_function.at(m_function.at(block).values[indexes.first() - 1]).origin());
        BlockID previous = block;
        for (BlockID part : m_builder.splitBlock(block, indexes.span())) {
            GenericVersionCut& cut = m_cuts[next++];
            cut.blockBefore = previous;
            cut.blockAfter = part;
            previous = part;
        }
    }

    for (CheckSite& site : m_sites) {
        if (!site.hasExit)
            continue;
        const GenericVersionCut* cut = findCut(pointAfter(site.point));
        RELEASE_ASSERT(cut);
        site.block = cut->blockBefore;
        site.continuation = cut->blockAfter;
    }
}

// A block is cut wherever the copy might come back. Where it does not, the two parts are one block again: a pass that looks for an operation right behind another finds it.
void GenericVersionBuilder::mergeBlocksCutForNothing()
{
    const CFG& cfg = m_analyses.cfg();
    bool hasMerged = false;
    // From the last cut of a block to the first: what has been merged into a part goes on into the part in front of it.
    for (size_t index = m_cuts.size(); index--;) {
        const GenericVersionCut& cut = m_cuts[index];
        const Block& blockBefore = m_function.at(cut.blockBefore);
        const Block& after = m_function.at(cut.blockAfter);
        if (blockBefore.terminator.kind != TerminatorKind::Jump || blockBefore.terminator.next().block != cut.blockAfter)
            continue;
        if (!after.parameters.isEmpty() || blockBefore.handler != after.handler || blockBefore.frequency != after.frequency || !cfg.singleIncomingEdge(cut.blockAfter))
            continue;
        Block& before = m_function.edit(cut.blockBefore, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
        Block merged = std::exchange(m_function.edit(cut.blockAfter), Block { });
        before.values.appendVector(merged.values);
        before.terminator = WTF::move(merged.terminator);
        hasMerged = true;
    }
    if (hasMerged)
        m_analyses.didChangeControlFlow();
}

void GenericVersionBuilder::findUnmergeableExits()
{
    BitVector isAfterCheck(m_function.blocks.size());
    Vector<BlockID, 16> worklist;
    auto visit = [&](BlockID block) {
        if (isAfterCheck.add(block.index()))
            worklist.append(block);
    };
    for (const CheckSite& site : m_sites) {
        if (site.hasExit)
            visit(site.continuation);
    }
    while (!worklist.isEmpty()) {
        m_function.at(worklist.takeLast()).forEachTarget([&](const Target& target) {
            visit(target.block);
        });
    }

    for (CheckSite& site : m_sites)
        site.hasExit = site.hasExit && RegionCloner::canMergeAt(m_analyses, isAfterCheck, site.continuation);
}

void GenericVersionBuilder::eraseProof(ValueID id, ExpectedTypes expectedTypes)
{
    Opcode opcode = m_function.at(id).opcode;
    if (typeValidations.contains(opcode)) {
        becomeUndefined(m_function, id);
        return;
    }
    // An Upcast behind a Check adds what the type only declares, the contents of a collection for one. Its source is no longer inside its type.
    if (opcode != Opcode::Check && opcode != Opcode::AssumeType && opcode != Opcode::Upcast)
        return;

    Value& value = m_function.edit(id);
    TypeID castType = value.castType();
    if (value.opcode == Opcode::AssumeType && m_function.at(sameObjectThroughCasts(m_function, value.valueOperand())).opcode == Opcode::NewArray)
        m_erasedProofsOfLiterals.append({ value.valueOperand(), castType });
    convertToUpcast(value, value.valueOperand(), m_anyValue);
    if (expectedTypes == ExpectedTypes::Kept)
        value.setExpected(m_context.types.intern(m_context.types.at(castType).withoutClassProofs()));
}

// A cast was replaced by another cast of the same operand, or a payload changed. Use lists, identities and liveness depend only on operands and on which opcodes are casts.
void GenericVersionBuilder::didChangeCastsInPlace()
{
    static_assert(isCast(Opcode::Check) && isCast(Opcode::AssumeType) && isCast(Opcode::Upcast));
    static_assert(OperandIndex::Check::value == OperandIndex::Upcast::value && OperandIndex::AssumeType::value == OperandIndex::Upcast::value);
    m_analyses.invalidate(valueAnalyses - analysesThatReadNoTypes);
}

void GenericVersionBuilder::eraseProofsInCopy(const GenericCopy& copy, const BitVector& isTainted, size_t numberOfTypedBlocks)
{
    BitVector isTaintedCopy(m_function.values.size());
    for (size_t index : isTainted) {
        if (ValueID id = copy.copyOf(ValueID(index))) {
            eraseProof(id, ExpectedTypes::Kept);
            isTaintedCopy.quickSet(id.index());
        }
    }

    for (size_t index = 0; index < numberOfTypedBlocks; ++index) {
        BlockID genericBlock = copy.copyOf(BlockID(index));
        const Terminator& terminator = m_function.blocks[index].terminator;
        if (genericBlock && terminator.kind == TerminatorKind::ReturnCall && isTainted.get(terminator.callOperand().index()))
            m_function.edit(genericBlock, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator.setCasts({ });
    }

    // A callee brings its own generic version along, so inlining here as well would double the code at each level of calls.
    for (BlockID block : copy.copiedBlocks()) {
        for (ValueID id : m_function.at(block).values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::Call || (value.opcode == Opcode::CallDirect && !value.isAlwaysInlined()))
                m_function.edit(id, ValueEdit::Definition).setIsNeverInlined(true);

            bool takesTaintedOperand = false;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                takesTaintedOperand |= operand && isTaintedCopy.get(operand.index());
            });
            if (!takesTaintedOperand)
                continue;
            if (Value::opcodesWithReceivers.contains(value.opcode))
                m_function.edit(id, ValueEdit::Definition).setReceivers({ });
            if (Value::opcodesWithCallees.contains(value.opcode))
                m_function.edit(id, ValueEdit::Definition).setCallees({ });
        }
    }
    m_analyses.didChangeValues();
}

Value GenericVersionBuilder::createMismatchReport(ValueID value, TypeID expectedType, TypeSiteID site) const
{
    Value report = IRBuilder::make(Opcode::ReportMismatch, { value });
    report.setExpectedType(expectedType);
    report.setSite(site);
    return report;
}

void GenericVersionBuilder::lowerCheck(GenericCopy& copy, const CheckSite& site)
{
    const Value& check = m_function.at(site.check);
    ValueID operand = check.valueOperand();
    TypeID castType = check.castType();
    TypeSiteID typeSite = check.site();
    m_builder.setOrigin(check.origin());

    BlockID genericContinuation = copy.copyOf(site.continuation);
    RELEASE_ASSERT(genericContinuation);

    Block& block = m_function.edit(site.block, { BlockEdit::Values, BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments });
    RELEASE_ASSERT(block.values.last() == site.check && block.terminator.next().block == site.continuation);
    block.values.removeLast();
    block.terminator = Terminator { };
    m_builder.setHandler(m_reportsMismatches ? block.handler : std::nullopt);
    m_analyses.didChangeControlFlow();

    m_builder.moveToEndOf(site.block);
    BlockID failedUnasked;
    ValueID valueInCopy;
    if (m_builder.failsPredictions()) {
        failedUnasked = m_builder.addBlock(m_function.at(genericContinuation).frequency);
        valueInCopy = m_builder.addParameter(failedUnasked, m_function.at(operand).type);
        m_builder.emitFailureOfPredictions(Target { failedUnasked, { operand }, { } });
    }
    ValueID test = m_builder.appendTypeTest(operand, castType);
    IRBuilder::ProvenBranch branch = m_builder.emitBranchWithProof(test, OperandIndex::TypeTest::value, m_function.at(site.block).frequency, m_function.at(genericContinuation).frequency);

    convertToUpcast(m_function.edit(site.check, ValueEdit::Definition), branch.provenOnTaken, castType);
    m_function.edit(branch.taken, BlockEdit::Values).values.append(site.check);
    m_analyses.didChangeValues();
    m_builder.emitJump(site.continuation);

    m_builder.moveToEndOf(branch.notTaken);
    if (m_reportsMismatches)
        m_builder.append(createMismatchReport(branch.provenOnNotTaken, castType, typeSite));
    if (failedUnasked) {
        m_builder.emitJump(failedUnasked, { branch.provenOnNotTaken });
        m_builder.moveToEndOf(failedUnasked);
        m_valuesTurnedAway.append(valueInCopy);
    } else
        valueInCopy = branch.provenOnNotTaken;
    m_builder.emitJump(genericContinuation);
    copy.addDefinition(site.check, valueInCopy);
    m_valuesTurnedAway.append(branch.provenOnNotTaken);
    if (ValueID checkInCopy = copy.copyOf(site.check))
        m_valuesTurnedAway.append(checkInCopy);
}

CalleeSetID GenericVersionBuilder::withCalleesNobodyHasSeen(CalleeSetID stamp)
{
    if (!stamp)
        return { };
    const CalleeSet& callees = m_context.types.at(stamp);
    if (!callees.isComplete)
        return stamp;
    return m_context.types.intern(CalleeSet { .functions = callees.functions, .builtins = callees.builtins, .isComplete = false });
}

LayoutSetID GenericVersionBuilder::withReceiversNobodyHasSeen(LayoutSetID stamp)
{
    const LayoutSet& receivers = m_context.types.at(stamp);
    if (!receivers.isComplete)
        return stamp;
    if (receivers.members.isEmpty() || receivers.hasMembersWithoutLayout)
        return { };
    return m_context.types.intern(LayoutSet { .members = receivers.members, .isComplete = false });
}

void GenericVersionBuilder::replaceChecksWithGuards(std::span<const ValueID> checks, ExpectedTypes expectedTypes)
{
    if (checks.empty())
        return;

    if (m_reportsMismatches) {
        const UseLists& useLists = m_analyses.useLists();
        Vector<ProgramPoint, 8> points(checks.size(), [&](size_t index) {
            return useLists.pointOf(checks[index]);
        });
        for (size_t index = 0; index < checks.size(); ++index) {
            const Value& check = m_function.at(checks[index]);
            m_builder.setOrigin(check.origin());
            m_builder.insertBefore(points[index], createMismatchReport(check.valueOperand(), check.castType(), check.site()));
        }
        m_builder.applyInsertions();
    }

    m_hasGuardsInTypedBlocks = true;
    BitVector isTainted = computeTaintedValues(m_function, m_analyses.useLists(), checks);
    BitVector isReplaced(m_function.values.size());
    for (ValueID check : checks)
        isReplaced.set(check.index());
    bool hasErasedValidation = false;
    for (size_t index : isTainted) {
        if (m_function.values[index].opcode != Opcode::Check || isReplaced.get(index)) {
            hasErasedValidation |= typeValidations.contains(m_function.values[index].opcode);
            eraseProof(ValueID(index), expectedTypes);
        }
    }
    for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
        const Block& block = m_function.blocks[index];
        const Terminator& terminator = block.terminator;
        if (terminator.kind == TerminatorKind::ReturnCall && isTainted.get(terminator.callOperand().index()) && terminator.casts().size())
            m_function.edit(BlockID(index), BlockEdit::Terminator).terminator.setCasts({ });
        // What flow has proven of an operation was read off a run in which every check had passed. Where the type of the check is expected no longer, it is what is left to expect.
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            bool hasReceivers = Value::opcodesWithReceivers.contains(value.opcode) && value.receivers();
            bool hasCallees = Value::opcodesWithCallees.contains(value.opcode);
            if (!hasReceivers && !hasCallees)
                continue;
            bool takesTaintedOperand = false;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                takesTaintedOperand |= operand && isTainted.get(operand.index());
            });
            if (!takesTaintedOperand)
                continue;
            if (hasReceivers)
                m_function.edit(id, ValueEdit::Definition).setReceivers(expectedTypes == ExpectedTypes::Dropped ? withReceiversNobodyHasSeen(value.receivers()) : LayoutSetID());
            if (!hasCallees)
                continue;
            CalleeSetID callees = withCalleesNobodyHasSeen(value.callees());
            if (callees != value.callees())
                m_function.edit(id, ValueEdit::Definition).setCallees(callees);
        }
    }
    // A validation that is erased lets go of its operand.
    if (hasErasedValidation)
        m_analyses.didChangeValues();
    else
        didChangeCastsInPlace();
    m_valuesTurnedAway.append(checks);
    m_context.build.statistics().count(StatisticID::ChecksReplacedByGuards, checks.size());
}

static TypeID typeOfKind(TypeArena& types, OperandKind kind)
{
    switch (kind) {
    case OperandKind::Boolean:
        return TypeTable::boolean;
    case OperandKind::Integer:
        return TypeTable::integer;
    case OperandKind::Number:
        return TypeTable::number;
    case OperandKind::String:
        return types.intern(Type::fromPrimitives(Primitive::AnyString));
    case OperandKind::PropertyKey:
        return types.intern(Type::fromPrimitives(Primitive::AnyString | Primitive::AnySymbol));
    case OperandKind::Object:
    case OperandKind::Layout:
        return TypeTable::object;
    case OperandKind::Callable:
        return TypeTable::function;
    case OperandKind::Array:
        return types.intern(Type::builtinClass(BuiltinClasses::Array));
    case OperandKind::Any:
    case OperandKind::Declared:
    case OperandKind::Environment:
    case OperandKind::EvalOperand:
    case OperandKind::Results:
    case OperandKind::Convertible:
    case OperandKind::IteratorState:
    case OperandKind::Enumerator:
    case OperandKind::Exception:
    case OperandKind::ClassToken:
    case OperandKind::Guard:
    case OperandKind::StringBuilder:
    case OperandKind::Cursor:
    case OperandKind::KeyOrder:
    case OperandKind::FlatString:
    case OperandKind::CodeAddress:
    case OperandKind::BuilderCursor:
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// An operation that takes one kind of value has no form for any other.
bool GenericVersionBuilder::assumeOperandKinds()
{
    TypeArena& types = m_context.types;
    Vector<OperandOfOtherKind, 8> found;
    auto visit = [&](ProgramPoint point, ValueID user, const auto& instruction) {
        instruction.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            TypeID required = typeOfKind(types, instruction.operandKind(operandIndex));
            TypeID type = m_function.at(operand).type;
            if (!required || !types.at(type).isJavaScriptValue() || types.at(type).isSubsetOf(types.at(required), m_program))
                return;
            found.append({ point, user, operandIndex, types.intersectionOf(type, required) });
        });
    };
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        const Block& contents = m_function.at(block);
        uint32_t numberOfValues = contents.values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index)
            visit({ block, index }, contents.values[index], m_function.at(contents.values[index]));
        visit({ block, numberOfValues }, { }, contents.terminator);
    }
    if (found.isEmpty())
        return false;
    insertAssumedTypes(found.span());
    return true;
}

void GenericVersionBuilder::insertAssumedTypes(std::span<const OperandOfOtherKind> uses)
{
    for (const OperandOfOtherKind& use : uses) {
        auto instruction = [&]() -> Instruction& {
            if (use.user)
                return m_function.edit(use.user, ValueEdit::Definition);
            return m_function.edit(use.point.block, BlockEdit::Terminator).terminator;
        };
        const Instruction& user = use.user ? static_cast<const Instruction&>(m_function.at(use.user)) : m_function.at(use.point.block).terminator;
        ValueID operand = user.operand(use.operandIndex);
        m_builder.setOrigin(user.origin());
        if (m_context.build.validates())
            m_builder.insertBefore(use.point, makeValidateType(operand, use.castType));
        ValueID assumed = m_builder.insertBefore(use.point, makeAssumeType(operand, use.castType));
        instruction().setOperand(use.operandIndex, assumed);
    }
    m_builder.applyInsertions();
    m_analyses.didChangeValues();
}

// The slot of an object is written without a test. storeUnprovenInitialValues() has dealt with what an allocation writes, but for a field of which flow says that nothing outside its type is ever stored.
static bool writesSlotOfObjectWithoutProof(const Program& program, const Value& value, uint32_t operandIndex)
{
    switch (holderOfRequiredTypes(program, value)) {
    case RequiredTypeHolder::StoredSlot:
        return true;
    case RequiredTypeHolder::InitializedSlot: {
        std::optional<uint32_t> valueIndex = value.variadicOperandIndex(operandIndex);
        if (!valueIndex)
            return true;
        const Field& field = fieldAt(program, declarationOf(program, { value.layout(), value.fields()[*valueIndex] }));
        return !field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
    }
    case RequiredTypeHolder::PositionThatFlowFollows:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Flow follows what a check turns away through the same operations, so what it has proven of a position holds for the generic version too.
bool GenericVersionBuilder::assumeTypesOfProvenPositions()
{
    TypeArena& types = m_context.types;
    TypingContext typing { m_program, m_function, types };
    BitVector mayBeTurnedAway = computeValuesTurnedAway(m_function, m_analyses.cfg(), m_valuesTurnedAway.span());
    Vector<OperandOfOtherKind, 8> found;
    // A literal is born with the storage that fits the types of its elements: one whose element is not known to be a number gets the storage that holds everything.
    BitVector literalsSeen;
    auto assumeElementsOfLiteral = [&](ValueID array, TypeID required) {
        const ClassPart* part = types.at(required).singleBuiltinClass();
        if (!part || !part->contentsAreProven || (part->arrayStorage != ArrayStorage::Int32 && part->arrayStorage != ArrayStorage::Double))
            return;
        ValueID literal = m_analyses.sameObjectThroughCasts(array);
        if (m_function.at(literal).opcode != Opcode::NewArray || literalsSeen.set(literal.index()))
            return;
        TypeID elements = part->contentsAt(m_program.builtins(), ContentsSlot::Element);
        ProgramPoint point = m_analyses.useLists().pointOf(literal);
        for (uint32_t index = OperandIndex::NewArray::elements; index < m_function.at(literal).numberOfOperands(); ++index) {
            ValueID element = m_function.at(literal).operand(index);
            if (!mayBeTurnedAway.get(element.index()) && !types.at(m_function.at(element).type).isSubsetOf(types.at(elements), m_program))
                found.append({ point, literal, index, elements });
        }
    };
    auto visit = [&](ProgramPoint point, ValueID user, ValueID operand, uint32_t operandIndex, TypeID required) {
        if (!operand || !required || required == TypeTable::anything || mayBeTurnedAway.get(operand.index()))
            return;
        TypeID type = m_function.at(operand).type;
        if (!types.at(type).isJavaScriptValue() || !types.at(required).isJavaScriptValue() || types.at(type).isSubsetOf(types.at(required), m_program))
            return;
        found.append({ point, user, operandIndex, required });
        assumeElementsOfLiteral(operand, required);
    };

    bool hasChanged = false;
    bool hasOneResult = m_function.results.size() == 1 && !m_function.isUnsplitCoroutine();
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        // An ill-typed store in the typed code is somebody's mistake, for the verifier to find.
        if (!m_hasGuardsInTypedBlocks && !m_isGenericBlock.isEmpty() && !m_isGenericBlock.get(block.index()))
            continue;
        const Block& contents = m_function.at(block);
        uint32_t numberOfValues = contents.values.size();
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            ValueID user = contents.values[index];
            const Value& value = m_function.at(user);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Declared && !writesSlotOfObjectWithoutProof(m_program, value, operandIndex))
                    visit({ block, index }, user, operand, operandIndex, requiredTypeOfOperand(typing, value, operandIndex));
            });
        }
        if (!hasOneResult)
            continue;

        TypeID requiredResult = typing.typeIDOf(m_function.result());
        Terminator& terminator = m_function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
        if (terminator.kind == TerminatorKind::Return)
            visit({ block, numberOfValues }, { }, terminator.valueOperand(), OperandIndex::Return::value, requiredResult);
        if (terminator.kind != TerminatorKind::ReturnCall || requiredResult == TypeTable::anything)
            continue;

        Type returned = typing.typeOf(terminator.callOperand());
        Vector<TailCast, 2> casts;
        for (TailCast cast : terminator.casts()) {
            returned = returned.castTo(typing.typeOf(cast.declared), types);
            casts.append(cast);
        }
        if (returned.isSubsetOf(types.at(requiredResult), m_program))
            continue;
        TailCast assumed;
        assumed.declared.type = requiredResult;
        assumed.resolution = CastResolution::Assume;
        casts.append(assumed);
        terminator.setCasts(casts.span());
        hasChanged = true;
    }
    // A literal need not come by a position of this function: it may be stored by name.
    for (auto [array, proven] : std::exchange(m_erasedProofsOfLiterals, { }))
        assumeElementsOfLiteral(array, proven);
    if (!found.isEmpty())
        insertAssumedTypes(found.span());
    return hasChanged || !found.isEmpty();
}

void GenericVersionBuilder::finishTyping()
{
    typeValuesWithoutCutting(typingInputs());
    m_analyses.didChangeTypes();
    if (assumeOperandKinds()) {
        typeValuesWithoutCutting(typingInputs());
        m_analyses.didChangeTypes();
    }
    // Whatever it stores behind an allocation, it puts a value in for.
    size_t numberOfValuesBefore = m_function.values.size();
    storeUnprovenInitialValues(m_program, m_context.id, m_function, m_context.types, m_context.build);
    if (m_function.values.size() != numberOfValuesBefore)
        m_analyses.didChangeValues();
    // The casts of a tail call are no operands.
    if (assumeTypesOfProvenPositions()) {
        typeValuesWithoutCutting(typingInputs());
        m_analyses.didChangeTypes();
    }
    finishTypedValues(typingInputs());
}

bool GenericVersionBuilder::tryCreateGenericVersion()
{
    uint64_t maxBlocks = m_context.limit(LimitID::MaxBlocksInGenericVersion);
    // An access to a checked receiver runs unknown code in the copy alone: in the typed code it becomes a load.
    bool comesBackBehindCalls = isLibraryCode(m_program, m_function);
    Vector<ProgramPoint> rejoinPoints = findRejoinPoints(m_analyses, [&](ValueID value) {
        if (!comesBackBehindCalls)
            return false;
        Opcode opcode = m_function.at(value).opcode;
        EffectRule rule = effectRuleOf(opcode);
        return (rule == EffectRule::Call || rule == EffectRule::Builtin || opcode == Opcode::Await) && m_analyses.mayCallUnknownCode(value);
    }, [](ProgramPoint) {
        return true;
    });
    // The check of what a call returns stands right behind the call. Behind the check the typed code has the checked type, and later what the callee proves. A join in front of it would keep the check for good.
    for (ProgramPoint& point : rejoinPoints) {
        const BlockValues& values = m_function.at(point.block).values;
        auto isCastAt = [&](uint32_t index) {
            Opcode opcode = m_function.at(values[index]).opcode;
            return opcode == Opcode::Check || opcode == Opcode::Upcast || opcode == Opcode::AssumeType;
        };
        while (point.index && point.index < values.size() && isCastAt(point.index))
            ++point.index;
    }
    removeRepeatedElements(rejoinPoints);

    auto savedBlocks = m_function.blocks;
    size_t savedValueCount = m_function.values.size();
    auto restoreBlocks = [&] {
        m_function.replaceBlocks(WTF::move(savedBlocks));
        m_function.editAllValues().shrink(savedValueCount);
        m_analyses.didChangeControlFlow();
    };

    splitBlocks(rejoinPoints.span());
    findUnmergeableExits();

    Vector<ValueID, 8> checks;
    Vector<ValueID, 8> checksWithExit;
    Vector<ValueID, 8> checksWithoutExit;
    GenericCopy copy(m_context, m_builder);
    for (const CheckSite& site : m_sites) {
        checks.append(site.check);
        if (!site.hasExit) {
            checksWithoutExit.append(site.check);
            continue;
        }
        checksWithExit.append(site.check);
        copy.addStart(site.continuation);
        copy.addEntry(site.continuation);
        copy.addValueToBeNarrowed(site.check);
    }
    if (checksWithExit.isEmpty()) {
        restoreBlocks();
        return false;
    }
    for (ProgramPoint point : rejoinPoints) {
        if (!point.index)
            copy.addRejoinBlock(point.block);
        else if (const GenericVersionCut* cut = findCut(point))
            copy.addRejoinBlock(cut->blockAfter);
    }

    switch (copy.plan(maxBlocks)) {
    case GenericCopy::Extent::NotCopied:
    case GenericCopy::Extent::OnlyCopiedBlocks:
        if (m_function.blocks.size() > maxBlocks)
            m_context.gaveUp(LimitID::MaxBlocksInGenericVersion, checks.first());
        restoreBlocks();
        return false;
    case GenericCopy::Extent::ToEndOfFunction:
    case GenericCopy::Extent::ToWaysBack:
        break;
    }

    // A value of the copy that is live where the copy comes back gets a parameter at each join, and SSARepair looks at every block for each such value.
    uint64_t numberOfPlannedValues = 0;
    for (BlockID block : copy.plannedBlocks())
        numberOfPlannedValues += m_function.at(block).values.size();
    if (numberOfPlannedValues * copy.plannedBlocks().size() > m_context.limit(LimitID::MaxGenericVersionJoinWork)) {
        m_context.gaveUp(LimitID::MaxGenericVersionJoinWork, checks.first());
        restoreBlocks();
        return false;
    }

    replaceChecksWithGuards(checksWithoutExit.span());
    BitVector isTainted = computeTaintedValues(m_function, m_analyses.useLists(), checksWithExit.span());
    size_t numberOfTypedBlocks = m_function.blocks.size();
    copy.clone(genericBlockFrequency);
    eraseProofsInCopy(copy, isTainted, numberOfTypedBlocks);
    for (const CheckSite& site : m_sites) {
        if (site.hasExit)
            lowerCheck(copy, site);
    }
    m_builder.setHandler(std::nullopt);

    copy.addWaysBack(checks.span(), GenericCopy::TypesTestedOnWayBack::Expected);
    SSARepair repair(m_builder, m_analyses);
    copy.addVariablesTo(repair);
    size_t numberOfBlocksBeforeRepair = m_function.blocks.size();
    repair.run();
    m_analyses.didChangeControlFlow();
    mergeBlocksCutForNothing();

    Statistics& statistics = m_context.build.statistics();
    statistics.count(StatisticID::GenericVersionsCreated);
    statistics.count(StatisticID::ExitsToGenericVersion, checksWithExit.size());
    statistics.count(StatisticID::WaysBackFromGenericVersion, copy.numberOfWaysBack());
    statistics.count(StatisticID::BlocksInGenericVersions, copy.copiedBlocks().size());

    for (BlockID block : copy.copiedBlocks())
        m_isGenericBlock.set(block.index());
    // SSARepair cuts a covered block behind a definition, and what it cuts off a generic block is as generic.
    const CFG& cfg = m_analyses.cfg();
    for (BlockID block : cfg.reversePostOrder()) {
        if (block.index() < numberOfBlocksBeforeRepair)
            continue;
        std::optional<Edge> incoming = cfg.singleIncomingEdge(block);
        if (incoming && m_isGenericBlock.get(incoming->from.index()))
            m_isGenericBlock.set(block.index());
    }
    finishTyping();
    return true;
}

void GenericVersionBuilder::fallBackToGuards()
{
    if (m_function.role != FunctionRole::GenericVersion) {
        m_context.build.statistics().count(StatisticID::GenericVersionsNotCreated);
        m_context.addDecline(Wire::DeclineKind::GenericVersionNotCreated, m_sites.first().check, { }, LimitID::None, static_cast<uint32_t>(m_sites.size()));
    }

    Vector<ValueID, 8> checks(m_sites.size(), [&](size_t index) {
        return m_sites[index].check;
    });
    replaceChecksWithGuards(checks.span());
    finishTyping();
}

PassOutcome addGenericVersion(FunctionPassContext& context)
{
    if (!context.function.hasBody())
        return PassOutcome::Unchanged;
    // The generic version takes over what is alive where a check fails, and an object that it takes over has to be made.
    PassOutcome outcome = leaveURLsUnbuilt(context) ? PassOutcome::Changed : PassOutcome::Unchanged;
    outcome |= GenericVersionBuilder(context).run();
    // The analyses were told of every edit when it was made.
    if (outcome == PassOutcome::Changed)
        context.keptAnalyses = allAnalyses;
    return outcome;
}

static const DeclaredType* findDeclaredType(const Program& program, Position position)
{
    switch (position.kind) {
    case Position::Kind::Variable:
        return &program.variables[position.owner].declared;
    case Position::Kind::Field:
        return &program.layouts[position.owner].fields[position.index].declared;
    case Position::Kind::EntryParameter:
        return program.functions[position.owner].declaredEntryParameter(position.index);
    case Position::Kind::Result:
        return &program.functions[position.owner].results[position.index];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool markExpected(Program& program, Position position)
{
    DeclaredType* declared = const_cast<DeclaredType*>(findDeclaredType(program, position));
    if (!declared || declared->flags.contains(PositionFlags::IsExpected) || !program.at(declared->type).isJavaScriptValue())
        return false;
    declared->flags.add(PositionFlags::IsExpected);
    declared->flags.remove(PositionFlags::IsInt32Promise);
    return true;
}

static bool canBeAssignedByEvaluatedText(EvalBindingKind kind)
{
    switch (kind) {
    case EvalBindingKind::Variable:
    case EvalBindingKind::Arguments:
    case EvalBindingKind::PrivateName:
    case EvalBindingKind::SimpleCatchParameter:
        return true;
    case EvalBindingKind::Constant:
    case EvalBindingKind::FunctionName:
    case EvalBindingKind::This:
    case EvalBindingKind::NewTarget:
    case EvalBindingKind::HomeObject:
    case EvalBindingKind::ClassObject:
    case EvalBindingKind::PrivateMethod:
    case EvalBindingKind::PrivateAccessor:
    case EvalBindingKind::WithObject:
    case EvalBindingKind::EvalVariables:
    case EvalBindingKind::MappedArgument:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Evaluated text is one more writer that no type holds back.
static Vector<Position> findPositionsAssignedByEvaluatedText(const Program& program)
{
    Vector<Position> positions;
    auto add = [&](Position position) {
        if (findDeclaredType(program, position)->type != TypeTable::anything)
            positions.append(position);
    };
    for (const EvalScope& scope : program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!canBeAssignedByEvaluatedText(binding.kind))
                continue;
            if (binding.variable)
                add({ Position::Kind::Variable, binding.variable.index(), 0 });
            else if (binding.layout && binding.field && program.at(binding.layout).isEnvironment())
                add({ Position::Kind::Field, binding.layout.index(), *binding.field });
        }
    }
    return positions;
}

static bool isObjectField(const Program& program, Position position)
{
    return position.kind == Position::Kind::Field && !program.layouts[position.owner].isEnvironment();
}

static bool markNamespaceFields(Program& program, std::span<const Position> marked)
{
    if (marked.empty() || marked.front().kind != Position::Kind::Variable)
        return false;

    bool changed = false;
    for (const Module& module : program.modules) {
        if (!module.namespaceLayout)
            continue;
        for (uint32_t index = 0; index < module.exports.size(); ++index) {
            const ModuleExport& entry = module.exports[index];
            if (entry.kind != ModuleExportKind::Variable || !entry.variable)
                continue;
            if (std::ranges::binary_search(marked, Position { Position::Kind::Variable, entry.variable.index(), 0 }))
                changed |= markExpected(program, { Position::Kind::Field, module.namespaceLayout.index(), index });
        }
    }
    return changed;
}

static std::optional<Position> storedPosition(const Program& program, FunctionID id, const TypedStore& store)
{
    static_assert(!static_cast<unsigned>(EntryParameter::Capture));

    if (!store.operation)
        return Position { Position::Kind::Result, id.index(), 0 };

    const Value& operation = *store.operation;
    auto variadicIndex = [&] {
        return *operation.variadicOperandIndex(store.operandIndex);
    };
    switch (operation.opcode) {
    case Opcode::PutModuleVariable:
        return Position { Position::Kind::Variable, operation.variable().index(), 0 };
    case Opcode::PutEnvironmentField:
        return Position { Position::Kind::Field, operation.layout().index(), operation.field() };
    case Opcode::NewObject:
    case Opcode::CloneAs: {
        FieldReference field = declarationOf(program, { operation.layout(), operation.fields()[variadicIndex()] });
        bool staysInAllocation = fieldAt(program, field).flow.stability.contains(FieldStability::IsNeverStoredWithOtherType);
        if (!program.at(operation.layout()).isEnvironment() && !staysInAllocation && canStoreInitialValueBehindAllocation(program, operation, variadicIndex()))
            return std::nullopt;
        return Position { Position::Kind::Field, field.layout.index(), field.field };
    }
    case Opcode::NewClosure:
    case Opcode::CallDirect:
        return Position { Position::Kind::EntryParameter, operation.function().index(), variadicIndex() };
    case Opcode::Results:
        return Position { Position::Kind::Result, id.index(), variadicIndex() };
    default:
        return std::nullopt;
    }
}

static std::optional<Position> readPosition(const Program& program, const Function& function, const Value& read)
{
    switch (read.opcode) {
    case Opcode::GetModuleVariable:
        return Position { Position::Kind::Variable, read.variable().index(), 0 };
    case Opcode::GetEnvironmentField:
        return Position { Position::Kind::Field, read.layout().index(), read.field() };
    case Opcode::CallDirect:
        if (program.at(read.function()).results.size() != 1)
            return std::nullopt;
        return Position { Position::Kind::Result, read.function().index(), 0 };
    case Opcode::Call: {
        FunctionID only = onlyCalleeOf(program, program.typeOf(function, read.operand(OperandIndex::Call::callee)));
        if (!only)
            return std::nullopt;
        return Position { Position::Kind::Result, only.index(), 0 };
    }
    case Opcode::ResultAt: {
        const Value& producer = function.at(read.resultsOperand());
        if (producer.opcode != Opcode::CallDirect)
            return std::nullopt;
        return Position { Position::Kind::Result, producer.function().index(), read.index() };
    }
    default:
        return std::nullopt;
    }
}

// After flow a read by name through a proven receiver has the type of the fields that it may find. None for any other operation.
static std::optional<PropertyKeyID> keyOfTypedReadByName(const Program& program, const Value& read)
{
    PropertyKeyID key;
    switch (read.opcode) {
    case Opcode::PropGet:
        key = read.key();
        break;
    case Opcode::PrivateGet:
        key = program.at(read.layout()).fields[read.field()].key;
        break;
    default:
        return std::nullopt;
    }
    if (program.at(read.type).isAnything())
        return std::nullopt;
    return key;
}

static bool hasMarkedField(std::span<const Position> marked)
{
    auto firstField = std::ranges::lower_bound(marked, Position { Position::Kind::Field, 0, 0 });
    return firstField != marked.end() && firstField->kind == Position::Kind::Field;
}

static bool readsMarkedFieldByName(const Program& program, const Function& function, const Value& read, std::span<const Position> marked)
{
    std::optional<PropertyKeyID> key = keyOfTypedReadByName(program, read);
    if (!key || !hasMarkedField(marked))
        return false;
    return std::ranges::any_of(membersOf(program, program.typeOf(function, read.objectOperand())), [&](LayoutID member) {
        std::optional<FieldReference> field = findField(program, member, *key, BaseChain::Proven);
        if (!field)
            return false;
        FieldReference declaration = declarationOf(program, *field);
        return std::ranges::binary_search(marked, Position { Position::Kind::Field, declaration.layout.index(), declaration.field });
    });
}

static void forEachTypedStore(const TypingContext& typing, const CFG& cfg, NOESCAPE const Invocable<void(const TypedStore&)> auto& functor)
{
    const Function& function = typing.function;
    auto visit = [&](const TypedStore& store) {
        if (store.stored && store.required && store.required != TypeTable::anything)
            functor(store);
    };

    TypeID requiredResult;
    if (function.results.size() == 1 && !function.isUnsplitCoroutine())
        requiredResult = typing.typeIDOf(function.result());

    const OpcodeSet& opcodesWithRequiredTypes = opcodesWithRequiredOperandTypes();
    for (BlockID block : cfg.reversePostOrder()) {
        const Block& contents = function.at(block);
        for (ValueID id : contents.values) {
            const Value& value = function.at(id);
            if (!opcodesWithRequiredTypes.contains(value.opcode))
                continue;
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                if (value.operandKind(operandIndex) == OperandKind::Declared)
                    visit({ operand, requiredTypeOfOperand(typing, value, operandIndex), &value, operandIndex });
            });
        }
        const Terminator& terminator = contents.terminator;
        if (terminator.kind == TerminatorKind::Return)
            visit({ terminator.valueOperand(), requiredResult });
        else if (terminator.kind == TerminatorKind::ReturnCall)
            visit({ terminator.callOperand(), requiredResult });
    }
}

static Vector<TypedStore, 16> findTypedStores(const TypingContext& typing, const CFG& cfg)
{
    Vector<TypedStore, 16> stores;
    forEachTypedStore(typing, cfg, [&](const TypedStore& store) {
        stores.append(store);
    });
    return stores;
}

static void appendIllTypedStores(const TypingContext& typing, FunctionID id, std::span<const TypedStore> stores, const BitVector* storedValues, Vector<Position>& positions)
{
    for (const TypedStore& store : stores) {
        if (storedValues && !storedValues->get(store.stored.index()))
            continue;
        if (typing.typeOf(store.stored).isSubsetOf(typing.types.at(store.required), typing.program))
            continue;
        if (std::optional<Position> position = storedPosition(typing.program, id, store))
            positions.append(*position);
    }
}

namespace {

// In flow every read of a property may answer undefined, also the read of a field that flow's results then call always filled.
class PositionsProvenByTypes {
    WTF_MAKE_NONCOPYABLE(PositionsProvenByTypes);
public:
    PositionsProvenByTypes(Program& program, const FlowResults& flow, BuildContext& build)
        : m_program(program)
        , m_flow(flow)
        , m_build(build)
    {
    }

    ~PositionsProvenByTypes() { m_build.destroyLater(WTF::move(m_bodies)); }

    void findCandidates();
    bool hasCandidates() const { return !m_assumed.isEmpty() || !m_assumedWithoutStart.isEmpty(); }
    // Gives up the candidates that do not hold in the program as it is now. True if it has given one up.
    bool solve();
    // True while the candidates that are left hold.
    bool passes(FunctionID, const Function&, const Value& check) const;
    // Only right behind solve(): it names values by their numbers.
    void apply();

private:
    struct Arrival {
        Position position;
        ValueID value;
    };
    // The target holds as long as the source does.
    struct Link {
        Position target;
        Position source;
    };
    struct TestedCheck {
        ValueID check;
        bool passesWhileSourcesHold { false };
        Vector<Position, 2> sources;
        Vector<Position, 1> sourcesWithoutStart;
    };
    struct Body {
        FunctionID function;
        Vector<Arrival> arrivals;
        Vector<Link> links;
        Vector<TestedCheck> checks;
        Vector<BlockID, 2> tailCallsProvenByCallees;
        Vector<Position> positionsGivenUp;
        // The target is not undefined as long as the source is not: the stored value was read from the source.
        Vector<Link> copies;
        Vector<Position> variablesThatMayHoldUndefined;
        Vector<Position> variablesNamed;
        size_t numberOfChecksThatMayFail { 0 };
    };

    // The variables, then the fields of the layouts, the entry parameters and the results of the functions. None for a position that its owner does not have.
    std::optional<uint32_t> numberOf(Position position) const
    {
        auto numberIn = [&](const Vector<uint32_t>& firstOfOwner) -> std::optional<uint32_t> {
            uint32_t first = firstOfOwner[position.owner];
            if (position.index >= firstOfOwner[position.owner + 1] - first)
                return std::nullopt;
            return first + position.index;
        };
        switch (position.kind) {
        case Position::Kind::Variable:
            return position.owner;
        case Position::Kind::Field:
            return numberIn(m_firstField);
        case Position::Kind::EntryParameter:
            return numberIn(m_firstEntryParameter);
        case Position::Kind::Result:
            return numberIn(m_firstResult);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }
    bool isAmong(const BitVector& positions, Position position) const
    {
        std::optional<uint32_t> number = numberOf(position);
        return number && positions.quickGet(*number);
    }
    void numberPositions();
    void noteCandidates();
    bool isAssumed(Position position) const { return isAmong(m_isAssumed, position); }
    bool isAssumedWithoutStart(Position position) const { return isAmong(m_isAssumedWithoutStart, position); }
    bool holdsDeclaredType(Position position) const { return isAssumed(position) || !findDeclaredType(m_program, position)->flags.contains(PositionFlags::IsExpected); }
    const Type& declaredTypeOf(Position position) const { return m_program.at(findDeclaredType(m_program, position)->type); }
    Type typeWithoutStartOf(Position) const;
    void addVariableIfOpen(Position, const LocationFlow&, std::span<const Position> assignedByEvaluatedText);
    void scanStoresIntoVariables(Body&, const Function&, const Value&) const;
    void stopListingStartsThatNobodyReads(Function&) const;

    void scan(Body&) const;
    void scanCall(Body&, const Function&, const Value&) const;
    void scanReturns(Body&, const Function&, TypeArena&) const;
    bool appendResultsTestedBy(const Function&, ValueID call, TypeID testedType, Vector<Position, 2>& sources) const;
    bool appendPositionsTestedBy(FunctionID, const Function&, const Value& check, TestedCheck&) const;
    bool passes(const TestedCheck&) const;
    void arrive(Body&, const Function&, Position, ValueID) const;
    void giveUpParametersOf(Body&, FunctionID callee, bool alsoThis) const;
    void collectPositionsGivenUp(Body&) const;

    Program& m_program;
    const FlowResults& m_flow;
    BuildContext& m_build;
    Vector<Position> m_assumed;
    Vector<Position> m_assumedWithoutStart;
    // The same candidates by number: a body asks about every argument, store and test that it has, in every round.
    BitVector m_isAssumed;
    BitVector m_isAssumedWithoutStart;
    // Each with one entry more than there are owners.
    Vector<uint32_t> m_firstField;
    Vector<uint32_t> m_firstEntryParameter;
    Vector<uint32_t> m_firstResult;
    ParallelArray<Body> m_bodies;
};

// Stands around a typing of a body without its checks that is made in the body itself. Where it ends, the casts and all types are what they were where it began.
class BodyStateRestorer {
    WTF_MAKE_NONCOPYABLE(BodyStateRestorer);
public:
    explicit BodyStateRestorer(Function& function)
        : m_function(function)
        , m_types(function.values.size(), [&](size_t index) {
            return SavedTypes { function.values[index].type, function.values[index].expected() };
        })
    {
    }

    ~BodyStateRestorer()
    {
        // A cast may have been saved twice. What it was at first comes last.
        for (size_t index = m_casts.size(); index--;)
            m_function.edit(m_casts[index].id) = WTF::move(m_casts[index].value);
        for (size_t index = 0; index < m_types.size(); ++index) {
            Value& value = m_function.edit(ValueID(index));
            value.type = m_types[index].type;
            value.setExpected(m_types[index].expected);
        }
    }

    void willRewriteCast(ValueID id) { m_casts.append(SavedCast { id, m_function.at(id) }); }

private:
    struct SavedTypes {
        TypeID type;
        TypeID expected;
    };
    struct SavedCast {
        ValueID id;
        Value value;
    };

    Function& m_function;
    Vector<SavedTypes> m_types;
    Vector<SavedCast, 16> m_casts;
};

} // anonymous namespace

static ValueID definitionThroughCasts(const Function& function, ValueID value)
{
    while (function.at(value).opcode == Opcode::Upcast || function.at(value).opcode == Opcode::AssumeType)
        value = function.at(value).valueOperand();
    return value;
}

// What passes a check is the value that went in.
static ValueID definitionThroughCastsAndChecks(const Function& function, ValueID value)
{
    while (true) {
        switch (function.at(value).opcode) {
        case Opcode::Upcast:
        case Opcode::AssumeType:
        case Opcode::Check:
        case Opcode::CheckInitialized:
            value = function.at(value).valueOperand();
            continue;
        default:
            return value;
        }
    }
}

static std::optional<Position> variableReadBy(const Value& read)
{
    switch (read.opcode) {
    case Opcode::GetModuleVariable:
        return Position { Position::Kind::Variable, read.variable().index(), 0 };
    case Opcode::GetEnvironmentField:
        return Position { Position::Kind::Field, read.layout().index(), read.field() };
    default:
        return std::nullopt;
    }
}

// AddGenericVersion drops a check that has nothing left to test, so no generic version comes of it.
static bool mayTurnValueAway(const Program& program, const Function& function, const Value& check)
{
    return !program.typeOf(function, check.valueOperand()).isSubsetOf(program.at(check.castType()), program);
}

static Vector<ValueID, 16> findChecks(const Program& program, FunctionID id, const PositionsProvenByTypes* proofs)
{
    const Function& function = program.at(id);
    Vector<ValueID, 16> checks;
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            const Value& check = function.at(value);
            if (check.opcode == Opcode::Check && mayTurnValueAway(program, function, check) && !(proofs && proofs->passes(id, function, check)))
                checks.append(value);
        }
    }
    return checks;
}

// The rule of such a value reads its own words and the program, and the type of no operand. As read a value has the type that its rule gives, so no check that is erased moves it.
static bool isTypedWithoutOperands(const Function& function, const Value& value)
{
    switch (value.opcode) {
    case Opcode::CallDirect:
    case Opcode::NewClosure:
    case Opcode::NewObject:
    case Opcode::CloneAs:
        return true;
    case Opcode::ResultAt:
        return function.at(value.resultsOperand()).opcode == Opcode::CallDirect;
    default:
        return false;
    }
}

namespace {

// The types that values of a body have without its checks: what TypePropagation finds where every check and every cast behind one lets everything through and all that rests on a check starts over.
class TypesWithoutChecks {
    WTF_MAKE_NONCOPYABLE(TypesWithoutChecks);
public:
    TypesWithoutChecks(const TypePropagationInputs& inputs, std::span<const ValueID> checks)
        : m_function(inputs.function)
        , m_cfg(inputs.analyses.cfg())
        , m_propagation(inputs)
        , m_anyValue(anyValueType(inputs.types))
        , m_flags(FillWith { }, inputs.function.values.size(), static_cast<uint8_t>(0))
    {
        for (ValueID check : checks)
            m_flags[check.index()] = restsOnCheck;
    }

    ~TypesWithoutChecks()
    {
        for (size_t index = m_casts.size(); index--;)
            m_function.edit(m_casts[index].id) = WTF::move(m_casts[index].value);
        for (const SavedTypes& saved : m_types) {
            m_function.edit(saved.id, ValueEdit::Type).type = saved.type;
            m_function.edit(saved.id, ValueEdit::Type).setExpected(saved.expected);
        }
        for (const SavedTypes& saved : m_typesBeforeRounds) {
            const Value& value = m_function.at(saved.id);
            if (value.type == saved.type && value.expected() == saved.expected)
                continue;
            m_function.edit(saved.id, ValueEdit::Type).type = saved.type;
            m_function.edit(saved.id, ValueEdit::Type).setExpected(saved.expected);
        }
    }

    void type(std::span<const ValueID> asked)
    {
        listSources(asked);
        findWhatRestsOnChecks();
        if (m_valuesToType.isEmpty())
            return;
        letEverythingThroughCasts();
        if (isTypedFromItself()) {
            // Rounds that do not end within their limit type every value of the body.
            m_typesBeforeRounds = Vector<SavedTypes>(m_function.values.size(), [&](size_t index) {
                return SavedTypes { ValueID(index), m_function.values[index].type, m_function.values[index].expected() };
            });
            m_propagation.restartListedValues(m_valuesToType.span());
            m_propagation.solveTypes();
            return;
        }
        for (ValueID id : m_valuesToType) {
            Value& value = m_function.edit(id);
            if (value.opcode != Opcode::Parameter) {
                value.type = m_propagation.provenTypeByRule(id);
                continue;
            }
            PlaceOfParameter place = placeOf(id);
            value.type = m_propagation.provenTypeOfParameterByRule(id, place.block, place.index);
        }
    }

private:
    static constexpr uint8_t restsOnCheck = 1 << 0;
    static constexpr uint8_t isListed = 1 << 1;
    static constexpr uint8_t isBehindItsSources = 1 << 2;

    struct Visit {
        ValueID id;
        bool hasVisitedSources;
    };
    struct SavedTypes {
        ValueID id;
        TypeID type;
        TypeID expected;
    };
    struct SavedCast {
        ValueID id;
        Value value;
    };
    struct PlaceOfParameter {
        BlockID block;
        uint32_t index { 0 };
    };

    PlaceOfParameter placeOf(ValueID parameter)
    {
        if (m_placesOfParameters.isEmpty()) {
            m_placesOfParameters.grow(m_function.values.size());
            for (BlockID block : m_cfg.reversePostOrder()) {
                const auto& parameters = m_function.at(block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index)
                    m_placesOfParameters[parameters[index].index()] = { block, index };
            }
        }
        return m_placesOfParameters[parameter.index()];
    }

    void forEachSourceOf(ValueID id, NOESCAPE const Invocable<void(ValueID)> auto& functor)
    {
        if (m_function.at(id).opcode != Opcode::Parameter) {
            m_propagation.forEachSourceOf(id, functor);
            return;
        }
        PlaceOfParameter place = placeOf(id);
        if (place.block && place.block != entryBlock)
            m_propagation.forEachSourceOfParameter(place.block, place.index, functor);
    }

    // Whatever the values are typed from, each behind its sources but where a value is typed from itself.
    void listSources(std::span<const ValueID> asked)
    {
        for (ValueID first : asked) {
            m_stack.append({ first, false });
            while (!m_stack.isEmpty()) {
                auto [id, hasVisitedSources] = m_stack.last();
                if (hasVisitedSources) {
                    m_stack.removeLast();
                    m_flags[id.index()] |= isBehindItsSources;
                    m_sources.append(id);
                    continue;
                }
                // It may wait here more than once: whoever is typed from it has put it here.
                if (m_flags[id.index()] & isListed) {
                    m_stack.removeLast();
                    continue;
                }
                m_flags[id.index()] |= isListed;
                m_stack.last().hasVisitedSources = true;
                forEachSourceOf(id, [&](ValueID source) {
                    uint8_t flags = m_flags[source.index()];
                    if (!(flags & isListed))
                        m_stack.append({ source, false });
                    else if (!(flags & isBehindItsSources))
                        m_hasCycle = true;
                });
            }
        }
    }

    void findWhatRestsOnChecks()
    {
        bool hasChanged;
        do {
            hasChanged = false;
            for (ValueID id : m_sources) {
                if (m_flags[id.index()] & restsOnCheck)
                    continue;
                bool rests = false;
                forEachSourceOf(id, [&](ValueID source) {
                    rests |= m_flags[source.index()] & restsOnCheck;
                });
                if (rests) {
                    m_flags[id.index()] |= restsOnCheck;
                    hasChanged = true;
                }
            }
        } while (hasChanged && m_hasCycle);
        for (ValueID id : m_sources) {
            if (m_flags[id.index()] & restsOnCheck)
                m_valuesToType.append(id);
        }
    }

    void letEverythingThroughCasts()
    {
        for (ValueID id : m_valuesToType) {
            Value& value = m_function.edit(id);
            switch (value.opcode) {
            case Opcode::Upcast:
                m_casts.append({ id, value });
                value.setCastType(m_anyValue);
                break;
            case Opcode::AssumeType:
            case Opcode::Check:
                m_casts.append({ id, value });
                convertToUpcast(value, value.valueOperand(), m_anyValue);
                break;
            default:
                m_types.append({ id, value.type, value.expected() });
                break;
            }
        }
    }

    // Among the values to type: one stands in front of one of its sources.
    bool isTypedFromItself()
    {
        if (!m_hasCycle)
            return false;
        Vector<uint32_t> places(FillWith { }, m_function.values.size(), UINT32_MAX);
        for (uint32_t place = 0; place < m_valuesToType.size(); ++place)
            places[m_valuesToType[place].index()] = place;
        for (uint32_t place = 0; place < m_valuesToType.size(); ++place) {
            bool standsInFrontOfSource = false;
            forEachSourceOf(m_valuesToType[place], [&](ValueID source) {
                uint32_t placeOfSource = places[source.index()];
                standsInFrontOfSource |= placeOfSource != UINT32_MAX && placeOfSource >= place;
            });
            if (standsInFrontOfSource)
                return true;
        }
        return false;
    }

    Function& m_function;
    const CFG& m_cfg;
    TypePropagation m_propagation;
    TypeID m_anyValue;
    Vector<uint8_t, 64> m_flags;
    bool m_hasCycle { false };
    Vector<Visit, 32> m_stack;
    Vector<ValueID, 64> m_sources;
    Vector<ValueID, 32> m_valuesToType;
    Vector<SavedTypes, 32> m_types;
    Vector<SavedTypes> m_typesBeforeRounds;
    Vector<SavedCast, 16> m_casts;
    Vector<PlaceOfParameter> m_placesOfParameters;
};

} // anonymous namespace

static bool tellsObjectsApartByLayout(const Type& type)
{
    return type.layouts && !type.anyObject;
}

static void appendFieldsGivenObjectsThatChecksTurnAway(const Program& program, const Function& function, const CFG& cfg, std::span<const ValueID> checks, Vector<Position>& positions)
{
    Vector<ValueID, 8> checksOfLayouts;
    Vector<ValueID, 8> checksOfLayoutsThatMayFail;
    for (ValueID id : checks) {
        const Value& check = function.at(id);
        if (!tellsObjectsApartByLayout(program.at(check.castType())))
            continue;
        checksOfLayouts.append(id);
        if (check.cause() == CheckCause::MayFail)
            checksOfLayoutsThatMayFail.append(id);
    }
    if (checksOfLayouts.isEmpty())
        return;
    BitVector mayBeTurnedAway = computeValuesTurnedAway(function, cfg, checksOfLayoutsThatMayFail.span());
    BitVector isAnsweredForByCheckAlone = computeValuesTurnedAway(function, cfg, checksOfLayouts.span());
    auto append = [&](FieldReference field) {
        FieldReference declaration = declarationOf(program, field);
        if (tellsObjectsApartByLayout(program.at(fieldAt(program, declaration).declared.type)))
            positions.append({ Position::Kind::Field, declaration.layout.index(), declaration.field });
    };
    for (BlockID block : cfg.reversePostOrder()) {
        for (ValueID id : function.at(block).values) {
            const Value& store = function.at(id);
            if (store.opcode == Opcode::NewObject || store.opcode == Opcode::CloneAs) {
                std::span<const ValueID> initialValues = store.variadicOperands();
                if (program.at(store.layout()).isEnvironment() || store.fields().size() != initialValues.size())
                    continue;
                for (uint32_t index = 0; index < initialValues.size(); ++index) {
                    if (mayBeTurnedAway.get(initialValues[index].index()))
                        append({ store.layout(), store.fields()[index] });
                }
                continue;
            }
            if (store.opcode == Opcode::CopyDataProperties) {
                if (!isAnsweredForByCheckAlone.get(store.sourceOperand().index()))
                    continue;
                for (LayoutID member : membersOf(program, program.typeOf(function, store.targetOperand()))) {
                    for (uint32_t field = 0; field < program.at(member).fields.size(); ++field)
                        append({ member, field });
                }
                continue;
            }
            if ((store.opcode != Opcode::PropDefine && store.opcode != Opcode::PropSet) || !mayBeTurnedAway.get(store.valueOperand().index()))
                continue;
            for (LayoutID member : membersOf(program, program.typeOf(function, store.objectOperand()))) {
                if (std::optional<FieldReference> field = findField(program, member, store.key(), BaseChain::Proven))
                    append(*field);
            }
        }
    }
}

// After flow only the very value that a check has turned away counts.
static void collectTaintedStores(const Program& program, FunctionID id, Function* bodyToTypeInPlace, BuildContext& build, TaintedPositionsRun run, const PositionsProvenByTypes* proofs, Vector<Position>& positions)
{
    const Function& function = program.at(id);
    ScratchTypeArena scratch(program.types);
    TypeArena& types = scratch.arena();
    AnalysisContext context { build, program, nullptr, nullptr, id };

    bool isAfterFlow = run == TaintedPositionsRun::AfterFlow;
    Vector<ValueID, 16> checks = findChecks(program, id, proofs);
    if (isAfterFlow && checks.isEmpty())
        return;

    FunctionAnalyses analyses(context, function);
    if (isAfterFlow)
        appendFieldsGivenObjectsThatChecksTurnAway(program, function, analyses.cfg(), checks.span(), positions);
    TypingContext typing { program, function, types };
    // Erasing a check changes neither which operands are stores nor what they require.
    Vector<TypedStore, 16> stores = findTypedStores(typing, analyses.cfg());
    if (stores.isEmpty())
        return;

    BitVector isTainted;
    BitVector mayBeTurnedAway;
    bool storesTaintedValue = false;
    if (isAfterFlow)
        mayBeTurnedAway = computeValuesTurnedAway(function, analyses.cfg(), checks.span());

    // Past form AsRead the type of a value need not be what its rule gives.
    if (!checks.isEmpty() && bodyToTypeInPlace && program.form() == IRForm::AsRead) {
        auto mayBecomeIllTyped = [&](const TypedStore& store) {
            if (isAfterFlow && !mayBeTurnedAway.get(store.stored.index()))
                return false;
            return !isTypedWithoutOperands(function, function.at(store.stored)) && storedPosition(program, id, store);
        };
        Vector<ValueID, 16> storedValuesToType;
        for (const TypedStore& store : stores) {
            if (mayBecomeIllTyped(store))
                storedValuesToType.append(store.stored);
        }
        if (storedValuesToType.isEmpty()) {
            if (!isAfterFlow)
                appendIllTypedStores(typing, id, stores.span(), nullptr, positions);
            return;
        }
        TypesWithoutChecks typesWithoutChecks(TypePropagationInputs { program, id, *bodyToTypeInPlace, analyses, types, build }, checks.span());
        typesWithoutChecks.type(storedValuesToType.span());
        appendIllTypedStores(typing, id, stores.span(), isAfterFlow ? &mayBeTurnedAway : nullptr, positions);
        return;
    }

    if (!checks.isEmpty()) {
        if (!isAfterFlow)
            isTainted = computeTaintedValues(function, analyses.useLists(), checks.span());
        const BitVector& storedValues = isAfterFlow ? mayBeTurnedAway : isTainted;
        // Past form AsRead the type of a value need not be what its rule gives, and a typing counts the limits that it hits.
        bool looksAtTaintedStores = program.form() == IRForm::AsRead;
        auto mayBecomeIllTyped = [&](const TypedStore& store) {
            if (!storedValues.get(store.stored.index()))
                return false;
            return !looksAtTaintedStores || (!isTypedWithoutOperands(function, function.at(store.stored)) && storedPosition(program, id, store));
        };
        storesTaintedValue = std::ranges::any_of(stores, mayBecomeIllTyped);
    }
    if (!storesTaintedValue) {
        if (!isAfterFlow)
            appendIllTypedStores(typing, id, stores.span(), nullptr, positions);
        return;
    }
    if (isAfterFlow)
        isTainted = computeTaintedValues(function, analyses.useLists(), checks.span());

    std::optional<Function> copy;
    std::optional<BodyStateRestorer> restorer;
    if (bodyToTypeInPlace)
        restorer.emplace(*bodyToTypeInPlace);
    Function& erased = bodyToTypeInPlace ? *bodyToTypeInPlace : copy.emplace(function);
    auto willRewriteCast = [&](ValueID cast) {
        if (restorer)
            restorer->willRewriteCast(cast);
    };

    TypeID anyValue = anyValueType(types);
    for (ValueID check : checks) {
        willRewriteCast(check);
        convertToUpcast(erased.edit(check), erased.at(check).valueOperand(), anyValue);
    }
    // A check that has nothing left to test in the typed code is dropped in front of the copy, so it holds nothing back there.
    for (size_t index : isTainted) {
        Value& value = erased.edit(ValueID(index));
        if (value.opcode != Opcode::Upcast && value.opcode != Opcode::AssumeType && value.opcode != Opcode::Check)
            continue;
        willRewriteCast(ValueID(index));
        if (value.opcode == Opcode::Upcast)
            value.setCastType(anyValue);
        else
            convertToUpcast(value, value.valueOperand(), anyValue);
    }

    // A cast has become another cast of the same operand: the graph and the use lists of the body are those of the body without its checks.
    TypePropagation propagation(TypePropagationInputs { program, id, erased, analyses, types, build });
    propagation.restartDependentsOf(checks.span());
    propagation.solveTypes();
    appendIllTypedStores(TypingContext { program, erased, types }, id, stores.span(), isAfterFlow ? &mayBeTurnedAway : nullptr, positions);
}

bool mayReturnTaintedValue(const Program& program, FunctionID id, BuildContext& build)
{
    Vector<Position> positions;
    collectTaintedStores(program, id, nullptr, build, TaintedPositionsRun::BeforeFlow, nullptr, positions);
    return std::ranges::any_of(positions, [&](Position position) {
        return position.kind == Position::Kind::Result && position.owner == id.index();
    });
}

static ReadsOfBody listReads(const Program& program, const Function& function)
{
    ReadsOfBody reads;
    reads.isListed = true;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            if (std::optional<Position> position = readPosition(program, function, value))
                reads.positions.append(*position);
            else if (keyOfTypedReadByName(program, value))
                reads.typedReadsByName.append(id);
        }
    }
    AOT::sort(reads.positions);
    removeRepeatedElements(reads.positions);
    reads.positions.shrinkToFit();
    reads.typedReadsByName.shrinkToFit();
    return reads;
}

static bool haveCommonPosition(std::span<const Position> ascendingPositions, std::span<const Position> otherAscendingPositions)
{
    if (ascendingPositions.size() > otherAscendingPositions.size())
        std::swap(ascendingPositions, otherAscendingPositions);
    return std::ranges::any_of(ascendingPositions, [&](Position position) {
        return std::ranges::binary_search(otherAscendingPositions, position);
    });
}

static bool readsMarkedPosition(const Program& program, FunctionID id, const ReadsOfBody& reads, std::span<const Position> marked)
{
    const Function& function = program.at(id);
    // The positions of one kind and one owner stand together, by index.
    auto hasMarkedPositionOfItsOwn = [&](Position::Kind kind, size_t numberOfPositions) {
        auto first = std::ranges::lower_bound(marked, Position { kind, id.index(), 0 });
        return first != marked.end() && first->kind == kind && first->owner == id.index() && first->index < numberOfPositions;
    };
    if (hasMarkedPositionOfItsOwn(Position::Kind::EntryParameter, function.numberOfEntryParameters()) || hasMarkedPositionOfItsOwn(Position::Kind::Result, function.results.size()))
        return true;
    if (haveCommonPosition(reads.positions.span(), marked))
        return true;
    if (reads.typedReadsByName.isEmpty() || !hasMarkedField(marked))
        return false;
    return std::ranges::any_of(reads.typedReadsByName, [&](ValueID value) {
        return readsMarkedFieldByName(program, function, function.at(value), marked);
    });
}

// Which bodies read a position of an owner: a variable, the fields of a layout, the results of a function.
class ReadersByOwner {
public:
    ReadersByOwner(const Program& program, const ParallelLoops& loops, std::span<const ReadsOfBody> readsOfBodies)
        : m_firstLayout(program.variables.size())
        , m_firstFunction(m_firstLayout + program.layouts.size())
    {
        size_t numberOfOwners = m_firstFunction + program.functions.size();
        m_first.fill(0, numberOfOwners + 1);
        forEachIndexInParallel(loops, readsOfBodies.size(), [&](unsigned index) {
            forEachOwnerIn(readsOfBodies[index].positions.span(), [&](uint32_t owner) {
                AtomicRef(m_first[owner + 1]).fetch_add(1, std::memory_order_relaxed);
            });
        });
        for (size_t owner = 0; owner < numberOfOwners; ++owner)
            m_first[owner + 1] += m_first[owner];
        m_readers.grow(m_first[numberOfOwners]);
        Vector<uint32_t> next = m_first;
        // The order inside a row is that of arrival. Nobody reads it: a reader is a mark on a body.
        forEachIndexInParallel(loops, readsOfBodies.size(), [&](unsigned index) {
            forEachOwnerIn(readsOfBodies[index].positions.span(), [&](uint32_t owner) {
                m_readers[AtomicRef(next[owner]).fetch_add(1, std::memory_order_relaxed)] = FunctionID(index);
            });
        });
    }

    // None: nobody reads an entry parameter but its own function.
    std::optional<uint32_t> ownerOf(Position position) const
    {
        switch (position.kind) {
        case Position::Kind::Variable:
            return position.owner;
        case Position::Kind::Field:
            return m_firstLayout + position.owner;
        case Position::Kind::Result:
            return m_firstFunction + position.owner;
        case Position::Kind::EntryParameter:
            return std::nullopt;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::span<const FunctionID> readersOf(uint32_t owner) const LIFETIME_BOUND { return m_readers.span().subspan(m_first[owner], m_first[owner + 1] - m_first[owner]); }

    // The positions of one owner stand together.
    void forEachOwnerIn(std::span<const Position> ascendingPositions, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        std::optional<uint32_t> last;
        for (Position position : ascendingPositions) {
            std::optional<uint32_t> owner = ownerOf(position);
            if (owner && owner != last)
                functor(*owner);
            last = owner;
        }
    }

private:
    uint32_t m_firstLayout;
    uint32_t m_firstFunction;
    Vector<uint32_t> m_first;
    Vector<FunctionID> m_readers;
};

// What a round has to know of a body without touching its list.
enum class BodyToAsk : uint8_t {
    // The table does not hold all that the body reads: its list is not made, or names an owner that the list before it did not name.
    IsOutsideTable = 1 << 0,
    HasTypedReadsByName = 1 << 1,
};

static bool namesOwnerThatIsNotIn(std::span<const Position> ascendingPositions, std::span<const Position> otherAscendingPositions)
{
    auto isBefore = [](Position a, Position b) {
        return std::tie(a.kind, a.owner) < std::tie(b.kind, b.owner);
    };
    size_t other = 0;
    for (Position position : ascendingPositions) {
        while (other < otherAscendingPositions.size() && isBefore(otherAscendingPositions[other], position))
            ++other;
        if (other == otherAscendingPositions.size() || isBefore(position, otherAscendingPositions[other]))
            return true;
    }
    return false;
}

static Vector<GuardedRead> collectGuardedReads(const Program& program, FunctionID id, const Function& function, FunctionAnalyses& analyses, std::span<const Position> marked, Vector<ValueID, 16>& valuesReadingMarkedPositions)
{
    const UseLists& useLists = analyses.useLists();
    Vector<GuardedRead> reads;
    auto addReadOf = [&](ValueID value, const DeclaredType& declared) {
        GuardedRead read { value, useLists.pointAfter(value), declared, { } };
        const BlockValues& values = function.at(read.insertionPoint.block).values;
        while (read.insertionPoint.index < values.size() && function.at(values[read.insertionPoint.index]).opcode == Opcode::ResultAt)
            ++read.insertionPoint.index;
        for (const Use& use : useLists.usesOf(value)) {
            if (acceptsCastOfValue(function, use))
                read.uses.append(use);
        }
        if (!read.uses.isEmpty())
            reads.append(WTF::move(read));
    };
    auto addRead = [&](ValueID value, Position position) {
        if (!std::ranges::binary_search(marked, position))
            return;
        valuesReadingMarkedPositions.append(value);
        addReadOf(value, *findDeclaredType(program, position));
    };

    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    uint32_t numberOfEntryParameters = std::min<uint32_t>(function.numberOfEntryParameters(), entryParameters.size());
    for (uint32_t index = 0; index < numberOfEntryParameters; ++index)
        addRead(entryParameters[index], { Position::Kind::EntryParameter, id.index(), index });

    for (BlockID block : analyses.cfg().reversePostOrder()) {
        for (ValueID value : function.at(block).values) {
            if (std::optional<Position> position = readPosition(program, function, function.at(value)))
                addRead(value, *position);
            else if (readsMarkedFieldByName(program, function, function.at(value), marked)) {
                valuesReadingMarkedPositions.append(value);
                addReadOf(value, DeclaredType { .type = function.at(value).type, .flags = PositionFlags::IsExpected });
            }
        }
    }
    return reads;
}

static void resolveTailCasts(const TypingContext& typing, Function& function)
{
    bool hasExpectedResult = function.results.size() == 1 && function.result().flags.contains(PositionFlags::IsExpected);
    for (Block& block : function.editAllBlocks()) {
        Terminator& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        Type source = typing.typeOf(terminator.callOperand());
        Vector<TailCast, 4> casts;
        for (TailCast cast : terminator.casts()) {
            // In the run after flow: what flow has proven of the call stays proven. Resolved again it would be a check, which ends the tail call.
            bool isProvenByFlow = cast.resolution == CastResolution::Assume;
            if (hasExpectedResult && !isProvenByFlow) {
                cast.declared.flags.add(PositionFlags::IsExpected);
                cast.declared.flags.remove(PositionFlags::IsInt32Promise);
            }
            if (!isProvenByFlow)
                cast.resolution = resolveConvert(typing, source, cast.declared).cast;
            source = typeOfConvert(typing, source, source, cast.declared);
            casts.append(cast);
        }
        terminator.setCasts(casts.span());
    }
}

static uint32_t insertReadChecks(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build, std::span<const Position> marked)
{
    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    TypingContext typing { program, function, types };
    uint32_t numberOfChecks = 0;

    IRBuilder builder(context, function, types, &analyses);
    Vector<ValueID, 16> changedValues;
    for (const GuardedRead& read : collectGuardedReads(program, id, function, analyses, marked, changedValues)) {
        DeclaredType checked = read.declared;
        checked.flags.remove(PositionFlags::IsExpected);
        ConvertResolution resolution = resolveConvert(typing, typing.typeOf(read.declared), checked);
        if (resolution.cast != CastResolution::Check)
            continue;

        Value check = IRBuilder::make(Opcode::Check, { read.value });
        check.setCastType(resolution.castType);
        check.setSite(checked.site);
        check.setCause(CheckCause::AddedByPass);
        builder.setOrigin(function.at(read.value).origin());
        ValueID checkID = builder.insertBefore(read.insertionPoint, WTF::move(check));
        for (const Use& use : read.uses)
            replaceUse(function, use, checkID);
        changedValues.append(checkID);
        ++numberOfChecks;
    }
    builder.applyInsertions();

    // A check has the type that its read had, so most of what is typed from the read keeps its types, and nothing behind such a value needs a look.
    if (!analyses.cfg().hasCycle()) {
        BitVector isTypedFromChangedValue = computeTaintedValues(function, analyses.cfg(), changedValues.span());
        for (ValueID changed : changedValues)
            isTypedFromChangedValue.quickClear(changed.index());
        for (size_t index : isTypedFromChangedValue) {
            if (function.values[index].opcode == Opcode::Upcast)
                changedValues.append(ValueID(index));
        }
    }
    narrowValueTypesBehindChanges(TypePropagationInputs { program, id, function, analyses, types, build }, changedValues.span());
    storeUnprovenInitialValues(program, id, function, types, build);
    resolveTailCasts(typing, function);
    return numberOfChecks;
}

static uint64_t guardTaintedPositions(Program& program, BuildContext& build, TaintedPositionsRun run, const PositionsProvenByTypes* proofs)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    const ParallelLoops& loops = build.parallelLoops();
    ProgramEditor editor(program);

    Vector<FunctionID> worklist = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& functionsWithBody) {
        if (program.functions[index].hasBody())
            functionsWithBody.append(FunctionID(index));
    });
    sortLargestFirst(program, loops, worklist);

    uint64_t numberOfPositions = 0;
    std::atomic<uint64_t> numberOfChecks { 0 };
    bool markedObjectField = false;
    // Flow has looked at the text that may be evaluated.
    Vector<Position> assignedByEvaluatedText;
    if (run == TaintedPositionsRun::BeforeFlow)
        assignedByEvaluatedText = findPositionsAssignedByEvaluatedText(program);
    Vector<ReadsOfBody> readsOfBodies(program.functions.size());
    Vector<OptionSet<BodyToAsk>> bodiesToAsk(FillWith { }, program.functions.size(), OptionSet<BodyToAsk> { BodyToAsk::IsOutsideTable });
    std::optional<ReadersByOwner> readers;
    // A body that was edited keeps its list until the new one is made, which is held against it.
    auto listReadsOf = [&](FunctionID id) {
        ReadsOfBody& reads = readsOfBodies[id.index()];
        if (reads.isListed)
            return;
        ReadsOfBody listed = listReads(program, program.at(id));
        OptionSet<BodyToAsk>& toAsk = bodiesToAsk[id.index()];
        if (!readers)
            toAsk.remove(BodyToAsk::IsOutsideTable);
        else if (namesOwnerThatIsNotIn(listed.positions.span(), reads.positions.span()))
            toAsk.add(BodyToAsk::IsOutsideTable);
        toAsk.set(BodyToAsk::HasTypedReadsByName, !listed.typedReadsByName.isEmpty());
        reads = WTF::move(listed);
    };
    while (!worklist.isEmpty()) {
        Vector<Vector<Position>> storesByFunction(worklist.size());
        build.setRecordsLimitHits(false);
        {
            ProgramCaches caches(program, build);
            // No task reads the values of another task's body: typing a call reads the record of the callee.
            forEachIndexInParallel(loops, worklist.size(), [&](unsigned index) {
                collectTaintedStores(program, worklist[index], &program.at(worklist[index]), build, run, proofs, storesByFunction[index]);
                listReadsOf(worklist[index]);
            });
        }
        build.setRecordsLimitHits(true);

        Vector<Position> marked = std::exchange(assignedByEvaluatedText, { });
        for (Vector<Position>& stores : storesByFunction)
            marked.appendVector(WTF::move(stores));
        AOT::sort(marked);
        removeRepeatedElements(marked);
        marked.removeAllMatching([&](Position position) {
            return !markExpected(program, position);
        });
        if (marked.isEmpty())
            break;
        numberOfPositions += marked.size();

        bool markedObjectFieldInRound = std::ranges::any_of(marked, [&](Position position) {
            return isObjectField(program, position);
        });
        if (markedObjectFieldInRound)
            markEnforcedFields(program, build);
        markedObjectField |= markedObjectFieldInRound;
        markedObjectField |= markNamespaceFields(program, marked.span());

        if (!readers)
            readers.emplace(program, loops, readsOfBodies.span());
        Vector<bool> mayReadMarkedPosition(FillWith { }, program.functions.size(), false);
        std::optional<uint32_t> lastOwner;
        for (Position position : marked) {
            if (position.kind == Position::Kind::EntryParameter || position.kind == Position::Kind::Result)
                mayReadMarkedPosition[position.owner] = true;
            std::optional<uint32_t> owner = readers->ownerOf(position);
            if (!owner || owner == lastOwner)
                continue;
            lastOwner = owner;
            for (FunctionID reader : readers->readersOf(*owner))
                mayReadMarkedPosition[reader.index()] = true;
        }
        OptionSet<BodyToAsk> askedWhatever { BodyToAsk::IsOutsideTable };
        if (hasMarkedField(marked.span()))
            askedWhatever.add(BodyToAsk::HasTypedReadsByName);
        Vector<FunctionID> bodiesToLookAt;
        for (unsigned index = 0; index < program.functions.size(); ++index) {
            if (mayReadMarkedPosition[index] || bodiesToAsk[index].containsAny(askedWhatever))
                bodiesToLookAt.append(FunctionID(index));
        }
        worklist = collectInParallel<FunctionID>(loops, bodiesToLookAt.size(), [&](unsigned index, Vector<FunctionID>& found) {
            FunctionID id = bodiesToLookAt[index];
            if (!program.at(id).hasBody())
                return;
            listReadsOf(id);
            if (readsMarkedPosition(program, id, readsOfBodies[id.index()], marked.span()))
                found.append(id);
        });
        sortLargestFirst(program, loops, worklist);
        editFunctionsInParallel(editor, build, BehindEdit::Nothing, worklist.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            numberOfChecks.fetch_add(insertReadChecks(program, id, function, arenas.types, build, marked.span()), std::memory_order_relaxed);
            readsOfBodies[id.index()].isListed = false;
        });
    }
    build.destroyLater(WTF::move(readsOfBodies));

    if (markedObjectField)
        program.indexNames(loops);
    if (run == TaintedPositionsRun::BeforeFlow && !program.hasGuardedReads()) {
        program.setHasGuardedReads();
        Vector<FunctionID> functionsWithoutChecks = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& found) {
            if (program.functions[index].hasBody() && !waitsForGenericVersion(program, program.functions[index]))
                found.append(FunctionID(index));
        });
        sortLargestFirst(program, loops, functionsWithoutChecks);
        editFunctionsInParallel(editor, build, BehindEdit::Nothing, functionsWithoutChecks.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
            AnalysisContext context { build, program, nullptr, nullptr, id };
            FunctionAnalyses analyses(context, function);
            finishTypedValues(TypePropagationInputs { program, id, function, analyses, arenas.types, build });
        });
    }
    build.statistics().count(StatisticID::TaintedPositions, numberOfPositions);
    build.statistics().count(StatisticID::ChecksOnTaintedPositions, numberOfChecks.load(std::memory_order_relaxed));
    return numberOfPositions;
}

void guardTaintedPositions(Program& program, BuildContext& build, TaintedPositionsRun run)
{
    guardTaintedPositions(program, build, run, nullptr);
}

void PositionsProvenByTypes::numberPositions()
{
    uint32_t next = m_program.variables.size();
    auto number = [&](Vector<uint32_t>& firstOfOwner, size_t numberOfOwners, const auto& numberOfPositionsOf) {
        firstOfOwner = Vector<uint32_t>(numberOfOwners + 1, [&](size_t owner) {
            uint32_t first = next;
            next += owner < numberOfOwners ? numberOfPositionsOf(owner) : 0;
            return first;
        });
    };
    number(m_firstField, m_program.layouts.size(), [&](size_t index) { return m_program.layouts[index].fields.size(); });
    number(m_firstEntryParameter, m_program.functions.size(), [&](size_t index) { return m_program.functions[index].numberOfEntryParameters(); });
    number(m_firstResult, m_program.functions.size(), [&](size_t index) { return m_program.functions[index].results.size(); });
    m_isAssumed = BitVector(next);
    m_isAssumedWithoutStart = BitVector(next);
}

void PositionsProvenByTypes::noteCandidates()
{
    m_isAssumed.clearAll();
    m_isAssumedWithoutStart.clearAll();
    for (Position position : m_assumed)
        m_isAssumed.quickSet(*numberOf(position));
    for (Position position : m_assumedWithoutStart)
        m_isAssumedWithoutStart.quickSet(*numberOf(position));
}

void PositionsProvenByTypes::findCandidates()
{
    numberPositions();
    // Of an indirect call nobody here sees the arguments.
    BitVector isEnteredOtherwise(m_program.functions.size());
    for (const FlowIndirectCall& call : m_flow.indirectCalls())
        isEnteredOtherwise.set(call.callee.index());

    m_assumed = collectInParallel<Position>(m_build.parallelLoops(), m_program.functions.size(), [&](unsigned index, Vector<Position>& candidates) {
        const Function& function = m_program.functions[index];
        FunctionID id(index);
        if (!function.hasBody() || function.isModuleBody())
            return;
        auto addIfExpected = [&](Position position, const DeclaredType* declared) {
            if (declared && declared->flags.contains(PositionFlags::IsExpected) && m_program.at(declared->type).isJavaScriptValue())
                candidates.append(position);
        };
        if (callerStateOf(m_flow, id) == CallerState::AllKnown && !isEnteredOtherwise.get(index)) {
            if (std::optional<unsigned> thisValue = function.thisParameterIndex())
                addIfExpected({ Position::Kind::EntryParameter, index, *thisValue }, function.declaredEntryParameter(*thisValue));
            unsigned first = function.firstParameterIndex();
            for (unsigned parameter = first; parameter < first + function.numberOfPositionalParameters(); ++parameter)
                addIfExpected({ Position::Kind::EntryParameter, index, parameter }, function.declaredEntryParameter(parameter));
        }
        if (function.results.size() == 1 && !function.isUnsplitCoroutine())
            addIfExpected({ Position::Kind::Result, index, 0 }, &function.result());
    });

    Vector<Position> assignedByEvaluatedText = findPositionsAssignedByEvaluatedText(m_program);
    AOT::sort(assignedByEvaluatedText);
    for (uint32_t index = 0; index < m_program.variables.size(); ++index) {
        if (m_program.variables[index].initialization != VariableInitialization::Function)
            addVariableIfOpen({ Position::Kind::Variable, index, 0 }, m_program.variables[index].flow, assignedByEvaluatedText.span());
    }
    for (uint32_t index = 0; index < m_program.layouts.size(); ++index) {
        const Layout& layout = m_program.layouts[index];
        if (!layout.isEnvironment())
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field)
            addVariableIfOpen({ Position::Kind::Field, index, field }, layout.fields[field].flow.location, assignedByEvaluatedText.span());
    }
    AOT::sort(m_assumed);
    AOT::sort(m_assumedWithoutStart);
    noteCandidates();
}

void PositionsProvenByTypes::addVariableIfOpen(Position position, const LocationFlow& location, std::span<const Position> assignedByEvaluatedText)
{
    if (location.writers != WriterState::AllKnown || location.storeNaming != StoreNaming::EveryStoreNamesLocation || std::ranges::binary_search(assignedByEvaluatedText, position))
        return;
    const DeclaredType& declared = *findDeclaredType(m_program, position);
    const Type& type = m_program.at(declared.type);
    if (!type.isJavaScriptValue() || type.isAnything())
        return;
    // What a variable starts as arrives there too, unless nobody can read it. An allocation lists what it sets.
    bool startsUndefined = position.kind == Position::Kind::Variable && m_program.variables[position.owner].initialization == VariableInitialization::Undefined;
    if (location.initialization == InitializationState::Unknown && !(type.primitives & (startsUndefined ? Primitive::Undefined : Primitive::Absent)))
        return;
    if (declared.flags.contains(PositionFlags::IsExpected))
        m_assumed.append(position);
    if (location.initialization == InitializationState::StoredBeforeEveryRead && !!(type.primitives & Primitive::Undefined) && type != m_program.at(TypeTable::undefined))
        m_assumedWithoutStart.append(position);
}

Type PositionsProvenByTypes::typeWithoutStartOf(Position position) const
{
    Type type = declaredTypeOf(position);
    type.primitives = type.primitives & ~Primitive::Undefined;
    return type;
}

void PositionsProvenByTypes::scanStoresIntoVariables(Body& body, const Function& function, const Value& store) const
{
    bool isAllocation = store.opcode == Opcode::NewObject || store.opcode == Opcode::CloneAs;
    if (isAllocation && !m_program.at(store.layout()).isEnvironment())
        return;
    store.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (store.operandKind(operandIndex) != OperandKind::Declared)
            return;
        std::optional<Position> position = storedPosition(m_program, body.function, { operand, { }, &store, operandIndex });
        if (!position || (!isAssumed(*position) && !isAssumedWithoutStart(*position)))
            return;
        body.variablesNamed.append(*position);

        const Type& arriving = m_program.typeOf(function, operand);
        if (!arriving.isSubsetOf(declaredTypeOf(*position), m_program)) {
            // Of a position that flow has proven the types of the values may know less.
            (isAssumed(*position) ? body.positionsGivenUp : body.variablesThatMayHoldUndefined).append(*position);
            return;
        }
        // A store comes before every read, so nobody reads what the allocation lists for a `var`.
        if (isAllocation && function.at(operand).opcode == Opcode::Undefined && isAssumedWithoutStart(*position))
            return;
        body.arrivals.append({ *position, operand });
        if (!isAssumedWithoutStart(*position) || !(arriving.primitives & Primitive::Undefined))
            return;
        std::optional<Position> source = variableReadBy(function.at(definitionThroughCastsAndChecks(function, operand)));
        if (source && isAssumedWithoutStart(*source))
            body.copies.append({ *position, *source });
        else
            body.variablesThatMayHoldUndefined.append(*position);
    });
}

void PositionsProvenByTypes::arrive(Body& body, const Function& function, Position position, ValueID value) const
{
    if (!isAssumed(position))
        return;
    const Type& arriving = value ? m_program.typeOf(function, value) : m_program.at(TypeTable::undefined);
    if (!arriving.isSubsetOf(declaredTypeOf(position), m_program))
        body.positionsGivenUp.append(position);
    else if (value)
        body.arrivals.append({ position, value });
}

void PositionsProvenByTypes::giveUpParametersOf(Body& body, FunctionID callee, bool alsoThis) const
{
    const Function& target = m_program.at(callee);
    auto giveUp = [&](unsigned parameter) {
        Position position { Position::Kind::EntryParameter, callee.index(), parameter };
        if (isAssumed(position))
            body.positionsGivenUp.append(position);
    };
    std::optional<unsigned> thisValue = target.thisParameterIndex();
    if (alsoThis && thisValue)
        giveUp(*thisValue);
    unsigned first = target.firstParameterIndex();
    for (unsigned parameter = first; parameter < first + target.numberOfPositionalParameters(); ++parameter)
        giveUp(parameter);
}

void PositionsProvenByTypes::scanCall(Body& body, const Function& function, const Value& call) const
{
    static_assert(OperandIndex::Call::arguments == 2 && OperandIndex::Construct::arguments == 2 && OperandIndex::SuperConstruct::arguments == 2);
    static_assert(OperandIndex::Call::thisValue == 1 && OperandIndex::CallList::thisValue == 1);

    if (call.opcode == Opcode::CallDirect) {
        for (uint32_t index = 0; index < call.numberOfOperands(); ++index)
            arrive(body, function, { Position::Kind::EntryParameter, call.function().index(), index }, call.operand(index));
        return;
    }

    CalleeSetID stamp = call.callees();
    if (!stamp)
        return;
    bool passesThis = call.opcode == Opcode::Call || call.opcode == Opcode::CallList;
    bool passesList = call.opcode != Opcode::Call && call.opcode != Opcode::Construct && call.opcode != Opcode::SuperConstruct;
    // The bound arguments come first, and the bound `this` takes the place of the operand.
    bool mayCallBoundFunction = call.accessSite() && m_flow.mayCallBoundFunction(call.accessSite());
    for (FunctionID callee : m_program.at(stamp).functions) {
        if (mayCallBoundFunction) {
            giveUpParametersOf(body, callee, true);
            continue;
        }
        const Function& target = m_program.at(callee);
        if (std::optional<unsigned> thisValue = target.thisParameterIndex()) {
            Position position { Position::Kind::EntryParameter, callee.index(), *thisValue };
            if (passesThis)
                arrive(body, function, position, call.operand(OperandIndex::Call::thisValue));
            else if (isAssumed(position))
                body.positionsGivenUp.append(position);
        }
        if (passesList) {
            giveUpParametersOf(body, callee, false);
            continue;
        }
        unsigned first = target.firstParameterIndex();
        uint32_t numberOfArguments = call.numberOfOperands() - OperandIndex::Call::arguments;
        for (unsigned index = 0; index < target.numberOfPositionalParameters(); ++index)
            arrive(body, function, { Position::Kind::EntryParameter, callee.index(), first + index }, index < numberOfArguments ? call.operand(OperandIndex::Call::arguments + index) : ValueID());
    }
}

// False unless whatever the call answers has the tested type as long as the results that are appended hold.
bool PositionsProvenByTypes::appendResultsTestedBy(const Function& function, ValueID callID, TypeID testedType, Vector<Position, 2>& sources) const
{
    auto append = [&](FunctionID callee) {
        const Function& target = m_program.at(callee);
        if (target.results.size() != 1 || target.isUnsplitCoroutine() || !target.hasBody())
            return false;
        if (!m_program.at(target.result().type).isSubsetOf(m_program.at(testedType), m_program))
            return false;
        if (!target.result().flags.contains(PositionFlags::IsExpected))
            return true;
        Position position { Position::Kind::Result, callee.index(), 0 };
        if (!isAssumed(position))
            return false;
        sources.append(position);
        return true;
    };

    const Value& call = function.at(callID);
    if (call.opcode == Opcode::CallDirect)
        return append(call.function());
    if (call.opcode != Opcode::Call && call.opcode != Opcode::CallList)
        return false;
    CalleeSetID stamp = call.callees();
    if (!stamp)
        return false;
    const CalleeSet& callees = m_program.at(stamp);
    return callees.isComplete && callees.builtins.isEmpty() && !callees.functions.isEmpty() && std::ranges::all_of(callees.functions, append);
}

// False unless the check passes as long as the positions that are appended hold.
bool PositionsProvenByTypes::appendPositionsTestedBy(FunctionID id, const Function& function, const Value& check, TestedCheck& tested) const
{
    Vector<Position, 2>& sources = tested.sources;
    if (std::optional<Position> variable = variableReadBy(function.at(definitionThroughCastsAndChecks(function, check.valueOperand())))) {
        const Type& testedType = m_program.at(check.castType());
        if (!holdsDeclaredType(*variable))
            return false;
        if (!declaredTypeOf(*variable).isSubsetOf(testedType, m_program)) {
            if (!isAssumedWithoutStart(*variable) || !typeWithoutStartOf(*variable).isSubsetOf(testedType, m_program))
                return false;
            tested.sourcesWithoutStart.append(*variable);
        }
        if (isAssumed(*variable))
            sources.append(*variable);
        return true;
    }
    ValueID definition = definitionThroughCasts(function, check.valueOperand());
    size_t parameter = function.at(entryBlock).parameters.find(definition);
    if (parameter == notFound || parameter >= function.numberOfEntryParameters())
        return appendResultsTestedBy(function, definition, check.castType(), sources);
    Position position { Position::Kind::EntryParameter, id.index(), static_cast<uint32_t>(parameter) };
    if (!isAssumed(position) || !declaredTypeOf(position).isSubsetOf(m_program.at(check.castType()), m_program))
        return false;
    sources.append(position);
    return true;
}

bool PositionsProvenByTypes::passes(FunctionID id, const Function& function, const Value& check) const
{
    TestedCheck tested;
    return appendPositionsTestedBy(id, function, check, tested);
}

bool PositionsProvenByTypes::passes(const TestedCheck& tested) const
{
    return tested.passesWhileSourcesHold && std::ranges::all_of(tested.sources, [&](Position source) {
        return isAssumed(source);
    }) && std::ranges::all_of(tested.sourcesWithoutStart, [&](Position source) {
        return isAssumedWithoutStart(source);
    });
}

void PositionsProvenByTypes::scanReturns(Body& body, const Function& function, TypeArena& types) const
{
    Position result { Position::Kind::Result, body.function.index(), 0 };
    if (!isAssumed(result))
        return;
    TypingContext typing { m_program, function, types };
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        const Terminator& terminator = function.blocks[index].terminator;
        if (terminator.kind == TerminatorKind::Return) {
            arrive(body, function, result, terminator.valueOperand());
            continue;
        }
        if (terminator.kind != TerminatorKind::ReturnCall)
            continue;

        ValueID call = terminator.callOperand();
        Type answered = typing.typeOf(call);
        for (TailCast cast : terminator.casts()) {
            if (cast.resolution == CastResolution::Assume)
                answered = typeOfConvert(typing, answered, answered, cast.declared);
        }
        if (answered.isSubsetOf(declaredTypeOf(result), m_program)) {
            body.arrivals.append({ result, call });
            continue;
        }
        Vector<Position, 2> sources;
        if (!appendResultsTestedBy(function, call, function.result().type, sources)) {
            body.positionsGivenUp.append(result);
            continue;
        }
        for (Position source : sources)
            body.links.append({ result, source });
        body.tailCallsProvenByCallees.append(BlockID(index));
    }
}

void PositionsProvenByTypes::scan(Body& body) const
{
    const Function& function = m_program.at(body.function);
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = function.at(id);
            switch (value.opcode) {
            case Opcode::Call:
            case Opcode::CallList:
            case Opcode::Construct:
            case Opcode::ConstructList:
            case Opcode::SuperConstruct:
            case Opcode::SuperConstructList:
            case Opcode::CallDirect:
                scanCall(body, function, value);
                break;
            case Opcode::Check: {
                if (!mayTurnValueAway(m_program, function, value))
                    break;
                TestedCheck tested { id, false, { }, { } };
                tested.passesWhileSourcesHold = appendPositionsTestedBy(body.function, function, value, tested);
                body.checks.append(WTF::move(tested));
                break;
            }
            case Opcode::GetModuleVariable:
            case Opcode::GetEnvironmentField: {
                Position variable = *variableReadBy(value);
                if (isAssumed(variable) || isAssumedWithoutStart(variable))
                    body.variablesNamed.append(variable);
                break;
            }
            case Opcode::PutModuleVariable:
            case Opcode::PutEnvironmentField:
            case Opcode::NewObject:
            case Opcode::CloneAs:
                scanStoresIntoVariables(body, function, value);
                break;
            default:
                break;
            }
        }
    }
    ScratchTypeArena scratch(m_program.types);
    scanReturns(body, function, scratch.arena());
}

void PositionsProvenByTypes::collectPositionsGivenUp(Body& body) const
{
    for (const Link& link : body.links) {
        if (isAssumed(link.target) && !isAssumed(link.source))
            body.positionsGivenUp.append(link.target);
    }
    for (const Link& copy : body.copies) {
        if (isAssumedWithoutStart(copy.target) && !isAssumedWithoutStart(copy.source))
            body.variablesThatMayHoldUndefined.append(copy.target);
    }

    Vector<ValueID, 16> checksThatMayFail;
    for (const TestedCheck& tested : body.checks) {
        if (!passes(tested))
            checksThatMayFail.append(tested.check);
    }
    if (checksThatMayFail.size() == body.numberOfChecksThatMayFail)
        return;
    body.numberOfChecksThatMayFail = checksThatMayFail.size();
    if (body.arrivals.isEmpty())
        return;

    const Function& function = m_program.at(body.function);
    CFG cfg(function);
    BitVector isTainted = computeTaintedValues(function, cfg, checksThatMayFail.span());
    for (const Arrival& arrival : body.arrivals) {
        if (!isTainted.get(arrival.value.index()))
            continue;
        if (isAssumed(arrival.position))
            body.positionsGivenUp.append(arrival.position);
        else if (isAssumedWithoutStart(arrival.position))
            body.variablesThatMayHoldUndefined.append(arrival.position);
    }
}

bool PositionsProvenByTypes::solve()
{
    size_t numberOfCandidates = m_assumed.size() + m_assumedWithoutStart.size();
    // The run in between may have taken back a variable that flow had proven.
    m_assumedWithoutStart.removeAllMatching([&](Position position) {
        return !holdsDeclaredType(position);
    });
    noteCandidates();
    const ParallelLoops& loops = m_build.parallelLoops();
    m_build.destroyLater(std::exchange(m_bodies, { }));
    m_bodies = collectIntoArrayInParallel<Body>(loops, m_program.functions.size(), { }, [&](unsigned index, Vector<Body>& found) {
        if (!m_program.functions[index].hasBody())
            return;
        Body body;
        body.function = FunctionID(index);
        scan(body);
        bool saysNothing = body.arrivals.isEmpty() && body.links.isEmpty() && body.checks.isEmpty() && body.positionsGivenUp.isEmpty() && body.tailCallsProvenByCallees.isEmpty()
            && body.variablesThatMayHoldUndefined.isEmpty() && body.variablesNamed.isEmpty();
        if (!saysNothing)
            found.append(WTF::move(body));
    });

    // What a body gives up hangs on the candidates that its links, its copies and its checks rest on, and on nothing else.
    struct BodyRestingOnCandidate {
        // Twice the number of the position, and one more for the candidate without its start.
        uint32_t candidate;
        uint32_t body;

        auto operator<=>(const BodyRestingOnCandidate&) const = default;
    };
    Vector<BodyRestingOnCandidate> bodiesRestingOnCandidates = collectInParallel<BodyRestingOnCandidate>(loops, m_bodies.size(), [&](unsigned index, Vector<BodyRestingOnCandidate>& found) {
        const Body& body = m_bodies[index];
        auto add = [&](Position source, uint32_t isWithoutStart) {
            if (std::optional<uint32_t> number = numberOf(source))
                found.append({ 2 * *number + isWithoutStart, index });
        };
        for (const Link& link : body.links)
            add(link.source, 0);
        for (const Link& copy : body.copies)
            add(copy.source, 1);
        for (const TestedCheck& tested : body.checks) {
            for (Position source : tested.sources)
                add(source, 0);
            for (Position source : tested.sourcesWithoutStart)
                add(source, 1);
        }
    });
    sortAndRemoveDuplicates(loops, bodiesRestingOnCandidates);

    Vector<uint32_t> bodiesToLookAt(m_bodies.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    bool hasLookedAtEveryBody = false;
    while (hasCandidates()) {
        Vector<Position> givenUp = collectInParallel<Position>(loops, bodiesToLookAt.size(), [&](unsigned index, Vector<Position>& found) {
            found.appendVector(std::exchange(m_bodies[bodiesToLookAt[index]].positionsGivenUp, { }));
        });
        Vector<Position> mayHoldUndefined = collectInParallel<Position>(loops, bodiesToLookAt.size(), [&](unsigned index, Vector<Position>& found) {
            found.appendVector(std::exchange(m_bodies[bodiesToLookAt[index]].variablesThatMayHoldUndefined, { }));
        });
        AOT::sort(givenUp);
        AOT::sort(mayHoldUndefined);
        Vector<uint32_t> candidatesGivenUp;
        m_assumed.removeAllMatching([&](Position position) {
            if (!std::ranges::binary_search(givenUp, position))
                return false;
            candidatesGivenUp.append(2 * *numberOf(position));
            return true;
        });
        m_assumedWithoutStart.removeAllMatching([&](Position position) {
            if (!std::ranges::binary_search(givenUp, position) && !std::ranges::binary_search(mayHoldUndefined, position))
                return false;
            candidatesGivenUp.append(2 * *numberOf(position) + 1);
            return true;
        });
        noteCandidates();

        // In the first round every body says which of its checks may fail whatever holds.
        if (std::exchange(hasLookedAtEveryBody, true)) {
            bodiesToLookAt.shrink(0);
            for (uint32_t candidate : candidatesGivenUp) {
                auto first = std::ranges::lower_bound(bodiesRestingOnCandidates, BodyRestingOnCandidate { candidate, 0 });
                for (; first != bodiesRestingOnCandidates.end() && first->candidate == candidate; ++first)
                    bodiesToLookAt.append(first->body);
            }
            AOT::sort(bodiesToLookAt);
            removeRepeatedElements(bodiesToLookAt);
        }

        std::atomic<bool> givesUpMore { false };
        forEachIndexInParallel(loops, bodiesToLookAt.size(), [&](unsigned index) {
            Body& body = m_bodies[bodiesToLookAt[index]];
            collectPositionsGivenUp(body);
            if (!body.positionsGivenUp.isEmpty() || !body.variablesThatMayHoldUndefined.isEmpty())
                givesUpMore.store(true, std::memory_order_relaxed);
        });
        if (!givesUpMore.load(std::memory_order_relaxed))
            break;
    }
    return m_assumed.size() + m_assumedWithoutStart.size() != numberOfCandidates;
}

void PositionsProvenByTypes::stopListingStartsThatNobodyReads(Function& function) const
{
    for (Value& allocation : function.editAllValues()) {
        if (allocation.opcode != Opcode::NewObject || !m_program.at(allocation.layout()).isEnvironment())
            continue;
        std::span<const uint32_t> fields = allocation.fields();
        std::span<const ValueID> values = allocation.variadicOperands();
        Vector<uint32_t, 16> remainingFields;
        Vector<ValueID, 16> remainingValues;
        for (size_t index = 0; index < fields.size(); ++index) {
            if (function.at(values[index]).opcode == Opcode::Undefined && isAssumedWithoutStart({ Position::Kind::Field, allocation.layout().index(), fields[index] }))
                continue;
            remainingFields.append(fields[index]);
            remainingValues.append(values[index]);
        }
        if (remainingFields.size() == fields.size())
            continue;
        allocation.setFields(remainingFields.span());
        allocation.setVariadicOperands(remainingValues.span());
    }
}

void PositionsProvenByTypes::apply()
{
    for (Position position : m_assumed)
        const_cast<DeclaredType*>(findDeclaredType(m_program, position))->flags.remove(PositionFlags::IsExpected);
    for (Position position : m_assumedWithoutStart)
        const_cast<DeclaredType*>(findDeclaredType(m_program, position))->type = m_program.types.intern(typeWithoutStartOf(position));

    // Whoever has a proven parameter or result, tests one, or hands a value to one is typed anew. So is whoever names a proven variable.
    BitVector isChanged(m_program.functions.size());
    for (Position position : m_assumed) {
        if (position.kind == Position::Kind::EntryParameter || position.kind == Position::Kind::Result)
            isChanged.set(position.owner);
    }
    for (const Body& body : m_bodies.span()) {
        bool namesProvenVariable = std::ranges::any_of(body.variablesNamed, [&](Position variable) {
            return isAssumed(variable) || isAssumedWithoutStart(variable);
        });
        if (namesProvenVariable)
            isChanged.set(body.function.index());
        bool provesCheck = std::ranges::any_of(body.checks, [&](const TestedCheck& tested) {
            return passes(tested);
        });
        bool storesIntoProvenPosition = std::ranges::any_of(body.arrivals, [&](const Arrival& arrival) {
            return isAssumed(arrival.position);
        });
        if (provesCheck || storesIntoProvenPosition)
            isChanged.set(body.function.index());
    }
    Vector<uint32_t> bodyOfFunction;
    bodyOfFunction.fill(std::numeric_limits<uint32_t>::max(), m_program.functions.size());
    for (uint32_t index = 0; index < m_bodies.size(); ++index)
        bodyOfFunction[m_bodies[index].function.index()] = index;
    Vector<FunctionID> changed;
    for (size_t index : isChanged)
        changed.append(FunctionID(index));
    sortLargestFirst(m_program, m_build.parallelLoops(), changed);

    std::atomic<uint64_t> numberOfChecks { 0 };
    ProgramEditor editor(m_program);
    editFunctionsInParallel(editor, m_build, BehindEdit::Nothing, changed.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        if (uint32_t bodyIndex = bodyOfFunction[id.index()]; bodyIndex != std::numeric_limits<uint32_t>::max()) {
            const Body& body = m_bodies[bodyIndex];
            for (const TestedCheck& tested : body.checks) {
                if (!passes(tested))
                    continue;
                Value& check = function.edit(tested.check);
                ValueID operand = check.valueOperand();
                TypeID castType = check.castType();
                check.become(Opcode::AssumeType);
                check.setValueOperand(operand);
                check.setCastType(castType);
                numberOfChecks.fetch_add(1, std::memory_order_relaxed);
            }
            if (!body.variablesNamed.isEmpty())
                stopListingStartsThatNobodyReads(function);
            if (isAssumed({ Position::Kind::Result, id.index(), 0 })) {
                for (BlockID block : body.tailCallsProvenByCallees) {
                    Terminator& terminator = function.edit(block, { BlockEdit::Terminator, BlockEdit::Edges, BlockEdit::Arguments }).terminator;
                    Vector<TailCast, 4> casts;
                    for (TailCast cast : terminator.casts())
                        casts.append(cast);
                    casts.append(TailCast { DeclaredType { .type = function.result().type }, CastResolution::Assume });
                    terminator.setCasts(casts.span());
                }
            }
        }
        AnalysisContext context { m_build, m_program, nullptr, nullptr, id };
        FunctionAnalyses analyses(context, function);
        typeValuesFromNever(TypePropagationInputs { m_program, id, function, analyses, arenas.types, m_build });
    });
    m_build.statistics().count(StatisticID::PositionsProvenByTypes, m_assumed.size());
    m_build.statistics().count(StatisticID::ChecksProvenByTypes, numberOfChecks.load(std::memory_order_relaxed));
}

// A position that is taken back gets checks at its reads, which may fail. So the proofs are looked at again behind each run that has taken one back, and nothing is applied before both agree.
void guardTaintedPositionsAfterFlow(Program& program, const FlowResults& flow, BuildContext& build)
{
    PositionsProvenByTypes proofs(program, flow, build);
    proofs.findCandidates();
    if (!proofs.hasCandidates()) {
        guardTaintedPositions(program, build, TaintedPositionsRun::AfterFlow, nullptr);
        return;
    }
    proofs.solve();
    while (guardTaintedPositions(program, build, TaintedPositionsRun::AfterFlow, &proofs) && proofs.solve()) { }
    if (proofs.hasCandidates())
        proofs.apply();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
