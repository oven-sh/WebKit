/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlattenInnerObjects.h"

#if ENABLE(AOT)

#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCFG.h"
#include "AOTCallGraph.h"
#include "AOTEdgeResults.h"
#include "AOTFlowRecords.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTFunctionKinds.h"
#include "AOTIRBuilder.h"
#include "AOTIRProgram.h"
#include "AOTMemoryLocation.h"
#include "AOTNarrowFieldTypes.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSiteWeight.h"
#include "AOTThreads.h"
#include "AOTUseLists.h"
#include "AOTValueIdentity.h"
#include "AOTValueSubstitution.h"
#include <algorithm>
#include <optional>
#include <ranges>
#include <wtf/BitVector.h>
#include <wtf/Vector.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

static_assert(OperandIndex::GetSlot::object == OperandIndex::GetFlattenedSlot::object && OperandIndex::GetSlot::guard == OperandIndex::GetFlattenedSlot::guard);
static_assert(OperandIndex::GetSlotOrUndefined::object == OperandIndex::GetFlattenedSlotOrUndefined::object && OperandIndex::GetSlotOrUndefined::guard == OperandIndex::GetFlattenedSlotOrUndefined::guard);

namespace {

// How an allocation of the inner layout lists its fields.
struct Birth {
    Vector<uint32_t, 8> fields;
    AllocationClassID allocationClass;
};

struct Candidate {
    bool isFlattened() const { return !isRefuted || keepsSlot; }

    FieldReference field;
    LayoutID innerLayout;
    PropertyKeyID key;
    // Somebody may hold the inner object, or store another one: the field does not go away.
    bool isRefuted { false };
    bool mayKeepSlot { false };
    // The field stays, and its slot says whether the inner object still lies in the outer one. Whoever wants it as a value makes it first.
    bool keepsSlot { false };
    // The field of the outer layout that stands for the first field of the inner one. The others follow it.
    uint32_t firstField { 0 };
    Vector<PropertyKeyID> keys;
    std::optional<Birth> birth;
};

class Candidates {
public:
    explicit Candidates(const Program& program)
        : m_ofLayout(program.layouts.size())
    {
    }

    bool isEmpty() const { return m_list.isEmpty(); }
    uint32_t size() const { return m_list.size(); }
    Candidate& at(uint32_t index) { return m_list[index]; }
    const Candidate& at(uint32_t index) const { return m_list[index]; }

    void add(Candidate&& candidate)
    {
        m_ofLayout[candidate.field.layout.index()].append(m_list.size());
        m_keys.set(candidate.key.index());
        m_list.append(WTF::move(candidate));
    }

    std::span<const uint32_t> ofLayout(LayoutID layout) const { return m_ofLayout[layout.index()].span(); }
    bool isOuter(LayoutID layout) const { return !m_ofLayout[layout.index()].isEmpty(); }
    bool namesCandidate(PropertyKeyID key) const { return m_keys.get(key.index()); }

    std::optional<uint32_t> find(LayoutID layout, PropertyKeyID key) const
    {
        for (uint32_t index : ofLayout(layout)) {
            if (m_list[index].key == key)
                return index;
        }
        return std::nullopt;
    }

    void forEachNamed(PropertyKeyID key, const Invocable<void(uint32_t)> auto& functor) const
    {
        for (uint32_t index = 0; index < m_list.size(); ++index) {
            if (m_list[index].key == key)
                functor(index);
        }
    }

    std::optional<uint32_t> find(FieldReference field) const
    {
        for (uint32_t index : ofLayout(field.layout)) {
            if (m_list[index].field.field == field.field)
                return index;
        }
        return std::nullopt;
    }

private:
    Vector<Candidate> m_list;
    Vector<Vector<uint32_t, 1>> m_ofLayout;
    BitVector m_keys;
};

bool isPlainObjectLayout(const Layout& layout)
{
    if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance)
        return false;
    return !layout.base && !layout.isDerived() && layout.flow.baseState == BaseState::None && layout.flow.members.size() <= 1 && !layout.computedKeyCount;
}

bool isPlainDataField(const Field& field)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsAlsoInPrototype, FieldFlags::IsInherited, FieldFlags::IsAddedLate, FieldFlags::IsRemovedAsUnread };
    return field.key && !field.computedKey && !field.flags.containsAny(excludedFlags) && field.form.isEmpty() && field.flow.location.deferral == ClosureDeferral::Unknown;
}

bool holdsObjectThatIsOnlyAccessed(const Layout& layout, const Field& field)
{
    const LocationFlow& location = field.flow.location;
    constexpr OptionSet<ObjectMutation> mutationsThatShow { ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript, ObjectMutation::ChangeIntegrity };
    return location.writers == WriterState::AllKnown && location.stores == StoreCount::AtMostOnce && location.storeNaming == StoreNaming::EveryStoreNamesLocation
        && location.initialization == InitializationState::SetAtCreation && location.reads == ReadState::OnlyAccessed
        && field.flow.stability == everyFlagOf<FieldStability>() && layout.flow.impossibleMutations.containsAll(mutationsThatShow) && layout.flow.observedUses.isEmpty()
        && layout.flow.keys == KeyEnumeration::NeverEnumerated;
}

bool canBecomeFieldOfOuterLayout(const Field& field)
{
    constexpr OptionSet<FieldFlags> excludedFlags { FieldFlags::IsMethod, FieldFlags::IsBrand, FieldFlags::IsHidden, FieldFlags::IsAlsoInPrototype, FieldFlags::IsInherited, FieldFlags::IsAddedLate };
    return field.key && !field.computedKey && !field.flags.containsAny(excludedFlags) && field.form.isEmpty() && field.flow.location.deferral == ClosureDeferral::Unknown;
}

// An outer object that gets another object after all has the room for nothing.
constexpr size_t maxFieldsOfObjectFlattenedUntilHeld = 8;

bool canBeOuterLayout(const Layout& layout)
{
    return isPlainObjectLayout(layout) && layout.flow.allocation != AllocationState::NeverAllocated && layout.innerObjects.isEmpty();
}

// The one layout whose objects the field holds, beside undefined and null, if such an object can do without a cell.
LayoutID layoutThatMayLieIn(const Program& program, FieldReference reference)
{
    const Field& field = program.at(reference.layout).fields[reference.field];
    if (!isPlainDataField(field))
        return { };
    Type type = program.at(slotTypeOf(program, field)).withoutPrimitives(Primitive::UndefinedOrNull);
    LayoutID innerLayout = type.hasOnlyLayouts(program) ? type.singleLayout(program) : LayoutID();
    if (!innerLayout || innerLayout == reference.layout)
        return { };
    const Layout& inner = program.at(innerLayout);
    return isPlainObjectLayout(inner) && std::ranges::all_of(inner.fields, canBecomeFieldOfOuterLayout) ? innerLayout : LayoutID();
}

Candidates findCandidates(const Program& program, bool mayKeepSlots, bool logsDecisions)
{
    Candidates candidates(program);
    if (logsDecisions) [[unlikely]] {
        for (uint32_t index = 0; index < program.layouts.size(); ++index) {
            const Layout& layout = program.layouts[index];
            if ((layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance) || layout.flow.allocation == AllocationState::NeverAllocated)
                continue;
            for (uint32_t field = 0; field < layout.fields.size(); ++field) {
                const Field& record = layout.fields[field];
                Type type = program.at(record.declared.type).withoutPrimitives(Primitive::UndefinedOrNull);
                if (!record.key || !type.hasOnlyLayouts(program) || !type.singleLayout(program) || record.flags.contains(FieldFlags::IsInherited))
                    continue;
                const LocationFlow& location = record.flow.location;
                dataLogLn("flattened objects: field L", index, ".", field, " of L", type.singleLayout(program).index(),
                    program.at(record.declared.type).primitives ? " nullable" : "", isPlainObjectLayout(layout) ? "" : " outer-not-plain", isPlainObjectLayout(program.at(type.singleLayout(program))) ? "" : " inner-not-plain",
                    location.writers == WriterState::AllKnown ? "" : " writers", location.stores == StoreCount::AtMostOnce ? "" : " stores", location.initialization == InitializationState::SetAtCreation ? "" : " initialization",
                    location.reads == ReadState::OnlyAccessed ? "" : location.reads == ReadState::Unknown ? " reads" : " reads-other", record.flow.stability == everyFlagOf<FieldStability>() ? "" : " stability",
                    layout.flow.impossibleMutations.containsAll({ ObjectMutation::ReachNativeCode, ObjectMutation::ReachUnknownJavaScript }) ? "" : " reaches-unseen", layout.flow.impossibleMutations.contains(ObjectMutation::ChangeIntegrity) ? "" : " integrity",
                    layout.flow.observedUses.isEmpty() ? "" : " observed", layout.flow.keys == KeyEnumeration::NeverEnumerated ? "" : " enumerated");
            }
        }
    }
    // A layout below another has the fields of that one, at the same places. Flow finds the base of a class that extends a value.
    BitVector hasLayoutsBelow(program.layouts.size());
    for (const Layout& layout : program.layouts) {
        if (layout.base)
            hasLayoutsBelow.set(layout.base.index());
        if (layout.flow.base)
            hasLayoutsBelow.set(layout.flow.base.index());
        for (const Field& field : layout.fields) {
            if (field.originLayout)
                hasLayoutsBelow.set(field.originLayout.index());
        }
    }
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        const Layout& layout = program.layouts[index];
        if (!canBeOuterLayout(layout) || hasLayoutsBelow.get(index))
            continue;
        for (uint32_t field = 0; field < layout.fields.size(); ++field) {
            LayoutID innerLayout = layoutThatMayLieIn(program, { LayoutID(index), field });
            if (!innerLayout || hasLayoutsBelow.get(innerLayout.index()))
                continue;
            bool isNeverHeld = holdsObjectThatIsOnlyAccessed(layout, layout.fields[field]) && !program.at(slotTypeOf(program, layout.fields[field])).primitives;
            bool mayKeepSlot = mayKeepSlots && program.at(innerLayout).fields.size() <= maxFieldsOfObjectFlattenedUntilHeld;
            if (isNeverHeld || mayKeepSlot)
                candidates.add({ { LayoutID(index), field }, innerLayout, layout.fields[field].key, !isNeverHeld, mayKeepSlot, false, 0, { }, std::nullopt });
        }
    }
    return candidates;
}

bool mayBeFlattenedBirth(const Value& value)
{
    return value.opcode == Opcode::NewObject && value.storage() == AllocationStorage::Heap && !value.prototypeOperand() && !value.outerLayout() && !value.takesCellOfInnerObject();
}

bool isAccessOfSlot(Opcode opcode)
{
    return opcode == Opcode::GetSlot || opcode == Opcode::GetSlotOrUndefined || opcode == Opcode::PutSlot;
}

bool hasOnlyReceiver(const Program& program, const Value& access, LayoutID layout)
{
    if (!access.receivers())
        return false;
    const LayoutSet& receivers = program.at(access.receivers());
    return receivers.isComplete && !receivers.hasMembersWithoutLayout && receivers.members.size() == 1 && receivers.members[0] == layout;
}

// The field of the inner layout that the operation reads or stores, where it does nothing else with the inner object.
std::optional<uint32_t> fieldAccessedIn(const Program& program, const Value& user, ValueID innerObject, LayoutID innerLayout)
{
    if (!isAccessOfSlot(user.opcode) || user.objectOperand() != innerObject || user.guardOperand() || !hasOnlyReceiver(program, user, innerLayout))
        return std::nullopt;
    if (user.opcode == Opcode::PutSlot && user.valueOperand() == innerObject)
        return std::nullopt;
    return program.at(innerLayout).findField(user.key());
}

// Walks a function once, in the order of its blocks, in which an operation stands behind what it uses.
template<typename Visitor>
class Walk {
public:
    Walk(const Program& program, const Candidates& candidates, const Function& function, Visitor& visitor)
        : m_program(program)
        , m_candidates(candidates)
        , m_function(function)
        , m_visitor(visitor)
    {
    }

    void run()
    {
        for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
            const Block& block = m_function.blocks[blockIndex];
            for (ValueID id : block.values)
                visit(BlockID(blockIndex), id);
            block.forEachChildOutsideValues([&](ValueID child) {
                if (const InnerObject* inner = find(child))
                    m_visitor.refute(inner->candidate);
            });
        }
    }

private:
    struct InnerObject {
        ValueID value;
        uint32_t candidate { 0 };
        ValueID outerObject;
        LayoutSetID receiversOfRead;
    };

    const InnerObject* find(ValueID value) const
    {
        if (!value || !m_isInnerObject.get(value.index()))
            return nullptr;
        for (const InnerObject& inner : m_innerObjects) {
            if (inner.value == value)
                return &inner;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void add(InnerObject&& inner)
    {
        m_isInnerObject.set(inner.value.index());
        m_innerObjects.append(WTF::move(inner));
    }

    void visit(BlockID block, ValueID id)
    {
        const Value& value = m_function.at(id);
        visitUsesOfInnerObjects(id, value);
        if (value.opcode == Opcode::NewObject && m_candidates.isOuter(value.layout())) {
            visitBirth(block, id, value);
            return;
        }
        if (!Value::opcodesWithKey.contains(value.opcode) || !m_candidates.namesCandidate(value.key()))
            return;
        // An operation that goes by the name alone, as in the generic version of a function, may meet any object.
        if (!Value::opcodesWithReceivers.contains(value.opcode) || !value.receivers() || !m_program.at(value.receivers()).isComplete) {
            m_candidates.forEachNamed(value.key(), [&](uint32_t candidate) {
                m_visitor.refute(candidate);
            });
            return;
        }
        for (LayoutID receiver : m_program.at(value.receivers()).members) {
            std::optional<uint32_t> candidate = m_candidates.find(receiver, value.key());
            if (!candidate)
                continue;
            if (value.opcode == Opcode::GetSlot && !value.guardOperand() && hasOnlyReceiver(m_program, value, receiver)) {
                add({ id, *candidate, value.objectOperand(), value.receivers() });
                m_visitor.didFindRead(*candidate, id);
            } else
                m_visitor.didFindOtherOperation(*candidate, block, id);
        }
    }

    void visitUsesOfInnerObjects(ValueID id, const Value& value)
    {
        if (m_innerObjects.isEmpty())
            return;
        value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            const InnerObject* inner = find(operand);
            if (!inner)
                return;
            const Candidate& candidate = m_candidates.at(inner->candidate);
            if (isCast(value.opcode) && operandUsesOf(value.opcode, operandIndex).contains(OperandUse::Returns)) {
                InnerObject same = *inner;
                same.value = id;
                add(WTF::move(same));
                m_visitor.didFindCast(inner->candidate, id);
                return;
            }
            std::optional<uint32_t> field = fieldAccessedIn(m_program, value, operand, candidate.innerLayout);
            if (!field) {
                m_visitor.refute(inner->candidate);
                return;
            }
            m_visitor.didFindAccess(inner->candidate, id, *field, inner->outerObject, inner->receiversOfRead);
        });
    }

    void visitBirth(BlockID block, ValueID id, const Value& value)
    {
        for (uint32_t index : m_candidates.ofLayout(value.layout())) {
            const Candidate& candidate = m_candidates.at(index);
            auto fields = value.fields();
            size_t position = fields.size();
            for (size_t listed = 0; listed < position; ++listed) {
                if (fields[listed] == candidate.field.field)
                    position = listed;
            }
            if (position >= fields.size() || !mayBeFlattenedBirth(value)) {
                m_visitor.didFindBirthWithoutField(index);
                continue;
            }
            m_visitor.didFindBirth(index, block, id, static_cast<uint32_t>(position));
        }
    }

    const Program& m_program;
    const Candidates& m_candidates;
    const Function& m_function;
    Visitor& m_visitor;
    BitVector m_isInnerObject;
    Vector<InnerObject, 8> m_innerObjects;
};

Vector<uint32_t> countUses(const Function& function)
{
    Vector<uint32_t> counts(FillWith { }, function.values.size(), 0u);
    for (const Block& block : function.blocks) {
        for (ValueID id : block.values) {
            function.at(id).forEachOperand([&](ValueID operand, uint32_t) {
                ++counts[operand.index()];
            });
        }
        block.forEachChildOutsideValues([&](ValueID child) {
            ++counts[child.index()];
        });
    }
    return counts;
}

BlockID blockOf(const Function& function, ValueID wanted)
{
    for (uint32_t index = 0; index < function.blocks.size(); ++index) {
        if (function.blocks[index].values.contains(wanted))
            return BlockID(index);
    }
    return { };
}

// Finds the objects that a function makes for one holder alone.
class FreshObjects {
public:
    explicit FreshObjects(const Function& function)
        : m_function(function)
    {
    }

    // What the holder, an allocation or a store in `blockOfHolder`, takes was made for it alone.
    ValueID findObjectMadeFor(const Candidate& candidate, BlockID blockOfHolder, ValueID held)
    {
        ValueID object = sameObjectThroughCasts(m_function, held);
        const Value& allocation = m_function.at(object);
        if (!mayBeFlattenedBirth(allocation) || allocation.layout() != candidate.innerLayout)
            return { };
        if (!m_useCounts)
            m_useCounts = countUses(m_function);
        for (ValueID link : CastChain(m_function, held)) {
            if ((*m_useCounts)[link.index()] != 1)
                return { };
        }
        BlockID blockOfObject = blockOf(m_function, object);
        if (!blockOfObject)
            return { };
        if (blockOfObject != blockOfHolder) {
            if (!m_cfg)
                m_cfg = makeUnique<CFG>(m_function);
            if (!m_cfg->isReachable(blockOfHolder) || !m_cfg->isReachable(blockOfObject) || m_cfg->isInCycleAvoiding(blockOfHolder, blockOfObject))
                return { };
        }
        return object;
    }

private:
    const Function& m_function;
    std::optional<Vector<uint32_t>> m_useCounts;
    std::unique_ptr<CFG> m_cfg;
};

// Whoever puts the marker for undefined or null where a raw value lies from birth marks the cell. An object without a cell has no such mark.
bool mayStoreMarkerOverRawValue(const Program& program, const Function& function, FieldReference field, ValueID stored)
{
    return holdsRawValueFromBirth(program, field) && program.typeOf(function, stored).mayBeUndefinedOrNull();
}

// So does an allocation that leaves out a field of birth.
bool leavesCellUnmarked(const Program& program, const Function& function, const Value& allocation)
{
    auto listed = allocation.fields();
    auto values = allocation.valuesOperands();
    for (uint32_t field = 0; field < program.at(allocation.layout()).fields.size(); ++field) {
        if (isFilledAtBirth(program, { allocation.layout(), field }) && std::ranges::find(listed, field) == listed.end())
            return false;
    }
    for (size_t index = 0; index < listed.size(); ++index) {
        if (mayStoreMarkerOverRawValue(program, function, { allocation.layout(), listed[index] }, values[index]))
            return false;
    }
    return true;
}

Birth birthOf(const Value& allocation)
{
    return { Vector<uint32_t, 8>(allocation.fields()), allocation.allocationClass() };
}

// The births of one candidate, by the fields that they list.
class BirthsOfCandidate {
public:
    void add(const FlowResults& flow, const Birth& birth)
    {
        for (Entry& entry : m_entries) {
            if (entry.birth.fields != birth.fields)
                continue;
            ++entry.numberOfPlaces;
            // An object that keeps the order of its keys serves where nobody asks for the order.
            if (entry.birth.allocationClass && (!birth.allocationClass || (neverEnumeratesKeys(flow, entry.birth.allocationClass) && !neverEnumeratesKeys(flow, birth.allocationClass))))
                entry.birth.allocationClass = birth.allocationClass;
            return;
        }
        m_entries.append({ birth, 1 });
    }

    std::optional<Birth> takeMostFrequent()
    {
        Entry* best = nullptr;
        for (Entry& entry : m_entries) {
            if (!best || entry.numberOfPlaces > best->numberOfPlaces)
                best = &entry;
        }
        return best ? std::optional<Birth>(WTF::move(best->birth)) : std::nullopt;
    }

private:
    struct Entry {
        Birth birth;
        unsigned numberOfPlaces { 0 };
    };

    static bool neverEnumeratesKeys(const FlowResults& flow, AllocationClassID id) { return flow.at(id).keys == KeyEnumeration::NeverEnumerated; }

    Vector<Entry, 1> m_entries;
};

class Scanner {
public:
    Scanner(const Program& program, const Candidates& candidates, FunctionID id)
        : m_program(program)
        , m_candidates(candidates)
        , m_function(program.at(id))
        , m_freshObjects(m_function)
    {
    }

    struct FoundBirth {
        uint32_t candidate { 0 };
        Birth birth;
    };

    struct Result {
        Vector<uint32_t, 4> refuted;
        // An operation goes to the slot of the field in a way that a slot which defers an object does not serve.
        Vector<uint32_t, 4> cannotKeepSlot;
        Vector<uint32_t, 4> found;
        Vector<FoundBirth, 2> births;
    };

    Result run()
    {
        Walk<Scanner>(m_program, m_candidates, m_function, *this).run();
        return WTF::move(m_result);
    }

    void refute(uint32_t candidate)
    {
        if (!m_result.refuted.contains(candidate))
            m_result.refuted.append(candidate);
    }

    void didFindRead(uint32_t candidate, ValueID) { note(candidate); }
    void didFindCast(uint32_t, ValueID) { }
    void didFindAccess(uint32_t, ValueID, uint32_t, ValueID, LayoutSetID) { }

    void didFindOtherOperation(uint32_t candidate, BlockID block, ValueID id)
    {
        note(candidate);
        refute(candidate);
        const Value& value = m_function.at(id);
        constexpr OpcodeSet servedBySlotThatDefers { Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::PutSlot, Opcode::PutSlotOrAdd, Opcode::IsSlotFilled };
        bool hasOneReceiver = hasOnlyReceiver(m_program, value, m_candidates.at(candidate).field.layout);
        bool goesToSlot = familyOf(value.opcode) == OperationFamily::SlotAccess || familyOf(value.opcode) == OperationFamily::Guard;
        if (goesToSlot && (!hasOneReceiver || !servedBySlotThatDefers.contains(value.opcode)) && !m_result.cannotKeepSlot.contains(candidate))
            m_result.cannotKeepSlot.append(candidate);
        if (value.opcode == Opcode::PutSlot && hasOneReceiver)
            noteBirth(candidate, m_freshObjects.findObjectMadeFor(m_candidates.at(candidate), block, value.valueOperand()));
    }

    void didFindBirthWithoutField(uint32_t candidate)
    {
        note(candidate);
        refute(candidate);
    }

    void didFindBirth(uint32_t candidate, BlockID block, ValueID id, uint32_t position)
    {
        note(candidate);
        ValueID object = m_freshObjects.findObjectMadeFor(m_candidates.at(candidate), block, m_function.at(id).valuesOperands()[position]);
        if (!object)
            refute(candidate);
        noteBirth(candidate, object);
    }

private:
    void note(uint32_t candidate)
    {
        if (!m_result.found.contains(candidate))
            m_result.found.append(candidate);
    }

    void noteBirth(uint32_t candidate, ValueID object)
    {
        if (object && leavesCellUnmarked(m_program, m_function, m_function.at(object)))
            m_result.births.append({ candidate, birthOf(m_function.at(object)) });
    }

    const Program& m_program;
    const Candidates& m_candidates;
    const Function& m_function;
    FreshObjects m_freshObjects;
    Result m_result;
};

// The layouts of which something but a NewObject may make an object, or into whose objects something copies the fields of another.
BitVector findLayoutsMadeOtherwise(const Program& program, BuildContext& build)
{
    constexpr OpcodeSet namesLayoutWithoutMakingObjects {
        Opcode::GetEnvironmentField, Opcode::PutEnvironmentField, Opcode::MaterializeEnvironment, Opcode::NewClass, Opcode::GetClassPrototype, Opcode::DefineMethod, Opcode::FinishClass,
        Opcode::NewClassToken, Opcode::GetConstructPrototype, Opcode::PrivateGet, Opcode::PrivateSet, Opcode::PrivateDefine, Opcode::PrivateHas, Opcode::HasKeyOrder
    };
    OpcodeSet makesObjectsOtherwise;
    Value::opcodesWithLayout.forEach([&](Opcode opcode) {
        if (!namesLayoutWithoutMakingObjects.contains(opcode) && opcode != Opcode::NewObject)
            makesObjectsOtherwise.add(opcode);
    });
    Vector<uint32_t> found = collectInParallel<uint32_t>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<uint32_t>& layouts) {
        forEachListedName(program, FunctionID(index), makesObjectsOtherwise, [&](BlockID, ValueID, Opcode, NameKind kind, uint32_t name) {
            if (kind == NameKind::Layout)
                layouts.append(name);
        });
    });
    BitVector result(program.layouts.size());
    for (uint32_t layout : found)
        result.set(layout);
    for (const ExpectedResult& expected : program.expectedResults) {
        for (LayoutID layout : expected.layouts)
            result.set(layout.index());
    }
    for (const Data& data : program.data) {
        forEachDataNodeInPreorder(data.root, [&](uint32_t, const DataNode& node) {
            if (node.kind() == DataNode::Kind::Object && node.as<DataNode::Object>().layout)
                result.set(node.as<DataNode::Object>().layout.index());
        });
    }
    return result;
}

void addFieldsOfInnerObject(ProgramEditor& editor, Candidate& candidate)
{
    const Program& program = editor.program();
    String nameOfField = program.strings.toWTFString(program.at(candidate.key).text);
    uint32_t numberOfFields = program.at(candidate.innerLayout).fields.size();
    Vector<uint32_t, 8> added;
    for (uint32_t index = 0; index < numberOfFields; ++index) {
        Field field = program.at(candidate.innerLayout).fields[index];
        String name = makeString(nameOfField, '.', program.strings.toWTFString(program.at(field.key).text));
        field.key = editor.types().intern(PropertyKey { .kind = PropertyKeyKind::PrivateName, .text = editor.strings().intern(StringView(name)), .owner = candidate.field.layout });
        field.flags.add(FieldFlags::IsPrivate);
        field.originLayout = { };
        field.originField = std::nullopt;
        candidate.keys.append(field.key);
        uint32_t position = editor.atForHiddenFields(candidate.field.layout).appendField(WTF::move(field));
        if (!index)
            candidate.firstField = position;
        added.append(position);
    }

    // The fields are filled where the field was.
    for (FieldOrder& order : editor.atForHiddenFields(candidate.field.layout).flow.constructorOrders) {
        size_t position = order.fields.find(candidate.field.field);
        if (position == notFound)
            continue;
        order.fields.removeAt(position);
        order.fields.insertVector(position, added);
    }
    LocationFlow& location = editor.at(candidate.field).flow.location;
    location.stores = StoreCount::Never;
    location.reads = ReadState::NeverRead;
}

// The plan gives every object of the outer layout room for the slots of the inner one, and the field a slot that says whether the inner object was made.
void keepSlotOfInnerObject(ProgramEditor& editor, const Candidate& candidate)
{
    editor.at(candidate.field).flow.location.deferral = ClosureDeferral::FlattenedObject;
    editor.atForHiddenFields(candidate.field.layout).flattenedObjects.append({ candidate.field.field, candidate.innerLayout, Vector<uint32_t>(candidate.birth->fields.span()), candidate.birth->allocationClass });
}

// For the fields that go away.
class Rewriter {
public:
    Rewriter(const Program& program, const Candidates& candidates, Function& function)
        : m_program(program)
        , m_candidates(candidates)
        , m_function(function)
        , m_isErased(function.values.size())
    {
    }

    uint64_t run()
    {
        Walk<Rewriter>(m_program, m_candidates, m_function, *this).run();
        for (const Access& access : m_accesses) {
            Value& value = m_function.edit(access.value);
            value.setObjectOperand(access.outerObject);
            value.setKey(access.key);
            value.setReceivers(access.receivers);
        }
        for (ValueID birth : m_births)
            rebuild(birth);
        for (Block& block : m_function.editAllBlocks()) {
            block.values.removeAllMatching([&](ValueID id) {
                return m_isErased.quickGet(id.index());
            });
        }
        return m_births.size();
    }

    void refute(uint32_t candidate) { RELEASE_ASSERT(m_candidates.at(candidate).isRefuted); }
    void didFindOtherOperation(uint32_t candidate, BlockID, ValueID) { refute(candidate); }
    void didFindBirthWithoutField(uint32_t candidate) { refute(candidate); }

    void didFindRead(uint32_t candidate, ValueID id) { eraseUnlessRefuted(candidate, id); }
    void didFindCast(uint32_t candidate, ValueID id) { eraseUnlessRefuted(candidate, id); }

    void didFindAccess(uint32_t candidate, ValueID id, uint32_t field, ValueID outerObject, LayoutSetID receivers)
    {
        if (!m_candidates.at(candidate).isRefuted)
            m_accesses.append({ id, outerObject, m_candidates.at(candidate).keys[field], receivers });
    }

    void didFindBirth(uint32_t candidate, BlockID, ValueID id, uint32_t)
    {
        if (m_candidates.at(candidate).isRefuted)
            return;
        if (m_births.isEmpty() || m_births.last() != id)
            m_births.append(id);
    }

private:
    struct Access {
        ValueID value;
        ValueID outerObject;
        PropertyKeyID key;
        LayoutSetID receivers;
    };

    void eraseUnlessRefuted(uint32_t candidate, ValueID id)
    {
        if (!m_candidates.at(candidate).isRefuted)
            m_isErased.quickSet(id.index());
    }

    void rebuild(ValueID birth)
    {
        Value& allocation = m_function.edit(birth);
        Vector<uint32_t, 16> fields;
        Vector<ValueID, 16> values;
        auto listedFields = allocation.fields();
        auto listedValues = allocation.valuesOperands();
        for (size_t index = 0; index < listedFields.size(); ++index) {
            std::optional<uint32_t> candidate = m_candidates.find(FieldReference { allocation.layout(), listedFields[index] });
            if (!candidate || m_candidates.at(*candidate).isRefuted) {
                fields.append(listedFields[index]);
                values.append(listedValues[index]);
                continue;
            }
            for (ValueID link : CastChain(m_function, listedValues[index]))
                m_isErased.quickSet(link.index());
            ValueID object = sameObjectThroughCasts(m_function, listedValues[index]);
            m_isErased.quickSet(object.index());
            const Value& inner = m_function.at(object);
            auto innerFields = inner.fields();
            auto innerValues = inner.valuesOperands();
            for (size_t position = 0; position < innerFields.size(); ++position) {
                fields.append(m_candidates.at(*candidate).firstField + innerFields[position]);
                values.append(innerValues[position]);
            }
        }
        allocation.setFields(fields.span());
        allocation.setVariadicOperands(values.span());
    }

    const Program& m_program;
    const Candidates& m_candidates;
    Function& m_function;
    BitVector m_isErased;
    Vector<Access, 8> m_accesses;
    Vector<ValueID, 2> m_births;
};

struct CountsOfSlotsKept {
    uint64_t births { 0 };
    uint64_t readsMakingObjects { 0 };
};

// What an operation does with the inner object that it takes, where the inner object is of this layout.
UseOfBorrowedObject useOfInnerObject(const Program& program, const Function& function, const Value& user, uint32_t operandIndex, ValueID innerObject, LayoutID innerLayout)
{
    constexpr OpcodeSet slotOperations { Opcode::GetSlot, Opcode::GetSlotOrUndefined, Opcode::PutSlot, Opcode::IsSlotFilled };
    UseOfBorrowedObject use = useOfBorrowedInnerObject(program, function, user, operandIndex, innerObject);
    if (use != UseOfBorrowedObject::Borrows || !slotOperations.contains(user.opcode))
        return use;
    std::optional<uint32_t> field = hasOnlyReceiver(program, user, innerLayout) ? program.at(innerLayout).findField(user.key()) : std::nullopt;
    if (!field)
        return UseOfBorrowedObject::Holds;
    if (user.opcode == Opcode::PutSlot && mayStoreMarkerOverRawValue(program, function, { innerLayout, *field }, user.valueOperand()))
        return UseOfBorrowedObject::Holds;
    return UseOfBorrowedObject::Borrows;
}

// A test proves something of the value that it names, to operations that name the same value or a cast of it.
bool isTiedToOtherOperations(const Value& user)
{
    return familyOf(user.opcode) == OperationFamily::Guard || (OperandPosition::guard(user.opcode) && user.guardOperand());
}

// In this order: each includes what the one before it allows.
enum class StateOfSlot : uint8_t {
    // The slot is as the read found it.
    Unchanged,
    // Somebody may have made the object. The field names the same object as before.
    MaybeMade,
    // Somebody may have stored to the field.
    MaybeStored,
};

// How often, by the estimate of heat, an outer object is born, an object is laid into an outer object, and a place runs that makes an object which was laid.
struct HeatOfSlotKept {
    uint32_t candidate { 0 };
    double outerObjectsBorn { 0 };
    double laidIntoOuterObject { 0 };
    double made { 0 };
};

// For the fields that stay.
class PlacesOfSlotsKept {
public:
    using State = StateOfSlot;
    struct UseAt {
        Use use;
        bool borrows { false };
        State state { State::Unchanged };
        uint32_t name { 0 };
    };
    // A value that stands for what the read gave: the read; a cast of a name; a parameter of a block that an edge gives a name.
    struct Name {
        ValueID value;
        uint32_t parent { 0 };
        BlockID isParameterOf;
        uint32_t parameterIndex { 0 };
        bool isNarrowed { false };
        bool meetsOthers { false };
        bool isBehindMeeting { false };
    };
    struct Read {
        ValueID value;
        uint32_t candidate { 0 };
        BlockID block;
        bool hasOneReceiver { false };
        // The object is made where the read stands, and everybody uses it.
        bool makesObject { true };
        // A name goes where this pass cannot follow.
        bool hasNameOutOfReach { false };
        Vector<Name, 4> names;
        Vector<UseAt, 4> uses;
    };
    // An allocation of the outer layout, or a store to the field, that takes a fresh object.
    struct Holder {
        ValueID value;
        BlockID block;
        ValueID held;
        ValueID object;
        uint32_t candidate { 0 };
    };

    PlacesOfSlotsKept(const Program& program, const Candidates& candidates, const AnalysisContext& context, const Function& function)
        : m_program(program)
        , m_candidates(candidates)
        , m_function(function)
        , m_analyses(context, function)
        , m_freshObjects(function)
    {
        findPlaces();
        for (Read& read : m_reads)
            findUses(read);
        for (Read& read : m_reads)
            followToUses(read);
        agreeOnLayoutsOfMeetings();
    }

    FunctionAnalyses& analyses() { return m_analyses; }
    const Vector<Read, 8>& reads() const { return m_reads; }
    const Vector<Holder, 4>& holders() const { return m_holders; }

    static bool isReadAgain(const UseAt& use) { return !use.borrows || use.state != State::Unchanged; }

    // For the log alone: what becomes of each read, and who wants the object.
    void dump(FunctionID id) const
    {
        for (const Read& read : m_reads) {
            const Candidate& candidate = m_candidates.at(read.candidate);
            dataLog("flattened objects: read of L", candidate.field.layout.index(), ".", candidate.field.field, " in f", id.index(), " v", read.value.index(), read.makesObject ? " MAKES the object" : " borrows");
            if (!read.hasOneReceiver)
                dataLog(", several receivers");
            if (read.hasNameOutOfReach)
                dataLog(", a name goes out of reach");
            for (const UseAt& use : read.uses) {
                bool isStored = use.state == State::MaybeStored;
                if (use.borrows && !isStored)
                    continue;
                dataLog("; ", use.borrows ? "borrowed" : "held", " by ");
                if (use.use.kind == Use::Kind::Operand)
                    dataLog(nameOf(m_function.at(use.use.user).opcode));
                else
                    dataLog(nameOf(m_function.at(use.use.block).terminator.kind), use.use.kind == Use::Kind::EdgeArgument ? " (argument)" : "");
                dataLog(" in b", use.use.block.index(), isStored ? " behind a possible store" : "");
            }
            dataLogLn();
        }
    }

    Vector<HeatOfSlotKept, 2> estimateHeat(const Heat& heat, FunctionID id)
    {
        Vector<HeatOfSlotKept, 2> result;
        if (m_reads.isEmpty() && m_holders.isEmpty() && m_birthsOfOuterObjects.isEmpty())
            return result;
        FunctionSiteWeights weights(m_program, heat, id, m_analyses);
        auto of = [&](uint32_t candidate) -> HeatOfSlotKept& {
            for (HeatOfSlotKept& entry : result) {
                if (entry.candidate == candidate)
                    return entry;
            }
            result.append({ candidate, 0, 0, 0 });
            return result.last();
        };
        for (const BirthOfOuterObject& birth : m_birthsOfOuterObjects)
            of(birth.candidate).outerObjectsBorn += weights.heatOf(birth.block);
        for (const Holder& holder : m_holders)
            of(holder.candidate).laidIntoOuterObject += weights.heatOf(holder.block);
        for (const Read& read : m_reads) {
            if (read.makesObject) {
                of(read.candidate).made += weights.heatOf(read.block);
                continue;
            }
            for (const UseAt& use : read.uses) {
                if (use.borrows)
                    continue;
                double heatOfUse = weights.heatOf(use.use.block);
                if (use.use.kind == Use::Kind::EdgeArgument)
                    heatOfUse = std::min(heatOfUse, weights.heatOf(m_function.at(use.use.block).terminator.targets[use.use.targetIndex].block));
                of(read.candidate).made += heatOfUse;
            }
        }
        return result;
    }

private:
    std::optional<uint32_t> candidateKeepingSlot(LayoutID layout, PropertyKeyID key) const
    {
        std::optional<uint32_t> found = m_candidates.find(layout, key);
        return found && m_candidates.at(*found).keepsSlot ? found : std::nullopt;
    }

    void findPlaces()
    {
        for (uint32_t blockIndex = 0; blockIndex < m_function.blocks.size(); ++blockIndex) {
            BlockID block(blockIndex);
            for (ValueID id : m_function.blocks[blockIndex].values) {
                const Value& value = m_function.at(id);
                if (value.opcode == Opcode::NewObject) {
                    findHoldersAtBirth(block, id, value);
                    continue;
                }
                if (!Value::opcodesWithKey.contains(value.opcode) || !m_candidates.namesCandidate(value.key()) || !Value::opcodesWithReceivers.contains(value.opcode) || !value.receivers())
                    continue;
                for (LayoutID receiver : m_program.at(value.receivers()).members) {
                    std::optional<uint32_t> candidate = candidateKeepingSlot(receiver, value.key());
                    if (!candidate)
                        continue;
                    bool hasOneReceiver = hasOnlyReceiver(m_program, value, receiver);
                    if (value.opcode == Opcode::GetSlot || value.opcode == Opcode::GetSlotOrUndefined) {
                        m_reads.append({ id, *candidate, block, hasOneReceiver, true, false, { }, { } });
                        break;
                    }
                    if (value.opcode == Opcode::PutSlot || value.opcode == Opcode::PutSlotOrAdd)
                        addHolder(block, id, *candidate, value.valueOperand(), hasOneReceiver && value.opcode == Opcode::PutSlot);
                }
            }
        }
    }

    void findHoldersAtBirth(BlockID block, ValueID id, const Value& value)
    {
        if (!m_candidates.isOuter(value.layout()))
            return;
        for (uint32_t candidate : m_candidates.ofLayout(value.layout())) {
            if (m_candidates.at(candidate).keepsSlot)
                m_birthsOfOuterObjects.append({ candidate, block });
        }
        auto fields = value.fields();
        for (size_t position = 0; position < fields.size(); ++position) {
            std::optional<uint32_t> candidate = m_candidates.find(FieldReference { value.layout(), fields[position] });
            if (candidate && m_candidates.at(*candidate).keepsSlot)
                addHolder(block, id, *candidate, value.valuesOperands()[position], mayBeFlattenedBirth(value));
        }
    }

    void addHolder(BlockID block, ValueID id, uint32_t index, ValueID held, bool hasRoom)
    {
        const Candidate& candidate = m_candidates.at(index);
        ValueID object = hasRoom ? m_freshObjects.findObjectMadeFor(candidate, block, held) : ValueID();
        if (object && std::ranges::equal(m_function.at(object).fields(), candidate.birth->fields) && leavesCellUnmarked(m_program, m_function, m_function.at(object)))
            m_holders.append({ id, block, held, object, index });
    }

    void findUses(Read& read)
    {
        const UseLists& useLists = m_analyses.useLists();
        LayoutID innerLayout = m_candidates.at(read.candidate).innerLayout;
        read.names.append({ read.value, 0, { }, 0, false, false, false });
        for (uint32_t index = 0; index < read.names.size(); ++index) {
            ValueID name = read.names[index].value;
            for (const Use& use : useLists.usesOf(name)) {
                bool isBehindMeeting = read.names[index].isBehindMeeting;
                if (use.kind == Use::Kind::EdgeArgument) {
                    BlockID target = m_function.at(use.block).terminator.targets[use.targetIndex].block;
                    ValueID parameter = m_function.at(target).parameters[use.operandIndex];
                    if (m_analyses.cfg().predecessors(target).size() == 1) {
                        read.names.append({ parameter, index, target, use.operandIndex, false, false, isBehindMeeting });
                        continue;
                    }
                    // The edge hands over where the slots lie, as whoever borrows the object would find them.
                    read.uses.append({ use, true, State::Unchanged, index });
                    bool isKnown = std::ranges::any_of(read.names, [&](const Name& known) {
                        return known.value == parameter;
                    });
                    if (!isKnown)
                        read.names.append({ parameter, index, target, use.operandIndex, false, true, true });
                    continue;
                }
                UseOfBorrowedObject kind = use.kind == Use::Kind::Operand ? useOfInnerObject(m_program, m_function, m_function.at(use.user), use.operandIndex, name, innerLayout) : UseOfBorrowedObject::Holds;
                if (kind == UseOfBorrowedObject::HandsOn) {
                    read.names.append({ use.user, index, { }, 0, false, false, isBehindMeeting });
                    continue;
                }
                read.uses.append({ use, kind == UseOfBorrowedObject::Borrows, State::Unchanged, index });
                if (use.kind != Use::Kind::Operand)
                    continue;
                const Value& user = m_function.at(use.user);
                if (kind == UseOfBorrowedObject::Borrows && provesOf(user.opcode) != Proof::None)
                    findNamesNarrowedBehind(read, index, use.user);
                // It would be given the object from a read of its own, and what it proves, or what was proven for it, is said of the names of this read.
                if (kind == UseOfBorrowedObject::Holds && (provesOf(user.opcode) != Proof::None || isTiedToOtherOperations(user)))
                    read.hasNameOutOfReach = true;
            }
        }
    }

    void findNamesNarrowedBehind(Read& read, uint32_t tested, ValueID test)
    {
        for (const Use& use : m_analyses.useLists().usesOf(test)) {
            if (use.kind != Use::Kind::TerminatorOperand)
                continue;
            const Terminator& terminator = m_function.at(use.block).terminator;
            for (uint32_t targetIndex = 0; targetIndex < terminator.targets.size(); ++targetIndex) {
                uint32_t first = firstEdgeResultParameter(m_function, use.block, targetIndex);
                for (uint32_t result = 0; result < numberOfEdgeResults(m_function, use.block, targetIndex); ++result) {
                    if (operandNarrowedOnEdge(m_function, { use.block, targetIndex, result }) != read.names[tested].value)
                        continue;
                    BlockID target = terminator.targets[targetIndex].block;
                    if (m_analyses.cfg().predecessors(target).size() != 1) {
                        read.hasNameOutOfReach = true;
                        continue;
                    }
                    read.names.append({ m_function.at(target).parameters[first + result], tested, target, first + result, true, false, read.names[tested].isBehindMeeting });
                }
            }
        }
    }

    static State worseOf(State a, State b) { return std::max(a, b); }

    State stateBehind(const Effects& effects, uint64_t mask, bool hasGuard) const
    {
        // A guard is good where it stands. Behind a write it may have ended.
        if (effects.mayRunOtherCode() || (effects.writes.maskOf(AbstractHeap::ObjectFields) & mask) || (hasGuard && !effects.writes.isEmpty()))
            return State::MaybeStored;
        // The run time makes the object when it reads the field, and to make it is to allocate.
        if (effects.mayCollect && (effects.reads.maskOf(AbstractHeap::ObjectFields) & mask))
            return State::MaybeMade;
        return State::Unchanged;
    }

    State stateBehind(ValueID id, PropertyKeyID key, uint64_t mask, bool hasGuard)
    {
        const Value& value = m_function.at(id);
        // Any of them may come to make the object.
        if ((value.opcode == Opcode::GetSlot || value.opcode == Opcode::GetSlotOrUndefined) && value.key() == key)
            return State::MaybeMade;
        return stateBehind(m_analyses.effectsOf(id).onAnyEdge(), mask, hasGuard);
    }

    // Finds the state of the slot at each use, over every path from the read, and with that what becomes of the read.
    void followToUses(Read& read)
    {
        const CFG& cfg = m_analyses.cfg();
        if (!read.hasOneReceiver || read.hasNameOutOfReach || !cfg.isReachable(read.block))
            return;
        PropertyKeyID key = m_candidates.at(read.candidate).key;
        uint64_t mask = maskOf(m_program, MemoryLocation::objectField(key));
        bool hasGuard = !!m_function.at(read.value).guardOperand();

        for (const UseAt& use : read.uses) {
            // Wanted wherever the block is left.
            if (!use.borrows && (use.use.kind == Use::Kind::HandlerArgument || use.use.kind == Use::Kind::BlockSubject))
                return;
        }
        // Whoever holds the object gets a read in front of it that makes the object. Another read of the field may have read the same outer object under another name, so whoever holds what that read gave counts too.
        BitVector holds(m_function.values.size());
        BitVector holdsAtEnd(m_function.blocks.size());
        for (const Read& other : m_reads) {
            if (m_candidates.at(other.candidate).key != key)
                continue;
            for (const UseAt& use : other.uses) {
                if (use.borrows)
                    continue;
                if (use.use.kind == Use::Kind::Operand)
                    holds.set(use.use.user.index());
                else
                    holdsAtEnd.set(use.use.block.index());
            }
        }

        // Each name by itself: what has happened to the slot since the run of the read whose value the name stands for.
        constexpr uint8_t standsForNothing = 0;
        auto carrying = [](State state) { return static_cast<uint8_t>(static_cast<uint8_t>(state) + 1); };
        using Carried = Vector<uint8_t, 8>;
        unsigned numberOfNames = read.names.size();
        constexpr uint32_t noName = UINT32_MAX;
        Vector<uint32_t> nameOf(FillWith { }, m_function.values.size(), noName);
        for (unsigned name = 0; name < numberOfNames; ++name)
            nameOf[read.names[name].value.index()] = name;
        BitVector isUser(m_function.values.size());
        BitVector hasUseAtEnd(m_function.blocks.size());
        for (UseAt& use : read.uses) {
            if (use.use.kind == Use::Kind::Operand)
                isUser.set(use.use.user.index());
            else
                hasUseAtEnd.set(use.use.block.index());
        }
        auto worsen = [&](Carried& carried, State state) {
            if (state == State::Unchanged)
                return;
            for (uint8_t& entry : carried) {
                if (entry != standsForNothing)
                    entry = std::max(entry, carrying(state));
            }
        };
        auto note = [&](UseAt& use, const Carried& carried) {
            if (carried[use.name] != standsForNothing)
                use.state = worseOf(use.state, static_cast<State>(carried[use.name] - 1));
        };

        Vector<Carried> atHeadOf(m_function.blocks.size());
        Vector<BlockID, 8> worklist;
        auto enter = [&](BlockID block, const Carried& carried) {
            Carried& known = atHeadOf[block.index()];
            bool didChange = known.isEmpty();
            if (didChange)
                known = carried;
            else {
                for (unsigned name = 0; name < numberOfNames; ++name) {
                    if (carried[name] > known[name]) {
                        known[name] = carried[name];
                        didChange = true;
                    }
                }
            }
            if (didChange && !worklist.contains(block))
                worklist.append(block);
        };
        auto leave = [&](BlockID block, Carried& carried) {
            if (hasUseAtEnd.get(block.index())) {
                for (UseAt& use : read.uses) {
                    if (use.use.kind != Use::Kind::Operand && use.use.block == block)
                        note(use, carried);
                }
            }
            if (holdsAtEnd.get(block.index()))
                worsen(carried, State::MaybeMade);
            worsen(carried, stateBehind(m_analyses.effectsOfTerminator(block).onAnyEdge(), mask, hasGuard));
            auto follow = [&](const Target& target, bool givesResults) {
                Carried behind = carried;
                for (unsigned name = 0; name < numberOfNames; ++name) {
                    const Name& parameter = read.names[name];
                    if (parameter.isParameterOf != target.block)
                        continue;
                    if (parameter.parameterIndex >= target.arguments.size()) {
                        behind[name] = givesResults ? carried[parameter.parent] : standsForNothing;
                        continue;
                    }
                    uint32_t given = nameOf[target.arguments[parameter.parameterIndex].index()];
                    behind[name] = given == noName ? standsForNothing : carried[given];
                }
                bool standsForSomething = std::ranges::any_of(behind, [](uint8_t entry) {
                    return entry != standsForNothing;
                });
                if (standsForSomething)
                    enter(target.block, behind);
            };
            const Block& contents = m_function.at(block);
            for (const Target& target : contents.terminator.targets)
                follow(target, true);
            // What leaves for the handler in the middle of the block has seen no more than this.
            if (contents.handler)
                follow(*contents.handler, false);
        };
        auto walk = [&](BlockID block, uint32_t firstIndex, Carried carried) {
            const BlockValues& values = m_function.at(block).values;
            for (uint32_t index = firstIndex; index < values.size(); ++index) {
                ValueID id = values[index];
                if (id == read.value) {
                    // This run gives the read and its casts another value.
                    for (unsigned name = 0; name < numberOfNames; ++name) {
                        if (!read.names[name].isBehindMeeting)
                            carried[name] = standsForNothing;
                    }
                    carried[0] = carrying(State::Unchanged);
                    worsen(carried, State::MaybeMade);
                    carried[0] = carrying(State::Unchanged);
                    continue;
                }
                if (isUser.get(id.index())) {
                    for (UseAt& use : read.uses) {
                        if (use.use.kind == Use::Kind::Operand && use.use.user == id)
                            note(use, carried);
                    }
                }
                if (uint32_t name = nameOf[id.index()]; name != noName)
                    carried[name] = carried[read.names[name].parent];
                if (holds.get(id.index()))
                    worsen(carried, State::MaybeMade);
                worsen(carried, stateBehind(id, key, mask, hasGuard));
            }
            leave(block, carried);
        };

        size_t indexOfRead = m_function.at(read.block).values.find(read.value);
        RELEASE_ASSERT(indexOfRead != notFound);
        Carried atRead(FillWith { }, numberOfNames, standsForNothing);
        atRead[0] = carrying(State::Unchanged);
        walk(read.block, static_cast<uint32_t>(indexOfRead + 1), WTF::move(atRead));
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            walk(block, 0, atHeadOf[block.index()]);
        }

        for (const UseAt& use : read.uses) {
            if (use.state == State::MaybeStored)
                return;
            if (use.state != State::Unchanged && use.borrows && read.names[use.name].isBehindMeeting)
                return;
        }
        read.makesObject = false;
    }

    // What meets in one parameter is borrowed by the same operations, so it is of one layout.
    void agreeOnLayoutsOfMeetings()
    {
        UncheckedKeyHashMap<uint32_t, uint32_t> layoutOfMeeting;
        for (Read& read : m_reads) {
            if (read.makesObject)
                continue;
            uint32_t layout = m_candidates.at(read.candidate).innerLayout.index() + 1;
            for (const Name& name : read.names) {
                if (name.meetsOthers && layoutOfMeeting.get(name.value.index() + 1) && layoutOfMeeting.get(name.value.index() + 1) != layout)
                    read.makesObject = true;
            }
            if (read.makesObject)
                continue;
            for (const Name& name : read.names) {
                if (name.meetsOthers)
                    layoutOfMeeting.set(name.value.index() + 1, layout);
            }
        }
    }

    const Program& m_program;
    const Candidates& m_candidates;
    const Function& m_function;
    FunctionAnalyses m_analyses;
    FreshObjects m_freshObjects;
    struct BirthOfOuterObject {
        uint32_t candidate { 0 };
        BlockID block;
    };

    Vector<Read, 8> m_reads;
    Vector<Holder, 4> m_holders;
    Vector<BirthOfOuterObject, 4> m_birthsOfOuterObjects;
};

class RewriterKeepingSlots {
public:
    RewriterKeepingSlots(const Program& program, const Candidates& candidates, const AnalysisContext& context, Function& function, TypeArena& types)
        : m_candidates(candidates)
        , m_function(function)
        , m_places(program, candidates, context, function)
        , m_builder(context, function, types, &m_places.analyses())
    {
    }

    CountsOfSlotsKept run()
    {
        for (const Read& read : m_places.reads())
            m_originalReads.append(m_function.at(read.value));
        for (const Read& read : m_places.reads())
            rewrite(read);
        m_builder.applyInsertions();
        removeParametersWithoutUse();
        for (const Holder& holder : m_places.holders())
            leaveObjectUnmade(holder);
        return m_counts;
    }

private:
    using Read = PlacesOfSlotsKept::Read;
    using UseAt = PlacesOfSlotsKept::UseAt;
    using Holder = PlacesOfSlotsKept::Holder;
    using Name = PlacesOfSlotsKept::Name;
    struct ParameterWithoutUse {
        BlockID block;
        uint32_t index { 0 };
    };

    static void become(Value& read, Opcode whereFilled, Opcode orUndefined)
    {
        Value previous = read;
        read.become(previous.opcode == Opcode::GetSlot ? whereFilled : orUndefined);
        read.setObjectOperand(previous.objectOperand());
        read.setGuardOperand(previous.guardOperand());
        read.setKey(previous.key());
        read.setReceivers(previous.receivers());
        read.setLevel(previous.level());
        if (previous.accessSite())
            read.setAccessSite(previous.accessSite());
        if (previous.opcode == Opcode::GetSlot)
            read.setHoldsDeclaredType(previous.holdsDeclaredType());
    }

    static void becomeReadThatMakesObject(Value& read) { become(read, Opcode::GetDeferredClosureSlot, Opcode::GetDeferredClosureSlotOrUndefined); }
    static void becomeReadThatBorrows(Value& read) { become(read, Opcode::GetFlattenedSlot, Opcode::GetFlattenedSlotOrUndefined); }

    ValueID insertCastTo(ProgramPoint point, ValueID object, const Name& parameter)
    {
        Value cast = IRBuilder::make(parameter.isNarrowed ? Opcode::AssumeType : Opcode::Upcast, { object });
        cast.setCastType(m_function.at(parameter.value).type);
        return m_builder.insertBefore(point, WTF::move(cast));
    }

    // Gives what `object` is under the name: the casts and the narrowings between the read and the name, once more.
    ValueID insertStepsTo(ProgramPoint point, const Read& read, uint32_t name, ValueID object)
    {
        Vector<uint32_t, 4> steps;
        for (; name; name = read.names[name].parent)
            steps.append(name);
        for (uint32_t step : steps | std::views::reverse) {
            if (read.names[step].isParameterOf) {
                object = insertCastTo(point, object, read.names[step]);
                continue;
            }
            Value copy = m_function.at(read.names[step].value);
            copy.setOperand(0, object);
            object = m_builder.insertBefore(point, WTF::move(copy));
        }
        return object;
    }

    void readAgainInFrontOf(const Read& read, const UseAt& use, ValueID nameInUse)
    {
        ProgramPoint point = pointBeforeTerminatorOf(m_function, use.use.block);
        if (use.use.kind == Use::Kind::Operand)
            point.index = static_cast<uint32_t>(m_function.at(use.use.block).values.find(use.use.user));
        Value again = m_originalReads[&read - m_places.reads().begin()];
        m_builder.setOrigin(again.origin());
        // Behind a meeting the name itself says whether the object lies in another, and in which.
        if (read.names[use.name].isBehindMeeting) {
            RELEASE_ASSERT(!use.borrows);
            if (m_holdersServedBehindMeetings.contains(use.use))
                return;
            m_holdersServedBehindMeetings.append(use.use);
            replaceUse(m_function, use.use, m_builder.insertBefore(point, IRBuilder::make(Opcode::MakeBorrowedObject, { nameInUse })));
            ++m_counts.readsMakingObjects;
            return;
        }
        if (use.borrows)
            becomeReadThatBorrows(again);
        else {
            becomeReadThatMakesObject(again);
            ++m_counts.readsMakingObjects;
        }
        ValueID object = insertStepsTo(point, read, use.name, m_builder.insertBefore(point, WTF::move(again)));
        // What a test has proven of the object is proven of the name that the test was given.
        if (use.borrows && use.use.kind == Use::Kind::Operand && isTiedToOtherOperations(m_function.at(use.use.user)))
            object = m_builder.insertBefore(point, IRBuilder::make(Opcode::ReadAgain, { nameInUse, object }));
        replaceUse(m_function, use.use, object);
    }

    void rewrite(const Read& read)
    {
        if (read.makesObject) {
            becomeReadThatMakesObject(m_function.edit(read.value));
            ++m_counts.readsMakingObjects;
            return;
        }
        // Only an operation says where the slots of a borrowed object lie.
        Vector<ValueID, 4> nameInUse;
        for (const Name& name : read.names) {
            ValueID parent = nameInUse.isEmpty() ? ValueID() : nameInUse[name.parent];
            if (name.isParameterOf) {
                // Several reads may come to one parameter by way of a meeting.
                ValueID& inPlaceOfParameter = m_inPlaceOfParameters.add(name.value.index() + 1, ValueID()).iterator->value;
                if (!inPlaceOfParameter) {
                    m_builder.setOrigin(m_function.at(read.value).origin());
                    ProgramPoint head = ProgramPoint::atHeadOf(name.isParameterOf);
                    if (name.meetsOthers)
                        inPlaceOfParameter = m_builder.insertBefore(head, IRBuilder::make(Opcode::BorrowedParameter, { name.value }));
                    else {
                        inPlaceOfParameter = insertCastTo(head, parent, name);
                        m_parametersWithoutUse.append({ name.isParameterOf, name.parameterIndex });
                    }
                }
                nameInUse.append(inPlaceOfParameter);
                continue;
            }
            if (parent && parent != m_function.at(name.value).operand(0))
                m_function.edit(name.value, ValueEdit::Definition).setOperand(0, parent);
            nameInUse.append(name.value);
        }
        for (const UseAt& use : read.uses) {
            if (PlacesOfSlotsKept::isReadAgain(use))
                readAgainInFrontOf(read, use, nameInUse[use.name]);
            else if (nameInUse[use.name] != read.names[use.name].value)
                replaceUse(m_function, use.use, nameInUse[use.name]);
        }
        becomeReadThatBorrows(m_function.edit(read.value));
    }

    void removeParametersWithoutUse()
    {
        std::ranges::sort(m_parametersWithoutUse, { }, [](const ParameterWithoutUse& parameter) {
            return parameter.block.index();
        });
        for (size_t begin = 0; begin < m_parametersWithoutUse.size();) {
            BlockID block = m_parametersWithoutUse[begin].block;
            BitVector indexes;
            for (; begin < m_parametersWithoutUse.size() && m_parametersWithoutUse[begin].block == block; ++begin)
                indexes.set(m_parametersWithoutUse[begin].index);
            m_builder.removeParameters(block, indexes);
        }
    }

    // The allocation stays, as the list of what goes into the room, right in front of the operation that takes it: that operation does the work. The casts between them go.
    void leaveObjectUnmade(const Holder& holder)
    {
        for (ValueID link : CastChain(m_function, holder.held))
            m_function.edit(blockOf(m_function, link), BlockEdit::Values).values.removeFirst(link);
        BlockValues& values = m_function.edit(holder.block, BlockEdit::Values).values;
        size_t index = values.find(holder.value);
        RELEASE_ASSERT(index != notFound);
        values.insert(index, holder.object);
        Value& taker = m_function.edit(holder.value);
        for (uint32_t operandIndex = 0; operandIndex < taker.numberOfOperands(); ++operandIndex) {
            if (taker.operand(operandIndex) == holder.held)
                taker.setOperand(operandIndex, holder.object);
        }
        // Whoever makes the object late makes it as this allocation would have, and finds the allocation by its class.
        m_function.edit(holder.object, ValueEdit::Definition).setAllocationClass(m_candidates.at(holder.candidate).birth->allocationClass);
        m_function.edit(holder.object, ValueEdit::Definition).setIsMadeOnDemand(true);
        ++m_counts.births;
    }

    const Candidates& m_candidates;
    Function& m_function;
    PlacesOfSlotsKept m_places;
    IRBuilder m_builder;
    Vector<Value, 8> m_originalReads;
    Vector<ParameterWithoutUse, 2> m_parametersWithoutUse;
    UncheckedKeyHashMap<uint32_t, ValueID> m_inPlaceOfParameters;
    Vector<Use, 4> m_holdersServedBehindMeetings;
    CountsOfSlotsKept m_counts;
};

// For the log alone: where each object that is still allocated goes first, and what keeps it from lying there.
class BirthCensus {
public:
    BirthCensus(const Program& program, const Candidates& candidates, const Vector<ASCIILiteral>& verdicts)
        : m_program(program)
        , m_candidates(candidates)
        , m_verdicts(verdicts)
    {
    }

    void dump(FunctionID id) const
    {
        const Function& function = m_program.at(id);
        for (const Block& block : function.blocks) {
            for (ValueID user : block.values) {
                const Value& value = function.at(user);
                if (isCast(value.opcode))
                    continue;
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    if (!operand)
                        return;
                    ValueID object = sameObjectThroughCasts(function, operand);
                    if (function.at(object).opcode == Opcode::NewObject)
                        dumpUse(id, function.at(object), value, operand, operandIndex);
                });
            }
            block.terminator.forEachOperand([&](ValueID operand, uint32_t) {
                if (operand && function.at(sameObjectThroughCasts(function, operand)).opcode == Opcode::NewObject)
                    dataLogLn(prefix(id, function.at(sameObjectThroughCasts(function, operand))), "to ", nameOf(block.terminator.kind));
            });
        }
    }

private:
    String prefix(FunctionID id, const Value& allocation) const
    {
        return makeString("flattened objects: birth L"_s, allocation.layout().index(), " in f"_s, id.index(), allocation.isMadeOnDemand() ? " FLAT "_s : " "_s);
    }

    void dumpUse(FunctionID id, const Value& allocation, const Value& user, ValueID operand, uint32_t operandIndex) const
    {
        if (user.opcode == Opcode::NewObject) {
            auto values = user.valuesOperands();
            for (size_t index = 0; index < values.size(); ++index) {
                if (values[index] == operand)
                    dumpField(id, allocation, "listed"_s, { user.layout(), user.fields()[index] });
            }
            return;
        }
        if ((user.opcode == Opcode::PutSlot || user.opcode == Opcode::PutSlotOrAdd) && user.valueOperand() == operand) {
            for (LayoutID member : m_program.at(user.receivers()).members) {
                if (std::optional<uint32_t> field = m_program.at(member).findField(user.key()))
                    dumpField(id, allocation, user.opcode == Opcode::PutSlot ? "stored"_s : "added"_s, { member, *field });
            }
            return;
        }
        constexpr OptionSet<OperandUse> looks { OperandUse::Inspects, OperandUse::ReadsContents, OperandUse::WritesContents };
        if (!(operandUsesOf(user.opcode, operandIndex) - looks).isEmpty())
            dataLogLn(prefix(id, allocation), "to ", nameOf(user.opcode));
    }

    void dumpField(FunctionID id, const Value& allocation, ASCIILiteral how, FieldReference reference) const
    {
        const Layout& outer = m_program.at(reference.layout);
        const Field& field = outer.fields[reference.field];
        const Layout& inner = m_program.at(allocation.layout());
        dataLog(prefix(id, allocation), how, " in L", reference.layout.index(), ".", reference.field, ": ");
        if (std::optional<uint32_t> candidate = m_candidates.find(reference)) {
            dataLogLn(m_verdicts[*candidate]);
            return;
        }
        const Type& held = m_program.at(slotTypeOf(m_program, field));
        Type type = held.withoutPrimitives(Primitive::UndefinedOrNull);
        size_t lateFields = std::ranges::count_if(inner.fields, [](const Field& field) {
            return field.flags.contains(FieldFlags::IsAddedLate);
        });
        dataLogLn("no candidate:", isPlainObjectLayout(outer) ? "" : " outer-not-plain", outer.innerObjects.isEmpty() ? "" : " outer-has-inner-cells", isPlainDataField(field) ? "" : " field-not-plain",
            field.flags.contains(FieldFlags::IsAddedLate) ? " field-added-late" : "", field.flags.contains(FieldFlags::IsPrivate) ? " field-private" : "", field.flags.contains(FieldFlags::IsInherited) ? " field-inherited" : "",
            !type.hasOnlyLayouts(m_program) ? " field-holds-more-than-layouts" : !type.singleLayout(m_program) ? " field-holds-several-layouts" : type.singleLayout(m_program) == reference.layout ? " field-holds-own-layout" : "",
            isPlainObjectLayout(inner) ? "" : " inner-not-plain", std::ranges::all_of(inner.fields, canBecomeFieldOfOuterLayout) ? "" : " inner-field-not-plain", lateFields ? " inner-fields-added-late" : "",
            inner.fields.size() > maxFieldsOfObjectFlattenedUntilHeld ? " inner-has-many-fields" : "");
    }

    const Program& m_program;
    const Candidates& m_candidates;
    const Vector<ASCIILiteral>& m_verdicts;
};

} // anonymous namespace

bool mayLieInOuterObject(const Program& program, FieldReference fieldOfOuterLayout, LayoutID innerLayout)
{
    return canBeOuterLayout(program.at(fieldOfOuterLayout.layout)) && layoutThatMayLieIn(program, fieldOfOuterLayout) == innerLayout
        && program.at(innerLayout).fields.size() <= maxFieldsOfObjectFlattenedUntilHeld;
}

// The estimate of heat says how often the source of a function runs.
static BitVector findBodiesLeftToUnseenCallers(const Program& program, const BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned count = program.functions.size();
    BitVector wasTakenIn(count);
    for (FunctionID source : collectInParallel<FunctionID>(loops, count, [&](unsigned index, Vector<FunctionID>& found) {
        for (const InlineFrame& frame : program.functions[index].inlineFrames) {
            if (frame.callee)
                found.append(firstSourceOf(program, frame.callee));
        }
    }))
        wasTakenIn.set(source.index());
    // Without the entrypoints of those functions, this is findBodiesWithoutCode.
    auto isStart = [&](FunctionID id) {
        switch (program.at(id).role) {
        case FunctionRole::GenericEntrypoint:
        case FunctionRole::TableEntrypoint:
            return !wasTakenIn.get(firstSourceOf(program, id).index());
        case FunctionRole::TableLookup:
        case FunctionRole::CoroutineBody:
        case FunctionRole::StubBody:
        case FunctionRole::Model:
            return true;
        default:
            return false;
        }
    };
    BitVector isCalledByHost = functionsCalledByHost(program, loops);
    BitVector isLeft(count);
    Vector<FunctionID> frontier;
    constexpr uint32_t isStartOfWalk = 1u << 31;
    for (uint32_t body : collectInParallel<uint32_t>(loops, count, [&](unsigned index, Vector<uint32_t>& found) {
        if (hasBody(program, FunctionID(index)))
            found.append(index | (isCalledByHost.get(index) || isStart(FunctionID(index)) ? isStartOfWalk : 0));
    })) {
        if (body & isStartOfWalk)
            frontier.append(FunctionID(body & ~isStartOfWalk));
        else
            isLeft.quickSet(body);
    }
    while (!frontier.isEmpty()) {
        Vector<FunctionID> called = collectInParallel<FunctionID>(loops, frontier.size(), [&](unsigned position, Vector<FunctionID>& found) {
            forEachFunctionReference(program, program.at(frontier[position]), [&](FunctionID target, FunctionReferenceKind kind) {
                if (kind == FunctionReferenceKind::Call && isLeft.get(target.index()))
                    found.append(target);
            });
        });
        frontier.shrink(0);
        for (FunctionID function : called) {
            if (isLeft.quickClear(function.index()))
                frontier.append(function);
        }
    }
    return isLeft;
}

void flattenInnerObjects(ProgramEditor& editor, const FlowResults& flow, const Heat& heat, BuildContext& build)
{
    const Program& program = editor.program();
    bool logsDecisions = build.options().logging.dumpedPass == PassID::FlattenInnerObjects;
    // For tests: every field that may keep its slot does, whatever the estimate of heat says, also where nobody holds the inner object.
    bool keepsSlotsWhereverLegal = build.recordedOptions().has(TestingSwitch::FlattenUntilHeldWhereverLegal);
    // The slot that says whether the object was made is a narrow reference.
    bool mayKeepSlots = keepsSlotsWhereverLegal && !build.recordedOptions().useWideReferences();
    Candidates candidates = findCandidates(program, mayKeepSlots, logsDecisions);
    if (candidates.isEmpty())
        return;

    struct Scanned {
        FunctionID function;
        Scanner::Result result;
    };
    Vector<Scanned> scanned = collectInParallel<Scanned>(build.parallelLoops(), program.functions.size(), [&](unsigned index, Vector<Scanned>& found) {
        FunctionID id(index);
        // The walk hears of nothing until it meets one of these.
        bool hasToDoWithCandidate = false;
        forEachListedName(program, id, Value::opcodesWithKey | OpcodeSet { Opcode::NewObject }, [&](BlockID, ValueID, Opcode opcode, NameKind kind, uint32_t name) {
            if (kind == NameKind::Layout)
                hasToDoWithCandidate |= opcode == Opcode::NewObject && candidates.isOuter(LayoutID(name));
            else if (kind == NameKind::Key)
                hasToDoWithCandidate |= candidates.namesCandidate(PropertyKeyID(name));
        });
        if (!hasToDoWithCandidate)
            return;
        Scanner::Result result = Scanner(program, candidates, id).run();
        if (!result.refuted.isEmpty() || !result.found.isEmpty())
            found.append({ id, WTF::move(result) });
    });
    BitVector isFound(candidates.size());
    BitVector isInFunctionThatStays(candidates.size());
    Vector<BirthsOfCandidate> birthsOfCandidates(candidates.size());
    for (const Scanned& entry : scanned) {
        // A function that may not be changed keeps the two objects, and so does everybody.
        bool mayChange = build.isEnabled(PassID::FlattenInnerObjects, entry.function);
        for (uint32_t candidate : entry.result.refuted)
            candidates.at(candidate).isRefuted = true;
        for (uint32_t candidate : entry.result.cannotKeepSlot)
            candidates.at(candidate).mayKeepSlot = false;
        for (uint32_t candidate : entry.result.found) {
            isFound.set(candidate);
            if (!mayChange)
                isInFunctionThatStays.set(candidate);
        }
        for (const Scanner::FoundBirth& found : entry.result.births)
            birthsOfCandidates[found.candidate].add(flow, found.birth);
    }
    for (uint32_t index = 0; index < candidates.size(); ++index)
        candidates.at(index).birth = birthsOfCandidates[index].takeMostFrequent();

    // One level at a time: a layout is outer or inner, not both. The fields that go away come first.
    BitVector layoutsMadeOtherwise = findLayoutsMadeOtherwise(program, build);
    BitVector isOuter(program.layouts.size());
    BitVector isInner(program.layouts.size());
    uint64_t numberOfFieldsGone = 0;
    uint64_t numberOfSlotsKept = 0;
    auto whyImpossible = [&](uint32_t index) -> ASCIILiteral {
        const Candidate& candidate = candidates.at(index);
        if (!isFound.get(index))
            return "stays an object: no operation names the field"_s;
        if (isInFunctionThatStays.get(index))
            return "stays an object: a function that may not be changed names the field"_s;
        if (layoutsMadeOtherwise.get(candidate.field.layout.index()))
            return "stays an object: something else than an allocation in the code makes the outer object"_s;
        if (isInner.get(candidate.field.layout.index()))
            return "stays an object: the outer layout is flattened itself"_s;
        if (isOuter.get(candidate.innerLayout.index()))
            return "stays an object: the inner layout holds flattened objects itself"_s;
        return { };
    };
    auto isPossible = [&](uint32_t index) {
        return whyImpossible(index).isNull();
    };
    Vector<ASCIILiteral> verdicts(candidates.size());
    auto accept = [&](const Candidate& candidate) {
        isOuter.set(candidate.field.layout.index());
        isInner.set(candidate.innerLayout.index());
    };
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates.at(index);
        candidate.isRefuted |= !isPossible(index) || (keepsSlotsWhereverLegal && candidate.mayKeepSlot && candidate.birth);
        if (candidate.isRefuted)
            continue;
        accept(candidate);
        ++numberOfFieldsGone;
    }
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates.at(index);
        if (!candidate.isRefuted) {
            verdicts[index] = "flattened"_s;
            continue;
        }
        verdicts[index] = !candidate.mayKeepSlot ? "stays an object: held, and too large to lie there for nothing, or its slot is shared with other layouts"_s : !candidate.birth ? "stays an object: none is born where it could be laid into the outer object"_s : whyImpossible(index);
        candidate.keepsSlot = verdicts[index].isNull();
        if (candidate.keepsSlot)
            accept(candidate);
    }

    // An object that is made after all has cost the room, the copy and the tests for nothing.
    Vector<FunctionID> functionsNamingCandidates;
    for (const Scanned& entry : scanned) {
        bool namesSlotKept = std::ranges::any_of(entry.result.found, [&](uint32_t candidate) {
            return candidates.at(candidate).keepsSlot;
        });
        if (namesSlotKept)
            functionsNamingCandidates.append(entry.function);
    }
    if (!functionsNamingCandidates.isEmpty()) {
        BitVector bodiesLeftToUnseenCallers = findBodiesLeftToUnseenCallers(program, build);
        functionsNamingCandidates.removeAllMatching([&](FunctionID id) {
            return bodiesLeftToUnseenCallers.get(id.index());
        });
    }
    Vector<HeatOfSlotKept> estimates = collectInParallel<HeatOfSlotKept>(build.parallelLoops(), functionsNamingCandidates.size(), [&](unsigned index, Vector<HeatOfSlotKept>& found) {
        FunctionID id = functionsNamingCandidates[index];
        PlacesOfSlotsKept places(program, candidates, AnalysisContext { build, program, &flow, nullptr, id }, program.at(id));
        if (logsDecisions) [[unlikely]]
            places.dump(id);
        found.appendVector(places.estimateHeat(heat, id));
    });
    Vector<HeatOfSlotKept> heatOfCandidates(candidates.size());
    for (const HeatOfSlotKept& estimate : estimates) {
        heatOfCandidates[estimate.candidate].outerObjectsBorn += estimate.outerObjectsBorn;
        heatOfCandidates[estimate.candidate].laidIntoOuterObject += estimate.laidIntoOuterObject;
        heatOfCandidates[estimate.candidate].made += estimate.made;
    }
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates.at(index);
        if (!candidate.keepsSlot)
            continue;
        // In words of a slot: an object that is never made saves its header.
        constexpr double wordsOfHeader = 2;
        double wordsOfSlots = program.at(candidate.innerLayout).fields.size();
        const HeatOfSlotKept& estimate = heatOfCandidates[index];
        double saved = estimate.laidIntoOuterObject * wordsOfHeader;
        double lost = estimate.made * (wordsOfHeader + wordsOfSlots) + std::max(0.0, estimate.outerObjectsBorn - estimate.laidIntoOuterObject) * wordsOfSlots;
        candidate.keepsSlot = saved >= lost || keepsSlotsWhereverLegal;
        verdicts[index] = candidate.keepsSlot ? "flattened until held"_s : "stays an object: too many are held, or nothing is laid into too many outer objects"_s;
        numberOfSlotsKept += candidate.keepsSlot;
    }
    for (uint32_t index = 0; index < candidates.size(); ++index) {
        Candidate& candidate = candidates.at(index);
        if (logsDecisions) [[unlikely]]
            dataLogLn("flattened objects: L", candidate.field.layout.index(), " field ", candidate.field.field, " L", candidate.innerLayout.index(), ": ", verdicts[index], "; heat: laid ", heatOfCandidates[index].laidIntoOuterObject, ", outer objects born ", heatOfCandidates[index].outerObjectsBorn, ", made ", heatOfCandidates[index].made);
        if (candidate.keepsSlot)
            keepSlotOfInnerObject(editor, candidate);
        else if (!candidate.isRefuted)
            addFieldsOfInnerObject(editor, candidate);
    }
    auto dumpCensus = [&] {
        BirthCensus census(program, candidates, verdicts);
        for (uint32_t index = 0; index < program.functions.size(); ++index) {
            if (program.functions[index].hasBody())
                census.dump(FunctionID(index));
        }
    };
    if (!numberOfFieldsGone && !numberOfSlotsKept) {
        if (logsDecisions) [[unlikely]]
            dumpCensus();
        return;
    }

    Vector<FunctionID> functions;
    for (const Scanned& entry : scanned) {
        bool hasWork = std::ranges::any_of(entry.result.found, [&](uint32_t candidate) {
            return candidates.at(candidate).isFlattened();
        });
        if (hasWork)
            functions.append(entry.function);
    }
    Vector<CountsOfSlotsKept> counts(functions.size());
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        uint64_t births = numberOfFieldsGone ? Rewriter(program, candidates, function).run() : 0;
        if (numberOfSlotsKept)
            counts[index] = RewriterKeepingSlots(program, candidates, AnalysisContext { build, program, &flow, nullptr, id }, function, arenas.types).run();
        counts[index].births += births;
        compactFunction(function);
    });
    if (logsDecisions) [[unlikely]]
        dumpCensus();
    CountsOfSlotsKept sum;
    for (const CountsOfSlotsKept& ofFunction : counts) {
        sum.births += ofFunction.births;
        sum.readsMakingObjects += ofFunction.readsMakingObjects;
    }
    build.statistics().count(StatisticID::FieldsHoldingFlattenedObjects, numberOfFieldsGone);
    build.statistics().count(StatisticID::FieldsDeferringFlattenedObjects, numberOfSlotsKept);
    build.statistics().count(StatisticID::AllocationsOfFlattenedObjects, sum.births);
    build.statistics().count(StatisticID::ReadsMakingFlattenedObjects, sum.readsMakingObjects);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
