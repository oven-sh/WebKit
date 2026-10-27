/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTSinkAllocationsInternal.h"

#if ENABLE(AOT)

#include "AOTBlockCycles.h"
#include "AOTBlockFrequencies.h"
#include "AOTBuildContext.h"
#include "AOTEdgeResults.h"
#include "AOTPlaceTypeTestsPlan.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <algorithm>
#include <tuple>
#include <utility>
#include <wtf/EnumSet.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

template<unsigned numberOfWords>
struct WidePrefilterMask {
    WidePrefilterMask(uint64_t low = 0)
    {
        words[0] = low;
    }

    NEVER_INLINE WidePrefilterMask operator~() const
    {
        WidePrefilterMask result;
        for (unsigned word = 0; word < numberOfWords; ++word)
            result.words[word] = ~words[word];
        return result;
    }
    NEVER_INLINE WidePrefilterMask& operator|=(const WidePrefilterMask& other)
    {
        for (unsigned word = 0; word < numberOfWords; ++word)
            words[word] |= other.words[word];
        return *this;
    }
    NEVER_INLINE WidePrefilterMask& operator&=(const WidePrefilterMask& other)
    {
        for (unsigned word = 0; word < numberOfWords; ++word)
            words[word] &= other.words[word];
        return *this;
    }
    friend NEVER_INLINE WidePrefilterMask operator|(const WidePrefilterMask& a, const WidePrefilterMask& b)
    {
        WidePrefilterMask result;
        for (unsigned word = 0; word < numberOfWords; ++word)
            result.words[word] = a.words[word] | b.words[word];
        return result;
    }
    friend NEVER_INLINE WidePrefilterMask operator&(const WidePrefilterMask& a, const WidePrefilterMask& b)
    {
        WidePrefilterMask result;
        for (unsigned word = 0; word < numberOfWords; ++word)
            result.words[word] = a.words[word] & b.words[word];
        return result;
    }
    friend NEVER_INLINE bool operator==(const WidePrefilterMask& a, const WidePrefilterMask& b)
    {
        uint64_t difference = 0;
        for (unsigned word = 0; word < numberOfWords; ++word)
            difference |= a.words[word] ^ b.words[word];
        return !difference;
    }

    uint64_t words[numberOfWords] { };
};

template<typename Mask> struct PrefilterMaskTraits;

template<> struct PrefilterMaskTraits<uint64_t> {
    static constexpr uint32_t numberOfBits = 64;
    static uint64_t ofBit(uint32_t bit) { return uint64_t { 1 } << bit; }
    static bool has(uint64_t mask, uint32_t bit) { return mask >> bit & 1; }
    static bool isEmpty(uint64_t mask) { return !mask; }
    static void forEachBit(uint64_t mask, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        for (; mask; mask &= mask - 1)
            functor(ctz(mask));
    }
    static void findBit(uint64_t mask, NOESCAPE const Invocable<bool(uint32_t)> auto& functor)
    {
        for (; mask; mask &= mask - 1) {
            if (functor(ctz(mask)))
                return;
        }
    }
};

template<unsigned numberOfWords> struct PrefilterMaskTraits<WidePrefilterMask<numberOfWords>> {
    using Mask = WidePrefilterMask<numberOfWords>;
    static constexpr uint32_t numberOfBits = 64 * numberOfWords;
    static Mask ofBit(uint32_t bit)
    {
        Mask result;
        result.words[bit / 64] = uint64_t { 1 } << bit % 64;
        return result;
    }
    static bool has(const Mask& mask, uint32_t bit) { return mask.words[bit / 64] >> bit % 64 & 1; }
    static NEVER_INLINE bool isEmpty(const Mask& mask)
    {
        uint64_t any = 0;
        for (uint64_t word : mask.words)
            any |= word;
        return !any;
    }
    static void forEachBit(const Mask& mask, NOESCAPE const Invocable<void(uint32_t)> auto& functor)
    {
        for (unsigned word = 0; word < numberOfWords; ++word) {
            for (uint64_t bits = mask.words[word]; bits; bits &= bits - 1)
                functor(64 * word + ctz(bits));
        }
    }
    static void findBit(const Mask& mask, NOESCAPE const Invocable<bool(uint32_t)> auto& functor)
    {
        for (unsigned word = 0; word < numberOfWords; ++word) {
            for (uint64_t bits = mask.words[word]; bits; bits &= bits - 1) {
                if (functor(64 * word + ctz(bits)))
                    return;
            }
        }
    }
};

// Runs the sinking analysis on one bit per object: virtual or materialized. It drops the candidates that do not pay and starts over, as the analysis does.
// It knows no fields, shapes or tags, so an object stays virtual wherever those decide.
class AllocationPrefilter {
    WTF_MAKE_NONCOPYABLE(AllocationPrefilter);
public:
    AllocationPrefilter(AllocationSinking&, std::span<const Candidate>, std::span<const DescribedObject>, Vector<VirtualObjectID>& objectOfValue, const Vector<uint64_t>* tracesOfFruitlessComponents);
    ~AllocationPrefilter();

    void run();
    bool isUnprofitable(uint32_t allocation) const { return !m_objects[allocation].isProfitable; }
    // A number for all uses of the allocation and of what it is connected with, each at its distance in blocks. It names no value and no block.
    uint64_t traceOf(uint32_t allocation) { return m_objects[findComponent(find(allocation))].traceOfComponent; }
    bool wasFruitlessBefore(uint32_t allocation) const { return m_objects[allocation].wasFruitlessBefore; }
    bool exceedsMaterializationLimit(uint32_t allocation) const { return m_objects[allocation].exceedsMaterializationLimit; }
    std::optional<Use> firstMaterializationOf(uint32_t allocation) const { return m_objects[allocation].firstMaterialization; }

private:
    static constexpr uint32_t noObject = UINT32_MAX;
    static constexpr uint32_t terminatorIndex = UINT32_MAX;

    struct Object {
        BlockID block;
        ValueID definition;
        uint32_t parent { 0 };
        uint32_t parameterIndex { 0 };
        uint32_t component { 0 };
        uint32_t chunk { 0 };
        uint32_t bit { 0 };
        bool isMaterializedByCall { false };
        bool isUnmodeled { false };
        bool isProfitable { true };
        bool isLinked { false };
        bool holdsUnmodeledObjects { false };
        bool isDecided { false };
        uint32_t firstMaterializationIndex { UINT32_MAX };
        uint32_t firstStoreIndex { UINT32_MAX };
        uint64_t trace { 0 };
        uint64_t traceOfComponent { 0 };
        bool wasFruitlessBefore { false };
        bool exceedsMaterializationLimit { false };
        std::optional<Use> firstMaterialization;
        EnumSet<VirtualObjectKind> kinds;
    };

    enum class EventKind : uint8_t { Allocation, Materialization, Store, StoreFromObject, Hold, BranchTest };

    struct Event {
        uint32_t chunk { 0 };
        uint32_t position { 0 };
        uint32_t index { 0 };
        uint32_t object { 0 };
        EventKind kind { EventKind::Materialization };
        uint32_t other { noObject };
        Use use;
        KnownBoolean answer { KnownBoolean::Unknown };
    };

    struct ParameterLink {
        uint32_t chunk { 0 };
        uint32_t position { 0 };
        Edge edge;
        uint32_t source { 0 };
        uint32_t parameter { 0 };
    };

    struct ObjectUse {
        uint32_t chunk { 0 };
        uint32_t position { 0 };
        uint32_t object { 0 };
    };

    struct Chunk {
        uint32_t numberOfBits { 0 };
        uint32_t firstPosition { UINT32_MAX };
        Vector<uint32_t, 8> objects;
        std::span<const Event> events;
        std::span<const ParameterLink> links;
        std::span<const ObjectUse> uses;
    };

    template<typename Mask> class Solver;

    bool isAllocation(uint32_t object) const { return object < m_numberOfAllocations; }
    std::optional<uint32_t> objectNamedBy(ValueID) const;
    uint32_t find(uint32_t object);
    uint32_t findComponent(uint32_t object);
    void addName(ValueID, uint32_t object);
    uint32_t ensureParameterObject(BlockID, uint32_t parameterIndex);

    void collectNames();
    void uniteAliases();
    void findKinds();
    void classifyUses();
    void classify(ValueID name, uint32_t object, const Use&);
    void classifyAllocationOperand(ValueID name, uint32_t object, const Use&);
    void classifyListOperand(uint32_t object, const Use&);
    void addEvent(EventKind, uint32_t object, const Use&, uint32_t other = noObject);
    void materialize(uint32_t object, const Use&);
    bool neverFolds(uint32_t object, const Value& store, BlockID) const;
    bool mayBeScalarReplacedBy(uint32_t object, const Use&, BuiltinCallForm formOfCall);
    bool storesLate(uint32_t holder) const;
    std::optional<uint32_t> candidateReadFrom(ValueID) const;
    KnownBoolean evaluateOnAllocatedShape(uint32_t object, const Use& test) const;
    uint32_t bitOf(uint32_t object) const { return m_objects[object].bit; }
    uint32_t positionOf(BlockID block) const { return m_cfg.reversePostOrderIndex(block); }
    void decideLocally();
    void removeDecidedObjects();
    void partition();
    static uint64_t mix(uint64_t seen)
    {
        seen *= 0x9e3779b97f4a7c15ull;
        return seen ^ seen >> 29;
    }
    void addToTrace(uint32_t object, uint64_t seen) { m_objects[object].trace += mix(seen); }

    AllocationSinking& m_sinking;
    FunctionAnalyses& m_analyses;
    const VirtualObjectContext& m_context;
    const Program& m_program;
    const Function& m_function;
    const CFG& m_cfg;
    const UseLists& m_useLists;
    std::span<const Candidate> m_candidates;
    std::span<const DescribedObject> m_described;
    Vector<VirtualObjectID>& m_objectOfValue;
    uint32_t m_numberOfAllocations;
    bool m_findsFirstMaterializations { false };
    const Vector<uint64_t>* m_tracesOfFruitlessComponents { nullptr };
    Vector<Object, 16> m_objects;
    Vector<ValueID, 32> m_names;
    Vector<Event, 32> m_events;
    Vector<ParameterLink, 8> m_parameterLinks;
    Vector<ObjectUse, 32> m_uses;
    Vector<Chunk, 2> m_chunks;
};

AllocationPrefilter::AllocationPrefilter(AllocationSinking& sinking, std::span<const Candidate> candidates, std::span<const DescribedObject> described, Vector<VirtualObjectID>& objectOfValue, const Vector<uint64_t>* tracesOfFruitlessComponents)
    : m_sinking(sinking)
    , m_analyses(sinking.analyses())
    , m_context(sinking.objectContext())
    , m_program(m_context.program)
    , m_function(m_context.function)
    , m_cfg(m_analyses.cfg())
    , m_useLists(m_analyses.useLists())
    , m_candidates(candidates)
    , m_described(described)
    , m_objectOfValue(objectOfValue)
    , m_numberOfAllocations(candidates.size())
    , m_findsFirstMaterializations(sinking.context().build.options().collectsDeclines())
    , m_tracesOfFruitlessComponents(tracesOfFruitlessComponents)
{
    for (uint32_t index = 0; index < m_numberOfAllocations; ++index) {
        m_objects.append(Object { candidates[index].block, candidates[index].definition, index });
        m_names.append(candidates[index].definition);
    }
}

AllocationPrefilter::~AllocationPrefilter()
{
    for (size_t index = m_numberOfAllocations; index < m_names.size(); ++index)
        m_objectOfValue[m_names[index].index()] = { };
}

std::optional<uint32_t> AllocationPrefilter::objectNamedBy(ValueID value) const
{
    VirtualObjectID object = value ? m_objectOfValue[value.index()] : VirtualObjectID();
    if (!object)
        return std::nullopt;
    return object.index();
}

uint32_t AllocationPrefilter::find(uint32_t object)
{
    while (m_objects[object].parent != object) {
        m_objects[object].parent = m_objects[m_objects[object].parent].parent;
        object = m_objects[object].parent;
    }
    return object;
}

void AllocationPrefilter::addName(ValueID value, uint32_t object)
{
    if (m_objectOfValue[value.index()])
        return;
    m_objectOfValue[value.index()] = VirtualObjectID(object);
    m_names.append(value);
}

uint32_t AllocationPrefilter::ensureParameterObject(BlockID block, uint32_t parameterIndex)
{
    ValueID parameter = m_function.at(block).parameters[parameterIndex];
    if (std::optional<uint32_t> object = objectNamedBy(parameter))
        return *object;
    uint32_t object = m_objects.size();
    m_objects.append(Object { block, parameter, object, parameterIndex });
    addName(parameter, object);
    return object;
}

void AllocationPrefilter::collectNames()
{
    for (size_t next = 0; next < m_names.size(); ++next) {
        ValueID name = m_names[next];
        uint32_t object = m_objectOfValue[name.index()].index();
        for (const Use& use : m_useLists.usesOf(name)) {
            switch (use.kind) {
            case Use::Kind::Operand:
                if (!use.operandIndex && interpretedOf(m_function.at(use.user).opcode) == Interpretation::Cast)
                    addName(use.user, object);
                break;
            case Use::Kind::EdgeArgument:
            case Use::Kind::HandlerArgument: {
                Edge edge = use.edge();
                uint32_t parameter = ensureParameterObject(targetOf(m_function, edge).block, use.operandIndex);
                m_parameterLinks.append(ParameterLink { 0, positionOf(use.block), edge, object, parameter });
                break;
            }
            case Use::Kind::TerminatorOperand:
            case Use::Kind::BlockSubject:
                break;
            }
        }
    }
}

void AllocationPrefilter::uniteAliases()
{
    for (bool changed = m_objects.size() > m_numberOfAllocations; changed;) {
        changed = false;
        for (uint32_t parameter = m_numberOfAllocations; parameter < m_objects.size(); ++parameter) {
            if (find(parameter) != parameter)
                continue;
            const Object& object = m_objects[parameter];
            std::optional<uint32_t> source;
            bool isAlias = true;
            for (Edge edge : m_cfg.incomingEdges(object.block)) {
                ValueID incoming = valuePassedOn(m_function, edge, object.parameterIndex);
                if (incoming == object.definition)
                    continue;
                std::optional<uint32_t> named = objectNamedBy(incoming);
                if (!named) {
                    isAlias = false;
                    break;
                }
                uint32_t root = find(*named);
                if (root == parameter)
                    continue;
                if (source && *source != root) {
                    isAlias = false;
                    break;
                }
                source = root;
            }
            if (!isAlias || !source)
                continue;
            m_objects[parameter].parent = *source;
            changed = true;
        }
    }
}

void AllocationPrefilter::findKinds()
{
    for (uint32_t allocation = 0; allocation < m_numberOfAllocations; ++allocation)
        m_objects[allocation].kinds.add(m_described[allocation].description.kind);
    for (bool changed = !m_parameterLinks.isEmpty(); changed;) {
        changed = false;
        for (const ParameterLink& link : m_parameterLinks) {
            EnumSet<VirtualObjectKind>& kinds = m_objects[find(link.parameter)].kinds;
            EnumSet<VirtualObjectKind> united = kinds | m_objects[find(link.source)].kinds;
            changed |= united != kinds;
            kinds = united;
        }
    }
}

void AllocationPrefilter::addEvent(EventKind kind, uint32_t object, const Use& use, uint32_t other)
{
    uint32_t index = use.kind == Use::Kind::Operand ? m_useLists.locationOf(use.user).index : terminatorIndex;
    m_events.append(Event { 0, positionOf(use.block), index, object, kind, other, use });
    Object& root = m_objects[find(object)];
    root.trace += mix(mix(static_cast<uint64_t>(kind) + 1 + (static_cast<uint64_t>(m_cfg.successors(use.block).size()) << 8)) + positionOf(use.block) - positionOf(root.block) + (static_cast<uint64_t>(index) << 32));
    if (other != noObject) {
        root.isLinked = true;
        m_objects[find(other)].isLinked = true;
    }
    if (use.block != root.block)
        return;
    if (kind == EventKind::Materialization && index < root.firstMaterializationIndex) {
        root.firstMaterializationIndex = index;
        root.firstMaterialization = use;
    } else if (kind == EventKind::Store)
        root.firstStoreIndex = std::min(root.firstStoreIndex, index);
}

void AllocationPrefilter::materialize(uint32_t object, const Use& use)
{
    addEvent(EventKind::Materialization, object, use);
    if (use.kind == Use::Kind::Operand && effectRuleOf(m_function.at(use.user).opcode) == EffectRule::Call)
        m_objects[find(object)].isMaterializedByCall = true;
}

bool AllocationPrefilter::neverFolds(uint32_t object, const Value& store, BlockID block) const
{
    constexpr EnumSet<VirtualObjectKind> writableKinds { VirtualObjectKind::Object, VirtualObjectKind::Environment, VirtualObjectKind::Array, VirtualObjectKind::Arguments };

    if (!isAllocation(object) || store.opcode == Opcode::AppendElement || store.opcode == Opcode::AppendHole)
        return false;
    const VirtualObjectDescription& description = m_described[object].description;
    VirtualFieldAccess access = lookUpField(m_context, description, store);
    if (access.lookup != FieldLookup::Field || !writableKinds.contains(description.kind))
        return true;
    if (description.kind == VirtualObjectKind::Arguments)
        return false;
    bool isListed = false;
    forEachInitialField(m_context, description, [&](const VirtualField& field, ValueID) {
        isListed |= field == access.field;
    });
    if (isListed ? access.whenPresent != PresentField::IsOwnData : access.whenAbsent != AbsentField::FindsNothing)
        return true;
    if (store.opcode == Opcode::PropDelete)
        return false;
    if (admissionOf(m_sinking, description, access.field, { m_function.at(store.valueOperand()).type }) != Admission::Refused)
        return false;
    return store.opcode != Opcode::PropSet || m_sinking.storesByNameWaitIn(block) || !canHoldUnprovenValue(m_program, description, access.field);
}

bool AllocationPrefilter::mayBeScalarReplacedBy(uint32_t object, const Use& use, BuiltinCallForm formOfCall)
{
    const Value& user = m_function.at(use.user);
    EnumSet<VirtualObjectKind> kinds = m_objects[find(object)].kinds;
    if (kinds.contains(VirtualObjectKind::Arguments))
        return true;
    if (std::optional<BytesOperands> bytes = kinds.contains(VirtualObjectKind::ViewRange) ? findBytesOperands(m_program, m_function, user, formOfCall) : std::nullopt)
        return bytes->operandIndexes.contains(use.operandIndex);
    return kinds.contains(VirtualObjectKind::Array) && user.opcode == Opcode::CallBuiltin && user.builtinCallForm() == BuiltinCallForm::List;
}

bool AllocationPrefilter::storesLate(uint32_t holder) const
{
    if (!isAllocation(holder))
        return false;
    VirtualObjectKind kind = m_described[holder].description.kind;
    return kind == VirtualObjectKind::Environment || kind == VirtualObjectKind::Object;
}

// What is read out of a virtual object may be another virtual object.
std::optional<uint32_t> AllocationPrefilter::candidateReadFrom(ValueID value) const
{
    while (interpretedOf(m_function.at(value).opcode) == Interpretation::Cast)
        value = m_function.at(value).operand(0);
    const Value& definition = m_function.at(value);
    if (interpretationOf(m_program, definition) != Interpretation::Read)
        return std::nullopt;
    return objectNamedBy(definition.operand(0));
}

KnownBoolean AllocationPrefilter::evaluateOnAllocatedShape(uint32_t object, const Use& use) const
{
    constexpr OpcodeSet testsIndependentOfStores {
        Opcode::ToBoolean, Opcode::TypeTest, Opcode::IsNotFrozen, Opcode::HasLayoutPrototype, Opcode::BelongsToInstance, Opcode::IsPristine, Opcode::IsClosureOf,
    };

    const Value& test = m_function.at(use.user);
    if (!isAllocation(object) || !testsIndependentOfStores.contains(test.opcode))
        return KnownBoolean::Unknown;
    bool namesOtherObject = false;
    test.forEachOperand([&](ValueID operand, uint32_t index) {
        namesOtherObject |= index != use.operandIndex && objectNamedBy(operand);
    });
    if (namesOtherObject)
        return KnownBoolean::Unknown;

    const VirtualObjectDescription& description = m_described[object].description;
    VirtualFamily family;
    KeyOrderTable orders;
    uint32_t descriptionIndex = family.addDescription(description);
    forEachInitialField(m_context, description, [&](const VirtualField& field, ValueID) {
        family.ensureField(field);
    });
    VirtualShape shape = shapeAtAllocation(m_context, family, orders, descriptionIndex);
    return evaluateTest(ShapeContext { m_context, family, orders }, shape, test, use.operandIndex);
}

void AllocationPrefilter::classifyAllocationOperand(ValueID name, uint32_t object, const Use& use)
{
    const Value& user = m_function.at(use.user);
    std::optional<uint32_t> holder = objectNamedBy(use.user);
    if (!holder || !isAllocation(*holder) || m_objects[*holder].definition != use.user) {
        if (!mayBeScalarReplacedBy(object, use, BuiltinCallForm::Whole))
            materialize(object, use);
        return;
    }

    const VirtualObjectDescription& description = m_described[*holder].description;
    if (description.staysInPlace) {
        if (description.isDead)
            m_objects[find(object)].isUnmodeled = true;
        else if (description.kind != VirtualObjectKind::BuiltinFields || !mayBeScalarReplacedBy(object, use, BuiltinCallForm::Fields))
            materialize(object, use);
        return;
    }
    bool isListed = false;
    forEachInitialField(m_context, description, [&](const VirtualField&, ValueID value) {
        isListed |= value == name;
    });
    if (isListed)
        addEvent(EventKind::Hold, object, use, *holder);
    else
        m_objects[find(object)].isUnmodeled = true;
}

void AllocationPrefilter::classifyListOperand(uint32_t object, const Use& use)
{
    static_assert(OperandIndex::AppendSpread::array == OperandIndex::AppendElements::array && OperandIndex::AppendSpread::iterable == OperandIndex::AppendElements::source);
    static_assert(OperandIndex::CopyDataProperties::target == OperandIndex::AppendElements::array && OperandIndex::CopyDataProperties::source == OperandIndex::AppendElements::source);

    const Value& user = m_function.at(use.user);
    switch (user.opcode) {
    case Opcode::AppendSpread:
    case Opcode::AppendElements:
    case Opcode::CopyDataProperties: {
        if (use.operandIndex != OperandIndex::AppendElements::array)
            return;
        std::optional<uint32_t> source = objectNamedBy(user.operand(OperandIndex::AppendElements::source));
        if (source)
            addEvent(EventKind::StoreFromObject, object, use, *source);
        else
            materialize(object, use);
        return;
    }
    case Opcode::CallList:
    case Opcode::ConstructList:
    case Opcode::SuperConstructList:
        if (use.operandIndex != OperandIndex::CallList::list)
            materialize(object, use);
        return;
    default:
        return;
    }
}

void AllocationPrefilter::classify(ValueID name, uint32_t object, const Use& use)
{
    uint64_t frequency = static_cast<uint64_t>(m_function.at(use.block).frequency.predicted());
    uint64_t operation = use.kind == Use::Kind::Operand ? static_cast<uint64_t>(m_function.at(use.user).opcode) : static_cast<uint64_t>(m_function.at(use.block).terminator.kind);
    uint64_t distance = positionOf(use.block) - positionOf(m_objects[find(object)].block);
    addToTrace(find(object), (static_cast<uint64_t>(use.kind) | frequency << 4 | static_cast<uint64_t>(use.operandIndex) << 8 | operation << 24) + mix(distance));
    if (use.isEdgeUse())
        return;
    m_uses.append(ObjectUse { 0, positionOf(use.block), object });
    if (use.kind == Use::Kind::BlockSubject)
        return;
    if (use.kind == Use::Kind::TerminatorOperand) {
        switch (m_function.at(use.block).terminator.kind) {
        case TerminatorKind::Return:
        case TerminatorKind::Branch:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::TryCallInline:
            materialize(object, use);
            return;
        case TerminatorKind::SwitchOnShape:
            addEvent(EventKind::BranchTest, object, use);
            return;
        case TerminatorKind::Jump:
        case TerminatorKind::Unreachable:
        case TerminatorKind::TestAndBranch:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    const Value& user = m_function.at(use.user);
    switch (interpretationOf(m_program, user)) {
    case Interpretation::Cast:
    case Interpretation::Guards:
        return;
    case Interpretation::Read:
        if (!use.operandIndex) {
            if (isAllocation(object) && lookUpField(m_context, m_described[object].description, user).lookup == FieldLookup::Unknown)
                materialize(object, use);
            return;
        }
        if (!objectNamedBy(user.operand(0)))
            materialize(object, use);
        return;
    case Interpretation::Store: {
        if (!use.operandIndex) {
            if (neverFolds(object, user, use.block))
                materialize(object, use);
            else
                addEvent(EventKind::Store, object, use, storesWithoutValue.contains(user.opcode) ? noObject : objectNamedBy(user.valueOperand()).value_or(noObject));
            return;
        }
        std::optional<uint32_t> holder = objectNamedBy(user.operand(0));
        if (!holder) {
            if (std::optional<uint32_t> source = candidateReadFrom(user.operand(0))) {
                m_objects[find(object)].isUnmodeled = true;
                m_objects[find(*source)].holdsUnmodeledObjects = true;
            } else
                materialize(object, use);
        } else if (!storesWithoutValue.contains(user.opcode) && user.valueOperand() == name)
            addEvent(EventKind::Hold, object, use, *holder);
        return;
    }
    case Interpretation::Test: {
        if (testsLeftToRun.contains(user.opcode)) {
            materialize(object, use);
            return;
        }
        const Terminator& terminator = m_function.at(use.block).terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.conditionOperand() != use.user)
            return;
        addEvent(EventKind::BranchTest, object, use);
        m_events.last().answer = evaluateOnAllocatedShape(object, use);
        return;
    }
    case Interpretation::List:
        classifyListOperand(object, use);
        return;
    case Interpretation::Allocation:
        classifyAllocationOperand(name, object, use);
        return;
    case Interpretation::None:
        if (buildsOperand(m_program, user, use.operandIndex))
            materialize(object, use);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void AllocationPrefilter::classifyUses()
{
    for (uint32_t allocation = 0; allocation < m_numberOfAllocations; ++allocation)
        m_events.append(Event { 0, positionOf(m_objects[allocation].block), m_useLists.locationOf(m_objects[allocation].definition).index, allocation, EventKind::Allocation, noObject, { } });
    for (ValueID name : m_names) {
        uint32_t object = m_objectOfValue[name.index()].index();
        for (const Use& use : m_useLists.usesOf(name))
            classify(name, object, use);
    }
}

template<typename Mask>
class AllocationPrefilter::Solver {
    WTF_MAKE_NONCOPYABLE(Solver);
public:
    Solver(AllocationPrefilter& prefilter, const Chunk& chunk)
        : m_prefilter(prefilter)
        , m_analyses(prefilter.m_analyses)
        , m_function(prefilter.m_function)
        , m_cfg(prefilter.m_cfg)
        , m_chunk(chunk)
        , m_firstPosition(chunk.firstPosition)
        , m_held(FillWith { }, chunk.numberOfBits, Mask(0))
    {
    }

    void run();

private:
    using Traits = PrefilterMaskTraits<Mask>;

    struct BlockState {
        Mask defined { 0 };
        Mask liveIn { 0 };
        Mask virtualIn { 0 };
        Mask unheldIn { 0 };
        Mask materializedAtEntry { 0 };
        uint32_t firstEvent { 0 };
        uint32_t endEvent { 0 };
        uint32_t firstLink { 0 };
        uint32_t endLink { 0 };
        bool isPending { false };
        bool wasVisited { false };
    };

    struct Site {
        Mask members { 0 };
        BlockID block;
        std::optional<Edge> edge;
        const Use* use { nullptr };
    };

    struct Counts {
        uint32_t bit { 0 };
        BlockID origin;
        unsigned numberOfMaterializations { 0 };
        unsigned numberOfRarerMaterializations { 0 };
        unsigned numberOfBorrowingMaterializations { 0 };
    };

    static Mask ofBit(uint32_t bit) { return Traits::ofBit(bit); }
    static bool has(const Mask& mask, uint32_t bit) { return Traits::has(mask, bit); }
    static bool isEmpty(const Mask& mask) { return Traits::isEmpty(mask); }
    uint32_t bitOf(uint32_t object) const { return m_prefilter.m_objects[object].bit; }
    uint32_t objectOf(uint32_t bit) const { return m_chunk.objects[bit]; }
    uint32_t positionOf(BlockID block) const { return m_cfg.reversePostOrderIndex(block); }
    BlockState& at(uint32_t position)
    {
        size_t index = position - m_firstPosition;
        if (index >= m_blocks.size()) [[unlikely]]
            m_blocks.grow(index + 1);
        return m_blocks[index];
    }

    void prepare();
    void findComponents();
    void findObjectsLeavingTheirBlocks();
    void findHeldObjects();
    void solveLiveness();
    void findObjectsMaterializedAtLoopEntries();
    void propagateVirtualObjects();
    void recountMaterializations();
    bool visit(uint32_t position);
    bool enterEdge(const Mask& leaving, const Mask& unheld, uint32_t fromPosition, Edge, BlockID target, std::span<const ParameterLink>);
    void materialize(Mask& current, const Mask& unheld, uint32_t bit, BlockID, const Edge*, const Use*);
    void count(uint32_t bit, BlockID, const Edge*, const Use*);
    bool dropUnprofitableCandidates();

    AllocationPrefilter& m_prefilter;
    FunctionAnalyses& m_analyses;
    const Function& m_function;
    const CFG& m_cfg;
    const Chunk& m_chunk;
    uint32_t m_firstPosition;
    Vector<BlockState, 16> m_blocks;
    Vector<Mask, 16> m_held;
    Vector<uint32_t, 16> m_visitedPositions;
    Vector<uint32_t, 8> m_eventsInCycles;
    Vector<uint32_t, 8> m_linksInCycles;
    Vector<Site, 16> m_sites;
    bool m_hasVisitedBlockTwice { false };
    Mask m_candidates { 0 };
    Mask m_active { 0 };
    Mask m_leavesBlock { 0 };
    Vector<Mask, 16> m_component;
    Mask m_joins { 0 };
    Mask m_holders { 0 };
    Mask m_unmodeled { 0 };
    Mask m_isMaterializedByCall { 0 };
    Mask m_materializedBeforeBranches { 0 };
    Mask m_abandoned { 0 };
    Mask m_diesVirtual { 0 };
    Mask m_entersJoin { 0 };
    Mask m_foldsStore { 0 };
    Mask m_isStoredLate { 0 };
    Mask m_isMaterialized { 0 };
    Mask m_isMaterializedTwice { 0 };
    Mask m_isMaterializedThrice { 0 };
    Mask m_counted { 0 };
    Mask m_exceededLimit { 0 };
    Vector<Counts, 4> m_counts;
};

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::prepare()
{
    for (uint32_t bit = 0; bit < m_chunk.numberOfBits; ++bit) {
        const Object& object = m_prefilter.m_objects[objectOf(bit)];
        at(positionOf(object.block)).defined |= ofBit(bit);
        m_candidates |= ofBit(bit);
        if (!m_prefilter.isAllocation(objectOf(bit)))
            m_joins |= ofBit(bit);
        if (object.isUnmodeled)
            m_unmodeled |= ofBit(bit);
        if (object.isMaterializedByCall)
            m_isMaterializedByCall |= ofBit(bit);
    }
    std::span<const Event> events = m_chunk.events;
    for (uint32_t index = 0; index < events.size();) {
        BlockState& state = at(events[index].position);
        state.firstEvent = index;
        while (index < events.size() && events[index].position == events[state.firstEvent].position)
            ++index;
        state.endEvent = index;
    }
    std::span<const ParameterLink> links = m_chunk.links;
    for (uint32_t index = 0; index < links.size();) {
        BlockState& state = at(links[index].position);
        state.firstLink = index;
        while (index < links.size() && links[index].position == links[state.firstLink].position)
            ++index;
        state.endLink = index;
    }

    if (!m_cfg.hasCycle())
        return;
    const BlockCycles& cycles = m_analyses.blockCycles();
    for (uint32_t index = 0; index < events.size(); ++index) {
        EventKind kind = events[index].kind;
        if ((kind == EventKind::Materialization || kind == EventKind::Hold || kind == EventKind::StoreFromObject) && cycles.isInCycle(m_cfg.reversePostOrder()[events[index].position]))
            m_eventsInCycles.append(index);
    }
    for (uint32_t index = 0; index < links.size(); ++index) {
        if (cycles.areInSameCycle(links[index].edge.from, targetOf(m_function, links[index].edge).block))
            m_linksInCycles.append(index);
    }
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::findComponents()
{
    m_component.fill(Mask(0), m_chunk.numberOfBits);
    for (uint32_t first = 0; first < m_chunk.numberOfBits;) {
        uint32_t component = m_prefilter.findComponent(objectOf(first));
        Mask members = 0;
        uint32_t end = first;
        for (; end < m_chunk.numberOfBits && m_prefilter.findComponent(objectOf(end)) == component; ++end)
            members |= ofBit(end);
        for (uint32_t bit = first; bit < end; ++bit)
            m_component[bit] = members;
        first = end;
    }
}

// An object stays virtual for a shorter time in each round, so what the first round finds holds for all.
template<typename Mask>
void AllocationPrefilter::Solver<Mask>::findObjectsLeavingTheirBlocks()
{
    std::span<const Event> events = m_chunk.events;
    for (uint32_t first = 0; first < events.size();) {
        uint32_t end = first;
        Mask current = 0;
        Mask unheld = 0;
        for (; end < events.size() && events[end].position == events[first].position; ++end) {
            const Event& event = events[end];
            uint32_t bit = bitOf(event.object);
            switch (event.kind) {
            case EventKind::Allocation:
                current |= ofBit(bit);
                unheld |= ofBit(bit);
                break;
            case EventKind::Materialization:
                if (has(current, bit))
                    current &= ~(ofBit(bit) | (m_held[bit] & ~unheld));
                break;
            case EventKind::Store:
            case EventKind::BranchTest:
                break;
            case EventKind::StoreFromObject:
                if (has(current, bit) && !has(current, bitOf(event.other)))
                    current &= ~(ofBit(bit) | (m_held[bit] & ~unheld));
                break;
            case EventKind::Hold:
                if (!has(current, bit))
                    break;
                if (has(current, bitOf(event.other)))
                    unheld &= ~ofBit(bit);
                else
                    current &= ~(ofBit(bit) | (m_held[bit] & ~unheld));
                break;
            }
        }
        m_leavesBlock |= current;
        first = end;
    }
    m_leavesBlock |= m_joins;
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::findHeldObjects()
{
    for (bool changed = true; changed;) {
        changed = false;
        auto add = [&](uint32_t holderBit, const Mask& held) {
            Mask united = m_held[holderBit] | held;
            changed |= united != m_held[holderBit];
            m_held[holderBit] = united;
        };
        for (const Event& event : m_chunk.events) {
            if (event.kind == EventKind::Hold)
                add(bitOf(event.other), ofBit(bitOf(event.object)) | m_held[bitOf(event.object)]);
        }
        for (const ParameterLink& link : m_chunk.links)
            add(bitOf(link.parameter), m_held[bitOf(link.source)]);
    }
    for (uint32_t bit = 0; bit < m_chunk.numberOfBits; ++bit) {
        if (isEmpty(m_held[bit]))
            continue;
        m_holders |= ofBit(bit);
        if (m_prefilter.m_objects[objectOf(bit)].holdsUnmodeledObjects)
            m_unmodeled |= m_held[bit];
    }
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::solveLiveness()
{
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    auto add = [&](uint32_t position, const Mask& bits) {
        BlockState& state = at(position);
        Mask united = state.liveIn | bits;
        if (united == state.liveIn)
            return false;
        state.liveIn = united;
        state.isPending = true;
        return true;
    };
    for (const ObjectUse& use : m_chunk.uses) {
        if (has(m_leavesBlock, bitOf(use.object)))
            add(use.position, ofBit(bitOf(use.object)));
    }
    for (bool mustRepeat = true; mustRepeat;) {
        mustRepeat = false;
        for (size_t index = m_blocks.size(); index--;) {
            if (!std::exchange(m_blocks[index].isPending, false))
                continue;
            Mask liveOut = m_blocks[index].liveIn & ~m_blocks[index].defined;
            if (isEmpty(liveOut))
                continue;
            for (BlockID predecessor : m_cfg.predecessors(order[m_firstPosition + index])) {
                uint32_t predecessorPosition = positionOf(predecessor);
                if (predecessorPosition >= m_firstPosition && add(predecessorPosition, liveOut))
                    mustRepeat |= predecessorPosition - m_firstPosition >= index;
            }
        }
        for (const ParameterLink& link : m_chunk.links) {
            if (bitOf(link.parameter) != bitOf(link.source) && has(m_leavesBlock, bitOf(link.source)) && has(at(positionOf(targetOf(m_function, link.edge).block)).liveIn, bitOf(link.parameter)))
                mustRepeat |= add(link.position, ofBit(bitOf(link.source)));
        }
    }
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::findObjectsMaterializedAtLoopEntries()
{
    if (m_eventsInCycles.isEmpty() && m_linksInCycles.isEmpty())
        return;
    const BlockCycles& cycles = m_analyses.blockCycles();
    std::span<const BlockID> order = m_cfg.reversePostOrder();
    Vector<std::pair<unsigned, Mask>, 4> materializedInOutermostCycle;
    Vector<std::pair<unsigned, Mask>, 4> materializedInCycle;
    auto add = [](auto& list, unsigned cycle, const Mask& bits) {
        for (auto& entry : list) {
            if (entry.first == cycle) {
                entry.second |= bits;
                return;
            }
        }
        list.append({ cycle, bits });
    };
    auto materializeIn = [&](BlockID block, uint32_t materializedBit) {
        Traits::forEachBit((ofBit(materializedBit) | m_held[materializedBit]) & m_leavesBlock & m_candidates & m_active, [&](uint32_t bit) {
            BlockID origin = m_prefilter.m_objects[objectOf(bit)].block;
            if (block == origin)
                return;
            if (!cycles.areInSameCycle(block, origin)) {
                add(materializedInOutermostCycle, *cycles.outermostCycleOf(block), ofBit(bit));
                return;
            }
            const BlockCycles::Forest& forest = cycles.forest();
            unsigned depthOfOrigin = cycles.depthOf(origin);
            for (unsigned cycle = forest.innermost(block); cycle != BlockCycles::Forest::none && forest[cycle].depth > depthOfOrigin; cycle = forest[cycle].parent)
                add(materializedInCycle, cycle, ofBit(bit));
        });
    };
    for (uint32_t index : m_eventsInCycles) {
        const Event& event = m_chunk.events[index];
        if (event.kind == EventKind::Materialization || !has(m_candidates, bitOf(event.other)))
            materializeIn(order[event.position], bitOf(event.object));
    }
    for (uint32_t index : m_linksInCycles) {
        const ParameterLink& link = m_chunk.links[index];
        if (!has(m_candidates, bitOf(link.parameter)))
            materializeIn(link.edge.from, bitOf(link.source));
    }
    if (materializedInOutermostCycle.isEmpty() && materializedInCycle.isEmpty())
        return;
    for (size_t index = 0; index < m_blocks.size(); ++index) {
        BlockState& state = m_blocks[index];
        BlockID block = order[m_firstPosition + index];
        if (isEmpty(state.liveIn) || !cycles.isInCycle(block))
            continue;
        for (const auto& [cycle, bits] : materializedInOutermostCycle) {
            if (cycle == *cycles.outermostCycleOf(block))
                state.materializedAtEntry |= bits;
        }
        if (materializedInCycle.isEmpty())
            continue;
        const BlockCycles::Forest& forest = cycles.forest();
        for (unsigned cycle = forest.innermost(block); cycle != BlockCycles::Forest::none; cycle = forest[cycle].parent) {
            for (const auto& [materializingCycle, bits] : materializedInCycle) {
                if (cycle == materializingCycle)
                    state.materializedAtEntry |= bits;
            }
        }
    }
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::count(uint32_t bit, BlockID block, const Edge* edge, const Use* use)
{
    const BlockFrequencies& frequencies = m_analyses.blockFrequencies();
    Expected<BlockFrequency> frequency = edge ? rarerOf(frequencies.classOf(edge->from), frequencies.classOf(*edge)) : frequencies.classOf(block);
    for (Counts& counts : m_counts) {
        if (counts.bit != bit)
            continue;
        ++counts.numberOfMaterializations;
        if (isRarerThan(frequency, frequencies.classOf(counts.origin)).predicted())
            ++counts.numberOfRarerMaterializations;
        else if (use && use->kind == Use::Kind::Operand) {
            BuildCause cause = describeCause(m_analyses, *use);
            if (cause.opcode && effectRuleOf(*cause.opcode) == EffectRule::Call && cause.reasons.isEmpty())
                ++counts.numberOfBorrowingMaterializations;
        }
        return;
    }
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::materialize(Mask& current, const Mask& unheld, uint32_t bit, BlockID block, const Edge* edge, const Use* use)
{
    Mask members = (ofBit(bit) | (m_held[bit] & ~unheld)) & current;
    current &= ~members;
    m_isMaterializedThrice |= m_isMaterializedTwice & members;
    m_isMaterializedTwice |= m_isMaterialized & members;
    m_isMaterialized |= members;
    if (use && m_prefilter.m_findsFirstMaterializations) {
        Traits::forEachBit(members, [&](uint32_t member) {
            std::optional<Use>& first = m_prefilter.m_objects[objectOf(member)].firstMaterialization;
            if (!first)
                first = *use;
        });
    }
    if (!isEmpty(members & ~m_joins))
        m_sites.append(Site { members & ~m_joins, block, edge ? std::optional<Edge>(*edge) : std::nullopt, use });
}

template<typename Mask>
bool AllocationPrefilter::Solver<Mask>::enterEdge(const Mask& leaving, const Mask& unheld, uint32_t fromPosition, Edge edge, BlockID target, std::span<const ParameterLink> links)
{
    uint32_t position = positionOf(target);
    BlockState* state = position >= m_firstPosition && position - m_firstPosition < m_blocks.size() ? &m_blocks[position - m_firstPosition] : nullptr;
    Mask entering = leaving;
    Mask added = 0;
    Mask alive = state ? state->liveIn & ~state->defined : Mask(0);

    for (const ParameterLink& link : links) {
        uint32_t sourceBit = bitOf(link.source);
        uint32_t parameterBit = bitOf(link.parameter);
        if (link.edge != edge || parameterBit == sourceBit || !has(entering, sourceBit))
            continue;
        if (edge.isHandlerEdge()) {
            m_unmodeled |= ofBit(sourceBit) | ofBit(parameterBit);
            added |= ofBit(parameterBit);
            continue;
        }
        if (!has(m_candidates, parameterBit)) {
            materialize(entering, unheld, sourceBit, edge.from, &edge, nullptr);
            continue;
        }
        if (has(alive, sourceBit) || has(added, parameterBit))
            m_abandoned |= ofBit(parameterBit);
        entering &= ~ofBit(sourceBit);
        added |= ofBit(parameterBit);
        m_entersJoin |= ofBit(sourceBit);
    }

    if (state) {
        Traits::forEachBit(entering & alive & state->materializedAtEntry, [&](uint32_t bit) {
            if (has(entering, bit))
                materialize(entering, unheld, bit, edge.from, &edge, nullptr);
        });
    }
    Mask heldOnly = entering & ~alive & ~unheld;
    if (!isEmpty(heldOnly)) {
        Traits::findBit(((entering & alive) | added) & m_holders, [&](uint32_t holder) {
            alive |= m_held[holder] & heldOnly;
            heldOnly &= ~m_held[holder];
            return isEmpty(heldOnly);
        });
    }
    if (!edge.isHandlerEdge())
        m_diesVirtual |= entering & ~alive;
    if (!state)
        return false;

    Mask next = state->virtualIn | (entering & alive) | added;
    Mask nextUnheld = state->unheldIn | (entering & alive & unheld) | added;
    if (next == state->virtualIn && nextUnheld == state->unheldIn)
        return false;
    state->virtualIn = next;
    state->unheldIn = nextUnheld;
    state->isPending = true;
    return position <= fromPosition;
}

template<typename Mask>
bool AllocationPrefilter::Solver<Mask>::visit(uint32_t position)
{
    BlockID id = m_cfg.reversePostOrder()[position];
    BlockState& state = m_blocks[position - m_firstPosition];
    Mask current = state.virtualIn;
    Mask somewhere = current;
    Mask unheld = state.unheldIn;
    bool branchMayFold = false;
    std::optional<uint32_t> deadTargetIndex;
    if (state.firstEvent == state.endEvent && state.firstLink == state.endLink && !m_cfg.hasHandler(id) && isEmpty(current & m_materializedBeforeBranches)) {
        std::span<const BlockID> successors = m_cfg.successors(id);
        if (successors.empty())
            m_diesVirtual |= current;
        bool mustRepeat = false;
        for (uint32_t targetIndex = 0; targetIndex < successors.size(); ++targetIndex)
            mustRepeat |= enterEdge(current, unheld, position, Edge { id, targetIndex }, successors[targetIndex], { });
        return mustRepeat;
    }
    const Block& block = m_function.at(id);

    for (const Event& event : m_chunk.events.subspan(state.firstEvent, state.endEvent - state.firstEvent)) {
        uint32_t bit = bitOf(event.object);
        switch (event.kind) {
        case EventKind::Allocation:
            if (!has(m_candidates & m_active, bit))
                break;
            if (has(current, bit))
                m_abandoned |= ofBit(bit);
            current |= ofBit(bit);
            somewhere |= ofBit(bit);
            unheld |= ofBit(bit);
            break;
        case EventKind::Materialization:
            if (has(current, bit))
                materialize(current, unheld, bit, id, nullptr, &event.use);
            break;
        case EventKind::BranchTest:
            if (!has(current, bit))
                break;
            branchMayFold = true;
            if (event.answer == KnownBoolean::Unknown)
                m_unmodeled |= ofBit(bit);
            else
                deadTargetIndex = event.answer == KnownBoolean::True ? 1 : 0;
            break;
        case EventKind::Store: {
            if (!has(current, bit))
                break;
            bool isStoredLate = event.other != noObject && has(current, bitOf(event.other)) && m_prefilter.storesLate(objectOf(bit)) && has(m_held[bitOf(event.other)], bit);
            if (!isStoredLate)
                m_foldsStore |= ofBit(bit);
            break;
        }
        case EventKind::StoreFromObject:
            if (!has(current, bit))
                break;
            if (has(current, bitOf(event.other)))
                m_foldsStore |= ofBit(bit);
            else
                materialize(current, unheld, bit, id, nullptr, &event.use);
            break;
        case EventKind::Hold: {
            if (!has(current, bit))
                break;
            uint32_t holderBit = bitOf(event.other);
            if (!has(current, holderBit)) {
                materialize(current, unheld, bit, id, nullptr, &event.use);
                break;
            }
            unheld &= ~ofBit(bit);
            if (m_prefilter.storesLate(objectOf(holderBit)) && has(m_held[bit], holderBit))
                m_isStoredLate |= ofBit(bit);
            break;
        }
        }
    }

    uint32_t numberOfTargets = block.terminator.targets.size();
    if (numberOfTargets > 1 && !branchMayFold) {
        Traits::forEachBit(current & m_materializedBeforeBranches, [&](uint32_t bit) {
            if (has(current, bit))
                materialize(current, unheld, bit, id, nullptr, nullptr);
        });
    }
    if (!numberOfTargets)
        m_diesVirtual |= current;
    std::span<const ParameterLink> links = m_chunk.links.subspan(state.firstLink, state.endLink - state.firstLink);
    bool mustRepeat = false;
    if (!isEmpty(current)) {
        for (uint32_t targetIndex = 0; targetIndex < numberOfTargets; ++targetIndex) {
            if (targetIndex != deadTargetIndex)
                mustRepeat |= enterEdge(current, unheld, position, Edge { id, targetIndex }, block.terminator.targets[targetIndex].block, links);
        }
    }
    if (block.handler && !isEmpty(somewhere))
        mustRepeat |= enterEdge(somewhere, somewhere, position, Edge::toHandlerOf(id), block.handler->block, links);
    return mustRepeat;
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::propagateVirtualObjects()
{
    for (uint32_t position : m_visitedPositions) {
        BlockState& state = m_blocks[position - m_firstPosition];
        state.virtualIn = 0;
        state.unheldIn = 0;
        state.wasVisited = false;
        state.isPending = false;
    }
    m_visitedPositions.shrink(0);
    m_sites.shrink(0);
    m_hasVisitedBlockTwice = false;
    m_abandoned &= ~m_active;
    m_diesVirtual &= ~m_active;
    m_entersJoin &= ~m_active;
    m_foldsStore &= ~m_active;
    m_isStoredLate &= ~m_active;
    m_isMaterialized &= ~m_active;
    m_isMaterializedTwice &= ~m_active;
    m_isMaterializedThrice &= ~m_active;
    findObjectsMaterializedAtLoopEntries();

    for (const Event& event : m_chunk.events) {
        if (event.kind == EventKind::Allocation && has(m_candidates & m_active, bitOf(event.object)))
            m_blocks[event.position - m_firstPosition].isPending = true;
    }
    for (bool mustRepeat = true; mustRepeat;) {
        mustRepeat = false;
        for (size_t index = 0; index < m_blocks.size(); ++index) {
            BlockState& state = m_blocks[index];
            if (!std::exchange(state.isPending, false))
                continue;
            uint32_t position = m_firstPosition + index;
            if (std::exchange(state.wasVisited, true))
                m_hasVisitedBlockTwice = true;
            else
                m_visitedPositions.append(position);
            mustRepeat |= visit(position);
        }
    }
    if (m_hasVisitedBlockTwice)
        recountMaterializations();
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::recountMaterializations()
{
    m_isMaterialized &= ~m_active;
    m_isMaterializedTwice &= ~m_active;
    m_isMaterializedThrice &= ~m_active;
    m_sites.shrink(0);
    for (uint32_t position : m_visitedPositions)
        visit(position);
}

template<typename Mask>
bool AllocationPrefilter::Solver<Mask>::dropUnprofitableCandidates()
{
    uint64_t maxNumberOfMaterializations = m_prefilter.m_sinking.limit(LimitID::MaxMaterializations);
    auto isFrameAllocatableBit = [&](uint32_t bit) {
        return isFrameAllocatable(m_prefilter.m_sinking, m_prefilter.m_candidates[objectOf(bit)]);
    };

    Mask pays = m_diesVirtual | m_entersJoin | ~m_isMaterialized | (m_isMaterialized & ~m_isMaterializedTwice & (m_foldsStore | m_isStoredLate));
    m_counted = 0;
    m_counts.shrink(0);
    Traits::forEachBit(m_candidates & m_active & ~m_joins & m_isMaterializedTwice & (m_isMaterializedThrice | ~pays), [&](uint32_t bit) {
        bool mayExceedLimit = maxNumberOfMaterializations != 2 || has(m_isMaterializedThrice, bit);
        bool mayBeFrameAllocated = !has(pays, bit) && !isEmpty(m_component[bit] & m_isMaterializedByCall) && isFrameAllocatableBit(bit);
        if (!mayExceedLimit && !mayBeFrameAllocated)
            return;
        m_counted |= ofBit(bit);
        m_counts.append(Counts { bit, m_prefilter.m_objects[objectOf(bit)].block });
    });

    Mask exceedsLimit = 0;
    if (!m_counts.isEmpty()) {
        for (const Site& site : m_sites) {
            Traits::forEachBit(site.members & m_counted, [&](uint32_t bit) {
                count(bit, site.block, site.edge ? &*site.edge : nullptr, site.use);
            });
        }
        for (const Counts& counts : m_counts) {
            unsigned numberOfFrequentMaterializations = counts.numberOfMaterializations - counts.numberOfRarerMaterializations;
            if (numberOfFrequentMaterializations > maxNumberOfMaterializations)
                exceedsLimit |= ofBit(counts.bit);
            if (counts.numberOfRarerMaterializations && counts.numberOfBorrowingMaterializations && numberOfFrequentMaterializations == counts.numberOfBorrowingMaterializations && isFrameAllocatableBit(counts.bit))
                pays |= ofBit(counts.bit);
        }
    }

    Mask changed = 0;
    Traits::forEachBit(m_candidates & m_active & ~m_joins & ~m_materializedBeforeBranches & m_isMaterializedTwice & m_foldsStore & (exceedsLimit | ~pays) & ~m_abandoned, [&](uint32_t bit) {
        m_materializedBeforeBranches |= ofBit(bit);
        changed |= m_component[bit];
    });
    Mask dropped = m_candidates & m_active & ~changed & (((~pays | exceedsLimit) & ~m_unmodeled & ~m_materializedBeforeBranches) | m_abandoned);
    m_candidates &= ~dropped;
    m_exceededLimit |= dropped & exceedsLimit;
    // A join that does nothing but enter joins that are dropped would be dropped in the next round.
    Mask onlyEntersJoins = m_candidates & m_active & ~changed & m_joins & m_entersJoin & ~(m_diesVirtual | m_foldsStore | m_isStoredLate | m_unmodeled | m_isMaterialized);
    for (bool mayDropMore = !isEmpty(dropped & m_joins) && !isEmpty(onlyEntersJoins); mayDropMore;) {
        mayDropMore = false;
        Mask entersCandidate = 0;
        for (const ParameterLink& link : m_chunk.links) {
            if (has(m_candidates, bitOf(link.parameter)))
                entersCandidate |= ofBit(bitOf(link.source));
        }
        Mask more = onlyEntersJoins & m_candidates & ~entersCandidate;
        if (!isEmpty(more)) {
            m_candidates &= ~more;
            dropped |= more;
            mayDropMore = true;
        }
    }
    Traits::forEachBit(dropped, [&](uint32_t bit) {
        if (!isEmpty(m_component[bit] & m_candidates))
            changed |= m_component[bit];
    });
    m_active = changed;
    return !isEmpty(changed);
}

template<typename Mask>
void AllocationPrefilter::Solver<Mask>::run()
{
    prepare();
    findComponents();
    findHeldObjects();
    findObjectsLeavingTheirBlocks();
    m_active = m_candidates;
    solveLiveness();
    Mask all = m_candidates;
    do
        propagateVirtualObjects();
    while (dropUnprofitableCandidates());

    // The analysis sets joins aside and takes them up again. What pays with them or without them is followed.
    Mask withoutJoins = 0;
    Traits::forEachBit(all & ~m_joins & ~m_candidates, [&](uint32_t bit) {
        if (!isEmpty(m_component[bit] & m_joins))
            withoutJoins |= m_component[bit];
    });
    if (!isEmpty(withoutJoins)) {
        Mask profitable = m_candidates;
        m_candidates = (m_candidates & ~withoutJoins) | (withoutJoins & ~m_joins);
        m_materializedBeforeBranches &= ~withoutJoins;
        m_active = withoutJoins;
        do
            propagateVirtualObjects();
        while (dropUnprofitableCandidates());
        m_candidates |= profitable;
    }

    for (uint32_t bit = 0; bit < m_chunk.numberOfBits; ++bit) {
        Object& object = m_prefilter.m_objects[objectOf(bit)];
        object.isProfitable = has(m_candidates, bit);
        object.exceedsMaterializationLimit = has(m_exceededLimit & ~m_candidates, bit);
    }
}

// An allocation that nothing holds, that holds nothing and that is passed to no block is materialized once if a use in its own block needs the cell.
void AllocationPrefilter::decideLocally()
{
    for (const ParameterLink& link : m_parameterLinks) {
        if (find(link.source) == find(link.parameter))
            continue;
        m_objects[find(link.source)].isLinked = true;
        m_objects[find(link.parameter)].isLinked = true;
    }
    bool hasDecided = false;
    for (uint32_t allocation = 0; allocation < m_numberOfAllocations; ++allocation) {
        Object& object = m_objects[allocation];
        if (object.isLinked)
            continue;
        if (object.isUnmodeled)
            object.isDecided = true;
        else if (object.firstMaterializationIndex != UINT32_MAX && !m_function.at(object.block).handler) {
            object.isProfitable = object.firstStoreIndex < object.firstMaterializationIndex;
            object.isDecided = true;
        }
        hasDecided |= object.isDecided;
    }
    if (hasDecided)
        removeDecidedObjects();
}

void AllocationPrefilter::removeDecidedObjects()
{
    m_events.removeAllMatching([&](const Event& event) {
        return m_objects[find(event.object)].isDecided;
    });
    m_uses.removeAllMatching([&](const ObjectUse& use) {
        return m_objects[find(use.object)].isDecided;
    });
    m_parameterLinks.removeAllMatching([&](const ParameterLink& link) {
        return m_objects[find(link.source)].isDecided;
    });
}

uint32_t AllocationPrefilter::findComponent(uint32_t object)
{
    while (m_objects[object].component != object) {
        m_objects[object].component = m_objects[m_objects[object].component].component;
        object = m_objects[object].component;
    }
    return object;
}

void AllocationPrefilter::partition()
{
    constexpr uint32_t bitsInWord = 64;

    for (uint32_t object = 0; object < m_objects.size(); ++object)
        m_objects[object].component = find(object);
    auto unite = [&](uint32_t a, uint32_t b) {
        uint32_t rootOfA = findComponent(find(a));
        uint32_t rootOfB = findComponent(find(b));
        if (rootOfA != rootOfB)
            m_objects[std::max(rootOfA, rootOfB)].component = std::min(rootOfA, rootOfB);
    };
    for (const ParameterLink& link : m_parameterLinks)
        unite(link.source, link.parameter);
    for (const Event& event : m_events) {
        if (event.kind == EventKind::Hold || event.kind == EventKind::StoreFromObject)
            unite(event.object, event.other);
    }

    Vector<std::tuple<uint32_t, uint32_t, uint32_t>, 16> roots;
    Vector<uint32_t, 16> firstPositionOfComponent(FillWith { }, m_objects.size(), UINT32_MAX);
    for (uint32_t object = 0; object < m_objects.size(); ++object) {
        if (find(object) == object)
            firstPositionOfComponent[findComponent(object)] = std::min(firstPositionOfComponent[findComponent(object)], positionOf(m_objects[object].block));
    }
    for (const ParameterLink& link : m_parameterLinks)
        addToTrace(find(link.source), mix(link.position - positionOf(m_objects[find(link.source)].block)) + positionOf(m_objects[find(link.parameter)].block) - link.position + (static_cast<uint64_t>(link.edge.isHandlerEdge()) << 40));
    for (uint32_t object = 0; object < m_objects.size(); ++object) {
        if (find(object) != object)
            continue;
        const Object& root = m_objects[object];
        uint64_t allocated = isAllocation(object) ? static_cast<uint64_t>(m_function.at(root.definition).opcode) + 1 : 0;
        uint64_t distance = positionOf(root.block) - firstPositionOfComponent[findComponent(object)];
        m_objects[findComponent(object)].traceOfComponent += mix(root.trace + mix(allocated | static_cast<uint64_t>(m_function.at(root.block).frequency.predicted()) << 16 | distance << 24));
    }
    if (m_tracesOfFruitlessComponents && !m_tracesOfFruitlessComponents->isEmpty()) {
        bool hasDecided = false;
        for (uint32_t object = 0; object < m_objects.size(); ++object) {
            Object& root = m_objects[object];
            if (find(object) != object || (root.isDecided && !root.isProfitable) || !std::ranges::binary_search(*m_tracesOfFruitlessComponents, m_objects[findComponent(object)].traceOfComponent))
                continue;
            root.isDecided = true;
            root.isProfitable = false;
            root.wasFruitlessBefore = true;
            hasDecided = true;
        }
        if (hasDecided)
            removeDecidedObjects();
    }
    for (uint32_t object = 0; object < m_objects.size(); ++object) {
        if (find(object) == object && !m_objects[object].isDecided)
            roots.append({ firstPositionOfComponent[findComponent(object)], findComponent(object), object });
    }
    AOT::sort(roots);

    for (size_t first = 0; first < roots.size();) {
        size_t end = first;
        while (end < roots.size() && std::get<1>(roots[end]) == std::get<1>(roots[first]))
            ++end;
        uint32_t size = end - first;
        if (m_chunks.isEmpty() || m_chunks.last().numberOfBits + size > bitsInWord)
            m_chunks.append(Chunk { });
        Chunk& chunk = m_chunks.last();
        for (size_t index = first; index < end; ++index) {
            Object& object = m_objects[std::get<2>(roots[index])];
            object.chunk = m_chunks.size() - 1;
            object.bit = chunk.numberOfBits++;
            chunk.objects.append(std::get<2>(roots[index]));
            chunk.firstPosition = std::min(chunk.firstPosition, positionOf(object.block));
        }
        first = end;
    }
    for (uint32_t object = 0; object < m_objects.size(); ++object) {
        m_objects[object].chunk = m_objects[find(object)].chunk;
        m_objects[object].bit = m_objects[find(object)].bit;
    }

    for (Event& event : m_events)
        event.chunk = m_objects[event.object].chunk;
    for (ParameterLink& link : m_parameterLinks)
        link.chunk = m_objects[link.source].chunk;
    for (ObjectUse& use : m_uses)
        use.chunk = m_objects[use.object].chunk;
    AOT::sort(m_events, { }, [](const Event& event) {
        return std::tuple { event.chunk, event.position, event.index, event.kind };
    });
    AOT::sort(m_parameterLinks, { }, [](const ParameterLink& link) {
        return std::tuple { link.chunk, link.position, link.edge.targetIndex, link.parameter, link.source };
    });
    if (m_chunks.size() > 1)
        AOT::sort(m_uses, { }, &ObjectUse::chunk);
    auto spanOfChunk = [](auto& list, uint32_t chunk) {
        auto range = std::ranges::equal_range(list, chunk, { }, [](const auto& entry) {
            return entry.chunk;
        });
        return std::span { range.begin(), range.end() };
    };
    for (uint32_t index = 0; index < m_chunks.size(); ++index) {
        m_chunks[index].events = spanOfChunk(m_events, index);
        m_chunks[index].links = spanOfChunk(m_parameterLinks, index);
        m_chunks[index].uses = spanOfChunk(m_uses, index);
    }
}

void AllocationPrefilter::run()
{
    collectNames();
    uniteAliases();
    findKinds();
    classifyUses();
    for (uint32_t allocation = 0; allocation < m_numberOfAllocations; ++allocation) {
        const VirtualObjectDescription& description = m_described[allocation].description;
        if (description.staysInPlace || description.absentResult || !description.canBeBuilt)
            m_objects[allocation].isUnmodeled = true;
    }
    decideLocally();
    partition();

    for (const Chunk& chunk : m_chunks) {
        if (chunk.numberOfBits <= 64)
            Solver<uint64_t>(*this, chunk).run();
        else if (chunk.numberOfBits <= 1024)
            Solver<WidePrefilterMask<16>>(*this, chunk).run();
    }
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

// One value carries a field for every variant of a join. What is read from a literal array is cast to what its variants hold.
static bool givesOneFieldSeveralTypes(const VirtualObjectContext& context, const VirtualFamily& family)
{
    std::span<const VirtualObjectDescription> descriptions = family.descriptions();
    if (descriptions.size() < 2 || std::ranges::all_of(descriptions, [](const VirtualObjectDescription& description) { return description.kind == VirtualObjectKind::Array; }))
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
    if (m_policy.leavesWhenNoAllocationPays && findsThatNoAllocationPays(described.span()))
        return false;

    collectJoins();
    m_inputs.isLargeForItsWork = isLargeForItsWork();
    Vector<uint32_t> rootOfCandidate;
    uniteFamilies(rootOfCandidate);
    createFamilies(described.span(), rootOfCandidate.span());
    if (m_policy.leavesWhenNoAllocationPays) {
        abandonAllocationsBuiltAtOnce();
        if (!hasCandidates())
            return false;
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

            DescribedObject described = describeVirtualObject(m_analyses, id, m_policy.followsUndefinedAndNullInFields ? ListedValues::MayAlsoBeUndefinedOrNull : ListedValues::HaveTypesOfFields);
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

bool AllocationSinking::findsThatNoAllocationPays(std::span<const DescribedObject> described)
{
    RELEASE_ASSERT(m_policy.returnedObjects == ReturnedObjects::AreBuilt);
    AllocationPrefilter prefilter(*this, m_candidates.span(), described, m_candidateOfValue, m_policy.tracesOfFruitlessAllocations);
    prefilter.run();
    m_tracesOfAllocations = Vector<uint64_t>(m_candidates.size(), [&](size_t index) {
        return prefilter.traceOf(index);
    });
    bool someAllocationIsLeft = false;
    for (uint32_t index = 0; index < m_candidates.size(); ++index) {
        Candidate& candidate = m_candidates[index];
        if (std::optional<Use> use = prefilter.firstMaterializationOf(index); use && context().build.options().collectsDeclines())
            candidate.firstCause = describeCause(m_analyses, *use);
        candidate.wasFruitlessBefore = prefilter.wasFruitlessBefore(index);
        candidate.isLeftOut = prefilter.isUnprofitable(index);
        if (prefilter.exceedsMaterializationLimit(index))
            abandon(VirtualObjectID(index), LimitID::MaxMaterializations);
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
        return buildsOperand(m_program, user, firstUse->operandIndex);
    case Interpretation::Read:
    case Interpretation::Store: {
        ValueID accessed = sameObjectThroughCasts(m_function, user.operand(0)).valueInFrontOfFacts();
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
    AOT::sort(traces);
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

bool AllocationSinking::mayGetTests(BlockID block) const
{
    BlocksThatMayGetTests& blocks = *m_policy.blocksThatMayGetTests;
    if (!blocks.asked.set(block.index()))
        blocks.answers.set(block.index(), mayPlaceTestsOfAccessesByName(blocks.heat, m_analyses, block));
    return blocks.answers.get(block.index());
}

// A store by name waits for PlaceTypeTests: the value that fits then goes on as a value of the field's type, and only the rare arm keeps the store by name.
bool AllocationSinking::storesByNameWaitIn(BlockID block) const
{
    if (m_inputs.isLargeForItsWork)
        return true;
    if (m_policy.blocksThatMayGetTests && !mayGetTests(block))
        return false;
    return !m_inputs.hasOperationsOnSlots || m_function.at(block).frequency.predicted() == BlockFrequency::Normal;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
