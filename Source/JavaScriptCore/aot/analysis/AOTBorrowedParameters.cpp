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
#include "AOTIR.h"
#include "AOTLargeVectors.h"
#include "AOTResultRules.h"
#include "AOTSchemaRuntimeColumns.h"
#include "AOTThreads.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <tuple>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(3 * maxBorrowedParameters <= 64);

static constexpr unsigned contentsShift = maxBorrowedParameters;
static constexpr unsigned callsShift = 2 * maxBorrowedParameters;
static constexpr uint64_t parameterBits = (1ull << maxBorrowedParameters) - 1;
// What a value may be: bit i, entry parameter i; bit contentsShift + i, a reference that was read through it.
static constexpr uint64_t referenceBits = parameterBits | parameterBits << contentsShift;

static constexpr OperandUses usesOfFollowedOperand { OperandUse::Inspects, OperandUse::Returns };

static constexpr uint64_t bitOfParameter(uint32_t entryParameter) { return 1ull << entryParameter; }
static constexpr uint64_t bitOfContents(uint32_t entryParameter) { return 1ull << (contentsShift + entryParameter); }
static constexpr uint64_t bitOfCalls(uint32_t entryParameter) { return 1ull << (callsShift + entryParameter); }

static constexpr uint64_t parametersIn(uint64_t references)
{
    return (references | references >> contentsShift) & parameterBits;
}

static constexpr uint64_t contentsOf(uint64_t references) { return parametersIn(references) << contentsShift; }
static constexpr uint64_t callsOf(uint64_t references) { return parametersIn(references) << callsShift; }

static constexpr uint64_t allBitsOf(uint64_t parameters)
{
    return parameters | parameters << contentsShift | parameters << callsShift;
}

// Of a parameter that is not borrowed nothing else is known either.
static constexpr uint64_t withDependentBits(uint64_t lost) { return lost | allBitsOf(lost & parameterBits); }

uint64_t BorrowedParameters::wordOf(FunctionID function) const
{
    return function.index() < m_words.size() ? m_words[function.index()] : 0;
}

bool BorrowedParameters::isBorrowed(FunctionID function, uint32_t entryParameter) const
{
    return entryParameter < maxBorrowedParameters && wordOf(function) & bitOfParameter(entryParameter);
}

bool BorrowedParameters::borrowsContents(FunctionID function, uint32_t entryParameter) const
{
    return entryParameter < maxBorrowedParameters && wordOf(function) & bitOfContents(entryParameter);
}

bool BorrowedParameters::mayBeCalled(FunctionID function, uint32_t entryParameter) const
{
    return entryParameter >= maxBorrowedParameters || !(wordOf(function) & bitOfCalls(entryParameter));
}

// The escape analysis of a caller takes an object that is called and is no closure for one that unknown code got, so a call is named only where there may be one.
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

namespace {

class BodyScanner {
public:
    BodyScanner(const Program&, const Function&, const BitVector* closuresPlannedInFrame = nullptr);

    BorrowingBody run();
    std::span<const HandedOnReferences> heldByWhatIsHandedOn() const LIFETIME_BOUND { return m_heldByWhatIsHandedOn.span(); }
    void explainLossesTo(PrintStream& out, FunctionID id)
    {
        m_explanation = &out;
        m_id = id;
    }

private:
    uint64_t trackEntryParameters();

    // What a value may be, and what a cell of this frame holds that the value may be.
    struct Carried {
        uint64_t references { 0 };
        uint64_t held { 0 };

        uint64_t all() const { return references | held; }
    };

    void propagate();
    uint64_t referencesGivenBy(const Value&) const;
    uint64_t heldReferencesGivenBy(ValueID) const;
    bool add(ValueID, uint64_t references);
    bool addHeld(ValueID, uint64_t references);
    bool mayHoldReference(ValueID) const;

    void checkOperandsOf(ValueID);
    void checkOperandsOf(const Terminator&);
    void checkOperandOfCall(const Value& call, uint32_t operandIndex, const CalleeList&, Carried);
    void checkDeclaredOperand(ValueID user, uint32_t operandIndex, Carried);
    bool readsProvenSlots(const Value& access, uint32_t operandIndex) const;
    template<typename User> bool isOnlyInspectedByInlineCode(const User&, uint32_t operandIndex) const;

    uint64_t referencesIn(ValueID id) const { return id ? m_mayBe[id.index()] : 0; }
    uint64_t heldReferencesIn(ValueID id) const { return id && !m_mayHold.isEmpty() ? m_mayHold[id.index()] : 0; }
    bool isClosureInOwnFrame(ValueID) const;
    Carried borrowedReferencesIn(ValueID id) const { return { referencesIn(id) & ~m_lost, heldReferencesIn(id) & ~m_lost }; }
    uint64_t countedCallsOf(uint64_t references) const { return callsOf(references) & ~m_uncountedCalls; }

    void lose(uint64_t bits)
    {
        uint64_t lost = withDependentBits(bits);
        if (m_explanation) [[unlikely]]
            explainLoss(lost & ~m_lost);
        m_lost |= lost;
    }
    void explainLoss(uint64_t bits) const;
    void handOn(FunctionID callee, std::optional<uint32_t> entryParameter, Carried);
    const Function& readRecordOf(FunctionID callee);

    const Program& m_program;
    const Function& m_function;
    const BitVector* m_closuresPlannedInFrame;
    EffectSummaries m_unknownSummaries;
    EffectContext m_effects;
    Vector<uint64_t> m_mayBe;
    // Empty as long as no closure of the frame holds a reference.
    Vector<uint64_t> m_mayHold;
    uint64_t m_uncountedCalls { 0 };
    uint64_t m_lost { 0 };
    Vector<HandedOnReferences> m_handedOn;
    // The references that a closure of this frame holds which is handed on. They go with what the callee reads through the parameter.
    Vector<HandedOnReferences> m_heldByWhatIsHandedOn;
    Vector<FunctionID> m_calleesWhoseRecordsWereRead;
    PrintStream* m_explanation { nullptr };
    FunctionID m_id;
    ValueID m_currentUser;
};

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

} // anonymous namespace

void BodyScanner::explainLoss(uint64_t bits) const
{
    forEachLostPart(bits, [&](uint32_t entryParameter, ASCIILiteral part) {
        m_explanation->print("not borrowed: f", m_id.index(), " parameter ", entryParameter, ", ", part, ": ");
        if (m_currentUser)
            m_explanation->println(nameOf(m_function.at(m_currentUser).opcode), " v", m_currentUser.index());
        else
            m_explanation->println("a terminator");
    });
}

// A saved frame holds what is live in it, and the function goes on when its caller has long gone on.
static OpcodeSet opcodesOfFunctionsThatBorrowNothing()
{
    OpcodeSet opcodes { Opcode::GetArgument, Opcode::GetMyArgumentByVal, Opcode::CreateRest, Opcode::CallForwardVarargs, Opcode::ConstructSuperForwardVarargs };
    for (Opcode opcode : allMembersOf<Opcode>) {
        if (maySuspend(opcode))
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
    if (!function.hasBody() || !function.hasOrdinaryFrame() || function.isUnsplitCoroutine())
        return false;
    if (!hasReferenceAmongTrackedParameters(program, function))
        return false;
    return std::ranges::none_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return opcodesThatBorrowNothing.contains(function.at(id).opcode);
        });
    });
}

// A conversion, a test against `prototype` and the like read members of a function, which makes it an object with properties.
template<typename Entry>
static bool treatsOperandAsObject(Entry entry, uint32_t operandIndex)
{
    OperandBehavior behavior = declaredBehaviorOf(entry, operandIndex);
    return !behavior.protocols.isEmpty() || !behavior.namedMembers.empty() || !behavior.calledMembers.empty();
}

// Native code looks at a closure that lies in a frame, and at no other cell of a frame.
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

// A read by name counts too. Where it is none of proven slots, its receiver is not borrowed, and nobody asks what it gave.
static std::optional<unsigned> positionOfCellReadBy(Opcode opcode)
{
    if (readsFollowedByFramePointers.contains(opcode))
        return positionOfAccessedCell(opcode);
    if (effectRuleOf(opcode) == EffectRule::Access && accessOf(opcode) == Access::Read)
        return receiverOf(opcode);
    return std::nullopt;
}

// A value with any other opcode gives no reference: it returns no operand and reads no cell.
static const OpcodeSet& opcodesThatGiveReferences()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        for (Opcode opcode : allMembersOf<Opcode>) {
            if (mayReturnOperand(opcode) || positionOfCellReadBy(opcode))
                result.add(opcode);
        }
        return result;
    }();
    return opcodes;
}

static void mergeEntriesOfOneParameter(Vector<HandedOnReferences>& entries)
{
    std::ranges::sort(entries, [](const HandedOnReferences& a, const HandedOnReferences& b) {
        return std::tuple(a.callee, a.entryParameter) < std::tuple(b.callee, b.entryParameter);
    });
    size_t numberOfMergedEntries = 0;
    for (size_t index = 0; index < entries.size(); ++index) {
        HandedOnReferences entry = entries[index];
        if (numberOfMergedEntries) {
            HandedOnReferences& previous = entries[numberOfMergedEntries - 1];
            if (previous.callee == entry.callee && previous.entryParameter == entry.entryParameter) {
                previous.bits |= entry.bits;
                continue;
            }
        }
        entries[numberOfMergedEntries++] = entry;
    }
    entries.shrink(numberOfMergedEntries);
}

BodyScanner::BodyScanner(const Program& program, const Function& function, const BitVector* closuresPlannedInFrame)
    : m_program(program)
    , m_function(function)
    , m_closuresPlannedInFrame(closuresPlannedInFrame)
    , m_effects { program, m_unknownSummaries }
    , m_mayBe(FillWith { }, function.values.size(), 0)
{
}

const Function& BodyScanner::readRecordOf(FunctionID callee)
{
    if (m_calleesWhoseRecordsWereRead.isEmpty() || m_calleesWhoseRecordsWereRead.last() != callee)
        m_calleesWhoseRecordsWereRead.append(callee);
    return m_program.at(callee);
}

BorrowingBody BodyScanner::run()
{
    uint64_t tracked = trackEntryParameters();
    propagate();
    for (const Block& block : m_function.blocks) {
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::CallDirect)
                readRecordOf(value.function());
            checkOperandsOf(id);
        }
        checkOperandsOf(block.terminator);
    }

    BorrowingBody body;
    body.word = tracked & ~m_lost;
    for (HandedOnReferences entry : m_handedOn) {
        entry.bits &= body.word;
        if (entry.bits)
            body.handedOn.append(entry);
    }
    mergeEntriesOfOneParameter(body.handedOn);
    moveToExactBuffer(body.handedOn);
    std::ranges::sort(m_calleesWhoseRecordsWereRead);
    removeRepeatedElements(m_calleesWhoseRecordsWereRead);
    moveToExactBuffer(m_calleesWhoseRecordsWereRead);
    body.calleesWhoseRecordsWereRead = WTF::move(m_calleesWhoseRecordsWereRead);
    return body;
}

uint64_t BodyScanner::trackEntryParameters()
{
    const CompactList<ValueID>& parameters = m_function.at(entryBlock).parameters;
    uint32_t numberOfTrackedParameters = std::min<uint32_t>(parameters.size(), maxBorrowedParameters);
    uint64_t tracked = 0;
    for (uint32_t entryParameter = 0; entryParameter < numberOfTrackedParameters; ++entryParameter) {
        const Type& type = m_program.typeOf(m_function, parameters[entryParameter]);
        if (!mayBeReference(type))
            continue;
        m_mayBe[parameters[entryParameter].index()] |= bitOfParameter(entryParameter);
        tracked |= allBitsOf(bitOfParameter(entryParameter));
        if (type.environment)
            m_uncountedCalls |= bitOfCalls(entryParameter);
    }
    return tracked;
}

void BodyScanner::propagate()
{
    const OpcodeSet& givingOpcodes = opcodesThatGiveReferences();
    for (bool changed = true; changed;) {
        changed = false;
        for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : m_function.at(block).values) {
                const Value& value = m_function.at(id);
                if (givingOpcodes.contains(value.opcode))
                    changed |= add(id, referencesGivenBy(value));
                if (m_closuresPlannedInFrame)
                    changed |= addHeld(id, heldReferencesGivenBy(id));
            }

            forEachOutgoingEdge(m_function, block, [&](Edge, const Target& target) {
                const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
                for (uint32_t argumentIndex = 0; argumentIndex < target.arguments.size(); ++argumentIndex) {
                    changed |= add(parameters[argumentIndex], referencesIn(target.arguments[argumentIndex]));
                    if (!m_mayHold.isEmpty())
                        changed |= addHeld(parameters[argumentIndex], heldReferencesIn(target.arguments[argumentIndex]));
                }
            });
            forEachEdgeResult(m_function, block, [&](const EdgeResult& result, ValueID parameter) {
                if (result.kind == EdgeResultKind::NarrowedOperand) {
                    changed |= add(parameter, referencesIn(result.subject));
                    if (!m_mayHold.isEmpty())
                        changed |= addHeld(parameter, heldReferencesIn(result.subject));
                }
            });
        }
    }
}

uint64_t BodyScanner::referencesGivenBy(const Value& value) const
{
    uint64_t references = 0;
    forEachReturnedOperand(m_function, value, [&](ValueID operand) {
        references |= referencesIn(operand);
    });
    std::optional<unsigned> positionOfCell = positionOfCellReadBy(value.opcode);
    if (!positionOfCell)
        return references;

    uint64_t cell = referencesIn(value.operand(*positionOfCell));
    references |= contentsOf(cell);
    references |= heldReferencesIn(value.operand(*positionOfCell));
    return references;
}

// A capture is a whole word, so it takes a pointer into a frame.
bool BodyScanner::isClosureInOwnFrame(ValueID id) const
{
    return m_closuresPlannedInFrame && m_closuresPlannedInFrame->get(id.index());
}

uint64_t BodyScanner::heldReferencesGivenBy(ValueID id) const
{
    const Value& value = m_function.at(id);
    uint64_t held = 0;
    forEachReturnedOperand(m_function, value, [&](ValueID operand) {
        held |= heldReferencesIn(operand);
    });
    // What is read out of a cell may be a cell of this frame again.
    if (std::optional<unsigned> positionOfCell = positionOfCellReadBy(value.opcode))
        held |= heldReferencesIn(value.operand(*positionOfCell));
    if (!isClosureInOwnFrame(id))
        return held;
    value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        if (value.variadicOperandIndex(operandIndex))
            held |= referencesIn(operand) | heldReferencesIn(operand);
    });
    return held;
}


bool BodyScanner::add(ValueID id, uint64_t references)
{
    uint64_t& known = m_mayBe[id.index()];
    if (!(references & ~known) || !mayHoldReference(id))
        return false;
    known |= references;
    return true;
}

bool BodyScanner::addHeld(ValueID id, uint64_t references)
{
    if (!references)
        return false;
    if (m_mayHold.isEmpty())
        m_mayHold.fill(0, m_function.values.size());
    uint64_t& known = m_mayHold[id.index()];
    if (!(references & ~known) || !mayHoldReference(id))
        return false;
    known |= references;
    return true;
}

bool BodyScanner::mayHoldReference(ValueID id) const
{
    return mayBeReference(m_program.typeOf(m_function, id)) || numberOfResultsOf(m_program, m_function, m_function.at(id)).has_value();
}

void BodyScanner::handOn(FunctionID callee, std::optional<uint32_t> entryParameter, Carried carried)
{
    if (!entryParameter || *entryParameter >= maxBorrowedParameters) {
        lose(carried.all());
        return;
    }
    if (uint64_t bits = carried.references | countedCallsOf(carried.all()))
        m_handedOn.append(HandedOnReferences { callee, *entryParameter, bits });
    if (carried.held)
        m_heldByWhatIsHandedOn.append(HandedOnReferences { callee, *entryParameter, carried.held });
}

void BodyScanner::checkOperandsOf(ValueID id)
{
    m_currentUser = id;
    const Value& user = m_function.at(id);
    std::optional<CalleeList> callees;
    user.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        Carried carried = borrowedReferencesIn(operand);
        if (!carried.all() || user.operandKind(operandIndex) == OperandKind::Guard)
            return;

        switch (effectRuleOf(user.opcode)) {
        case EffectRule::Call:
            if (!callees)
                callees = calleesOfCall(m_program, user);
            checkOperandOfCall(user, operandIndex, *callees, carried);
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

void BodyScanner::checkOperandsOf(const Terminator& terminator)
{
    m_currentUser = { };
    terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
        uint64_t references = borrowedReferencesIn(operand).all();
        if (!references || terminator.operandKind(operandIndex) == OperandKind::Guard)
            return;

        if (effectRuleOf(terminator.kind) == EffectRule::Inline) {
            if (!isOnlyInspectedByInlineCode(terminator, operandIndex))
                lose(references);
            return;
        }
        // The call of a ReturnCall is judged as any call: the cell lies in an older frame than the one that goes.
        OperandUses uses = summarizeOperand(m_effects, m_function, terminator, operandIndex).uses;
        if (!uses.containsOnly(OperandUse::Inspects) || treatsOperandAsObject(terminator.kind, operandIndex))
            lose(references);
    });
}

void BodyScanner::checkOperandOfCall(const Value& call, uint32_t operandIndex, const CalleeList& callees, Carried carried)
{
    ASSERT(!maySuspend(call.opcode));
    bool namesEveryCallee = callees.isComplete && callees.builtins.isEmpty();
    if (!call.operandUses(operandIndex).containsAny(callingUses)) {
        if (!namesEveryCallee) {
            lose(carried.all());
            return;
        }
        for (FunctionID callee : callees.functions)
            handOn(callee, entryParameterOf(readRecordOf(callee), call, operandIndex), carried);
        return;
    }

    // Where the stub of a call with a list cannot copy the list to the stack, the run-time function of the operation makes the call, and native code enters no closure of a frame.
    if (runtimeOf(call.opcode)) {
        lose(carried.all());
        return;
    }
    lose(countedCallsOf(carried.all()));
    // Only a function that takes its callee gets hold of the closure it is entered through, and a closure of such a function lies in no frame.
    if (call.opcode == Opcode::Call)
        return;
    // A construction hands the called value on as new.target.
    if (!namesEveryCallee) {
        lose(carried.all());
        return;
    }
    for (FunctionID callee : callees.functions) {
        for (uint32_t entryParameter : entryParametersOfCalledValue(readRecordOf(callee), call))
            handOn(callee, entryParameter, carried);
    }
}

void BodyScanner::checkDeclaredOperand(ValueID id, uint32_t operandIndex, Carried carried)
{
    const Value& user = m_function.at(id);
    // The cell goes with this frame. Whoever calls the closure runs its function, which does with a capture what it does with the entry parameter.
    if (std::optional<uint32_t> capture = user.variadicOperandIndex(operandIndex); capture && isClosureInOwnFrame(id)) {
        handOn(user.function(), *capture, carried);
        return;
    }

    OperandUses uses = summarizeOperand(m_effects, m_function, user, operandIndex).uses;
    bool followsResult = propagatesFramePointers(user.opcode);
    if (!uses.containsOnly(usesOfFollowedOperand) || (uses.contains(OperandUse::Returns) && !followsResult) || treatsOperandAsObject(user.opcode, operandIndex)) {
        lose(carried.all());
        return;
    }
    // What such an operation reads out of the cell is followed no further.
    if (!followsResult && mayHoldReference(id))
        lose(contentsOf(carried.references) | carried.held);
}

// What ResolveAccesses makes a read of a slot.
bool BodyScanner::readsProvenSlots(const Value& access, uint32_t operandIndex) const
{
    if (accessOf(access.opcode) != Access::Read || receiverOf(access.opcode) != operandIndex)
        return false;
    if (!m_program.typeOf(m_function, access.operand(operandIndex)).hasOnlyLayouts(m_program))
        return false;

    ProvenAccess storage;
    const ProvenAccess& proven = provenAccessOf(m_program, m_function, access, storage);
    if (!proven.isComplete || proven.isLookupByKey || !proven.accessors.isEmpty())
        return false;
    return std::ranges::all_of(proven.operations, [](Opcode operation) {
        return readsFollowedByFramePointers.contains(operation);
    });
}

template<typename User>
bool BodyScanner::isOnlyInspectedByInlineCode(const User& user, uint32_t operandIndex) const
{
    if (mayBeCellOtherThanFunction(m_program.typeOf(m_function, user.operand(operandIndex))))
        return false;
    return summarizeOperand(m_effects, m_function, user, operandIndex).uses.containsOnly(inlineUsesOfFrameCells);
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

// Functions that hand a parameter to each other keep it.
static void takeAwayWhatCalleesLose(const ParallelLoops& loops, Vector<uint64_t>& words, const Vector<BorrowingBody>& bodies)
{
    unsigned numberOfFunctions = words.size();
    auto forEachCalleeOf = [&](unsigned caller, const auto& functor) {
        FunctionID previous;
        for (const HandedOnReferences& entry : bodies[caller].handedOn) {
            if (entry.callee == previous || entry.callee.index() >= numberOfFunctions)
                continue;
            previous = entry.callee;
            functor(entry.callee.index());
        }
    };

    Vector<uint32_t> forwardingFunctions = findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return !bodies[index].handedOn.isEmpty();
    });

    // The callee in the upper half, the caller in the lower.
    Vector<uint64_t> edges = collectInParallel<uint64_t>(loops, forwardingFunctions.size(), [&](unsigned position, Vector<uint64_t>& found) {
        uint32_t caller = forwardingFunctions[position];
        forEachCalleeOf(caller, [&](unsigned callee) {
            found.append(static_cast<uint64_t>(callee) << 32 | caller);
        });
    });
    parallelSort(loops, edges.mutableSpan(), std::less<uint64_t> { });
    Vector<uint32_t> firstCallerOf(FillWith { }, numberOfFunctions + 1, 0);
    for (uint64_t edge : edges)
        ++firstCallerOf[(edge >> 32) + 1];
    for (unsigned index = 0; index < numberOfFunctions; ++index)
        firstCallerOf[index + 1] += firstCallerOf[index];

    Vector<uint32_t> worklist = WTF::move(forwardingFunctions);
    Vector<uint8_t> isQueued(FillWith { }, numberOfFunctions, 0);
    for (uint32_t index : worklist)
        isQueued[index] = 1;
    while (!worklist.isEmpty()) {
        unsigned index = worklist.takeLast();
        isQueued[index] = 0;

        uint64_t lost = 0;
        for (const HandedOnReferences& entry : bodies[index].handedOn)
            lost |= bitsLostThrough(words, entry);
        lost = withDependentBits(lost) & words[index];
        if (!lost)
            continue;

        words[index] &= ~lost;
        for (uint32_t position = firstCallerOf[index]; position < firstCallerOf[index + 1]; ++position) {
            uint32_t caller = static_cast<uint32_t>(edges[position]);
            if (!words[caller] || isQueued[caller])
                continue;
            worklist.append(caller);
            isQueued[caller] = 1;
        }
    }
}

// One line for each part of a parameter that is not borrowed: the operation of the body that loses it, or the callee that it is handed on to and that loses it.
static void explainLosses(const Program& program, BuildContext& build, const Vector<uint64_t>& words, const Vector<BorrowingBody>& bodies, const OpcodeSet& opcodesThatBorrowNothing)
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
        BodyScanner scanner(program, function);
        scanner.explainLossesTo(out, FunctionID(index));
        scanner.run();
        for (const HandedOnReferences& entry : bodies[index].handedOn) {
            forEachLostPart(withDependentBits(bitsLostThrough(words, entry)) & bodies[index].word, [&](uint32_t entryParameter, ASCIILiteral part) {
                out.println("not borrowed: f", index, " parameter ", entryParameter, ", ", part, ": handed on to f", entry.callee.index(), " parameter ", entry.entryParameter);
            });
        }
    }
}

uint64_t BorrowedParameters::wordOfBody(const Program& program, BuildContext& build, FunctionID id, const BitVector& closuresPlannedInFrame) const
{
    const Function& function = program.at(id);
    if (!build.isEnabled(FlowResultKind::OperandSummaries, id.index()) || !mayBorrow(program, function, opcodesOfFunctionsThatBorrowNothing()))
        return 0;
    BodyScanner scanner(program, function, &closuresPlannedInFrame);
    BorrowingBody body = scanner.run();
    // A function that hands a parameter to itself keeps it, as in the table.
    uint64_t word = body.word;
    auto wordOfCallee = [&](const HandedOnReferences& entry) {
        return entry.callee == id ? word : wordOf(entry.callee);
    };
    for (bool changed = true; changed;) {
        uint64_t lost = 0;
        for (const HandedOnReferences& entry : body.handedOn)
            lost |= bitsLostThrough(wordOfCallee(entry), entry);
        for (const HandedOnReferences& entry : scanner.heldByWhatIsHandedOn()) {
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

void BorrowedParameters::add(FunctionID function, uint64_t word)
{
    m_words[function.index()] |= word;
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

    auto scan = [&](unsigned index) {
        const Function& function = program.functions[index];
        if (!build.isEnabled(FlowResultKind::OperandSummaries, index) || !mayBorrow(program, function, opcodesThatBorrowNothing))
            return BorrowingBody { };
        return BodyScanner(program, function).run();
    };
    auto stillHolds = [&](unsigned index) {
        if (index >= numberOfKeptBodies || hasChanged[index] || recordLooksDifferent[index])
            return false;
        return std::ranges::none_of(kept.m_bodies[index].calleesWhoseRecordsWereRead, [&](FunctionID callee) {
            return recordLooksDifferent[callee.index()];
        });
    };

    BorrowedParameters table;
    Vector<uint64_t>& words = table.m_words;
    reserveCapacityInParallel(loops, words, numberOfFunctions);
    words.fill(0, numberOfFunctions);
    growInParallel(loops, kept.m_bodies, numberOfFunctions);
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

    const bool explains = build.options().logging.dumpedPass == PassID::AllocateInFrames;
    if (someBodySaysSomethingElse.load(std::memory_order_relaxed)) {
        takeAwayWhatCalleesLose(loops, words, kept.m_bodies);
        kept.m_wordsOfLatestTable = words;
        if (explains) [[unlikely]]
            explainLosses(program, build, words, kept.m_bodies, opcodesThatBorrowNothing);
        return table;
    }
    kept.m_wordsOfLatestTable.insertFill(numberOfKeptBodies, 0, numberOfFunctions - numberOfKeptBodies);
    if (checksKeptBodies) {
        takeAwayWhatCalleesLose(loops, words, kept.m_bodies);
        RELEASE_ASSERT(words == kept.m_wordsOfLatestTable);
    }
    words = kept.m_wordsOfLatestTable;
    return table;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
