/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTReadCapturesFromEnvironments.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTDominators.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueSubstitution.h"
#include "AOTVector.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <span>
#include <tuple>
#include <wtf/BitVector.h>
#include <wtf/DataLog.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

constexpr uint32_t noCandidate = UINT32_MAX;

enum class Obstacle : uint8_t {
    None,
    NamedByEvalScope,
    NamedByOtherOperation,
    FilledWithOtherValue,
    HoldsSeveralFunctions,
    IsNeverFilled,
    IsFilledTwiceInOneBlock,
    MayBeFilledAgain,
    FunctionIsNotEdited,
};

ASCIILiteral obstacleNameOf(Obstacle obstacle)
{
    switch (obstacle) {
    case Obstacle::None:
        return "none"_s;
    case Obstacle::NamedByEvalScope:
        return "named by an eval scope"_s;
    case Obstacle::NamedByOtherOperation:
        return "the layout is named by another operation"_s;
    case Obstacle::FilledWithOtherValue:
        return "filled with something else than a new closure"_s;
    case Obstacle::HoldsSeveralFunctions:
        return "holds closures of several functions"_s;
    case Obstacle::IsNeverFilled:
        return "never filled"_s;
    case Obstacle::IsFilledTwiceInOneBlock:
        return "filled twice in one block"_s;
    case Obstacle::MayBeFilledAgain:
        return "a place that fills it may get the same environment again"_s;
    case Obstacle::FunctionIsNotEdited:
        return "named in a function that the pass leaves alone"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// What becomes of the closure. Whatever it is, a read that only leads to GetCapture is answered by the environment.
enum class Fate : uint8_t { IsLeftAlone, IsNeverMade, IsMadeAtFirstUse, IsMadeAsBefore };

struct CaptureCandidateTable {
    std::optional<uint32_t> find(LayoutID layout, uint32_t field) const
    {
        if (!layouts.get(layout.index()))
            return std::nullopt;
        FieldReference reference { layout, field };
        auto found = std::ranges::lower_bound(fields, reference);
        if (found == fields.end() || *found != reference)
            return std::nullopt;
        return static_cast<uint32_t>(found - fields.begin());
    }

    void forEachCandidateOf(LayoutID layout, NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        size_t candidate = std::ranges::lower_bound(fields, layout, { }, &FieldReference::layout) - fields.begin();
        for (; candidate < fields.size() && fields[candidate].layout == layout; ++candidate)
            functor(static_cast<uint32_t>(candidate));
    }

    // Ascending.
    Vector<FieldReference> fields;
    BitVector layouts;
};

struct FoundCaptures {
    enum class Kind : uint8_t { Store, Reader, Callee, ReadAsValue, Obstacle };

    Kind kind { Kind::Obstacle };
    Obstacle obstacle { Obstacle::None };
    Opcode user { Opcode::Undefined };
    uint32_t candidate { noCandidate };
    FunctionID function;
    BlockID block;
    uint32_t indexInBlock { 0 };
    ValueID environment;
    ValueID store;
    ValueID closure;
    // Where the closure is made, if that is in front of the store in the same block and the closure captures the environment.
    std::optional<uint32_t> indexOfClosure;
    bool closureHasOtherUses { false };
    FunctionID callee;
};

// The closure is a value of the function that makes it too. There it stands for the environment it is stored in, which is at hand wherever the closure is.
static std::optional<uint32_t> findClosureInFrontOf(const Function& function, const Block& block, uint32_t indexOfStore)
{
    const Value& store = function.at(block.values[indexOfStore]);
    if (store.opcode != Opcode::PutEnvironmentField)
        return std::nullopt;
    ValueID stored = store.valueOperand();
    const Value& closure = function.at(stored);
    if (closure.opcode != Opcode::NewClosure || closure.isMadeOnDemand() || !std::ranges::contains(closure.variadicOperands(), store.operand(0)))
        return std::nullopt;
    for (uint32_t index = indexOfStore; index--;) {
        if (block.values[index] == stored)
            return index;
    }
    return std::nullopt;
}

// The values of a function that are the closure in a candidate field: the reads, the closure where it is made, their casts, and the block parameters that such values reach.
class ClosureWebs {
public:
    struct Cause {
        uint32_t candidate;
        Opcode user;
    };

    ClosureWebs(const Function& function, NOESCAPE const Invocable<uint32_t(const Value&)> auto& candidateReadBy, NOESCAPE const Invocable<uint32_t(const Value&)> auto& candidateStoredBy)
        : m_function(function)
    {
        m_candidateOf.fill(noCandidate, function.values.size());
        for (const Block& block : function.blocks) {
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                ValueID id = block.values[index];
                const Value& value = function.at(id);
                if (value.opcode == Opcode::GetEnvironmentField) {
                    uint32_t candidate = candidateReadBy(value);
                    m_candidateOf[id.index()] = candidate;
                    m_isEmpty &= candidate == noCandidate;
                    continue;
                }
                if (value.opcode != Opcode::PutEnvironmentField)
                    continue;
                uint32_t candidate = candidateStoredBy(value);
                if (candidate == noCandidate || !findClosureInFrontOf(function, block, index))
                    continue;
                m_candidateOf[value.valueOperand().index()] = candidate;
                m_closuresWithStores.append({ value.valueOperand(), id });
                m_isEmpty = false;
            }
        }
        if (m_isEmpty)
            return;
        AOT::sort(m_closuresWithStores);
        m_isObject.ensureSize(function.values.size());
        spread();
        findObjects();
        spreadObjects();
    }

    bool isEmpty() const { return m_isEmpty; }
    uint32_t candidateOf(ValueID id) const { return m_candidateOf[id.index()]; }
    bool isObject(ValueID id) const { return m_isObject.quickGet(id.index()); }
    bool standsForCapturesOnly(ValueID id) const { return candidateOf(id) != noCandidate && !isObject(id); }
    std::span<const Cause> causes() const LIFETIME_BOUND { return m_causes.span(); }

    // Of a closure that belongs to a web because of where it is made.
    ValueID storeOf(ValueID closure) const
    {
        auto found = std::ranges::lower_bound(m_closuresWithStores, closure, { }, &std::pair<ValueID, ValueID>::first);
        return found != m_closuresWithStores.end() && found->first == closure ? found->second : ValueID();
    }

private:
    static bool isCast(Opcode opcode) { return opcode == Opcode::AssumeType || opcode == Opcode::Upcast; }

    void spread()
    {
        bool didChange = true;
        while (didChange) {
            didChange = false;
            for (const Block& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    const Value& value = m_function.at(id);
                    if (!isCast(value.opcode) || m_candidateOf[id.index()] != noCandidate)
                        continue;
                    uint32_t candidate = m_candidateOf[value.operand(0).index()];
                    if (candidate == noCandidate)
                        continue;
                    m_candidateOf[id.index()] = candidate;
                    didChange = true;
                }
                block.forEachTarget([&](const Target& target) {
                    const auto& parameters = m_function.at(target.block).parameters;
                    for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                        uint32_t candidate = m_candidateOf[target.arguments[index].index()];
                        uint32_t& candidateOfParameter = m_candidateOf[parameters[index].index()];
                        if (candidate == noCandidate || candidateOfParameter != noCandidate)
                            continue;
                        candidateOfParameter = candidate;
                        didChange = true;
                    }
                });
            }
        }
    }

    void noteObject(ValueID id, Opcode user)
    {
        if (m_candidateOf[id.index()] == noCandidate || m_isObject.quickSet(id.index()))
            return;
        m_causes.append({ m_candidateOf[id.index()], user });
    }

    void findObjects()
    {
        for (const Block& block : m_function.blocks) {
            block.forEachTarget([&](const Target& target) {
                const auto& parameters = m_function.at(target.block).parameters;
                for (uint32_t index = 0; index < parameters.size(); ++index) {
                    if (index < target.arguments.size() && m_candidateOf[target.arguments[index].index()] == m_candidateOf[parameters[index].index()])
                        continue;
                    noteObject(parameters[index], Opcode::Undefined);
                    if (index < target.arguments.size())
                        noteObject(target.arguments[index], Opcode::Undefined);
                }
            });
            for (ValueID id : block.values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::GetCapture)
                    continue;
                if (value.opcode == Opcode::PutEnvironmentField && storeOf(value.valueOperand()) == id)
                    continue;
                value.forEachChild([&](ValueID operand) {
                    if (m_candidateOf[id.index()] == noCandidate || m_candidateOf[id.index()] != m_candidateOf[operand.index()] || !isCast(value.opcode))
                        noteObject(operand, value.opcode);
                });
            }
            static_cast<const Instruction&>(block.terminator).forEachChild([&](ValueID operand) {
                noteObject(operand, Opcode::Undefined);
            });
            if (block.subject)
                noteObject(block.subject, Opcode::Undefined);
        }
    }

    void spreadObjects()
    {
        bool didChange = !m_causes.isEmpty();
        auto share = [&](ValueID a, ValueID b) {
            if (m_isObject.quickGet(a.index()) == m_isObject.quickGet(b.index()))
                return;
            m_isObject.quickSet(a.index());
            m_isObject.quickSet(b.index());
            didChange = true;
        };
        while (didChange) {
            didChange = false;
            for (const Block& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (m_candidateOf[id.index()] != noCandidate && isCast(m_function.at(id).opcode))
                        share(id, m_function.at(id).operand(0));
                }
                block.forEachTarget([&](const Target& target) {
                    const auto& parameters = m_function.at(target.block).parameters;
                    for (uint32_t index = 0; index < target.arguments.size(); ++index) {
                        if (m_candidateOf[parameters[index].index()] != noCandidate && m_candidateOf[parameters[index].index()] == m_candidateOf[target.arguments[index].index()])
                            share(parameters[index], target.arguments[index]);
                    }
                });
            }
        }
    }

    const Function& m_function;
    Vector<uint32_t> m_candidateOf;
    // Ascending.
    Vector<std::pair<ValueID, ValueID>, 8> m_closuresWithStores;
    BitVector m_isObject;
    Vector<Cause, 4> m_causes;
    bool m_isEmpty { true };
};

struct ReadCaptureSource {
    bool isEnvironmentItself { false };
    uint32_t field { 0 };
};

struct AddedField {
    LayoutID layout;
    uint32_t field { 0 };
    DeclaredType declared;
    // It is filled where this field is.
    uint32_t fieldOfClosure { 0 };
};

// Behind the first store of a closure that captures the values.
struct AddedStores {
    FunctionID function;
    BlockID block;
    uint32_t indexInBlock { 0 };
    LayoutID layout;
    ValueID environment;
    Vector<uint32_t, 4> fields;
    Vector<ValueID, 4> values;
};

// A store of a closure that is made at its first use from now on.
struct RemovedStore {
    FunctionID function;
    BlockID block;
    ValueID store;
    uint32_t field { 0 };
    // Nothing else uses it.
    ValueID closure;

    friend auto operator<=>(const RemovedStore&, const RemovedStore&) = default;
};

struct CaptureReadPlan {
    bool takes(uint32_t candidate) const { return obstacles[candidate] == Obstacle::None; }

    std::span<const RemovedStore> storesRemovedIn(FunctionID function) const
    {
        auto range = std::ranges::equal_range(removedStores, function, { }, &RemovedStore::function);
        return { range.begin(), range.end() };
    }

    std::span<const AddedStores> storesAddedIn(FunctionID function) const
    {
        auto range = std::ranges::equal_range(addedStores, function, { }, &AddedStores::function);
        return { range.begin(), range.end() };
    }

    Vector<Obstacle> obstacles;
    Vector<Opcode> users;
    Vector<Fate> fates;
    Vector<FunctionID> callees;
    // NewClosure, for the closures that are made at their first use.
    Vector<Value> closures;
    Vector<Vector<ReadCaptureSource, 4>> sources;
    Vector<TypeID> typeOfEnvironment;
    Vector<AddedField> addedFields;
    // Ascending by function. Within a function the last place comes first.
    Vector<AddedStores> addedStores;
    // Ascending.
    Vector<RemovedStore> removedStores;
    Vector<FunctionID> editedFunctions;
};

// A field that stands for a capture is right only as long as the closure in the environment is the one that was stored first.
class EnvironmentOrigins {
public:
    explicit EnvironmentOrigins(const Function& function)
        : m_function(function)
    {
    }

    struct Origin {
        bool isMadeInThisActivation { false };
        // Where one allocation is all that the value can be: whoever enters this block gets another environment.
        BlockID allocationBlock;
    };

    Origin originOf(ValueID environment)
    {
        prepare();
        BitVector seen(m_function.values.size());
        Vector<ValueID, 4> worklist { environment };
        Origin origin { true, { } };
        unsigned numberOfAllocations = 0;
        while (!worklist.isEmpty()) {
            ValueID id = worklist.takeLast();
            if (seen.quickSet(id.index()))
                continue;
            const Value& value = m_function.at(id);
            switch (value.opcode) {
            case Opcode::NewObject:
                ++numberOfAllocations;
                origin.allocationBlock = m_blockOf[id.index()];
                break;
            case Opcode::AssumeType:
            case Opcode::Upcast:
                worklist.append(value.operand(0));
                break;
            case Opcode::Parameter:
                if (!appendArgumentsFor(id, worklist))
                    return { };
                break;
            default:
                return { };
            }
        }
        if (numberOfAllocations != 1)
            origin.allocationBlock = { };
        return origin;
    }

    // Whether control gets from the end of one block to the start of the other without entering `avoided`.
    bool canReach(BlockID from, BlockID to, BlockID avoided) const
    {
        BitVector seen(m_function.blocks.size());
        Vector<BlockID, 16> worklist;
        auto visitSuccessorsOf = [&](BlockID block) {
            m_function.at(block).forEachTarget([&](const Target& target) {
                if (target.block != avoided && !seen.quickSet(target.block.index()))
                    worklist.append(target.block);
            });
        };
        visitSuccessorsOf(from);
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            if (block == to)
                return true;
            visitSuccessorsOf(block);
        }
        return false;
    }

private:
    void prepare()
    {
        if (!m_blockOf.isEmpty())
            return;
        m_blockOf.grow(m_function.values.size());
        for (uint32_t index = 0; index < m_function.blocks.size(); ++index) {
            const Block& block = m_function.blocks[index];
            for (ValueID parameter : block.parameters)
                m_blockOf[parameter.index()] = BlockID(index);
            for (ValueID id : block.values)
                m_blockOf[id.index()] = BlockID(index);
        }
    }

    bool appendArgumentsFor(ValueID parameter, Vector<ValueID, 4>& worklist) const
    {
        BlockID block = m_blockOf[parameter.index()];
        if (!block || block == entryBlock)
            return false;
        size_t position = m_function.at(block).parameters.find(parameter);
        bool isPassedOnEveryEdge = true;
        for (const Block& predecessor : m_function.blocks) {
            predecessor.forEachTarget([&](const Target& target) {
                if (target.block != block)
                    return;
                if (position < target.arguments.size())
                    worklist.append(target.arguments[position]);
                else
                    isPassedOnEveryEdge = false;
            });
        }
        return isPassedOnEveryEdge;
    }

    const Function& m_function;
    Vector<BlockID> m_blockOf;
};

struct CaptureScanContext {
    const Program& program;
    const BuildContext& build;
    const CaptureCandidateTable& candidates;
    OpcodeSet opcodesNamingLayouts;
};

} // anonymous namespace

static OpcodeSet findOpcodesNamingLayouts()
{
    return Value::opcodesWithLayout;
}

static LayoutID layoutNamedBy(const Value& value)
{
    return Value::opcodesWithLayout.contains(value.opcode) ? value.layout() : LayoutID();
}

static CaptureCandidateTable findCandidates(const Program& program)
{
    CaptureCandidateTable candidates;
    candidates.layouts.ensureSize(program.layouts.size());
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!layout.isEnvironment() || layout.flow.allocation == AllocationState::NeverAllocated)
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            const DeclaredType& declared = layout.fields[field].declared;
            const Type& type = program.at(declared.type);
            if (declared.flags.contains(PositionFlags::IsExpected) || type.primitives || !type.singleFunction())
                continue;
            candidates.fields.append({ LayoutID(index), field });
            candidates.layouts.quickSet(index);
        }
    }
    return candidates;
}

static void rejectWhatMayBeFilledAgain(const Function& function, FunctionID id, Vector<FoundCaptures>& found, size_t firstFound)
{
    struct Place {
        uint32_t candidate;
        BlockID block;
        ValueID environment;
    };
    Vector<Place, 16> places;
    for (const FoundCaptures& entry : found.span().subspan(firstFound)) {
        if (entry.kind == FoundCaptures::Kind::Store)
            places.append({ entry.candidate, entry.block, entry.environment });
    }
    if (places.isEmpty())
        return;

    EnvironmentOrigins origins(function);
    // Most functions store all their closures into one environment in one block.
    struct Answer {
        BlockID from;
        BlockID to;
        ValueID environment;
        bool mayBeFilledAgain;
    };
    Vector<Answer, 2> answers;
    auto mayBeFilledAgain = [&](const Place& earlier, const Place& later) {
        for (const Answer& answer : answers) {
            if (answer.from == earlier.block && answer.to == later.block && answer.environment == later.environment)
                return answer.mayBeFilledAgain;
        }
        EnvironmentOrigins::Origin origin = origins.originOf(later.environment);
        bool result = !origin.isMadeInThisActivation || origins.canReach(earlier.block, later.block, origin.allocationBlock);
        answers.append({ earlier.block, later.block, later.environment, result });
        return result;
    };
    for (const Place& later : places) {
        for (const Place& earlier : places) {
            if (earlier.candidate != later.candidate || !mayBeFilledAgain(earlier, later))
                continue;
            found.append({ .kind = FoundCaptures::Kind::Obstacle, .obstacle = Obstacle::MayBeFilledAgain, .user = function.at(later.environment).opcode, .candidate = later.candidate, .function = id });
            break;
        }
    }
}

static void scanFunction(const CaptureScanContext& context, FunctionID id, Vector<FoundCaptures>& found)
{
    const Function& function = context.program.at(id);
    const CaptureCandidateTable& candidates = context.candidates;
    bool isEdited = context.build.isEnabled(PassID::ReadCapturesFromEnvironments, id);

    auto reject = [&](uint32_t candidate, Obstacle obstacle, Opcode user) {
        found.append({ .kind = FoundCaptures::Kind::Obstacle, .obstacle = obstacle, .user = user, .candidate = candidate, .function = id });
    };
    Vector<uint8_t> useCounts;
    auto hasOneUse = [&](ValueID used) {
        if (useCounts.isEmpty()) {
            useCounts.fill(0, function.values.size());
            auto count = [&](ValueID operand) {
                useCounts[operand.index()] = std::min(useCounts[operand.index()] + 1, 2);
            };
            for (const Block& block : function.blocks) {
                for (ValueID user : block.values)
                    function.at(user).forEachChild(count);
                block.forEachChildOutsideValues(count);
            }
        }
        return useCounts[used.index()] == 1;
    };
    auto noteStore = [&](uint32_t candidate, BlockID block, uint32_t indexInBlock, ValueID environment, ValueID stored) {
        const Value& closure = function.at(stored);
        if (closure.opcode != Opcode::NewClosure || closure.isMadeOnDemand()) {
            reject(candidate, Obstacle::FilledWithOtherValue, closure.opcode);
            return;
        }
        found.append({ .kind = FoundCaptures::Kind::Store, .candidate = candidate, .function = id, .block = block, .indexInBlock = indexInBlock, .environment = environment, .store = function.at(block).values[indexInBlock], .closure = stored, .indexOfClosure = findClosureInFrontOf(function, function.at(block), indexInBlock), .closureHasOtherUses = !hasOneUse(stored), .callee = closure.function() });
    };
    size_t firstFound = found.size();

    bool hasRead = false;
    forEachListedValue(context.program, id, context.opcodesNamingLayouts, [&](BlockID block, uint32_t indexInBlock, ValueID valueID, const Value& value) {
        LayoutID layout = layoutNamedBy(value);
        if (!layout || !candidates.layouts.get(layout.index()))
            return;
        if (!isEdited) {
            candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
                reject(candidate, Obstacle::FunctionIsNotEdited, value.opcode);
            });
            return;
        }
        switch (value.opcode) {
        case Opcode::NewObject: {
            std::span<const uint32_t> fields = value.fields();
            std::span<const ValueID> initialValues = value.variadicOperands();
            for (size_t index = 0; index < fields.size(); ++index) {
                if (auto candidate = candidates.find(layout, fields[index]))
                    noteStore(*candidate, block, indexInBlock, valueID, initialValues[index]);
            }
            break;
        }
        case Opcode::PutEnvironmentField:
            if (auto candidate = candidates.find(layout, value.field()))
                noteStore(*candidate, block, indexInBlock, value.operand(0), value.valueOperand());
            break;
        case Opcode::GetEnvironmentField:
            hasRead |= candidates.find(layout, value.field()).has_value();
            break;
        default:
            candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
                reject(candidate, Obstacle::NamedByOtherOperation, value.opcode);
            });
            break;
        }
    });
    rejectWhatMayBeFilledAgain(function, id, found, firstFound);
    bool hasClosureWithOtherUses = std::ranges::any_of(found.span().subspan(firstFound), [](const FoundCaptures& entry) {
        return entry.kind == FoundCaptures::Kind::Store && entry.indexOfClosure && entry.closureHasOtherUses;
    });
    if (!hasRead && !hasClosureWithOtherUses)
        return;

    found.append({ .kind = FoundCaptures::Kind::Reader, .function = id });
    auto candidateNamedBy = [&](const Value& access) {
        return candidates.find(access.layout(), access.field()).value_or(noCandidate);
    };
    ClosureWebs webs(function, candidateNamedBy, candidateNamedBy);
    for (FoundCaptures& entry : found.mutableSpan().subspan(firstFound)) {
        if (entry.kind == FoundCaptures::Kind::Store && entry.indexOfClosure)
            entry.closureHasOtherUses = webs.isObject(entry.closure);
    }
    // A closure that is an object where it is made need not come out of the field.
    auto userOf = [&](uint32_t candidate) {
        auto cause = std::ranges::find(webs.causes(), candidate, &ClosureWebs::Cause::candidate);
        return cause != webs.causes().end() ? cause->user : Opcode::Undefined;
    };
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (value.opcode == Opcode::GetEnvironmentField && webs.candidateOf(valueID) != noCandidate && webs.isObject(valueID))
                found.append({ .kind = FoundCaptures::Kind::ReadAsValue, .user = userOf(webs.candidateOf(valueID)), .candidate = webs.candidateOf(valueID), .function = id });
            if (value.opcode != Opcode::GetCapture)
                continue;
            if (uint32_t candidate = webs.candidateOf(value.operand(0)); candidate != noCandidate)
                found.append({ .kind = FoundCaptures::Kind::Callee, .candidate = candidate, .function = id, .callee = value.function() });
        }
    }
}

namespace {

struct CaptureStore {
    LayoutID layout;
    FunctionID function;
    ValueID environment;
    BlockID block;
    uint32_t candidate { noCandidate };
    uint32_t indexInBlock { 0 };
    ValueID store;
    ValueID closure;
    // Behind this operation the environment and all that the closure captures are at hand, and nothing has used the closure yet.
    uint32_t indexOfFirstUse { 0 };

    friend auto operator<=>(const CaptureStore&, const CaptureStore&) = default;
};

class Planner {
public:
    Planner(const Program& program, const CaptureCandidateTable& candidates, Vector<FoundCaptures>&& found)
        : m_program(program)
        , m_candidates(candidates)
        , m_found(WTF::move(found))
    {
        size_t size = candidates.fields.size();
        m_plan.obstacles.fill(Obstacle::None, size);
        m_plan.users.fill(Opcode::Undefined, size);
        m_plan.fates.fill(Fate::IsLeftAlone, size);
        m_plan.callees.grow(size);
        m_plan.closures.grow(size);
        m_isReadAsValue.ensureSize(size);
        m_isUsedWhereItIsMade.ensureSize(size);
        m_plan.sources.grow(size);
        m_plan.typeOfEnvironment.grow(size);
    }

    CaptureReadPlan run();

private:
    // The stores of one block into one environment.
    using Run = std::span<const CaptureStore>;

    void reject(uint32_t candidate, Obstacle obstacle, Opcode user = Opcode::Undefined)
    {
        if (m_plan.obstacles[candidate] != Obstacle::None)
            return;
        m_plan.obstacles[candidate] = obstacle;
        m_plan.users[candidate] = user;
    }

    void noteCallee(uint32_t candidate, FunctionID callee)
    {
        if (!m_plan.callees[candidate])
            m_plan.callees[candidate] = callee;
        else if (m_plan.callees[candidate] != callee)
            reject(candidate, Obstacle::HoldsSeveralFunctions);
    }

    // A candidate and the runs that fill it, ascending.
    struct Member {
        Vector<uint32_t, 2> runs;
        uint32_t candidate { noCandidate };
    };

    void sortWhatWasFound();
    Vector<Run, 2> runsOf(std::span<const CaptureStore> storesOfLayout) const;
    void planLayout(LayoutID, std::span<const CaptureStore>);
    void planGroup(LayoutID, std::span<const Run>, std::span<const Member>, uint32_t& nextField);

    const Program& m_program;
    const CaptureCandidateTable& m_candidates;
    Vector<FoundCaptures> m_found;
    Vector<CaptureStore> m_stores;
    Vector<FunctionID> m_readers;
    BitVector m_isReadAsValue;
    BitVector m_isUsedWhereItIsMade;
    CaptureReadPlan m_plan;
};

} // anonymous namespace

void Planner::sortWhatWasFound()
{
    for (const EvalScope& scope : m_program.evalScopes) {
        for (const EvalBinding& binding : scope.bindings) {
            if (!binding.layout || !binding.field)
                continue;
            if (auto candidate = m_candidates.find(binding.layout, *binding.field))
                reject(*candidate, Obstacle::NamedByEvalScope);
        }
    }
    for (const FoundCaptures& found : m_found) {
        switch (found.kind) {
        case FoundCaptures::Kind::Store:
            m_stores.append({ m_candidates.fields[found.candidate].layout, found.function, found.environment, found.block, found.candidate, found.indexInBlock, found.store, found.closure, found.indexOfClosure.value_or(found.indexInBlock) });
            noteCallee(found.candidate, found.callee);
            if (found.closureHasOtherUses)
                m_isUsedWhereItIsMade.quickSet(found.candidate);
            break;
        case FoundCaptures::Kind::ReadAsValue:
            if (!m_isReadAsValue.quickSet(found.candidate))
                m_plan.users[found.candidate] = found.user;
            break;
        case FoundCaptures::Kind::Reader:
            m_readers.append(found.function);
            break;
        case FoundCaptures::Kind::Callee:
            noteCallee(found.candidate, found.callee);
            break;
        case FoundCaptures::Kind::Obstacle:
            reject(found.candidate, found.obstacle, found.user);
            break;
        }
    }
    m_found.clear();
    AOT::sort(m_stores);
    BitVector isFilled(m_candidates.fields.size());
    for (const CaptureStore& store : m_stores)
        isFilled.quickSet(store.candidate);
    for (uint32_t candidate = 0; candidate < m_candidates.fields.size(); ++candidate) {
        if (!isFilled.quickGet(candidate))
            reject(candidate, Obstacle::IsNeverFilled);
    }
}

auto Planner::runsOf(std::span<const CaptureStore> stores) const -> Vector<Run, 2>
{
    Vector<Run, 2> runs;
    for (size_t first = 0; first < stores.size();) {
        size_t end = first;
        while (end < stores.size() && stores[end].function == stores[first].function && stores[end].environment == stores[first].environment && stores[end].block == stores[first].block)
            ++end;
        runs.append(stores.subspan(first, end - first));
        first = end;
    }
    return runs;
}

void Planner::planLayout(LayoutID layout, std::span<const CaptureStore> stores)
{
    Vector<Run, 2> runs = runsOf(stores);
    Vector<Member, 16> members;
    m_candidates.forEachCandidateOf(layout, [&](uint32_t candidate) {
        if (!m_plan.takes(candidate))
            return;
        Member member { { }, candidate };
        for (uint32_t index = 0; index < runs.size(); ++index) {
            size_t numberOfStores = std::ranges::count(runs[index], candidate, &CaptureStore::candidate);
            if (numberOfStores > 1) {
                reject(candidate, Obstacle::IsFilledTwiceInOneBlock);
                return;
            }
            if (numberOfStores)
                member.runs.append(index);
        }
        members.append(WTF::move(member));
    });
    if (members.isEmpty())
        return;

    // Closures that are stored at the same places can share the fields for what they capture.
    AOT::sort(members, [](const Member& a, const Member& b) {
        if (a.runs != b.runs)
            return std::ranges::lexicographical_compare(a.runs, b.runs);
        return a.candidate < b.candidate;
    });
    uint32_t nextField = m_program.at(layout).fields.size();
    for (size_t first = 0; first < members.size();) {
        size_t end = first;
        while (end < members.size() && members[end].runs == members[first].runs)
            ++end;
        planGroup(layout, runs.span(), members.span().subspan(first, end - first), nextField);
        first = end;
    }

    TypeID typeOfEnvironment = m_program.at(runs[0][0].function).at(runs[0][0].environment).type;
    for (const Member& member : members) {
        uint32_t candidate = member.candidate;
        m_plan.typeOfEnvironment[candidate] = typeOfEnvironment;
        if (!m_isReadAsValue.quickGet(candidate))
            m_plan.fates[candidate] = Fate::IsNeverMade;
        else
            m_plan.fates[candidate] = m_isUsedWhereItIsMade.quickGet(candidate) ? Fate::IsMadeAsBefore : Fate::IsMadeAtFirstUse;
    }
    for (const CaptureStore& store : stores) {
        if (m_plan.fates[store.candidate] != Fate::IsMadeAtFirstUse)
            continue;
        m_plan.closures[store.candidate] = m_program.at(store.function).at(store.closure);
        m_plan.removedStores.append({ store.function, store.block, store.store, m_candidates.fields[store.candidate].field, store.closure });
    }
}

void Planner::planGroup(LayoutID layout, std::span<const Run> runsOfLayout, std::span<const Member> group, uint32_t& nextField)
{
    std::span<const uint32_t> runs = group[0].runs.span();
    struct Capture {
        uint32_t candidate;
        uint32_t index;
        // What each run of the group captures here, and where in its block the closure can first be used.
        Vector<ValueID, 2> operands;
        Vector<uint32_t, 2> indexesInBlocks;
    };
    Vector<Capture, 32> captures;
    for (const Member& member : group) {
        uint32_t numberOfCaptures = m_program.at(m_plan.callees[member.candidate]).captures.size();
        for (uint32_t index = 0; index < numberOfCaptures; ++index) {
            Capture capture { member.candidate, index, { }, { } };
            for (uint32_t run : runs) {
                const CaptureStore& store = *std::ranges::find(runsOfLayout[run], member.candidate, &CaptureStore::candidate);
                std::span<const ValueID> captured = m_program.at(store.function).at(store.closure).variadicOperands();
                RELEASE_ASSERT(index < captured.size());
                capture.operands.append(captured[index]);
                capture.indexesInBlocks.append(store.indexOfFirstUse);
            }
            captures.append(WTF::move(capture));
        }
    }

    auto declaredFor = [&](const Capture& capture) -> const DeclaredType& {
        return m_program.at(m_plan.callees[capture.candidate]).captures[capture.index];
    };
    auto isEnvironmentItself = [&](const Capture& capture) {
        for (size_t position = 0; position < runs.size(); ++position) {
            if (capture.operands[position] != runsOfLayout[runs[position]][0].environment)
                return false;
        }
        return true;
    };

    // A field serves the captures that every run fills with one value and that are declared alike.
    struct FieldOfGroup {
        uint32_t field;
        uint32_t firstCapture;
        Vector<uint32_t, 2> indexesInBlocks;
    };
    Vector<FieldOfGroup, 8> fields;
    for (uint32_t index = 0; index < captures.size(); ++index) {
        const Capture& capture = captures[index];
        if (isEnvironmentItself(capture)) {
            m_plan.sources[capture.candidate].append({ true, 0 });
            continue;
        }
        FieldOfGroup* shared = nullptr;
        for (FieldOfGroup& field : fields) {
            if (captures[field.firstCapture].operands == capture.operands && declaredFor(captures[field.firstCapture]) == declaredFor(capture)) {
                shared = &field;
                break;
            }
        }
        if (!shared) {
            m_plan.addedFields.append({ layout, nextField, declaredFor(capture), m_candidates.fields[capture.candidate].field });
            fields.append({ nextField++, index, capture.indexesInBlocks });
            shared = &fields.last();
        }
        for (size_t position = 0; position < runs.size(); ++position)
            shared->indexesInBlocks[position] = std::min(shared->indexesInBlocks[position], capture.indexesInBlocks[position]);
        m_plan.sources[capture.candidate].append({ false, shared->field });
    }

    for (const FieldOfGroup& field : fields) {
        for (size_t position = 0; position < runs.size(); ++position) {
            const CaptureStore& store = runsOfLayout[runs[position]][0];
            m_plan.addedStores.append({ store.function, store.block, field.indexesInBlocks[position], layout, store.environment, { field.field }, { captures[field.firstCapture].operands[position] } });
        }
    }
}

CaptureReadPlan Planner::run()
{
    sortWhatWasFound();
    for (size_t first = 0; first < m_stores.size();) {
        size_t end = first;
        while (end < m_stores.size() && m_stores[end].layout == m_stores[first].layout)
            ++end;
        planLayout(m_stores[first].layout, m_stores.span().subspan(first, end - first));
        first = end;
    }

    AOT::sort(m_plan.addedStores, [](const AddedStores& a, const AddedStores& b) {
        if (a.function != b.function)
            return a.function < b.function;
        return std::tuple { b.block, b.indexInBlock, b.fields[0] } < std::tuple { a.block, a.indexInBlock, a.fields[0] };
    });
    AOT::sort(m_plan.removedStores);
    m_plan.editedFunctions = WTF::move(m_readers);
    for (const AddedStores& added : m_plan.addedStores)
        m_plan.editedFunctions.append(added.function);
    for (const RemovedStore& removed : m_plan.removedStores)
        m_plan.editedFunctions.append(removed.function);
    AOT::sort(m_plan.editedFunctions);
    removeRepeatedElements(m_plan.editedFunctions);
    return WTF::move(m_plan);
}

static void changeLayouts(ProgramEditor& editor, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan)
{
    for (const AddedField& added : plan.addedFields) {
        Layout& layout = editor.at(added.layout);
        const Field& fieldOfClosure = layout.fields[added.fieldOfClosure];
        Field field;
        field.declared = added.declared;
        field.flags = fieldOfClosure.flags | OptionSet<FieldFlags> { FieldFlags::IsHidden, FieldFlags::IsAddedLate };
        field.flow.location.writers = WriterState::AllKnown;
        field.flow.location.stores = fieldOfClosure.flow.location.stores;
        field.flow.location.storeNaming = StoreNaming::EveryStoreNamesLocation;
        field.flow.location.initialization = fieldOfClosure.flow.location.initialization;
        field.flow.occupancy = fieldOfClosure.flow.occupancy;
        field.flow.stability = fieldOfClosure.flow.stability;
        uint32_t index = layout.appendField(WTF::move(field));
        RELEASE_ASSERT(index == added.field);
    }
    // Only the operations of the program and the scopes of eval reach a field of an environment, and the scan has seen them all.
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        FieldReference reference = candidates.fields[candidate];
        if (plan.fates[candidate] == Fate::IsNeverMade) {
            LocationFlow& location = editor.at(reference.layout).fields[reference.field].flow.location;
            location.writers = WriterState::AllKnown;
            location.storeNaming = StoreNaming::EveryStoreNamesLocation;
            location.reads = ReadState::NeverRead;
        }
        if (plan.fates[candidate] == Fate::IsMadeAtFirstUse) {
            Field& field = editor.at(reference.layout).fields[reference.field];
            field.declared.type = editor.types().intern(editor.program().at(field.declared.type).withPrimitives(Primitive::Absent));
            field.flags.remove({ FieldFlags::IsRequired, FieldFlags::IsImmutable });
            field.flow.location.stores = StoreCount::Unknown;
            field.flow.location.initialization = InitializationState::Unknown;
            field.flow.location.reads = ReadState::Unknown;
            field.flow.occupancy = Occupancy::Unknown;
        }
    }
}

static void storeCaptures(const CaptureReadPlan& plan, FunctionID id, IRBuilder& builder)
{
    Function& function = builder.function();
    for (const AddedStores& added : plan.storesAddedIn(id)) {
        builder.setOrigin(function.at(function.at(added.block).values[added.indexInBlock]).origin());
        Vector<ValueID, 4> stores;
        for (size_t index = 0; index < added.fields.size(); ++index) {
            Value store = IRBuilder::make(Opcode::PutEnvironmentField, { added.environment, added.values[index] });
            store.setLayout(added.layout);
            store.setField(added.fields[index]);
            stores.append(builder.addDetached(WTF::move(store)));
        }
        function.edit(added.block, BlockEdit::Values).values.insertSpan(added.indexInBlock + 1, stores.span());
    }
}

static void removeStores(const CaptureReadPlan& plan, FunctionID id, IRBuilder& builder)
{
    Function& function = builder.function();
    std::span<const RemovedStore> removed = plan.storesRemovedIn(id);
    if (removed.empty())
        return;
    BitVector closures(function.values.size());
    for (const RemovedStore& store : removed)
        closures.quickSet(store.closure.index());
    while (!removed.empty()) {
        BlockID block = removed[0].block;
        size_t numberInBlock = std::ranges::find_if(removed, [&](const RemovedStore& store) {
            return store.block != block;
        }) - removed.begin();
        std::span<const RemovedStore> removedInBlock = removed.first(numberInBlock);
        removed = removed.subspan(numberInBlock);

        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            auto range = std::ranges::equal_range(removedInBlock, values[index], { }, &RemovedStore::store);
            if (range.empty())
                continue;
            Value& store = function.edit(values[index]);
            if (store.opcode == Opcode::PutEnvironmentField) {
                erasedIndexes.set(index);
                continue;
            }
            RELEASE_ASSERT(store.opcode == Opcode::NewObject);
            Vector<uint32_t, 16> remainingFields;
            Vector<ValueID, 16> remainingValues;
            std::span<const uint32_t> fields = store.fields();
            std::span<const ValueID> initialValues = store.variadicOperands();
            for (size_t position = 0; position < fields.size(); ++position) {
                if (std::ranges::find(range, fields[position], &RemovedStore::field) != range.end())
                    continue;
                remainingFields.append(fields[position]);
                remainingValues.append(initialValues[position]);
            }
            store.setFields(remainingFields.span());
            store.setVariadicOperands(remainingValues.span());
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(block, erasedIndexes);
    }
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (closures.get(values[index].index()))
                erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
    }
}

static void readCaptures(const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, const ClosureWebs& webs, IRBuilder& builder)
{
    Function& function = builder.function();
    ValueSubstitution substitution(function);
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        for (ValueID parameter : function.at(block).parameters) {
            if (!webs.standsForCapturesOnly(parameter))
                continue;
            function.edit(parameter, ValueEdit::Type).type = plan.typeOfEnvironment[webs.candidateOf(parameter)];
            function.edit(parameter, ValueEdit::Type).setExpected({ });
        }

        const BlockValues& values = function.at(block).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            Value& value = function.edit(id);
            if (webs.standsForCapturesOnly(id)) {
                ValueID store = webs.storeOf(id);
                substitution.replace(id, store ? function.at(store).operand(0) : value.operand(0));
                erasedIndexes.set(index);
                continue;
            }
            if (value.opcode == Opcode::PutEnvironmentField && webs.storeOf(value.valueOperand()) == id && webs.standsForCapturesOnly(value.valueOperand())) {
                erasedIndexes.set(index);
                continue;
            }
            if (value.opcode != Opcode::GetCapture)
                continue;
            ValueID closure = value.operand(0);
            if (!webs.standsForCapturesOnly(closure))
                continue;
            uint32_t candidate = webs.candidateOf(closure);
            ReadCaptureSource source = plan.sources[candidate][value.index()];
            if (source.isEnvironmentItself) {
                substitution.replace(id, closure);
                erasedIndexes.set(index);
                continue;
            }
            value.become(Opcode::GetEnvironmentField);
            value.setOperand(0, closure);
            value.setLayout(candidates.fields[candidate].layout);
            value.setField(source.field);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(block, erasedIndexes);
    }
    substitution.apply(function, nullptr);
}

static Value makeRead(ValueID environment, LayoutID layout, uint32_t field)
{
    Value read = IRBuilder::make(Opcode::GetEnvironmentField, { environment });
    read.setLayout(layout);
    read.setField(field);
    return read;
}

// The field holds nothing until somebody wants the object. It stays the same object from then on.
static void makeClosureAtRead(const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, uint32_t candidate, IRBuilder& builder, ProgramPoint point)
{
    Function& function = builder.function();
    FieldReference reference = candidates.fields[candidate];
    ValueID environment = function.at(function.at(point.block).values[point.index]).operand(0);

    IRBuilder::Expansion expansion = builder.beginExpansion(point);
    ValueID held = builder.append(makeRead(environment, reference.layout, reference.field));
    IRBuilder::ProvenBranch branch = builder.emitBranchWithProof(builder.appendTypeTest(held, TypeTable::absent), 0, BlockFrequency::Rare, BlockFrequency::Normal);

    Vector<ValueID, 8> captures;
    for (ReadCaptureSource source : plan.sources[candidate])
        captures.append(source.isEnvironmentItself ? environment : builder.append(makeRead(environment, reference.layout, source.field)));
    Value closure(plan.closures[candidate]);
    closure.setVariadicOperands(captures.span());
    ValueID made = builder.append(WTF::move(closure));
    Value store = IRBuilder::make(Opcode::PutEnvironmentField, { environment, made });
    store.setLayout(reference.layout);
    store.setField(reference.field);
    builder.append(WTF::move(store));
    builder.endExpansion(expansion, made);

    builder.moveToEndOf(branch.notTaken);
    builder.endExpansion(expansion, branch.provenOnNotTaken);
}

static void makeClosuresAtReads(const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, const ClosureWebs& webs, IRBuilder& builder)
{
    Function& function = builder.function();
    uint32_t numberOfBlocks = function.blocks.size();
    uint32_t numberOfValues = function.values.size();
    Vector<std::pair<uint32_t, uint32_t>, 8> reads;
    for (uint32_t blockIndex = 0; blockIndex < numberOfBlocks; ++blockIndex) {
        BlockID block(blockIndex);
        reads.shrink(0);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            ValueID id = values[index];
            if (id.index() >= numberOfValues || function.at(id).opcode != Opcode::GetEnvironmentField)
                continue;
            uint32_t candidate = webs.candidateOf(id);
            if (candidate != noCandidate && webs.isObject(id) && plan.fates[candidate] == Fate::IsMadeAtFirstUse)
                reads.append({ index, candidate });
        }
        // What follows a read moves to another block. What precedes it stays where it is.
        for (auto [index, candidate] : reads.span() | std::views::reverse)
            makeClosureAtRead(candidates, plan, candidate, builder, { block, index });
    }
}

static void dump(const Program& program, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan, PrintStream& out)
{
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        FieldReference reference = candidates.fields[candidate];
        out.print("closure in environment L", reference.layout.index(), " field ", reference.field);
        if (FunctionID callee = plan.callees[candidate])
            out.print(" f", callee.index(), " captures ", program.at(callee).captures.size());
        switch (plan.fates[candidate]) {
        case Fate::IsLeftAlone:
            out.print(": ", obstacleNameOf(plan.obstacles[candidate]));
            break;
        case Fate::IsNeverMade:
            out.print(": never made");
            break;
        case Fate::IsMadeAtFirstUse:
            out.print(": made at its first use as a value");
            break;
        case Fate::IsMadeAsBefore:
            out.print(": made as before, used where it is made and read as a value");
            break;
        }
        if (plan.users[candidate] != Opcode::Undefined)
            out.print(" (", nameOf(plan.users[candidate]), ")");
        out.println();
    }
    out.println("fields added for captures: ", plan.addedFields.size());
}

namespace {

constexpr uint32_t noField = UINT32_MAX;

// A claim about a function: entry parameter `environment` is an environment of the layout, and entry parameter k holds what lies in field fields[k] of that environment.
struct OwnCaptures {
    LayoutID layout;
    uint32_t environment { 0 };
    Vector<uint32_t, 4> fields;
};

struct OwnCapturesTable {
    const OwnCaptures* find(FunctionID id) const
    {
        uint32_t claim = claimOf[id.index()];
        return claim == noCandidate ? nullptr : &claims[claim];
    }

    // The versions of a function share the claim about it.
    Vector<uint32_t> claimOf;
    Vector<OwnCaptures> claims;
};

// The claim about `function` holds only if the one about `restsOn` does. Without that function it is refuted.
struct Doubt {
    FunctionID function;
    FunctionID restsOn;
    // Where the doubt comes from.
    FunctionID user;
    ValueID value;
};

// The value that a cast, or the proof of a test, is another name of.
class Roots {
public:
    explicit Roots(const Function& function)
        : m_function(function)
    {
    }

    ValueID rootOf(ValueID id)
    {
        for (unsigned steps = 0; id && steps < 64; ++steps) {
            const Value& value = m_function.at(id);
            if (value.opcode == Opcode::AssumeType || value.opcode == Opcode::Upcast) {
                id = value.operand(0);
                continue;
            }
            if (value.opcode != Opcode::Parameter)
                return id;
            prepare();
            ValueID proven = m_provenBy[id.index()];
            if (!proven)
                return id;
            id = proven;
        }
        return id;
    }

private:
    void prepare()
    {
        if (!m_provenBy.isEmpty())
            return;
        m_provenBy.grow(m_function.values.size());
        BitVector hasSeveral(m_function.values.size());
        for (const Block& block : m_function.blocks) {
            if (block.terminator.kind != TerminatorKind::Branch)
                continue;
            const Value& test = m_function.at(block.terminator.conditionOperand());
            // The proof of any other test is a guard, which is no name of what was tested.
            if (test.opcode != Opcode::TypeTest)
                continue;
            for (const Target& target : block.terminator.targets) {
                const auto& parameters = m_function.at(target.block).parameters;
                for (uint32_t index = 0; index < target.proofs.size(); ++index) {
                    ValueID parameter = parameters[target.arguments.size() + index];
                    ValueID proven = test.operand(target.proofs[index]);
                    if (m_provenBy[parameter.index()] && m_provenBy[parameter.index()] != proven)
                        hasSeveral.quickSet(parameter.index());
                    m_provenBy[parameter.index()] = proven;
                }
            }
        }
        for (size_t index : hasSeveral)
            m_provenBy[index] = { };
    }

    const Function& m_function;
    Vector<ValueID> m_provenBy;
};

} // anonymous namespace

static bool hasParametersOfSource(const Function& function)
{
    const Vector<ParameterOrigin>& parameterMap = function.derivation.parameterMap;
    for (uint32_t index = 0; index < parameterMap.size(); ++index) {
        if (parameterMap[index].original != index)
            return false;
    }
    return true;
}

static OwnCapturesTable claimOwnCaptures(const Program& program, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan)
{
    OwnCapturesTable table;
    table.claimOf.fill(noCandidate, program.functions.size());
    BitVector liesInSeveralFields(program.functions.size());
    for (uint32_t candidate = 0; candidate < candidates.fields.size(); ++candidate) {
        if (!plan.takes(candidate) || plan.fates[candidate] == Fate::IsLeftAlone)
            continue;
        std::span<const ReadCaptureSource> sources = plan.sources[candidate].span();
        auto itself = std::ranges::find(sources, true, &ReadCaptureSource::isEnvironmentItself);
        if (itself == sources.end())
            continue;
        FunctionID callee = plan.callees[candidate];
        if (table.claimOf[callee.index()] != noCandidate) {
            liesInSeveralFields.quickSet(callee.index());
            continue;
        }
        OwnCaptures claim { candidates.fields[candidate].layout, static_cast<uint32_t>(itself - sources.begin()), { } };
        for (ReadCaptureSource source : sources)
            claim.fields.append(source.isEnvironmentItself ? noField : source.field);
        table.claimOf[callee.index()] = table.claims.size();
        table.claims.append(WTF::move(claim));
    }
    for (size_t index : liesInSeveralFields)
        table.claimOf[index] = noCandidate;
    if (table.claims.isEmpty())
        return table;

    // What is claimed here is only a guess. The calls decide.
    constexpr OptionSet<FunctionCapabilities> needed { FunctionCapabilities::HasOrdinaryFrame, FunctionCapabilities::IsNeverCalledByHost };
    for (uint32_t index = 0; index < program.functions.size(); ++index) {
        const Function& function = program.functions[index];
        if (!function.hasBody() || !function.capabilities.containsAll(needed)) {
            table.claimOf[index] = noCandidate;
            continue;
        }
        if (table.claimOf[index] != noCandidate || !hasParametersOfSource(function))
            continue;
        FunctionID source = function.derivation.source;
        for (unsigned steps = 0; source && steps < 4 && table.claimOf[source.index()] == noCandidate; ++steps)
            source = program.at(source).derivation.source;
        if (source && table.claimOf[source.index()] != noCandidate && function.captures.size() == program.at(source).captures.size())
            table.claimOf[index] = table.claimOf[source.index()];
    }
    return table;
}

// Whether every way to the point leads over a store of the value into the field.
static bool isStoredInFrontOf(const Function& function, ProgramPoint point, Roots& roots, ValueID environment, FieldReference field, ValueID stored)
{
    std::optional<CFG> cfg;
    std::optional<Dominators> dominators;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& store = function.at(values[index]);
            if (store.opcode != Opcode::PutEnvironmentField || store.layout() != field.layout || store.field() != field.field
                || roots.rootOf(store.operand(0)) != environment || roots.rootOf(store.valueOperand()) != stored)
                continue;
            if (block == point.block) {
                if (index < point.index)
                    return true;
                continue;
            }
            if (!dominators) {
                cfg.emplace(function);
                dominators.emplace(*cfg, false);
            }
            if (dominators->strictlyDominates(block, point.block))
                return true;
        }
    }
    return false;
}

// Whether what a call hands to a function, or a closure of it captures, is as the claim says.
static bool givesOwnCaptures(const OwnCapturesTable& table, const Function& function, FunctionID id, Roots& roots, const OwnCaptures& claim, std::span<const ValueID> given, ProgramPoint point, Vector<FunctionID, 2>& restsOn)
{
    if (given.size() < claim.fields.size())
        return false;
    ValueID environment = roots.rootOf(given[claim.environment]);
    if (!environment)
        return false;
    const OwnCaptures* own = table.find(id);
    const auto& parameters = function.at(entryBlock).parameters;
    bool givesOwnEnvironment = own && own->layout == claim.layout && parameters[own->environment] == environment;
    for (uint32_t index = 0; index < claim.fields.size(); ++index) {
        uint32_t field = claim.fields[index];
        if (field == noField)
            continue;
        ValueID root = roots.rootOf(given[index]);
        if (!root)
            return false;
        const Value& value = function.at(root);
        if (value.opcode == Opcode::GetEnvironmentField && value.layout() == claim.layout && value.field() == field && roots.rootOf(value.operand(0)) == environment)
            continue;
        if (givesOwnEnvironment) {
            size_t position = parameters.find(root);
            if (position != notFound && position < own->fields.size() && own->fields[position] == field) {
                restsOn.append(id);
                continue;
            }
        }
        if (value.opcode == Opcode::GetCapture && value.index() == index && table.find(value.function()) == &claim) {
            const Value& capturedEnvironment = function.at(environment);
            if (capturedEnvironment.opcode == Opcode::GetCapture && capturedEnvironment.index() == claim.environment && capturedEnvironment.function() == value.function()
                && roots.rootOf(capturedEnvironment.operand(0)) == roots.rootOf(value.operand(0))) {
                restsOn.append(value.function());
                continue;
            }
        }
        if (isStoredInFrontOf(function, point, roots, environment, { claim.layout, field }, root))
            continue;
        return false;
    }
    return true;
}

static void doubtOwnCaptures(const Program& program, const OwnCapturesTable& table, FunctionID id, Vector<Doubt>& doubts)
{
    const Function& function = program.at(id);
    std::optional<Roots> roots;
    Vector<FunctionID, 2> restsOn;
    auto namesFunctionWithClaim = [&](NameKind kind, uint32_t index) {
        return kind == NameKind::Function && table.find(FunctionID(index));
    };
    forEachListedValueNaming(program, id, Value::opcodesWithFunction, namesFunctionWithClaim, [&](BlockID block, ValueID valueID, const Value& value) {
        if (value.opcode == Opcode::GetCapture || value.opcode == Opcode::IsClosureOf)
            return;
        const OwnCaptures* claim = table.find(value.function());
        if (!claim)
            return;
        if (value.opcode != Opcode::CallDirect && value.opcode != Opcode::NewClosure) {
            doubts.append({ value.function(), { }, id, valueID });
            return;
        }
        if (!roots)
            roots.emplace(function);
        restsOn.shrink(0);
        // What a closure captures is stored behind it, in the same block.
        const BlockValues& valuesOfBlock = function.at(block).values;
        ProgramPoint point { block, static_cast<uint32_t>(value.opcode == Opcode::NewClosure ? valuesOfBlock.size() : valuesOfBlock.find(valueID)) };
        if (!givesOwnCaptures(table, function, id, *roots, *claim, value.variadicOperands(), point, restsOn)) {
            doubts.append({ value.function(), { }, id, valueID });
            return;
        }
        for (FunctionID other : restsOn) {
            if (other != value.function())
                doubts.append({ value.function(), other, id, valueID });
        }
    });
}

namespace {

struct CallBehindEntryChecks {
    FunctionID function;
    Vector<ValueID, 8> operands;
    // What the body has proven of its result.
    TypeID type;
};

} // anonymous namespace

// Where the operands of the call pass every test of the function that it goes to: the call that stands behind the tests.
static std::optional<CallBehindEntryChecks> findCallBehindEntryChecks(const Program& program, const BitVector& holdsEntryChecks, const Function& caller, const Value& call)
{
    constexpr std::nullopt_t none = std::nullopt;
    if (!holdsEntryChecks.get(call.function().index()))
        return none;
    const Function& checks = program.at(call.function());
    const auto& parameters = checks.at(entryBlock).parameters;
    if (parameters.size() != call.numberOfOperands())
        return none;
    Vector<ValueID, 32> given(checks.values.size());
    for (uint32_t index = 0; index < parameters.size(); ++index)
        given[parameters[index].index()] = call.operand(index);
    auto enter = [&](const Target& target, const Value* test) {
        const auto& targetParameters = checks.at(target.block).parameters;
        for (uint32_t index = 0; index < target.arguments.size(); ++index)
            given[targetParameters[index].index()] = given[target.arguments[index].index()];
        for (uint32_t index = 0; index < target.proofs.size(); ++index)
            given[targetParameters[target.arguments.size() + index].index()] = test ? given[test->operand(target.proofs[index]).index()] : ValueID();
        return target.block;
    };

    BlockID block = entryBlock;
    for (size_t steps = 0; steps < checks.blocks.size(); ++steps) {
        const Block& current = checks.at(block);
        const Terminator& terminator = current.terminator;
        switch (terminator.kind) {
        case TerminatorKind::Jump:
            if (!current.values.isEmpty())
                return none;
            block = enter(terminator.next(), nullptr);
            continue;
        case TerminatorKind::Branch: {
            if (current.values.size() != 1 || terminator.conditionOperand() != current.values[0])
                return none;
            const Value& test = checks.at(current.values[0]);
            if (test.opcode != Opcode::TypeTest)
                return none;
            ValueID tested = given[test.valueOperand().index()];
            if (!tested || !program.typeOf(caller, tested).isSubsetOf(program.at(test.testedType()), program))
                return none;
            block = enter(terminator.taken(), &test);
            continue;
        }
        case TerminatorKind::ReturnCall: {
            if (current.values.size() != 1 || terminator.operand(0) != current.values[0] || terminator.casts().size())
                return none;
            const Value& callOfBody = checks.at(current.values[0]);
            if (callOfBody.opcode != Opcode::CallDirect || !program.at(callOfBody.type).isSubsetOf(program.at(call.type), program))
                return none;
            CallBehindEntryChecks found { callOfBody.function(), { }, callOfBody.type };
            for (ValueID operand : callOfBody.variadicOperands()) {
                if (!given[operand.index()])
                    return none;
                found.operands.append(given[operand.index()]);
            }
            return found;
        }
        default:
            return none;
        }
    }
    return none;
}

static void callBodiesBehindEntryChecks(const AnalysisContext& context, const BitVector& holdsEntryChecks, Function& function, TypeArena& types)
{
    const Program& program = context.program;
    Vector<ValueID, 8> callsWithNarrowerTypes;
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            Value& call = function.edit(id);
            if (call.opcode != Opcode::CallDirect)
                continue;
            std::optional<CallBehindEntryChecks> found = findCallBehindEntryChecks(program, holdsEntryChecks, function, call);
            if (!found)
                continue;
            call.setFunction(found->function);
            call.setArgumentsOperands(found->operands.span());
            if (call.type != found->type) {
                call.type = found->type;
                call.setExpected({ });
                callsWithNarrowerTypes.append(id);
            }
        }
    }
    // What is computed from such a result has its type, and later its form, from the type of the result.
    if (callsWithNarrowerTypes.isEmpty())
        return;
    FunctionAnalyses analyses(context, function);
    narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, types, context.build }, callsWithNarrowerTypes.span());
}

static void readOwnCaptures(const Program& program, const OwnCaptures& own, IRBuilder& builder)
{
    Function& function = builder.function();
    const auto& parameters = function.at(entryBlock).parameters;
    Roots roots(function);
    ValueSubstitution substitution(function);
    BitVector replacedReads(function.values.size());
    // Reads with a wider type than the entry parameter has.
    Vector<std::pair<ValueID, ValueID>, 8> widerReads;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        for (uint32_t index = 0; index < values.size(); ++index) {
            const Value& read = function.at(values[index]);
            if (read.opcode != Opcode::GetEnvironmentField || read.layout() != own.layout || roots.rootOf(read.operand(0)) != parameters[own.environment])
                continue;
            size_t capture = own.fields.find(read.field());
            if (capture == notFound || !program.typeOf(function, parameters[capture]).isSubsetOf(program.at(read.type), program))
                continue;
            bool hasSameType = function.at(parameters[capture]).type == read.type;
            keepReadForValidation(builder, { block, index }, parameters[capture]);
            if (!hasSameType) {
                widerReads.append({ values[index], parameters[capture] });
                continue;
            }
            substitution.replace(values[index], parameters[capture]);
            replacedReads.quickSet(values[index].index());
        }
    }
    if (substitution.isEmpty() && widerReads.isEmpty())
        return;
    builder.applyInsertions();
    if (!widerReads.isEmpty()) {
        // Ascending: the blocks were visited in order, but the values of a function are not numbered by block.
        AOT::sort(widerReads);
        BitVector hasOtherUse(function.values.size());
        auto entryParameterFor = [&](ValueID operand) {
            auto found = std::ranges::lower_bound(widerReads, operand, { }, &std::pair<ValueID, ValueID>::first);
            return found != widerReads.end() && found->first == operand ? found->second : ValueID();
        };
        auto noteOtherUse = [&](ValueID operand) {
            if (entryParameterFor(operand))
                hasOtherUse.set(operand.index());
        };
        for (Block& block : function.editAllBlocks()) {
            for (ValueID id : block.values) {
                Value& user = function.edit(id);
                if (user.opcode != Opcode::CallDirect && user.opcode != Opcode::NewClosure) {
                    user.forEachChild(noteOtherUse);
                    continue;
                }
                for (uint32_t operandIndex = 0; operandIndex < user.numberOfOperands(); ++operandIndex) {
                    if (ValueID entryParameter = user.operand(operandIndex) ? entryParameterFor(user.operand(operandIndex)) : ValueID())
                        user.setOperand(operandIndex, entryParameter);
                }
            }
            block.forEachChildOutsideValues(noteOtherUse);
        }
        for (auto [read, entryParameter] : widerReads) {
            if (!hasOtherUse.get(read.index()))
                replacedReads.quickSet(read.index());
        }
    }
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.at(BlockID(blockIndex)).values;
        BitVector erasedIndexes;
        for (uint32_t index = 0; index < values.size(); ++index) {
            if (replacedReads.get(values[index].index()))
                erasedIndexes.set(index);
        }
        if (!erasedIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), erasedIndexes);
    }
    substitution.apply(function, nullptr);
}

// A function that lies in an environment runs with that environment among its captures, so what it would read there for a call of itself, or of a function beside it, it has at hand.
static void passOwnCaptures(ProgramEditor& editor, const FlowResults& flow, BuildContext& build, const CaptureCandidateTable& candidates, const CaptureReadPlan& plan)
{
    const Program& program = editor.program();
    OwnCapturesTable table = claimOwnCaptures(program, candidates, plan);
    Vector<Doubt> doubts;
    if (!table.claims.isEmpty()) {
        doubts = collectInParallel<Doubt>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Doubt>& doubtsInFunction) {
            doubtOwnCaptures(program, table, FunctionID(index), doubtsInFunction);
        });
    }
    bool didChange = !doubts.isEmpty();
    while (didChange) {
        didChange = false;
        for (const Doubt& doubt : doubts) {
            if (table.claimOf[doubt.function.index()] == noCandidate || (doubt.restsOn && table.claimOf[doubt.restsOn.index()] != noCandidate))
                continue;
            table.claimOf[doubt.function.index()] = noCandidate;
            didChange = true;
            if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]]
                dataLogLn("does not know its captures: f", doubt.function.index(), " because of v", doubt.value.index(), " (", nameOf(program.at(doubt.user).at(doubt.value).opcode), ") in f", doubt.user.index(), doubt.restsOn ? ", which hands on its own" : "");
        }
    }

    BitVector holdsEntryChecks(program.functions.size());
    for (uint32_t index : findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        RecordFacts facts = recordFactsOf(program, FunctionID(index));
        return facts.role == FunctionRole::EntryChecks && facts.hasBody;
    }))
        holdsEntryChecks.quickSet(index);

    Vector<FunctionID> functions = findIndicesInParallel(build.parallelLoops(), program.functions.size(), [&](unsigned index) {
        const OwnCaptures* own = table.find(FunctionID(index));
        OpcodeSet wanted { Opcode::CallDirect };
        if (own)
            wanted.add(Opcode::GetEnvironmentField);
        // The tests are read while the others are edited.
        if (!mayListValueWith(program, FunctionID(index), wanted) || holdsEntryChecks.quickGet(index) || !build.isEnabled(PassID::ReadCapturesFromEnvironments, FunctionID(index)))
            return false;
        const Function& function = program.functions[index];
        bool isFound = false;
        auto mayBeWanted = [&](NameKind kind, uint32_t name) {
            if (isFound)
                return false;
            return kind == NameKind::Function ? holdsEntryChecks.quickGet(name) : kind == NameKind::Layout && own && name == own->layout.index();
        };
        forEachListedValueNaming(program, FunctionID(index), wanted, mayBeWanted, [&](BlockID, ValueID, const Value& value) {
            if (value.opcode == Opcode::GetEnvironmentField)
                isFound = value.layout() == own->layout && own->fields.contains(value.field());
            else
                isFound = !!findCallBehindEntryChecks(program, holdsEntryChecks, function, value);
        });
        return isFound;
    }).map([](uint32_t index) {
        return FunctionID(index);
    });
    if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]] {
        for (FunctionID id : functions) {
            if (const OwnCaptures* own = table.find(id))
                dataLogLn("has its captures at hand: f", id.index(), " in environment L", own->layout.index(), " at entry parameter ", own->environment);
        }
    }
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        AnalysisContext context { build, program, &flow, nullptr, id };
        if (const OwnCaptures* own = table.find(id)) {
            IRBuilder builder(context, function, arenas.types);
            readOwnCaptures(program, *own, builder);
        }
        callBodiesBehindEntryChecks(context, holdsEntryChecks, function, arenas.types);
    });
}

void readCapturesFromEnvironments(ProgramEditor& editor, const FlowResults& flow, BuildContext& build)
{
    const Program& program = editor.program();
    CaptureCandidateTable candidates = findCandidates(program);
    if (candidates.fields.isEmpty()) {
        passOwnCaptures(editor, flow, build, candidates, { });
        return;
    }

    CaptureScanContext context { program, build, candidates, findOpcodesNamingLayouts() };
    Vector<FoundCaptures> found = collectInParallel<FoundCaptures>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<FoundCaptures>& foundInFunction) {
        if (program.functions[index].hasBody())
            scanFunction(context, FunctionID(index), foundInFunction);
    });
    CaptureReadPlan plan = Planner(program, candidates, WTF::move(found)).run();
    if (build.options().logging.dumpedPass == PassID::ReadCapturesFromEnvironments) [[unlikely]]
        dump(program, candidates, plan, WTF::dataFile());
    if (std::ranges::none_of(plan.obstacles, [](Obstacle obstacle) { return obstacle == Obstacle::None; })) {
        passOwnCaptures(editor, flow, build, candidates, plan);
        return;
    }

    changeLayouts(editor, candidates, plan);
    editFunctionsInParallel(editor, build, plan.editedFunctions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        IRBuilder builder(AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types);
        storeCaptures(plan, id, builder);
        ClosureWebs webs(function, [&](const Value& read) {
            auto candidate = candidates.find(read.layout(), read.field());
            return candidate && plan.takes(*candidate) ? *candidate : noCandidate;
        }, [&](const Value& store) {
            // A closure that is made as before is an object wherever it goes.
            auto candidate = candidates.find(store.layout(), store.field());
            return candidate && (plan.fates[*candidate] == Fate::IsNeverMade || plan.fates[*candidate] == Fate::IsMadeAtFirstUse) ? *candidate : noCandidate;
        });
        if (!webs.isEmpty())
            readCaptures(candidates, plan, webs, builder);
        removeStores(plan, id, builder);
        if (!webs.isEmpty())
            makeClosuresAtReads(candidates, plan, webs, builder);
    });
    passOwnCaptures(editor, flow, build, candidates, plan);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
