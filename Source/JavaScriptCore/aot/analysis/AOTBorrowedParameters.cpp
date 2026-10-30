/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTBorrowedParameters.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTEffectSummaries.h"
#include "AOTEffectsDeclared.h"
#include "AOTEffectsDescription.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTExactBuffer.h"
#include "AOTFrameAllocationPointers.h"
#include "AOTHeldOperands.h"
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTLiveness.h"
#include "AOTResultRules.h"
#include "AOTSchemaRuntimeColumns.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <tuple>
#include <wtf/DataLog.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(3 * maxBorrowedParameters <= 64);

static constexpr unsigned contentsShift = maxBorrowedParameters;
static constexpr unsigned callsShift = 2 * maxBorrowedParameters;
static constexpr uint64_t parameterBits = (1ull << maxBorrowedParameters) - 1;
static constexpr uint64_t referenceBits = parameterBits | parameterBits << contentsShift;

static constexpr OperandUses usesOfFollowedOperand { OperandUse::Inspects, OperandUse::Returns };

static constexpr uint64_t bitOfParameter(uint32_t entryParameter) { return 1ull << entryParameter; }
static constexpr uint64_t bitOfContents(uint32_t entryParameter) { return 1ull << (contentsShift + entryParameter); }
static constexpr uint64_t bitOfCalls(uint32_t entryParameter) { return 1ull << (callsShift + entryParameter); }
static constexpr uint64_t parametersIn(uint64_t references) { return (references | references >> contentsShift) & parameterBits; }
static constexpr uint64_t contentsOf(uint64_t references) { return parametersIn(references) << contentsShift; }
static constexpr uint64_t callsOf(uint64_t references) { return parametersIn(references) << callsShift; }
static constexpr uint64_t allBitsOf(uint64_t parameters) { return parameters | parameters << contentsShift | parameters << callsShift; }
static constexpr uint64_t withDependentBits(uint64_t lost) { return lost | allBitsOf(lost & parameterBits); }

OperandSummary BorrowedParameters::summaryOf(FunctionID function, uint32_t entryParameter) const
{
    if (!isBorrowed(function, entryParameter))
        return { };

    OperandUses uses = usesOfBorrowedParameter;
    if (!mayBeCalled(function, entryParameter))
        uses.remove(callingUses);
    if (!borrowsContents(function, entryParameter))
        uses.add(OperandUse::RetainsContents);
    return { uses, RetainedIn::UnknownMemory, 0 };
}

static void forEachLostPart(uint64_t bits, const Invocable<void(uint32_t entryParameter, ASCIILiteral part)> auto& functor)
{
    for (uint32_t entryParameter = 0; entryParameter < maxBorrowedParameters; ++entryParameter) {
        if (bits & bitOfParameter(entryParameter)) {
            functor(entryParameter, "itself"_s);
            continue;
        }
        if (bits & bitOfContents(entryParameter))
            functor(entryParameter, "what is read through it"_s);
        if (bits & bitOfCalls(entryParameter))
            functor(entryParameter, "the count of its calls"_s);
    }
}

// A function that saves its frame where it waits runs on the stack of its caller until then, and the saved frame keeps only what is live and what the suspension lists.
static constexpr OpcodeSet suspensionsThatKeepOnlyLiveValues { Opcode::AwaitBySavingFrame, Opcode::AwaitCallBySavingFrame };

static OpcodeSet opcodesOfFunctionsThatBorrowNothing()
{
    OpcodeSet opcodes { Opcode::GetArgument, Opcode::GetMyArgumentByVal, Opcode::CreateRest, Opcode::CallForwardVarargs, Opcode::ConstructSuperForwardVarargs };
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (maySuspend(opcode) && !suspensionsThatKeepOnlyLiveValues.contains(opcode))
            opcodes.add(opcode);
    }
    return opcodes;
}

static bool hasReferenceAmongTrackedParameters(const Program& program, const Function& function)
{
    const CompactList<ValueID>& parameters = function.at(entryBlock).parameters;
    uint32_t numberOfTrackedParameters = std::min<uint32_t>(parameters.size(), maxBorrowedParameters);
    for (uint32_t entryParameter = 0; entryParameter < numberOfTrackedParameters; ++entryParameter) {
        if (mayBeReference(program.typeOf(function, parameters[entryParameter])))
            return true;
    }
    return false;
}

static bool mayBorrow(const Program& program, const Function& function, const OpcodeSet& opcodesThatBorrowNothing)
{
    bool savesFrameWhereItWaits = function.role == FunctionRole::CoroutineBody && function.creator.as<CreatorRecord::Coroutine>().kind == CoroutineKind::AsyncFunction;
    if (!function.hasBody() || (!function.hasOrdinaryFrame() && !savesFrameWhereItWaits) || function.isUnsplitCoroutine())
        return false;
    if (!hasReferenceAmongTrackedParameters(program, function))
        return false;
    return std::ranges::none_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return opcodesThatBorrowNothing.contains(function.at(id).opcode);
        });
    });
}

template<typename Entry>
static bool treatsOperandAsObject(Entry entry, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(entry, operandIndex);
    return !behavior.protocols.isEmpty() || !behavior.namedMembers.empty() || !behavior.calledMembers.empty();
}

static bool mayBeCellOtherThanFunction(const Type& type)
{
    return !type.isJavaScriptValue() || type.anyObject || type.layouts || !type.classes.isEmpty();
}

static std::optional<unsigned> positionOfAccessedCell(Opcode followedAccess)
{
    if (std::optional<unsigned> closure = OperandPosition::closure(followedAccess))
        return closure;
    if (std::optional<unsigned> environment = OperandPosition::environment(followedAccess))
        return environment;
    return OperandPosition::object(followedAccess);
}

static bool isCellOfFollowedAccess(Opcode opcode, uint32_t operandIndex)
{
    if (!storesFollowedByFramePointers.contains(opcode) && !readsFollowedByFramePointers.contains(opcode))
        return false;
    return positionOfAccessedCell(opcode) == operandIndex;
}

static std::optional<unsigned> positionOfCellReadBy(Opcode opcode)
{
    if (readsFollowedByFramePointers.contains(opcode))
        return positionOfAccessedCell(opcode);
    if (effectRuleOf(opcode) == EffectRule::Access && accessOf(opcode) == Access::Read)
        return receiverOf(opcode);
    return std::nullopt;
}

uint32_t HandedOnTargetLists::intern(Vector<HandedOnTarget>&& targets)
{
    Hasher hasher;
    for (const HandedOnTarget& target : targets)
        add(hasher, target.callee.index(), target.entryParameter);
    unsigned hash = hasher.hash();

    Locker locker { m_lock };
    Vector<uint32_t, 1>& lists = m_listsByHash.add(hash, Vector<uint32_t, 1> { }).iterator->value;
    for (uint32_t list : lists) {
        if (*m_lists[list] == targets)
            return list;
    }
    lists.append(m_lists.size());
    moveToExactBuffer(targets);
    m_lists.append(makeUniqueWithoutFastMallocCheck<Vector<HandedOnTarget>>(WTF::move(targets)));
    return lists.last();
}

HandedOnTargetLists::ListOfOperand HandedOnTargetLists::listOfOperand(const Program& program, CalleeSetID set, const Value& call, uint32_t operandIndex)
{
    // All that entryParameterOf() reads from the call.
    constexpr unsigned bitsOfOperandIndex = 15;
    static_assert(sizeof(Opcode) <= 2);
    std::optional<uint64_t> key;
    if (operandIndex < 1u << bitsOfOperandIndex)
        key = static_cast<uint64_t>(set.index()) << 32 | static_cast<uint64_t>(call.opcode) << (bitsOfOperandIndex + 1) | operandIndex << 1 | isHeldOperand(call, operandIndex);
    if (key) {
        Locker locker { m_lock };
        if (auto known = m_listsOfOperands.find(*key); known != m_listsOfOperands.end())
            return known->value;
    }

    ListOfOperand result;
    std::span<const FunctionID> callees = program.at(set).written().functions;
    Vector<HandedOnTarget> targets;
    targets.reserveInitialCapacity(callees.size());
    for (FunctionID callee : callees) {
        std::optional<uint32_t> entryParameter = entryParameterOf(program.at(callee), call, operandIndex);
        if (!entryParameter || *entryParameter >= maxBorrowedParameters)
            result.hasCalleeWithoutParameter = true;
        targets.append({ callee, entryParameter.value_or(maxBorrowedParameters) });
    }
    result.list = intern(WTF::move(targets));
    if (key) {
        Locker locker { m_lock };
        m_listsOfOperands.add(*key, result);
    }
    return result;
}

// Fewer callees than this stay entries of their own: nearly every call has one or two.
AOT_THRESHOLD(size_t, minCalleesOfSharedTargetList, 16);

static CalleeSetID setOfManyFunctionsAlone(const Program& program, const Value& call)
{
    CalleeSetID id = calleeSetOfCall(program, call);
    if (!id)
        return { };
    const CalleeSet& set = program.at(id);
    if (!set.listsEveryCallee() || !set.written().builtins.empty() || set.written().functions.size() < minCalleesOfSharedTargetList)
        return { };
    return id;
}

static BorrowingBody scanBorrowingBody(const Program& program, const Function& function, HandedOnTargetLists* targetLists = nullptr, const BitVector* closuresPlannedInFrame = nullptr, Vector<HandedOnReferences>* heldByWhatIsHandedOn = nullptr, PrintStream* explanation = nullptr, FunctionID explainedFunction = { })
{
    struct Carried {
        uint64_t all() const { return references | held; }

        uint64_t references { 0 };
        uint64_t held { 0 };
    };

    EffectSummaries unknownSummaries;
    EffectContext effects { program, unknownSummaries };
    Vector<uint64_t> mayBe(FillWith { }, function.values.size(), 0);
    Vector<uint64_t> mayHold;
    uint64_t tracked = 0;
    uint64_t uncountedCalls = 0;
    uint64_t lost = 0;
    ValueID currentUser;
    struct {
        uint64_t word { 0 };
        Vector<HandedOnReferences> handedOn;
        Vector<HandedOnToManyCallees> handedOnToManyCallees;
        Vector<FunctionID> calleesWhoseRecordsWereRead;
        Vector<uint32_t> targetListsRead;
    } body;

    auto referencesIn = [&](ValueID id) -> uint64_t { return id ? mayBe[id.index()] : 0; };
    auto heldReferencesIn = [&](ValueID id) -> uint64_t { return id && !mayHold.isEmpty() ? mayHold[id.index()] : 0; };
    auto isClosureInOwnFrame = [&](ValueID id) { return closuresPlannedInFrame && closuresPlannedInFrame->get(id.index()); };
    auto countedCallsOf = [&](uint64_t references) { return callsOf(references) & ~uncountedCalls; };
    auto mayHoldReference = [&](ValueID id) {
        return mayBeReference(program.typeOf(function, id)) || numberOfResultsOf(program, function, function.at(id)).has_value();
    };

    auto addTo = [&](Vector<uint64_t>& known, ValueID id, uint64_t references) {
        if (!(references & ~known[id.index()]) || !mayHoldReference(id))
            return false;
        known[id.index()] |= references;
        return true;
    };
    auto addHeld = [&](ValueID id, uint64_t references) {
        if (!references)
            return false;
        if (mayHold.isEmpty())
            mayHold.fill(0, function.values.size());
        return addTo(mayHold, id, references);
    };
    auto addAllIn = [&](ValueID source, ValueID id) {
        bool changed = addTo(mayBe, id, referencesIn(source));
        if (!mayHold.isEmpty())
            changed |= addHeld(id, heldReferencesIn(source));
        return changed;
    };

    auto referencesGivenBy = [&](const Value& value) {
        uint64_t references = 0;
        forEachReturnedOperand(function, value, [&](ValueID operand) {
            references |= referencesIn(operand);
        });
        if (std::optional<unsigned> positionOfCell = positionOfCellReadBy(value.opcode))
            references |= contentsOf(referencesIn(value.operand(*positionOfCell))) | heldReferencesIn(value.operand(*positionOfCell));
        return references;
    };
    auto heldReferencesGivenBy = [&](ValueID id) {
        const Value& value = function.at(id);
        uint64_t held = 0;
        forEachReturnedOperand(function, value, [&](ValueID operand) {
            held |= heldReferencesIn(operand);
        });
        if (std::optional<unsigned> positionOfCell = positionOfCellReadBy(value.opcode))
            held |= heldReferencesIn(value.operand(*positionOfCell));
        if (!isClosureInOwnFrame(id))
            return held;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            if (value.variadicOperandIndex(operandIndex))
                held |= referencesIn(operand) | heldReferencesIn(operand);
        });
        return held;
    };

    auto lose = [&](uint64_t bits) {
        bits = withDependentBits(bits);
        if (explanation) [[unlikely]] {
            forEachLostPart(bits & ~lost, [&](uint32_t entryParameter, ASCIILiteral part) {
                explanation->print("not borrowed: f", explainedFunction.index(), " parameter ", entryParameter, ", ", part, ": ");
                if (currentUser)
                    explanation->println(nameOf(function.at(currentUser).opcode), " v", currentUser.index());
                else
                    explanation->println("a terminator");
            });
        }
        lost |= bits;
    };
    auto handOn = [&](FunctionID callee, std::optional<uint32_t> entryParameter, Carried carried) {
        if (!entryParameter || *entryParameter >= maxBorrowedParameters) {
            lose(carried.all());
            return;
        }
        if (uint64_t bits = carried.references | countedCallsOf(carried.all()))
            body.handedOn.append(HandedOnReferences { callee, *entryParameter, bits });
        if (carried.held && heldByWhatIsHandedOn)
            heldByWhatIsHandedOn->append(HandedOnReferences { callee, *entryParameter, carried.held });
    };
    auto readRecordOf = [&](FunctionID callee) -> const Function& {
        if (body.calleesWhoseRecordsWereRead.isEmpty() || body.calleesWhoseRecordsWereRead.last() != callee)
            body.calleesWhoseRecordsWereRead.append(callee);
        return program.at(callee);
    };

    auto isOnlyInspectedByInlineCode = [&](const auto& user, uint32_t operandIndex) {
        if (mayBeCellOtherThanFunction(program.typeOf(function, user.operand(operandIndex))))
            return false;
        return summarizeOperand(effects, function, user, operandIndex).uses.containsOnly(inlineUsesOfFrameCells);
    };
    auto readsProvenSlots = [&](const Value& access, uint32_t operandIndex) {
        if (accessOf(access.opcode) != Access::Read || receiverOf(access.opcode) != operandIndex)
            return false;
        if (!program.typeOf(function, access.operand(operandIndex)).hasOnlyLayouts())
            return false;

        ProvenAccess storage;
        const ProvenAccess& proven = provenAccessOf(program, function, access, storage);
        if (!proven.isComplete || proven.isLookupByKey || !proven.accessors.isEmpty())
            return false;
        return std::ranges::all_of(proven.operations, [](Opcode operation) {
            return readsFollowedByFramePointers.contains(operation);
        });
    };

    struct CalleesOfCurrentCall {
        bool isKnown { false };
        CalleeSetID manyFunctionsAlone;
        std::optional<CalleeList> list;
    };
    auto checkOperandOfCall = [&](const Value& call, uint32_t operandIndex, CalleesOfCurrentCall& callees, Carried carried) {
        ASSERT(!maySuspend(call.opcode));
        bool isCalled = call.operandUses(operandIndex).containsAny(callingUses);
        bool isCalledByNativeCode = isCalled && runtimeOf(call.opcode);
        if (isCalled && !isCalledByNativeCode) {
            lose(countedCallsOf(carried.all()));
            if (call.opcode == Opcode::Call)
                return;
        }
        if (!std::exchange(callees.isKnown, true) && targetLists)
            callees.manyFunctionsAlone = setOfManyFunctionsAlone(program, call);
        if (callees.manyFunctionsAlone && !isCalled) {
            HandedOnTargetLists::ListOfOperand targets = targetLists->listOfOperand(program, callees.manyFunctionsAlone, call, operandIndex);
            if (targets.hasCalleeWithoutParameter)
                lose(carried.all());
            body.targetListsRead.append(targets.list);
            if (uint64_t bits = carried.references | countedCallsOf(carried.all()))
                body.handedOnToManyCallees.append({ targets.list, bits });
            return;
        }
        if (!callees.list)
            callees.list = calleesOfCall(program, call);
        std::optional<ProvenValue<std::span<const FunctionID>>> functions = callees.list->functionsAlone();
        if (isCalledByNativeCode || !functions) {
            lose(carried.all());
            return;
        }
        for (FunctionID callee : functions->value()) {
            if (!isCalled) {
                handOn(callee, entryParameterOf(readRecordOf(callee), call, operandIndex), carried);
                continue;
            }
            for (uint32_t entryParameter : entryParametersOfCalledValue(readRecordOf(callee), call))
                handOn(callee, entryParameter, carried);
        }
    };
    auto checkDeclaredOperand = [&](ValueID id, uint32_t operandIndex, Carried carried) {
        const Value& user = function.at(id);
        if (std::optional<uint32_t> capture = user.variadicOperandIndex(operandIndex); capture && isClosureInOwnFrame(id)) {
            handOn(user.function(), *capture, carried);
            return;
        }

        OperandUses uses = summarizeOperand(effects, function, user, operandIndex).uses;
        bool followsResult = propagatesFramePointers(user.opcode);
        if (!uses.containsOnly(usesOfFollowedOperand) || (uses.contains(OperandUse::Returns) && !followsResult) || treatsOperandAsObject(user.opcode, operandIndex)) {
            lose(carried.all());
            return;
        }
        if (!followsResult && mayHoldReference(id))
            lose(contentsOf(carried.references) | carried.held);
    };

    // The caller's frame is gone when the function goes on.
    auto loseWhatIsLiveAtSuspensions = [&] {
        auto isSuspension = [&](ValueID id) { return suspensionsThatKeepOnlyLiveValues.contains(function.at(id).opcode); };
        Vector<BlockID, 4> suspendingBlocks;
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            if (std::ranges::any_of(function.blocks[blockIndex].values, isSuspension))
                suspendingBlocks.append(BlockID(blockIndex));
        }
        if (suspendingBlocks.isEmpty())
            return;

        BitVector carriers(function.values.size());
        for (uint32_t index = 0; index < function.values.size(); ++index) {
            if (mayBe[index] || (!mayHold.isEmpty() && mayHold[index]))
                carriers.quickSet(index);
        }
        CFG cfg(function);
        UseLists useLists(function, cfg);
        Liveness liveness(function, cfg, useLists, { .trackedValues = &carriers });
        for (BlockID block : suspendingBlocks) {
            if (!cfg.isReachable(block))
                continue;
            Liveness::LocalCalc liveBeforeValue(liveness, block);
            const BlockValues& values = function.at(block).values;
            for (uint32_t index = values.size(); index--;) {
                liveBeforeValue.execute(index);
                if (!isSuspension(values[index]))
                    continue;
                currentUser = values[index];
                liveBeforeValue.forEachLive([&](ValueID live) {
                    lose(referencesIn(live) | heldReferencesIn(live));
                });
            }
        }
        currentUser = { };
    };

    const CompactList<ValueID>& entryParameters = function.at(entryBlock).parameters;
    uint32_t numberOfTrackedParameters = std::min<uint32_t>(entryParameters.size(), maxBorrowedParameters);
    for (uint32_t entryParameter = 0; entryParameter < numberOfTrackedParameters; ++entryParameter) {
        const Type& type = program.typeOf(function, entryParameters[entryParameter]);
        if (!mayBeReference(type))
            continue;
        mayBe[entryParameters[entryParameter].index()] |= bitOfParameter(entryParameter);
        tracked |= allBitsOf(bitOfParameter(entryParameter));
        if (type.environment)
            uncountedCalls |= bitOfCalls(entryParameter);
    }

    static const OpcodeSet givingOpcodes = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (mayReturnOperand(opcode) || positionOfCellReadBy(opcode))
                result.add(opcode);
        }
        return result;
    }();
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : function.at(block).values) {
                const Value& value = function.at(id);
                if (givingOpcodes.contains(value.opcode))
                    changed |= addTo(mayBe, id, referencesGivenBy(value));
                if (closuresPlannedInFrame)
                    changed |= addHeld(id, heldReferencesGivenBy(id));
            }

            forEachOutgoingEdge(function, block, [&](Edge, const Target& target) {
                const CompactList<ValueID>& parameters = function.at(target.block).parameters;
                for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex)
                    changed |= addAllIn(target.arguments[argumentIndex], parameters[argumentIndex]);
            });
            forEachEdgeResult(function, block, [&](const EdgeResult& result, ValueID parameter) {
                if (result.kind == EdgeResultKind::NarrowedOperand)
                    changed |= addAllIn(result.subject, parameter);
            });
        }
    }

    loseWhatIsLiveAtSuspensions();

    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            const Value& user = function.at(id);
            if (user.opcode == Opcode::CallDirect)
                readRecordOf(user.function());
            currentUser = id;
            CalleesOfCurrentCall callees;
            user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                Carried carried { referencesIn(operand) & ~lost, heldReferencesIn(operand) & ~lost };
                if (!carried.all() || user.operandKind(operandIndex) == OperandKind::Guard)
                    return;

                switch (effectRuleOf(user.opcode)) {
                case EffectRule::Call:
                    checkOperandOfCall(user, operandIndex, callees, carried);
                    return;
                case EffectRule::Builtin:
                case EffectRule::Inline:
                    if (!mayLookAtFrameCells(effectRuleOf(user.opcode)) || !isOnlyInspectedByInlineCode(user, operandIndex))
                        lose(carried.all());
                    return;
                case EffectRule::Access:
                    if (!isCellOfFollowedAccess(user.opcode, operandIndex) && !readsProvenSlots(user, operandIndex))
                        lose(carried.all());
                    return;
                case EffectRule::Declared:
                case EffectRule::Operator:
                    checkDeclaredOperand(id, operandIndex, carried);
                    return;
                }
                RELEASE_ASSERT_NOT_REACHED();
            });
        }

        currentUser = { };
        const Terminator& terminator = block.terminator;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            uint64_t references = (referencesIn(operand) | heldReferencesIn(operand)) & ~lost;
            if (!references || terminator.operandKind(operandIndex) == OperandKind::Guard)
                return;

            if (effectRuleOf(terminator.kind) == EffectRule::Inline) {
                if (!isOnlyInspectedByInlineCode(terminator, operandIndex))
                    lose(references);
                return;
            }
            OperandUses uses = summarizeOperand(effects, function, terminator, operandIndex).uses;
            if (!uses.containsOnly(OperandUse::Inspects) || treatsOperandAsObject(terminator.kind, operandIndex))
                lose(references);
        });
    }

    body.word = tracked & ~lost;
    Vector<HandedOnReferences>& entries = body.handedOn;
    AOT::sortInLine(entries, [](const HandedOnReferences& a, const HandedOnReferences& b) {
        return std::tuple(a.callee, a.entryParameter) < std::tuple(b.callee, b.entryParameter);
    });
    size_t numberOfMergedEntries = 0;
    for (HandedOnReferences entry : entries) {
        entry.bits &= body.word;
        HandedOnReferences* previous = numberOfMergedEntries ? &entries[numberOfMergedEntries - 1] : nullptr;
        if (previous && previous->callee == entry.callee && previous->entryParameter == entry.entryParameter)
            previous->bits |= entry.bits;
        else if (entry.bits)
            entries[numberOfMergedEntries++] = entry;
    }
    entries.shrink(numberOfMergedEntries);
    Vector<HandedOnToManyCallees>& entriesOfLists = body.handedOnToManyCallees;
    AOT::sort(entriesOfLists, { }, &HandedOnToManyCallees::targetList);
    numberOfMergedEntries = 0;
    for (HandedOnToManyCallees entry : entriesOfLists) {
        entry.bits &= body.word;
        HandedOnToManyCallees* previous = numberOfMergedEntries ? &entriesOfLists[numberOfMergedEntries - 1] : nullptr;
        if (previous && previous->targetList == entry.targetList)
            previous->bits |= entry.bits;
        else if (entry.bits)
            entriesOfLists[numberOfMergedEntries++] = entry;
    }
    entriesOfLists.shrink(numberOfMergedEntries);
    AOT::sort(body.targetListsRead);
    removeRepeatedElements(body.targetListsRead);
    AOT::sort(body.calleesWhoseRecordsWereRead);
    removeRepeatedElements(body.calleesWhoseRecordsWereRead);
    return { body.word, body.handedOn, body.handedOnToManyCallees, body.calleesWhoseRecordsWereRead, body.targetListsRead };
}

static uint64_t bitsLostThrough(uint64_t wordOfCallee, const HandedOnReferences& entry)
{
    uint64_t references = entry.bits & referenceBits;
    if (!(wordOfCallee & bitOfParameter(entry.entryParameter)))
        return references;

    uint64_t lost = 0;
    if (!(wordOfCallee & bitOfContents(entry.entryParameter)))
        lost |= contentsOf(references);
    if (!(wordOfCallee & bitOfCalls(entry.entryParameter)))
        lost |= entry.bits & ~referenceBits;
    return lost;
}

static uint64_t bitsLostThrough(const Vector<uint64_t>& words, const HandedOnReferences& entry)
{
    return bitsLostThrough(entry.callee.index() < words.size() ? words[entry.callee.index()] : 0, entry);
}

// What some callee of a target list no longer grants. bitsLostThrough() of every target, for any bits, follows from these three.
enum class LossOfTargets : uint8_t {
    Parameter = 1 << 0,
    Contents = 1 << 1,
    Calls = 1 << 2,
};

static OptionSet<LossOfTargets> lossThrough(uint64_t wordOfCallee, uint32_t entryParameter)
{
    if (!(wordOfCallee & bitOfParameter(entryParameter)))
        return LossOfTargets::Parameter;
    OptionSet<LossOfTargets> loss;
    if (!(wordOfCallee & bitOfContents(entryParameter)))
        loss.add(LossOfTargets::Contents);
    if (!(wordOfCallee & bitOfCalls(entryParameter)))
        loss.add(LossOfTargets::Calls);
    return loss;
}

static uint64_t bitsLostThrough(OptionSet<LossOfTargets> loss, uint64_t bits)
{
    uint64_t references = bits & referenceBits;
    uint64_t lost = 0;
    if (loss.contains(LossOfTargets::Parameter))
        lost |= references;
    if (loss.contains(LossOfTargets::Contents))
        lost |= contentsOf(references);
    if (loss.contains(LossOfTargets::Calls))
        lost |= bits & ~referenceBits;
    return lost;
}

static void takeAwayWhatCalleesLose(BuildContext& build, Vector<uint64_t>& words, const Vector<BorrowingBody>& bodies, const HandedOnTargetLists& targetLists)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = words.size();
    Vector<uint32_t> callers = findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return !bodies[index].handedOn.isEmpty() || !bodies[index].handedOnToManyCallees.isEmpty();
    });

    // The number of a list depends on which thread interned it first. The lists in use are visited in the order of their contents.
    struct UserOfList {
        uint32_t caller;
        uint64_t bits;
    };
    struct ListOfCallee {
        uint32_t position;
        uint32_t entryParameter;
    };
    Vector<uint32_t> listsInUse;
    Vector<uint32_t> positionOfList(FillWith { }, targetLists.size(), UINT32_MAX);
    for (uint32_t caller : callers) {
        for (const HandedOnToManyCallees& entry : bodies[caller].handedOnToManyCallees) {
            if (std::exchange(positionOfList[entry.targetList], 0) == UINT32_MAX)
                listsInUse.append(entry.targetList);
        }
    }
    AOT::sort(listsInUse, [&](uint32_t a, uint32_t b) {
        return std::ranges::lexicographical_compare(targetLists.at(a), targetLists.at(b), [](const HandedOnTarget& a, const HandedOnTarget& b) {
            return std::tuple(a.callee, a.entryParameter) < std::tuple(b.callee, b.entryParameter);
        });
    });
    for (uint32_t position = 0; position < listsInUse.size(); ++position)
        positionOfList[listsInUse[position]] = position;

    Vector<uint32_t> firstUserOf(FillWith { }, listsInUse.size() + 1, 0);
    Vector<uint32_t> firstListOf(FillWith { }, numberOfFunctions + 1, 0);
    for (uint32_t caller : callers) {
        for (const HandedOnToManyCallees& entry : bodies[caller].handedOnToManyCallees)
            ++firstUserOf[positionOfList[entry.targetList] + 1];
    }
    auto forEachTarget = [&](uint32_t position, NOESCAPE const Invocable<void(const HandedOnTarget&)> auto& functor) {
        for (const HandedOnTarget& target : targetLists.at(listsInUse[position])) {
            if (target.entryParameter < maxBorrowedParameters && target.callee.index() < numberOfFunctions)
                functor(target);
        }
    };
    for (uint32_t position = 0; position < listsInUse.size(); ++position) {
        firstUserOf[position + 1] += firstUserOf[position];
        forEachTarget(position, [&](const HandedOnTarget& target) {
            ++firstListOf[target.callee.index() + 1];
        });
    }
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstListOf[index + 1] += firstListOf[index];
    Vector<UserOfList> users(firstUserOf.last());
    Vector<ListOfCallee> listsOfCallees(firstListOf.last());
    {
        Vector<uint32_t> nextUserOf(firstUserOf.span().first(listsInUse.size()));
        for (uint32_t caller : callers) {
            for (const HandedOnToManyCallees& entry : bodies[caller].handedOnToManyCallees)
                users[nextUserOf[positionOfList[entry.targetList]]++] = { caller, entry.bits };
        }
        Vector<uint32_t> nextListOf(firstListOf.span().first(numberOfFunctions));
        for (uint32_t position = 0; position < listsInUse.size(); ++position) {
            forEachTarget(position, [&](const HandedOnTarget& target) {
                listsOfCallees[nextListOf[target.callee.index()]++] = { position, target.entryParameter };
            });
        }
    }
    // The users of a list hear of each kind of loss once, whichever of its callees brings it.
    Vector<OptionSet<LossOfTargets>> lossOfList(listsInUse.size());

    Vector<uint64_t> edges = collectInParallel<uint64_t>(loops, callers.size(), [&](unsigned position, Vector<uint64_t>& found) {
        uint32_t caller = callers[position];
        FunctionID previous;
        for (const HandedOnReferences& entry : bodies[caller].handedOn) {
            if (entry.callee == previous || entry.callee.index() >= numberOfFunctions)
                continue;
            previous = entry.callee;
            found.append(static_cast<uint64_t>(entry.callee.index()) << 32 | caller);
        }
    });
    parallelSort(loops, edges.mutableSpan(), std::less<uint64_t> { });
    Vector<uint32_t> firstCallerOf(FillWith { }, numberOfFunctions + 1, 0);
    for (uint64_t edge : edges)
        ++firstCallerOf[(edge >> 32) + 1];
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstCallerOf[index + 1] += firstCallerOf[index];

    // A function is queued while its callers have not heard of what it lost.
    Vector<uint32_t> worklist;
    Vector<uint8_t> isQueued(FillWith { }, numberOfFunctions, 0);
    auto takeAway = [&](uint32_t index, uint64_t lost) {
        lost = withDependentBits(lost) & words[index];
        if (!lost)
            return;
        words[index] &= ~lost;
        if (isQueued[index] || (firstCallerOf[index] == firstCallerOf[index + 1] && firstListOf[index] == firstListOf[index + 1]))
            return;
        worklist.append(index);
        isQueued[index] = 1;
    };

    uint64_t numberOfEntriesRead = 0;
    auto tellUsersOf = [&](uint32_t position, OptionSet<LossOfTargets> loss) {
        loss.remove(lossOfList[position]);
        if (loss.isEmpty())
            return;
        lossOfList[position].add(loss);
        for (const UserOfList& user : users.span().subspan(firstUserOf[position], firstUserOf[position + 1] - firstUserOf[position]))
            takeAway(user.caller, bitsLostThrough(loss, user.bits));
        numberOfEntriesRead += firstUserOf[position + 1] - firstUserOf[position];
    };
    for (uint32_t caller : callers) {
        uint64_t lost = 0;
        for (const HandedOnReferences& entry : bodies[caller].handedOn)
            lost |= bitsLostThrough(words, entry);
        numberOfEntriesRead += bodies[caller].handedOn.size();
        takeAway(caller, lost);
    }
    for (uint32_t position = 0; position < listsInUse.size(); ++position) {
        OptionSet<LossOfTargets> loss;
        // A callee past the table is one of which nothing is known.
        for (const HandedOnTarget& target : targetLists.at(listsInUse[position])) {
            if (target.entryParameter < maxBorrowedParameters)
                loss.add(lossThrough(target.callee.index() < numberOfFunctions ? words[target.callee.index()] : 0, target.entryParameter));
        }
        numberOfEntriesRead += targetLists.at(listsInUse[position]).size();
        tellUsersOf(position, loss);
    }
    while (!worklist.isEmpty()) {
        FunctionID callee(worklist.takeLast());
        isQueued[callee.index()] = 0;
        for (uint32_t position = firstCallerOf[callee.index()]; position < firstCallerOf[callee.index() + 1]; ++position) {
            uint32_t caller = static_cast<uint32_t>(edges[position]);
            if (!words[caller])
                continue;
            uint64_t lost = 0;
            for (const HandedOnReferences& entry : std::ranges::equal_range(bodies[caller].handedOn, callee, { }, &HandedOnReferences::callee)) {
                lost |= bitsLostThrough(words[callee.index()], entry);
                ++numberOfEntriesRead;
            }
            takeAway(caller, lost);
        }
        for (uint32_t index = firstListOf[callee.index()]; index < firstListOf[callee.index() + 1]; ++index) {
            ++numberOfEntriesRead;
            tellUsersOf(listsOfCallees[index].position, lossThrough(words[callee.index()], listsOfCallees[index].entryParameter));
        }
    }
    build.statistics().count(StatisticID::HandedOnReferencesRead, numberOfEntriesRead);
}

static void explainLosses(const Program& program, BuildContext& build, const Vector<uint64_t>& words, const Vector<BorrowingBody>& bodies, const HandedOnTargetLists& targetLists, const OpcodeSet& opcodesThatBorrowNothing)
{
    PrintStream& out = WTF::dataFile();
    for (unsigned index = 0; index < words.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !hasReferenceAmongTrackedParameters(program, function))
            continue;
        if (!build.isEnabled(FlowResultKind::OperandSummaries, index) || !mayBorrow(program, function, opcodesThatBorrowNothing)) {
            out.println("not borrowed: f", index, ": the function borrows nothing");
            continue;
        }
        scanBorrowingBody(program, function, nullptr, nullptr, nullptr, &out, FunctionID(index));
        auto explain = [&](const HandedOnReferences& entry) {
            forEachLostPart(withDependentBits(bitsLostThrough(words, entry)) & bodies[index].word, [&](uint32_t entryParameter, ASCIILiteral part) {
                out.println("not borrowed: f", index, " parameter ", entryParameter, ", ", part, ": handed on to f", entry.callee.index(), " parameter ", entry.entryParameter);
            });
        };
        for (const HandedOnReferences& entry : bodies[index].handedOn)
            explain(entry);
        for (const HandedOnToManyCallees& entry : bodies[index].handedOnToManyCallees) {
            for (const HandedOnTarget& target : targetLists.at(entry.targetList)) {
                if (target.entryParameter < maxBorrowedParameters)
                    explain({ target.callee, target.entryParameter, entry.bits });
            }
        }
    }
}

uint64_t BorrowedParameters::wordOfBody(const Program& program, BuildContext& build, FunctionID id, const BitVector& closuresPlannedInFrame) const
{
    const Function& function = program.at(id);
    if (!build.isEnabled(FlowResultKind::OperandSummaries, id.index()) || !mayBorrow(program, function, opcodesOfFunctionsThatBorrowNothing()))
        return 0;
    Vector<HandedOnReferences> heldByWhatIsHandedOn;
    BorrowingBody body = scanBorrowingBody(program, function, nullptr, &closuresPlannedInFrame, &heldByWhatIsHandedOn);
    uint64_t word = body.word;
    auto wordOfCallee = [&](const HandedOnReferences& entry) {
        return entry.callee == id ? word : wordOf(entry.callee);
    };
    for (bool changed = true; changed;) {
        uint64_t lost = 0;
        for (const HandedOnReferences& entry : body.handedOn)
            lost |= bitsLostThrough(wordOfCallee(entry), entry);
        for (const HandedOnReferences& entry : heldByWhatIsHandedOn) {
            uint64_t needed = bitOfParameter(entry.entryParameter) | bitOfContents(entry.entryParameter);
            if ((wordOfCallee(entry) & needed) != needed)
                lost |= entry.bits;
        }
        lost = withDependentBits(lost) & word;
        word &= ~lost;
        changed = !!lost;
    }
    return word;
}

BorrowedParameters computeBorrowedParameters(const Program& program, BuildContext& build)
{
    BorrowingBodies bodies;
    return computeBorrowedParameters(program, build, bodies, { }, { });
}

void BorrowingBodies::didLower(std::span<const FunctionID> functions)
{
    for (FunctionID id : functions) {
        if (id.index() < m_wasLoweredSinceScanned.size())
            m_wasLoweredSinceScanned[id.index()] = 1;
    }
}

BorrowedParameters computeBorrowedParameters(const Program& program, BuildContext& build, BorrowingBodies& kept, std::span<const uint8_t> hasChanged, std::span<const uint8_t> recordLooksDifferent)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.functions.size();
    unsigned numberOfKeptBodies = kept.m_bodies.size();
    RELEASE_ASSERT(numberOfKeptBodies <= numberOfFunctions && hasChanged.size() == numberOfKeptBodies && recordLooksDifferent.size() == numberOfKeptBodies);
    OpcodeSet opcodesThatBorrowNothing = opcodesOfFunctionsThatBorrowNothing();
    const bool checksKeptBodies = build.options().verifiesAfterEveryPass;
    kept.m_targetLists.forgetOperands();

    auto scan = [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!build.isEnabled(FlowResultKind::OperandSummaries, index) || !mayBorrow(program, function, opcodesThatBorrowNothing))
            return BorrowingBody { };
        return scanBorrowingBody(program, function, &kept.m_targetLists);
    };
    Vector<uint8_t> listLooksDifferent(kept.m_targetLists.size(), [&](size_t list) -> uint8_t {
        return std::ranges::any_of(kept.m_targetLists.at(list), [&](const HandedOnTarget& target) {
            return recordLooksDifferent[target.callee.index()];
        });
    });
    auto stillHolds = [&](unsigned index) {
        if (index >= numberOfKeptBodies || hasChanged[index] || recordLooksDifferent[index])
            return false;
        const BorrowingBody& body = kept.m_bodies[index];
        return std::ranges::none_of(body.calleesWhoseRecordsWereRead, [&](FunctionID callee) { return recordLooksDifferent[callee.index()]; })
            && std::ranges::none_of(body.targetListsRead, [&](uint32_t list) { return listLooksDifferent[list]; });
    };

    BorrowedParameters table;
    Vector<uint64_t>& words = table.m_words;
    reserveCapacityInParallel(loops, words, numberOfFunctions);
    words.fill(0, numberOfFunctions);
    growWithRoomToSpareInParallel(loops, kept.m_bodies, numberOfFunctions);
    std::atomic<bool> someBodySaysSomethingElse { false };
    kept.m_wasLoweredSinceScanned.insertFill(numberOfKeptBodies, 0, numberOfFunctions - numberOfKeptBodies);
    forEachIndexInParallel(loops, numberOfFunctions, largeFunctions(program, loops).span(), [&](unsigned index) {
        BorrowingBody& body = kept.m_bodies[index];
        if (stillHolds(index))
            RELEASE_ASSERT(!checksKeptBodies || kept.m_wasLoweredSinceScanned[index] || body == scan(index));
        else {
            BorrowingBody scanned = scan(index);
            kept.m_wasLoweredSinceScanned[index] = 0;
            if (scanned != body) {
                body = WTF::move(scanned);
                someBodySaysSomethingElse.store(true, std::memory_order_relaxed);
            }
        }
        words[index] = body.word;
    });

    if (someBodySaysSomethingElse.load(std::memory_order_relaxed)) {
        takeAwayWhatCalleesLose(build, words, kept.m_bodies, kept.m_targetLists);
        kept.m_wordsOfLatestTable = words;
        if (build.options().logging.dumpedPass == PassID::AllocateInFrames) [[unlikely]]
            explainLosses(program, build, words, kept.m_bodies, kept.m_targetLists, opcodesThatBorrowNothing);
        return table;
    }
    kept.m_wordsOfLatestTable.insertFill(numberOfKeptBodies, 0, numberOfFunctions - numberOfKeptBodies);
    if (checksKeptBodies) {
        takeAwayWhatCalleesLose(build, words, kept.m_bodies, kept.m_targetLists);
        RELEASE_ASSERT(words == kept.m_wordsOfLatestTable);
    }
    words = kept.m_wordsOfLatestTable;
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
