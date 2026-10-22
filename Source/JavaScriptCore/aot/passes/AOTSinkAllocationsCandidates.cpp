/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include <utility>
#include <wtf/EnumSet.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Most functions make no more objects than a word has bits. The few that make more are the large ones, where an analysis that is left out saves most.
struct WideObjectMask {
    static constexpr uint32_t numberOfWords = 4;

    constexpr WideObjectMask(uint64_t low = 0)
        : words { low, 0, 0, 0 }
    {
    }

    explicit constexpr operator bool() const { return words[0] | words[1] | words[2] | words[3]; }
    constexpr WideObjectMask operator~() const
    {
        WideObjectMask result;
        for (uint32_t word = 0; word < numberOfWords; ++word)
            result.words[word] = ~words[word];
        return result;
    }
    constexpr WideObjectMask& operator|=(const WideObjectMask& other)
    {
        for (uint32_t word = 0; word < numberOfWords; ++word)
            words[word] |= other.words[word];
        return *this;
    }
    constexpr WideObjectMask& operator&=(const WideObjectMask& other)
    {
        for (uint32_t word = 0; word < numberOfWords; ++word)
            words[word] &= other.words[word];
        return *this;
    }
    friend constexpr WideObjectMask operator|(WideObjectMask a, const WideObjectMask& b) { return a |= b; }
    friend constexpr WideObjectMask operator&(WideObjectMask a, const WideObjectMask& b) { return a &= b; }
    friend constexpr bool operator==(const WideObjectMask&, const WideObjectMask&) = default;

    uint64_t words[numberOfWords];
};

template<typename ObjectMask> constexpr uint32_t numberOfObjectsIn = 64;
template<> constexpr uint32_t numberOfObjectsIn<WideObjectMask> = 64 * WideObjectMask::numberOfWords;

template<typename ObjectMask> constexpr ObjectMask maskOfObject(uint32_t object) { return ObjectMask(1) << object; }
template<> constexpr WideObjectMask maskOfObject<WideObjectMask>(uint32_t object)
{
    WideObjectMask result;
    result.words[object / 64] = uint64_t { 1 } << object % 64;
    return result;
}

void forEachObjectIn(uint64_t objects, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    for (; objects; objects &= objects - 1)
        functor(ctz(objects));
}

void forEachObjectIn(const WideObjectMask& objects, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
{
    for (uint32_t word = 0; word < WideObjectMask::numberOfWords; ++word) {
        forEachObjectIn(objects.words[word], [&](uint32_t object) {
            functor(64 * word + object);
        });
    }
}

uint32_t firstObjectIn(uint64_t objects) { return ctz(objects); }
uint32_t firstObjectIn(const WideObjectMask& objects)
{
    for (uint32_t word = 0; word < WideObjectMask::numberOfWords; ++word) {
        if (objects.words[word])
            return 64 * word + ctz(objects.words[word]);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

bool isOneObject(uint64_t objects) { return hasOneBitSet(objects); }
bool isOneObject(const WideObjectMask& objects)
{
    uint32_t count = 0;
    for (uint64_t word : objects.words)
        count += std::popcount(word);
    return count == 1;
}

template<typename ObjectMask>
struct ScreenedObjectsIn {
    ObjectMask unbuilt { 0 };
    ObjectMask perhapsBuilt { 0 };
    ObjectMask built { 0 };
    ObjectMask consumed { 0 };
};

enum class ScreenedFrequencies : bool { AreIgnored, AreCompared };

template<typename ObjectMask>
class AllocationScreening {
    WTF_MAKE_NONCOPYABLE(AllocationScreening);
public:
    using ScreenedObjects = ScreenedObjectsIn<ObjectMask>;

    AllocationScreening(AllocationSinking&, std::span<const Candidate>, std::span<const DescribedObject>, Vector<VirtualObjectID>& objectOfValue, ScreenedFrequencies);
    ~AllocationScreening();

    static constexpr uint32_t maxNumberOfObjects = numberOfObjectsIn<ObjectMask>;

    ScreeningResult run();
    // Bit k: the allocation with that index, or something that it is held by, holds or meets. For the others nothing is gained whatever becomes of these.
    BitVector allocationsThatMayPay();
    // For each allocation a number for all that was seen of it and of what it holds, is held by or meets, in the order of the blocks. It names no value and no block:
    // while the number stays, the function still does the same with these objects, whatever has changed around them.
    Vector<uint64_t> tracesOfAllocations();
    // The first use in the order of the blocks that makes the allocation necessary. Found for whoever collects the reasons why allocations stay.
    std::optional<Use> firstBuilderOf(uint32_t allocation) const { return m_objects[allocation].firstBuilder; }

private:
    struct Object {
        BlockID block;
        ObjectMask held { 0 };
        uint32_t group { 0 };
        BlockFrequency frequency { BlockFrequency::Normal };
        Vector<ValueID, 2> names;
        uint64_t trace { 0 };
        ObjectMask sources { 0 };
        std::optional<Use> firstBuilder;
    };

    static constexpr ObjectMask maskOf(uint32_t object) { return maskOfObject<ObjectMask>(object); }

    static void forEachObject(const ObjectMask& objects, NOESCAPE const Invocable<void(uint32_t)> auto& functor) { forEachObjectIn(objects, functor); }

    static uint64_t mix(uint64_t trace, uint64_t seen)
    {
        trace = (trace ^ seen) * 0x9e3779b97f4a7c15ull;
        return trace ^ trace >> 29;
    }
    void note(ObjectMask mentioned, uint64_t operation, const ScreenedObjects& before, const ScreenedObjects& after)
    {
        forEachObject(mentioned, [&](uint32_t object) {
            ObjectMask mask = maskOf(object);
            uint64_t states = !!(before.unbuilt & mask) | !!(after.unbuilt & mask) << 1 | !!(after.perhapsBuilt & mask) << 2 | !!(after.consumed & mask) << 3;
            m_objects[object].trace = mix(m_objects[object].trace, operation << 8 | static_cast<uint64_t>(m_frequencyOfBlock) << 4 | states);
        });
    }

    std::optional<uint32_t> objectNamedBy(ValueID) const;
    ObjectMask maskNamedBy(ValueID) const;
    ObjectMask maskNamedByOperandsOf(const Instruction&) const;
    bool isJoin(uint32_t object) const { return !!(m_joins & maskOf(object)); }
    bool mayBeCell(uint32_t object) const { return !!(m_joinsOfCells & maskOf(object)); }
    const VirtualObjectDescription* descriptionOf(uint32_t object) const;
    void addName(ValueID, uint32_t object);
    void mayPay(ObjectMask objects) { m_objectsThatMayPay |= objects; }
    uint32_t groupOf(uint32_t object);
    void connect(uint32_t, uint32_t);
    void connectAll(ObjectMask);
    std::optional<FieldLookup> lookUp(uint32_t object, const Value& access) const;
    bool alwaysPasses(uint32_t object, Opcode cast) const;
    bool neverFolds(uint32_t object, const Value& store) const;
    bool mayBeTakenApartBy(uint32_t object, const Value& user) const;

    void visit(BlockID, uint32_t position);
    ScreenedObjects enter(BlockID, uint32_t position);
    void bindParameter(BlockID, uint32_t parameterIndex, std::span<const Edge>, std::span<ScreenedObjects> objectsOnEdges, bool isEnteredBackwards);
    void execute(ScreenedObjects&, ValueID);
    void allocate(ScreenedObjects&, ValueID, ObjectMask named);
    void executeTerminator(ScreenedObjects&, const Terminator&);
    void build(ScreenedObjects&, ObjectMask, Opcode builder);
    void build(ScreenedObjects&, ObjectMask);
    void noteBuilderOf(ObjectMask roots, ObjectMask built);
    void leave(BlockID, uint32_t position, const ScreenedObjects&, ObjectMask unbuiltSomewhere);
    void leaveToHandler(const Target& handler, ObjectMask unbuiltSomewhere);

    bool comparesFrequencies() const { return m_frequencies; }
    BlockFrequency frequencyOf(uint32_t object);
    void didReach(BlockFrequency, ObjectMask unbuilt);
    ObjectMask groupsOf(ObjectMask);
    ObjectMask groupsBuiltApartInFrame();

    const AllocationSinking& m_sinking;
    FunctionAnalyses& m_analyses;
    const VirtualObjectContext& m_context;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    std::span<const Candidate> m_candidates;
    std::span<const DescribedObject> m_described;
    Vector<VirtualObjectID>& m_objectOfValue;
    const BlockFrequencies* m_frequencies { nullptr };
    Vector<Object, 16> m_objects;
    Vector<ValueID, 16> m_addedNames;
    Vector<ScreenedObjects> m_objectsAtEnd;
    BitVector m_blocksWithAllocations;
    BitVector m_enteredBlocks;
    uint32_t m_numberOfAllocations { 0 };
    bool m_storesByNameWait { false };
    bool m_findsBuilders { false };
    Use m_user;
    ObjectMask m_named { 0 };
    BlockFrequency m_frequencyOfBlock { BlockFrequency::Normal };
    ObjectMask m_joins { 0 };
    ObjectMask m_joinsOfCells { 0 };
    ObjectMask m_objectsThatMayPay { 0 };
    ObjectMask m_objectsBuiltByCalls { 0 };
    ObjectMask m_objectsLeavingTheirBlocks { 0 };
    ObjectMask m_objectsOnRarerPaths { 0 };
};

template<typename ObjectMask>
AllocationScreening<ObjectMask>::AllocationScreening(AllocationSinking& sinking, std::span<const Candidate> candidates, std::span<const DescribedObject> described, Vector<VirtualObjectID>& objectOfValue, ScreenedFrequencies frequencies)
    : m_sinking(sinking)
    , m_analyses(sinking.analyses())
    , m_context(sinking.objectContext())
    , m_program(m_context.program)
    , m_function(m_context.function)
    , m_cfg(m_analyses.cfg())
    , m_candidates(candidates)
    , m_described(described)
    , m_objectOfValue(objectOfValue)
    , m_objectsAtEnd(m_function.blocks.size())
    , m_blocksWithAllocations(m_function.blocks.size())
    , m_enteredBlocks(m_function.blocks.size())
    , m_numberOfAllocations(candidates.size())
{
    RELEASE_ASSERT(m_numberOfAllocations <= maxNumberOfObjects);
    m_findsBuilders = sinking.context().build.options().collectsDeclines();
    if (frequencies == ScreenedFrequencies::AreCompared)
        m_frequencies = &m_analyses.blockFrequencies();
    for (uint32_t index = 0; index < m_numberOfAllocations; ++index)
        m_objects.append(Object { candidates[index].block, 0, index, BlockFrequency::Normal, { candidates[index].definition } });
    for (const Candidate& candidate : candidates)
        m_blocksWithAllocations.quickSet(candidate.block.index());
}

template<typename ObjectMask>
AllocationScreening<ObjectMask>::~AllocationScreening()
{
    for (ValueID name : m_addedNames)
        m_objectOfValue[name.index()] = { };
}

template<typename ObjectMask>
std::optional<uint32_t> AllocationScreening<ObjectMask>::objectNamedBy(ValueID value) const
{
    VirtualObjectID object = m_objectOfValue[value.index()];
    if (!object)
        return std::nullopt;
    return object.index();
}

template<typename ObjectMask>
ObjectMask AllocationScreening<ObjectMask>::maskNamedBy(ValueID value) const
{
    std::optional<uint32_t> object = objectNamedBy(value);
    return object ? maskOf(*object) : ObjectMask(0);
}

template<typename ObjectMask>
ObjectMask AllocationScreening<ObjectMask>::maskNamedByOperandsOf(const Instruction& instruction) const
{
    ObjectMask named = 0;
    instruction.forEachOperand([&](ValueID operand, uint32_t) {
        named |= maskNamedBy(operand);
    });
    return named;
}

template<typename ObjectMask>
const VirtualObjectDescription* AllocationScreening<ObjectMask>::descriptionOf(uint32_t object) const
{
    return object < m_numberOfAllocations ? &m_described[object].description : nullptr;
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::addName(ValueID value, uint32_t object)
{
    m_objectOfValue[value.index()] = VirtualObjectID(object);
    m_addedNames.append(value);
    m_objects[object].names.append(value);
}

template<typename ObjectMask>
uint32_t AllocationScreening<ObjectMask>::groupOf(uint32_t object)
{
    while (m_objects[object].group != object) {
        m_objects[object].group = m_objects[m_objects[object].group].group;
        object = m_objects[object].group;
    }
    return object;
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::connect(uint32_t a, uint32_t b)
{
    m_objects[groupOf(a)].group = groupOf(b);
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::connectAll(ObjectMask objects)
{
    if (!objects || isOneObject(objects))
        return;
    uint32_t first = firstObjectIn(objects);
    forEachObject(objects, [&](uint32_t object) {
        connect(first, object);
    });
}

template<typename ObjectMask>
std::optional<FieldLookup> AllocationScreening<ObjectMask>::lookUp(uint32_t object, const Value& access) const
{
    const VirtualObjectDescription* description = descriptionOf(object);
    if (!description)
        return std::nullopt;
    return lookUpField(m_context, *description, access).lookup;
}

template<typename ObjectMask>
bool AllocationScreening<ObjectMask>::alwaysPasses(uint32_t object, Opcode cast) const
{
    constexpr OpcodeSet castsOfEveryObject { Opcode::FinishObject, Opcode::Upcast, Opcode::NarrowByField, Opcode::AssumeType, Opcode::CheckInitialized, Opcode::RequireObjectCoercible };

    const VirtualObjectDescription* description = descriptionOf(object);
    return description && !description->absentResult && castsOfEveryObject.contains(cast);
}

template<typename ObjectMask>
bool AllocationScreening<ObjectMask>::neverFolds(uint32_t object, const Value& store) const
{
    constexpr EnumSet<VirtualObjectKind> writableKinds { VirtualObjectKind::Object, VirtualObjectKind::Environment, VirtualObjectKind::Array, VirtualObjectKind::Arguments };

    if (mayBeCell(object))
        return true;
    const VirtualObjectDescription* description = descriptionOf(object);
    if (!description || store.opcode == Opcode::AppendElement || store.opcode == Opcode::AppendHole)
        return false;
    VirtualFieldAccess access = lookUpField(m_context, *description, store);
    if (access.lookup != FieldLookup::Field || !writableKinds.contains(description->kind))
        return true;
    if (description->kind == VirtualObjectKind::Arguments)
        return false;
    if (access.whenPresent != PresentField::IsOwnData && access.whenAbsent != AbsentField::FindsNothing)
        return true;
    TypeID storedType = m_function.at(store.valueOperand()).type;
    TypeID fieldType = typeOfField(m_context, *description, access.field);
    if (storedType == fieldType || m_program.at(storedType).isSubsetOf(m_program.at(fieldType), m_program))
        return false;
    return store.opcode != Opcode::PropSet || m_storesByNameWait || !canHoldUnprovenValue(m_program, *description, access.field);
}

template<typename ObjectMask>
bool AllocationScreening<ObjectMask>::mayBeTakenApartBy(uint32_t object, const Value& user) const
{
    if (mayBeCell(object))
        return false;
    const VirtualObjectDescription* description = descriptionOf(object);
    if (!description || description->kind == VirtualObjectKind::Arguments)
        return true;
    return description->kind == VirtualObjectKind::Array && user.opcode == Opcode::CallBuiltin && user.builtinCallForm() == BuiltinCallForm::List;
}

static uint32_t indexOfOperandTakenApart(Opcode listUse)
{
    switch (listUse) {
    case Opcode::AppendSpread:
        return OperandIndex::AppendSpread::iterable;
    case Opcode::AppendElements:
        return OperandIndex::AppendElements::source;
    case Opcode::CopyDataProperties:
        return OperandIndex::CopyDataProperties::source;
    case Opcode::CallList:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        return OperandIndex::CallList::list;
    case Opcode::DestructureIterable:
        return OperandIndex::DestructureIterable::iterable;
    case Opcode::ResultAt:
        return OperandIndex::ResultAt::results;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

template<typename ObjectMask>
BlockFrequency AllocationScreening<ObjectMask>::frequencyOf(uint32_t object)
{
    return isJoin(object) ? m_objects[object].frequency : m_frequencies->classOf(m_objects[object].block);
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::didReach(BlockFrequency frequency, ObjectMask unbuilt)
{
    if (frequency == BlockFrequency::Normal)
        return;
    forEachObject(unbuilt & ~m_objectsOnRarerPaths, [&](uint32_t object) {
        if (isRarerThan(frequency, frequencyOf(object)))
            m_objectsOnRarerPaths |= maskOf(object);
    });
}

template<typename ObjectMask>
ObjectMask AllocationScreening<ObjectMask>::groupsOf(ObjectMask objects)
{
    ObjectMask groups = 0;
    forEachObject(objects, [&](uint32_t object) {
        groups |= maskOf(groupOf(object));
    });
    return groups;
}

template<typename ObjectMask>
ObjectMask AllocationScreening<ObjectMask>::groupsBuiltApartInFrame()
{
    ObjectMask groupsBuiltApart = groupsOf(m_objectsBuiltByCalls) & groupsOf(comparesFrequencies() ? m_objectsOnRarerPaths : m_objectsLeavingTheirBlocks);
    if (!groupsBuiltApart)
        return 0;
    ObjectMask groups = 0;
    for (uint32_t object = 0; object < m_numberOfAllocations; ++object) {
        ObjectMask group = maskOf(groupOf(object));
        if ((groupsBuiltApart & group & ~groups) && isFrameAllocatable(m_sinking, m_candidates[object]))
            groups |= group;
    }
    return groups;
}

template<typename ObjectMask>
ScreeningResult AllocationScreening<ObjectMask>::run()
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    for (uint32_t position = m_cfg.reversePostOrderIndex(m_objects[0].block); position < order.size(); ++position) {
        BlockID block = order[position];
        if (m_enteredBlocks.quickGet(block.index()) || m_blocksWithAllocations.quickGet(block.index()))
            visit(block, position);
    }

    if (m_objectsThatMayPay)
        return ScreeningResult::SomeAllocationMayPay;
    if (!groupsBuiltApartInFrame())
        return ScreeningResult::NoAllocationPays;
    return comparesFrequencies() ? ScreeningResult::SomeAllocationMayPay : ScreeningResult::DependsOnFrequencies;
}

template<typename ObjectMask>
BitVector AllocationScreening<ObjectMask>::allocationsThatMayPay()
{
    ObjectMask groups = groupsOf(m_objectsThatMayPay) | groupsBuiltApartInFrame();
    BitVector allocations(m_numberOfAllocations);
    for (uint32_t object = 0; object < m_numberOfAllocations; ++object) {
        if (groups & maskOf(groupOf(object)))
            allocations.quickSet(object);
    }
    return allocations;
}

template<typename ObjectMask>
Vector<uint64_t> AllocationScreening<ObjectMask>::tracesOfAllocations()
{
    Vector<uint64_t, 64> traceOfGroup(FillWith { }, m_objects.size(), uint64_t { 0 });
    for (uint32_t object = 0; object < m_objects.size(); ++object) {
        ObjectMask mask = maskOf(object);
        uint64_t marks = !!(m_objectsThatMayPay & mask) | !!(m_joinsOfCells & mask) << 1 | !!(m_objectsBuiltByCalls & mask) << 2 | !!(m_objectsLeavingTheirBlocks & mask) << 3 | !!(m_joins & mask) << 4;
        traceOfGroup[groupOf(object)] += mix(m_objects[object].trace, marks);
    }
    return Vector<uint64_t>(m_numberOfAllocations, [&](size_t object) {
        return mix(traceOfGroup[groupOf(object)], 1);
    });
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::visit(BlockID id, uint32_t position)
{
    const Block& block = m_function.at(id);
    ScreenedObjects objects;
    if (m_enteredBlocks.quickGet(id.index()))
        objects = enter(id, position);

    m_storesByNameWait = m_sinking.storesByNameWaitIn(id);
    m_frequencyOfBlock = block.frequency;
    ObjectMask unbuiltSomewhere = objects.unbuilt;
    if (objects.unbuilt || objects.consumed || m_blocksWithAllocations.quickGet(id.index())) {
        for (ValueID value : block.values) {
            ScreenedObjects before = objects;
            m_named = 0;
            m_user = Use { .kind = Use::Kind::Operand, .block = id, .user = value };
            execute(objects, value);
            unbuiltSomewhere |= objects.unbuilt;
            constexpr uint64_t throwsUnderHandler = 1ull << 32;
            if (m_named)
                note(m_named, static_cast<uint64_t>(m_function.at(value).opcode), before, objects);
            if (block.handler && objects.unbuilt && mayThrow(m_function.at(value).opcode))
                note(objects.unbuilt, throwsUnderHandler, before, objects);
        }
    }
    ScreenedObjects before = objects;
    ObjectMask named = maskNamedByOperandsOf(block.terminator);
    m_user = Use { .kind = Use::Kind::TerminatorOperand, .block = id };
    executeTerminator(objects, block.terminator);
    constexpr uint64_t isTerminator = 1ull << 33;
    constexpr uint64_t leavesFunction = 1ull << 34;
    if (named)
        note(named, isTerminator | static_cast<uint64_t>(block.terminator.kind), before, objects);
    if (block.terminator.targets.isEmpty() && before.unbuilt)
        note(before.unbuilt, leavesFunction, before, objects);
    m_objectsAtEnd[id.index()] = objects;
    leave(id, position, objects, unbuiltSomewhere);
}

template<typename ObjectMask>
typename AllocationScreening<ObjectMask>::ScreenedObjects AllocationScreening<ObjectMask>::enter(BlockID id, uint32_t position)
{
    const Block& block = m_function.at(id);
    std::span<const Edge> edges = m_cfg.incomingEdges(id);

    bool isEnteredBackwards = false;
    Vector<ScreenedObjects, 4> objectsOnEdges;
    for (Edge edge : edges) {
        ScreenedObjects objects;
        if (m_cfg.reversePostOrderIndex(edge.from) >= position)
            isEnteredBackwards = true;
        else
            objects = m_objectsAtEnd[edge.from.index()];
        if (edge.isHandlerEdge())
            objects.unbuilt = 0;
        objects.perhapsBuilt &= objects.unbuilt;
        objectsOnEdges.append(objects);
    }

    for (uint32_t parameterIndex = 0; parameterIndex < block.parameters.size(); ++parameterIndex)
        bindParameter(id, parameterIndex, edges, objectsOnEdges.mutableSpan(), isEnteredBackwards);

    ScreenedObjects result;
    for (const ScreenedObjects& objects : objectsOnEdges) {
        result.unbuilt |= objects.unbuilt;
        result.perhapsBuilt |= objects.perhapsBuilt;
        result.built |= objects.built;
        result.consumed |= objects.consumed;
    }
    mayPay(result.unbuilt & (result.built | result.consumed) & ~m_joins);
    if (edges.size() > 1)
        mayPay(result.unbuilt & result.perhapsBuilt & ~m_joins);
    if (isEnteredBackwards)
        mayPay(result.unbuilt);
    if (block.subject)
        mayPay(maskNamedBy(block.subject));
    if (comparesFrequencies())
        didReach(m_frequencies->classOf(id), result.unbuilt);
    return result;
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::bindParameter(BlockID id, uint32_t parameterIndex, std::span<const Edge> edges, std::span<ScreenedObjects> objectsOnEdges, bool isEnteredBackwards)
{
    const Block& block = m_function.at(id);
    ValueID parameter = block.parameters[parameterIndex];

    ObjectMask sources = 0;
    bool everyEdgePassesAnObject = true;
    for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        ValueID incoming = valuePassedOn(m_function, edges[edgeIndex], parameterIndex);
        if (incoming == parameter)
            continue;
        ObjectMask source = incoming ? maskNamedBy(incoming) : ObjectMask(0);
        mayPay(source & objectsOnEdges[edgeIndex].consumed);
        sources |= source;
        everyEdgePassesAnObject &= !!source;
    }
    if (!sources)
        return;

    bool isAlias = everyEdgePassesAnObject && isOneObject(sources);
    bool isGuard = m_function.at(parameter).type == TypeTable::guard;
    bool isUnderstood = !isEnteredBackwards && parameter != block.subject && (isAlias || (!isGuard && m_objects.size() < maxNumberOfObjects));
    if (!isUnderstood) {
        mayPay(sources);
        connectAll(sources);
    }
    if (isAlias || !isUnderstood) {
        addName(parameter, firstObjectIn(sources));
        return;
    }

    uint32_t join = m_objects.size();
    BlockFrequency frequency = BlockFrequency::Normal;
    if (comparesFrequencies()) {
        frequency = m_frequencies->classOf(id);
        forEachObject(sources, [&](uint32_t source) {
            frequency = moreFrequentOf(frequency, frequencyOf(source));
        });
    }
    m_objects.append(Object { id, 0, join, frequency, { } });
    m_joins |= maskOf(join);
    addName(parameter, join);
    connectAll(sources | maskOf(join));

    for (size_t edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
        ScreenedObjects& objects = objectsOnEdges[edgeIndex];
        ValueID incoming = valuePassedOn(m_function, edges[edgeIndex], parameterIndex);
        ObjectMask source = incoming ? maskNamedBy(incoming) : ObjectMask(0);
        if (!(source & objects.unbuilt)) {
            objects.built |= maskOf(join);
            if (!incoming || !virtualConstantOf(m_program, m_function, incoming))
                m_joinsOfCells |= maskOf(join);
            continue;
        }
        if (source & m_joinsOfCells)
            m_joinsOfCells |= maskOf(join);
        objects.unbuilt &= ~source;
        objects.perhapsBuilt &= ~source;
        objects.consumed |= source;
        m_objects[join].sources |= source;
        mayPay(m_objects[firstObjectIn(source)].held & objects.unbuilt);
        objects.unbuilt |= maskOf(join);
    }
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::build(ScreenedObjects& objects, ObjectMask roots)
{
    ObjectMask members = roots;
    forEachObject(roots, [&](uint32_t root) {
        members |= m_objects[root].held;
    });
    members &= objects.unbuilt;
    objects.unbuilt &= ~members;
    objects.perhapsBuilt &= ~members;
    objects.built |= members;
    if (m_findsBuilders && members)
        noteBuilderOf(roots, members);
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::noteBuilderOf(ObjectMask roots, ObjectMask built)
{
    const Instruction& user = m_user.kind == Use::Kind::Operand ? static_cast<const Instruction&>(m_function.at(m_user.user)) : m_function.at(m_user.block).terminator;
    std::optional<uint32_t> operandIndex;
    user.forEachOperand([&](ValueID operand, uint32_t index) {
        if (!operandIndex && operand && (maskNamedBy(operand) & roots))
            operandIndex = index;
    });
    m_user.operandIndex = operandIndex.value_or(0);

    ObjectMask seen = 0;
    while (ObjectMask next = built & ~seen) {
        seen |= next;
        forEachObject(next, [&](uint32_t object) {
            if (!m_objects[object].firstBuilder)
                m_objects[object].firstBuilder = m_user;
            built |= m_objects[object].sources;
        });
    }
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::build(ScreenedObjects& objects, ObjectMask roots, Opcode builder)
{
    ObjectMask unbuiltBefore = objects.unbuilt;
    build(objects, roots);
    if (effectRuleOf(builder) == EffectRule::Call)
        m_objectsBuiltByCalls |= unbuiltBefore & ~objects.unbuilt;
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::allocate(ScreenedObjects& objects, ValueID id, ObjectMask named)
{
    uint32_t object = *objectNamedBy(id);
    const VirtualObjectDescription& description = m_described[object].description;
    ObjectMask mentioned = named & objects.unbuilt;
    if (description.staysInPlace) {
        if (description.isDead)
            mayPay(mentioned);
        else
            build(objects, mentioned, m_function.at(id).opcode);
    } else if (named) {
        ObjectMask listed = 0;
        forEachInitialField(m_context, description, [&](const VirtualField&, ValueID value) {
            listed |= maskNamedBy(value);
        });
        mayPay(mentioned & ~listed);
        connectAll(named | maskOf(object));
        forEachObject(listed & mentioned, [&](uint32_t held) {
            m_objects[object].held |= maskOf(held) | m_objects[held].held;
        });
    }
    objects.unbuilt |= maskOf(object);
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::execute(ScreenedObjects& objects, ValueID id)
{
    const Value& value = m_function.at(id);
    bool isAllocation = interpretedOf(value.opcode) == Interpretation::Allocation && m_objectOfValue[id.index()];
    if (!isAllocation && !objects.unbuilt && !objects.consumed)
        return;

    ObjectMask named = maskNamedByOperandsOf(value);
    mayPay(named & ~(objects.unbuilt | objects.built));
    m_named = named | (isAllocation ? maskNamedBy(id) : ObjectMask(0));
    if (isAllocation) {
        allocate(objects, id, named);
        return;
    }
    ObjectMask mentioned = named & objects.unbuilt;
    if (!mentioned)
        return;

    auto leaveUnbuilt = [&] {
        mayPay(mentioned);
        connectAll(named);
    };
    auto unbuiltObjectAt = [&](uint32_t operandIndex) -> std::optional<uint32_t> {
        ValueID operand = value.operand(operandIndex);
        std::optional<uint32_t> object = operand ? objectNamedBy(operand) : std::nullopt;
        return object && (objects.unbuilt & maskOf(*object)) ? object : std::nullopt;
    };

    switch (interpretationOf(m_program, value)) {
    case Interpretation::Cast: {
        std::optional<uint32_t> object = unbuiltObjectAt(0);
        if (!object || mayBeCell(*object)) {
            build(objects, mentioned, value.opcode);
            return;
        }
        if (named != maskOf(*object))
            leaveUnbuilt();
        if (!alwaysPasses(*object, value.opcode))
            objects.perhapsBuilt |= maskOf(*object);
        addName(id, *object);
        return;
    }
    case Interpretation::Guards: {
        bool everyOperandIsAName = true;
        value.forEachOperand([&](ValueID operand, uint32_t) {
            everyOperandIsAName &= !!maskNamedBy(operand);
        });
        if (everyOperandIsAName && isOneObject(named))
            addName(id, firstObjectIn(named));
        else
            build(objects, mentioned, value.opcode);
        return;
    }
    case Interpretation::Read: {
        std::optional<uint32_t> object = unbuiltObjectAt(0);
        if (!object || mayBeCell(*object) || lookUp(*object, value) == FieldLookup::Unknown) {
            build(objects, mentioned, value.opcode);
            return;
        }
        if (named != maskOf(*object))
            leaveUnbuilt();
        mayPay(m_objects[*object].held);
        objects.perhapsBuilt |= maskOf(*object);
        return;
    }
    case Interpretation::Store: {
        std::optional<uint32_t> object = unbuiltObjectAt(0);
        if (!object || neverFolds(*object, value))
            build(objects, mentioned, value.opcode);
        else
            leaveUnbuilt();
        return;
    }
    case Interpretation::Test:
        if (testsLeftToRun.contains(value.opcode) || !(mentioned & ~m_joinsOfCells))
            build(objects, mentioned, value.opcode);
        else
            leaveUnbuilt();
        return;
    case Interpretation::List: {
        std::optional<uint32_t> object = unbuiltObjectAt(indexOfOperandTakenApart(value.opcode));
        if (!object || mayBeCell(*object))
            build(objects, mentioned, value.opcode);
        else
            leaveUnbuilt();
        return;
    }
    case Interpretation::Allocation: {
        bool mayTakeObjectApart = false;
        forEachObject(mentioned, [&](uint32_t object) {
            mayTakeObjectApart |= mayBeTakenApartBy(object, value);
        });
        if (mayTakeObjectApart)
            leaveUnbuilt();
        else
            build(objects, mentioned, value.opcode);
        return;
    }
    case Interpretation::None:
        if (familyOf(value.opcode) == OperationFamily::RuntimeValidation)
            leaveUnbuilt();
        else
            build(objects, mentioned, value.opcode);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::executeTerminator(ScreenedObjects& objects, const Terminator& terminator)
{
    if (!objects.unbuilt && !objects.consumed)
        return;

    ObjectMask named = maskNamedByOperandsOf(terminator);
    mayPay(named & objects.consumed);
    ObjectMask mentioned = named & objects.unbuilt;
    switch (terminator.kind) {
    case TerminatorKind::Jump:
    case TerminatorKind::Unreachable:
    case TerminatorKind::TestAndBranch:
    case TerminatorKind::SwitchOnShape:
        mayPay(mentioned);
        break;
    case TerminatorKind::Return:
    case TerminatorKind::Branch:
    case TerminatorKind::Throw:
    case TerminatorKind::Rethrow:
    case TerminatorKind::Switch:
    case TerminatorKind::SwitchString:
    case TerminatorKind::ReturnCall:
    case TerminatorKind::TryCallInline:
        build(objects, mentioned);
        break;
    }
    if (terminator.targets.isEmpty())
        mayPay(objects.unbuilt);
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::leaveToHandler(const Target& handler, ObjectMask unbuiltSomewhere)
{
    forEachObject(unbuiltSomewhere & ~m_objectsThatMayPay, [&](uint32_t object) {
        bool isLive = m_objects[object].names.containsIf([&](ValueID name) {
            return handler.arguments.contains(name) || m_sinking.isLiveAtHead(name, handler.block);
        });
        if (isLive)
            mayPay(maskOf(object));
    });
}

template<typename ObjectMask>
void AllocationScreening<ObjectMask>::leave(BlockID id, uint32_t position, const ScreenedObjects& objects, ObjectMask unbuiltSomewhere)
{
    const Block& block = m_function.at(id);
    if (block.handler && unbuiltSomewhere)
        leaveToHandler(*block.handler, unbuiltSomewhere);
    if (!objects.unbuilt && !objects.built && !objects.consumed)
        return;

    forEachOutgoingEdge(m_function, id, [&](Edge edge, const Target& target) {
        if (m_cfg.reversePostOrderIndex(target.block) <= position) {
            mayPay(objects.unbuilt);
            return;
        }
        m_enteredBlocks.quickSet(target.block.index());
        if (edge.isHandlerEdge())
            return;
        m_objectsLeavingTheirBlocks |= objects.unbuilt;
        if (comparesFrequencies())
            didReach(rarerOf(m_frequencies->classOf(id), m_frequencies->classOf(edge)), objects.unbuilt);
    });
}

} // anonymous namespace

static bool namesField(const Program& program, const Value& value)
{
    switch (interpretationOf(program, value)) {
    case Interpretation::Read:
    case Interpretation::Store:
    case Interpretation::Test:
        return true;
    case Interpretation::None:
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::List:
    case Interpretation::Guards:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Where allocations of one kind meet, each access behind the meeting is a use of every one of them, and it names the same field of each.
struct AccessesLookedUp {
    VirtualFamilyID family;
    VirtualObjectKind kind;
    LayoutID layout;
    TypeID type;
    BitVector users;
};

static void ensureFieldsInUse(const VirtualObjectContext& context, const EscapeAnalysis& escape, const VirtualObjectDescription& description, VirtualFamily& family, uint64_t maxFields, BitVector& usersLookedUp)
{
    auto hasTooManyFields = [&] {
        return family.numberOfFields() > maxFields;
    };

    forEachInitialField(context, description, [&](const VirtualField& field, ValueID) {
        if (!hasTooManyFields())
            family.ensureField(field);
    });

    std::optional<ObjectID> object = escape.objectDefinedBy(description.allocation);
    if (!object)
        return;
    for (const ObjectUse& objectUse : escape.usesOf(*object)) {
        if (hasTooManyFields())
            return;
        if (objectUse.use.kind != Use::Kind::Operand)
            continue;
        const Value& user = context.function.at(objectUse.use.user);
        if (user.opcode == Opcode::DestructureIterable && description.kind == VirtualObjectKind::BuiltinPrefix) {
            for (uint32_t position = 0; position < user.count() && !hasTooManyFields(); ++position)
                family.ensureField(VirtualField::element(position));
            continue;
        }
        if (!namesField(context.program, user) || usersLookedUp.set(objectUse.use.user.index()))
            continue;
        VirtualFieldAccess access = lookUpField(context, description, user);
        if (access.lookup == FieldLookup::Field)
            family.ensureField(access.field);
    }
}

// One value carries a field for every variant of a join. After a test has left one variant, that value is read, stored and
// passed on at the type the variant's own description gives the field, and no cast states what the tag proves.
static bool givesOneFieldSeveralTypes(const VirtualObjectContext& context, const VirtualFamily& family)
{
    std::span<const VirtualObjectDescription> descriptions = family.descriptions();
    if (descriptions.size() < 2)
        return false;

    for (uint32_t field = 0; field < family.numberOfFields(); ++field) {
        TypeID commonType;
        for (const VirtualObjectDescription& description : descriptions) {
            TypeID type = typeOfField(context, description, family.fieldAt(field));
            if (type == TypeTable::never)
                continue;
            if (commonType && commonType != type)
                return true;
            commonType = type;
        }
    }
    return false;
}

bool AllocationSinking::collectCandidates()
{
    Vector<DescribedObject> described = collectAllocations();
    if (described.isEmpty())
        return false;
    constexpr OpcodeSet operationsOnSlots { Opcode::IsSlotFilled, Opcode::HasBirthFieldsFilled, Opcode::HasNoFieldOutsideSlot, Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::PutSlot, Opcode::PutSlotOrAdd };
    m_inputs.hasOperationsOnSlots = m_analyses.opcodesOfValues().intersects(operationsOnSlots);
    auto isLargeForItsWork = [&] {
        return uint64_t { m_candidates.size() } * m_function.blocks.size() > m_maxWork / expectedNumberOfSweeps;
    };
    m_inputs.isLargeForItsWork = isLargeForItsWork();
    if (m_policy.leavesWhenNoAllocationPays && findsThatNoAllocationPays(described.span())) {
        m_foundThatNoAllocationPays = true;
        return false;
    }

    collectJoins();
    m_inputs.isLargeForItsWork = isLargeForItsWork();
    Vector<uint32_t> rootOfCandidate;
    uniteFamilies(rootOfCandidate);
    createFamilies(described.span(), rootOfCandidate.span());
    if (m_policy.leavesWhenNoAllocationPays) {
        abandonAllocationsBuiltAtOnce();
        if (!hasCandidates()) {
            m_foundThatNoAllocationPays = true;
            return false;
        }
    }
    findNames();
    computeInputs();
    return true;
}

static constexpr OpcodeSet opcodesOfNames = [] {
    OpcodeSet opcodes;
    for (Opcode opcode : allMembersOf<Opcode>) {
        switch (interpretedOf(opcode)) {
        case Interpretation::Cast:
        case Interpretation::Guards:
        case Interpretation::Read:
        case Interpretation::List:
        case Interpretation::Test:
            opcodes.add(opcode);
            break;
        case Interpretation::None:
        case Interpretation::Allocation:
        case Interpretation::Store:
            // What a branch on it hands to its targets is about its operands.
            if (provesOf(opcode) != Proof::None || branchesOf(opcode) != Branches::No)
                opcodes.add(opcode);
            break;
        }
    }
    return opcodes;
}();

void AllocationSinking::findNames()
{
    const UseLists& useLists = m_analyses.useLists();
    m_names.names.ensureSize(m_function.values.size());
    m_names.valuesMentioningNames.ensureSize(m_function.values.size());
    m_names.terminatorsMentioningNames.ensureSize(m_function.blocks.size());
    m_names.blocksBindingNames.ensureSize(m_function.blocks.size());

    Vector<ValueID, 32> worklist;
    auto addName = [&](ValueID value) {
        if (!m_names.names.quickSet(value.index()))
            worklist.append(value);
    };
    auto addResultsOfEdgesFrom = [&](BlockID block) {
        if (m_names.terminatorsMentioningNames.quickSet(block.index()))
            return;
        const Terminator& terminator = m_function.at(block).terminator;
        for (const Target& target : terminator.targets) {
            const CompactList<ValueID>& parameters = m_function.at(target.block).parameters;
            for (uint32_t index = target.arguments.size(); index < parameters.size(); ++index)
                addName(parameters[index]);
        }
    };

    // Whoever looks for a live range to duplicate takes up again what was given up.
    for (const Candidate& candidate : m_candidates) {
        if (!candidate.isLeftOut)
            addName(candidate.definition);
    }
    while (!worklist.isEmpty()) {
        ValueID name = worklist.takeLast();
        ValueLocation location = useLists.locationOf(name);
        if (location.placement == ValuePlacement::Parameter)
            m_names.blocksBindingNames.quickSet(location.block.index());
        for (const Use& use : useLists.usesOf(name)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                m_names.valuesMentioningNames.quickSet(use.user.index());
                if (opcodesOfNames.contains(m_function.at(use.user).opcode))
                    addName(use.user);
                break;
            case Use::Kind::TerminatorOperand:
                addResultsOfEdgesFrom(use.block);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument:
                addName(m_function.at(targetOf(m_function, use.edge()).block).parameters[use.operandIndex]);
                break;
            case Use::Kind::BlockSubject:
                m_names.blocksBindingNames.quickSet(use.block.index());
                break;
            }
        }
    }
}

bool AllocationSinking::mentionsName(const Terminator& terminator, BlockID block) const
{
    // Whoever lets returned objects stay virtual hears of every return.
    return m_names.terminatorsMentioningNames.quickGet(block.index()) || m_buildsSomeObjectBeforeBranches || terminator.kind == TerminatorKind::Return;
}

const BitVector& AllocationSinking::loadableFieldsOf(VirtualObjectID object)
{
    if (m_loadableFields.size() < m_candidates.size())
        m_loadableFields.grow(m_candidates.size());
    LoadableFields& loadable = m_loadableFields[object.index()];
    const VirtualFamily& family = familyOf(object);
    if (loadable.numberOfFieldsLookedAt < family.numberOfFields()) {
        const VirtualObjectDescription& description = descriptionOf(object);
        for (uint32_t field = loadable.numberOfFieldsLookedAt; field < family.numberOfFields(); ++field) {
            if (canLoadSpeculatively(m_objectContext, description, family.fieldAt(field)) == SpeculativeLoad::Safe)
                loadable.fields.set(field);
        }
        loadable.numberOfFieldsLookedAt = family.numberOfFields();
    }
    return loadable.fields;
}

void AllocationSinking::findWhetherObjectsAreBuiltBeforeBranches()
{
    m_buildsSomeObjectBeforeBranches = std::ranges::any_of(m_candidates, &Candidate::isBuiltBeforeBranches);
}

Vector<DescribedObject> AllocationSinking::collectAllocations()
{
    Vector<DescribedObject> result;
    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        for (ValueID id : m_function.at(block).values) {
            if (interpretedOf(m_function.at(id).opcode) != Interpretation::Allocation)
                continue;

            DescribedObject described = describeVirtualObject(m_analyses, id);
            switch (described.support) {
            case VirtualObjectSupport::ExceedsLimit:
                if (m_policy.recordsLosses)
                    context().build.gaveUp(described.limit, m_analyses.subject(id));
                continue;
            case VirtualObjectSupport::Unknown:
            case VirtualObjectSupport::NotAnAllocation:
            case VirtualObjectSupport::NotFresh:
            case VirtualObjectSupport::HasEffectsAtAllocation:
            case VirtualObjectSupport::HasChosenStorage:
            case VirtualObjectSupport::HasNoPartsForm:
                continue;
            case VirtualObjectSupport::Supported:
                break;
            }

            m_candidateOfValue[id.index()] = VirtualObjectID(m_candidates.size());
            m_candidates.append(Candidate { .definition = id, .block = block });
            result.append(WTF::move(described));
        }
    }
    return result;
}

template<typename ObjectMask>
ScreeningResult AllocationSinking::screen(std::span<const DescribedObject> described, BitVector& allocationsThatMayPay)
{
    auto screen = [&](ScreenedFrequencies frequencies) {
        AllocationScreening<ObjectMask> screening(*this, m_candidates.span(), described, m_candidateOfValue, frequencies);
        ScreeningResult result = screening.run();
        allocationsThatMayPay = screening.allocationsThatMayPay();
        m_tracesOfAllocations = screening.tracesOfAllocations();
        for (uint32_t index = 0; index < m_candidates.size(); ++index) {
            if (std::optional<Use> builder = screening.firstBuilderOf(index))
                m_candidates[index].firstCause = describeCause(m_analyses, *builder);
        }
        return result;
    };
    ScreeningResult result = screen(ScreenedFrequencies::AreIgnored);
    if (result == ScreeningResult::DependsOnFrequencies)
        result = screen(ScreenedFrequencies::AreCompared);
    return result;
}

bool AllocationSinking::findsThatNoAllocationPays(std::span<const DescribedObject> described)
{
    RELEASE_ASSERT(m_policy.returnedObjects == ReturnedObjects::AreBuilt);
    if (described.size() > numberOfObjectsIn<WideObjectMask>)
        return false;

    BitVector allocationsThatMayPay;
    ScreeningResult result = described.size() > numberOfObjectsIn<uint64_t> ? screen<WideObjectMask>(described, allocationsThatMayPay) : screen<uint64_t>(described, allocationsThatMayPay);
    if (result == ScreeningResult::NoAllocationPays) {
        for (Candidate& candidate : m_candidates)
            candidate.isAbandoned = true;
        return true;
    }

    bool someAllocationIsLeft = false;
    for (uint32_t index = 0; index < m_candidates.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        candidate.wasFruitlessBefore = m_policy.tracesOfFruitlessAllocations && std::ranges::binary_search(*m_policy.tracesOfFruitlessAllocations, m_tracesOfAllocations[index]);
        candidate.isLeftOut = candidate.wasFruitlessBefore || !allocationsThatMayPay.quickGet(index);
        candidate.isAbandoned = candidate.isLeftOut;
        someAllocationIsLeft |= !candidate.isAbandoned;
    }
    return !someAllocationIsLeft;
}

bool AllocationSinking::isBuiltAtOnce(const Candidate& allocation) const
{
    if (m_function.at(allocation.block).handler)
        return false;
    const UseLists& useLists = m_analyses.useLists();
    std::optional<Use> firstUse;
    uint32_t indexOfFirstUser = 0;
    for (const Use& use : useLists.usesOf(allocation.definition)) {
        if (use.kind != Use::Kind::Operand || use.block != allocation.block)
            continue;
        uint32_t index = useLists.locationOf(use.user).index;
        if (firstUse && indexOfFirstUser <= index)
            continue;
        firstUse = use;
        indexOfFirstUser = index;
    }
    if (!firstUse)
        return false;
    const Value& user = m_function.at(firstUse->user);
    switch (interpretationOf(m_program, user)) {
    case Interpretation::None:
        return buildsItsOperands(m_program, user);
    case Interpretation::Read:
    case Interpretation::Store: {
        ValueID accessed = sameObjectThroughCasts(m_function, user.operand(0));
        return accessed != allocation.definition && m_function.at(accessed).opcode != Opcode::Parameter && !candidateDefinedBy(accessed);
    }
    case Interpretation::Allocation:
    case Interpretation::Cast:
    case Interpretation::Test:
    case Interpretation::List:
    case Interpretation::Guards:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AllocationSinking::abandonAllocationsBuiltAtOnce()
{
    for (Candidate& candidate : m_candidates) {
        if (!candidate.isJoin && !candidate.isAbandoned && isBuiltAtOnce(candidate))
            candidate.isAbandoned = true;
    }
}

// The groups of which nothing is left. A group with a member that is sunk or removed looks different the next time anyway.
void AllocationSinking::addTracesOfFruitlessAllocations(Vector<uint64_t>& traces) const
{
    if (m_tracesOfAllocations.isEmpty())
        return;
    Vector<uint64_t, 16> tracesWithSurvivors;
    for (uint32_t index = 0; index < m_tracesOfAllocations.size(); ++index) {
        LimitID cause = m_candidates[index].abandonedBy;
        if (!m_candidates[index].isAbandoned || cause == LimitID::MaxSinkingWork || cause == LimitID::MaxSinkingRounds)
            tracesWithSurvivors.append(m_tracesOfAllocations[index]);
    }
    size_t sizeBefore = traces.size();
    for (uint64_t trace : m_tracesOfAllocations) {
        if (!tracesWithSurvivors.contains(trace) && !std::ranges::binary_search(traces.span().first(sizeBefore), trace))
            traces.append(trace);
    }
    std::ranges::sort(traces);
    removeRepeatedElements(traces);
}

bool AllocationSinking::isJoinParameter(BlockID block, uint32_t parameterIndex) const
{
    const EscapeAnalysis& escape = m_analyses.escape();
    ValueID parameter = m_function.at(block).parameters[parameterIndex];
    // The guards of an object point to it, and none of them is the object.
    if (m_function.at(parameter).type == TypeTable::guard)
        return false;
    if (parameter == m_function.at(block).subject)
        return false;

    bool mayHoldCandidate = false;
    PointsToSet set = escape.pointsTo(parameter);
    for (ObjectID object : set.objects)
        mayHoldCandidate |= !!candidateDefinedBy(escape.at(object).definition);
    if (!mayHoldCandidate)
        return false;

    std::optional<ObjectID> aliasedObject;
    bool isAliasOfOneObject = true;
    for (Edge edge : m_analyses.cfg().incomingEdges(block)) {
        ValueID incoming = valuePassedOn(m_function, edge, parameterIndex);
        if (!incoming)
            return false;
        if (incoming == parameter)
            continue;

        std::optional<ObjectID> object = escape.singleObject(incoming);
        if (!object || (aliasedObject && aliasedObject != object))
            isAliasOfOneObject = false;
        aliasedObject = object;
    }
    return !isAliasOfOneObject;
}

void AllocationSinking::collectJoins()
{
    if (m_analyses.escape().gaveUp())
        return;

    for (BlockID block : m_analyses.cfg().reversePostOrder()) {
        if (block == entryBlock)
            continue;
        uint32_t numberOfParameters = m_function.at(block).parameters.size();
        for (uint32_t parameterIndex = 0; parameterIndex < numberOfParameters; ++parameterIndex) {
            if (!isJoinParameter(block, parameterIndex))
                continue;
            ValueID parameter = m_function.at(block).parameters[parameterIndex];
            m_candidateOfValue[parameter.index()] = VirtualObjectID(m_candidates.size());
            m_candidates.append(Candidate { .definition = parameter, .block = block, .isJoin = true });
        }
    }
}

void AllocationSinking::uniteFamilies(Vector<uint32_t>& parentOfCandidate)
{
    uint32_t numberOfCandidates = m_candidates.size();
    parentOfCandidate = Vector<uint32_t>(numberOfCandidates, [](size_t index) {
        return static_cast<uint32_t>(index);
    });

    auto find = [&](uint32_t index) {
        while (parentOfCandidate[index] != index) {
            parentOfCandidate[index] = parentOfCandidate[parentOfCandidate[index]];
            index = parentOfCandidate[index];
        }
        return index;
    };
    auto unite = [&](uint32_t a, uint32_t b) {
        uint32_t rootOfA = find(a);
        uint32_t rootOfB = find(b);
        if (rootOfA == rootOfB)
            return;
        if (rootOfA > rootOfB)
            std::swap(rootOfA, rootOfB);
        parentOfCandidate[rootOfB] = rootOfA;
    };

    const EscapeAnalysis& escape = m_analyses.escape();
    for (uint32_t join = 0; join < numberOfCandidates; ++join) {
        if (!m_candidates[join].isJoin)
            continue;
        PointsToSet set = escape.pointsTo(m_candidates[join].definition);
        for (ObjectID object : set.objects) {
            if (VirtualObjectID source = candidateDefinedBy(escape.at(object).definition))
                unite(join, source.index());
        }
    }

    for (uint32_t index = 0; index < numberOfCandidates; ++index)
        parentOfCandidate[index] = find(index);
}

void AllocationSinking::createFamilies(std::span<const DescribedObject> described, std::span<const uint32_t> rootOfCandidate)
{
    uint32_t numberOfCandidates = m_candidates.size();
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        uint32_t root = rootOfCandidate[index];
        RELEASE_ASSERT(root <= index);
        if (root == index) {
            m_candidates[index].family = VirtualFamilyID(m_families.size());
            m_families.append(VirtualFamily());
            continue;
        }
        m_candidates[index].family = m_candidates[root].family;
    }

    const EscapeAnalysis& escape = m_analyses.escape();
    uint64_t maxFields = limit(LimitID::MaxVirtualObjectFields);
    Vector<AccessesLookedUp, 8> accessesLookedUp;
    for (uint32_t index = 0; index < described.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        RELEASE_ASSERT(!candidate.isJoin);
        const VirtualObjectDescription& description = described[index].description;
        VirtualFamily& family = m_families[candidate.family.index()];
        candidate.descriptionIndex = family.addDescription(description);
        if (candidate.isAbandoned)
            continue;
        size_t lookedUp = accessesLookedUp.findIf([&](const AccessesLookedUp& entry) {
            return entry.family == candidate.family && entry.kind == description.kind && entry.layout == description.layout && entry.type == description.type;
        });
        if (lookedUp == notFound) {
            lookedUp = accessesLookedUp.size();
            accessesLookedUp.append(AccessesLookedUp { candidate.family, description.kind, description.layout, description.type, { } });
        }
        ensureFieldsInUse(m_objectContext, escape, description, family, maxFields, accessesLookedUp[lookedUp].users);
    }

    BitVector familiesWithTooManyFields(m_families.size());
    BitVector familiesWithoutJoins(m_families.size());
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        if (rootOfCandidate[index] != index)
            continue;
        VirtualFamilyID family = m_candidates[index].family;
        if (m_families[family.index()].numberOfFields() > maxFields)
            familiesWithTooManyFields.set(family.index());
        else if (givesOneFieldSeveralTypes(m_objectContext, m_families[family.index()]))
            familiesWithoutJoins.set(family.index());
    }
    for (uint32_t index = 0; index < numberOfCandidates; ++index) {
        const Candidate& candidate = m_candidates[index];
        if (familiesWithTooManyFields.get(candidate.family.index()))
            abandon(VirtualObjectID(index), LimitID::MaxVirtualObjectFields);
        else if (candidate.isJoin && familiesWithoutJoins.get(candidate.family.index()))
            abandon(VirtualObjectID(index));
    }

    for (uint32_t index = 0; index < described.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        if (candidate.isAbandoned || !described[index].description.absentResult)
            continue;
        candidate.tagBitWhenAbsent = m_families[candidate.family.index()].reserveConsecutiveTagBits(2, limit(LimitID::MaxVirtualObjectVariants));
        if (!candidate.tagBitWhenAbsent)
            abandon(VirtualObjectID(index), LimitID::MaxVirtualObjectVariants);
    }
}

void AllocationSinking::computeInputs()
{
    size_t numberOfValues = m_function.values.size();
    size_t numberOfBlocks = m_function.blocks.size();
    m_inputs.throwingValues.ensureSize(numberOfValues);
    m_inputs.valuesWritingObjects.ensureSize(numberOfValues);
    m_inputs.valuesCallingUnknownCode.ensureSize(numberOfValues);
    m_inputs.throwingTerminators.ensureSize(numberOfBlocks);
    m_inputs.conditionsOnlyBranchedOn.ensureSize(numberOfValues);
    m_inputs.blocksInCycles.ensureSize(numberOfBlocks);
    m_inputs.mergeBlocks.ensureSize(numberOfBlocks);
    m_inputs.blocksWithCandidates.ensureSize(numberOfBlocks);

    m_preparedBlocks.ensureSize(numberOfBlocks);
    m_blocksWithLiveDefinition.grow(m_candidates.size());
    m_relevantValuesOfBlocks.grow(numberOfBlocks);

    const CFG& cfg = m_analyses.cfg();
    const BlockCycles* cycles = cfg.hasCycle() ? &m_analyses.blockCycles() : nullptr;
    for (BlockID block : cfg.reversePostOrder()) {
        if (cycles && cycles->isInCycle(block))
            m_inputs.blocksInCycles.set(block.index());
        if (cfg.incomingEdges(block).size() > 1)
            m_inputs.mergeBlocks.set(block.index());
    }

    m_inputs.blocksWithJoins.ensureSize(numberOfBlocks);
    for (const Candidate& candidate : m_candidates) {
        if (!candidate.isAbandoned)
            (candidate.isJoin ? m_inputs.blocksWithJoins : m_inputs.blocksWithCandidates).set(candidate.block.index());
    }
}

void AllocationSinking::computeInputsOf(BlockID block)
{
    struct Traits {
        bool mayThrow { false };
        bool writesObjects { false };
        bool callsUnknownCode { false };
    };
    auto traitsOf = [&](ValueID id) -> Traits {
        if (m_analyses.hasNoEffects(id))
            return { };
        const EffectsOnEdges& effects = m_analyses.effectsOf(id);
        return {
            effects.mayThrow(),
            effects.normal.writes.containsAny(perObjectHeaps) || effects.handler.writes.containsAny(perObjectHeaps) || effects.normal.mayRunOtherCode(),
            effects.normal.mayCallUnknownCode && reliesOf(m_function.at(id).opcode).isEmpty(),
        };
    };

    const Block& contents = m_function.at(block);
    if (m_analyses.effectsOfTerminator(block).mayThrow())
        m_inputs.throwingTerminators.quickSet(block.index());

    uint32_t first = m_relevantValues.size();
    for (uint32_t index = 0; index < contents.values.size(); ++index) {
        ValueID id = contents.values[index];
        Traits traits = traitsOf(id);
        if (traits.mayThrow)
            m_inputs.throwingValues.quickSet(id.index());
        if (traits.writesObjects)
            m_inputs.valuesWritingObjects.quickSet(id.index());
        if (traits.callsUnknownCode)
            m_inputs.valuesCallingUnknownCode.quickSet(id.index());
        if (m_names.valuesMentioningNames.quickGet(id.index()) || m_candidateOfValue[id.index()])
            m_relevantValues.append(index);
        else if (traits.writesObjects || (traits.mayThrow && contents.handler))
            m_relevantValues.append(index | onlyWritesOrThrows);
    }
    m_relevantValuesOfBlocks[block.index()] = { first, static_cast<uint32_t>(m_relevantValues.size()) };

    const Terminator& terminator = contents.terminator;
    if (terminator.kind != TerminatorKind::Branch)
        return;
    ValueID condition = terminator.conditionOperand();
    if (branchesOf(m_function.at(condition).opcode) == Branches::No || m_analyses.useLists().useCount(condition) != 1)
        return;
    Traits traits = traitsOf(condition);
    if (!traits.mayThrow && !traits.writesObjects)
        m_inputs.conditionsOnlyBranchedOn.quickSet(condition.index());
}

void AllocationSinking::refuseUnprovenValues(VirtualObjectID object)
{
    familyOf(object).refuseUnprovenValues();
    m_mustSolveAgain = true;
}

// A store by name waits for PlaceTypeTests: the value that fits then goes on as a value of the field's type, and only the rare arm keeps the store by name.
// An object that takes such a value is carried further. Where one sweep over the function may already cost more than its share of the work, that tips the analysis over its limit, and it starts over.
bool AllocationSinking::storesByNameWaitIn(BlockID block) const
{
    return !m_inputs.hasOperationsOnSlots || m_inputs.isLargeForItsWork || m_function.at(block).frequency == BlockFrequency::Normal;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
