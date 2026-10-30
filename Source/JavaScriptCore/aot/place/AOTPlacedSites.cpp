/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPlacementSteps.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuildContext.h"
#include "AOTFlowResults.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionKinds.h"
#include "AOTObjectWords.h"
#include "AOTOpcodesNamingRecords.h"
#include "AOTPropertyAccessElements.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTRemoveUnreachableFunctions.h"
#include "AOTSchemaWalkers.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTVector.h"
#include <algorithm>
#include <functional>
#include <tuple>
#include <type_traits>
#include <wtf/BitSet.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet slotRowOpcodes = Value::opcodesWithFieldAccess | Value::opcodesWithPlacedKeys;
static constexpr OpcodeSet shapeTestOpcodes { Opcode::ShapeTest, Opcode::TypeTest };
static constexpr OpcodeSet weighedOpcodes = slotRowOpcodes | shapeTestOpcodes | Value::opcodesWithGuardBit | OpcodeSet { Opcode::CallTable };

static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });
static_assert(Value::opcodesWithAllocation == OpcodeSet { Opcode::NewClosure, Opcode::NewObject, Opcode::MaterializeEnvironment });
static_assert((Value::opcodesWithStaticsOffset | Value::opcodesWithOrderWord) == OpcodeSet { Opcode::NewArray, Opcode::NewRegExp, Opcode::SharedRegExpLiteral, Opcode::NewTemplateObject, Opcode::HasKeyOrder });

static constexpr auto keyOfGuardUse = [](const GuardUse& use) { return std::tuple(use.classLayout, use.side, use.kind, use.key); };

// The code of these operations hands the cell of their key to the run time.
static constexpr OpcodeSet opcodesNamingKeyCell {
    Opcode::KeyConstant, Opcode::PropHasOwn, Opcode::DefineGetter, Opcode::DefineSetter, Opcode::DefineMethod, Opcode::SuperSet,
};
static_assert((Value::opcodesWithKey | opcodesNamingKeyCell) == Value::opcodesWithKey);
static_assert(Value::opcodesWithKeys.contains(Opcode::DefineAccessors));

namespace {

struct PlacedFunctionSites : SiteLists {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(PlacedFunctionSites);

    Vector<VariableID> variables;
    Vector<TypeID> types;
    Vector<TypeID> typesTestedByCode;
    Vector<FunctionID> allocatedClosures;
    Vector<DataID> data;
    Vector<LayoutID> layouts;
    Vector<LayoutSetID> layoutSets;
    Vector<LayoutID> layoutsOfData;
    Vector<KeyCellUse> keyCellUses;
    Vector<GlobalBindingSite> globalBindingSites;
    FunctionID function;
    Vector<LayoutSetID> receiversOfStoresThatMayAdd;
    // The fields that stores and definitions with a known key may create, in body order.
    Vector<FieldCreationUse> sourceFieldCreations;
    uint32_t numberOfOperationsWithWordForSiteCache { 0 };

    static decltype(auto) withLists(auto& sites, const auto& functor)
    {
        return functor(sites.fieldAccesses, sites.shapeTests, sites.allocations, sites.keyOrderTests, sites.fieldCreations, sites.guards, sites.guardSets, sites.tableCalls, sites.profiledArrayLiterals, sites.regExpLiterals, sites.templateObjects,
            sites.copyOnWriteArrays, sites.arraysWithStorageInImage, sites.dataMadeOnce, sites.builtinCells, sites.bigIntConstants, sites.requireEdges, sites.moduleBuiltins, sites.moduleNamespaces,
            sites.variables, sites.types, sites.typesTestedByCode, sites.allocatedClosures, sites.data, sites.layouts, sites.layoutSets, sites.layoutsOfData, sites.keyCellUses, sites.globalBindingSites, sites.receiversOfStoresThatMayAdd, sites.sourceFieldCreations);
    }

    bool hasFoundNothing() const
    {
        return withLists(*this, [](const auto&... lists) { return (lists.isEmpty() && ...); }) && !maxClosureCaptures && !numberOfOperationsWithWordForSiteCache;
    }

    // The scans of all functions are kept until the sites are placed.
    void shrinkToFit()
    {
        withLists(*this, [](auto&... lists) { (lists.shrinkToFit(), ...); });
    }
};
// Name every new list in withLists().
static_assert(sizeof(SiteLists) == 19 * sizeof(Vector<uint32_t>) + sizeof(uint64_t) && sizeof(PlacedFunctionSites) == sizeof(SiteLists) + 12 * sizeof(Vector<uint32_t>) + 2 * sizeof(uint64_t));

// A scan as it is kept until the sites are placed: only the lists that hold something. Most functions fill a few of the 31.
class KeptSites {
    WTF_MAKE_NONCOPYABLE(KeptSites);
public:
    template<typename T> using ListOf = Vector<T> PlacedFunctionSites::*;

    static KeptSites* create(PlacedFunctionSites&& sites)
    {
        uint32_t filled = 0;
        forEachList(sites, [&](unsigned index, auto& list) {
            if (!list.isEmpty())
                filled |= 1u << index;
        });
        auto* kept = new (NotNull, fastMalloc(sizeof(KeptSites) + std::popcount(filled) * sizeof(VectorWords))) KeptSites(sites, filled);
        VectorWords* next = kept->lists();
        forEachList(sites, [&](unsigned, auto& list) {
            if (!list.isEmpty())
                new (NotNull, next++) std::remove_reference_t<decltype(list)>(WTF::move(list));
        });
        return kept;
    }

    static void destroy(KeptSites* kept)
    {
        VectorWords* next = kept->lists();
        forEachList(nothing(), [&](unsigned index, auto& list) {
            if (kept->m_filled >> index & 1)
                std::destroy_at(reinterpret_cast<std::remove_reference_t<decltype(list)>*>(next++));
        });
        fastFree(kept);
    }

    // Whoever walks the scans of all functions asks for the number of a list once.
    template<typename T>
    static unsigned numberOf(ListOf<T> wanted)
    {
        unsigned number = UINT_MAX;
        forEachList(nothing(), [&](unsigned index, auto& list) {
            if (static_cast<const void*>(&list) == static_cast<const void*>(&(nothing().*wanted)))
                number = index;
        });
        RELEASE_ASSERT(number != UINT_MAX);
        return number;
    }

    template<typename T>
    Vector<T>& list(unsigned number)
    {
        if (!(m_filled >> number & 1))
            return emptyList<T>();
        return reinterpret_cast<Vector<T>&>(lists()[std::popcount(m_filled & ((1u << number) - 1))]);
    }
    template<typename T> const Vector<T>& list(unsigned number) const { return const_cast<KeptSites*>(this)->list<T>(number); }
    template<typename T> const Vector<T>& list(ListOf<T> wanted) const { return list<T>(numberOf(wanted)); }

    const FunctionID function;
    const unsigned maxClosureCaptures;
    const uint32_t numberOfOperationsWithWordForSiteCache;

private:
    KeptSites(const PlacedFunctionSites& sites, uint32_t filled)
        : function(sites.function)
        , maxClosureCaptures(sites.maxClosureCaptures)
        , numberOfOperationsWithWordForSiteCache(sites.numberOfOperationsWithWordForSiteCache)
        , m_filled(filled)
    {
    }

    static PlacedFunctionSites& nothing()
    {
        static NeverDestroyed<PlacedFunctionSites> sites;
        return sites.get();
    }

    // Nobody puts anything into it.
    template<typename T>
    static Vector<T>& emptyList()
    {
        static NeverDestroyed<Vector<T>> list;
        return list.get();
    }

    static void forEachList(PlacedFunctionSites& sites, const auto& functor)
    {
        PlacedFunctionSites::withLists(sites, [&](auto&... lists) {
            static_assert(sizeof...(lists) <= 32 && ((sizeof(lists) == sizeof(VectorWords)) && ...));
            unsigned index = 0;
            (functor(index++, lists), ...);
        });
    }

    VectorWords* lists() { return reinterpret_cast<VectorWords*>(this + 1); }

    const uint32_t m_filled;
};
static_assert(!(sizeof(KeptSites) % alignof(VectorWords)));

using FunctionSitesArray = std::span<KeptSites* const>;

struct GuardSetKey {
    std::span<const GuardUse> guards;

    friend bool operator==(GuardSetKey a, GuardSetKey b) { return std::ranges::equal(a.guards, b.guards, { }, keyOfGuardUse, keyOfGuardUse); }
    friend bool operator<(GuardSetKey a, GuardSetKey b) { return std::ranges::lexicographical_compare(a.guards, b.guards, { }, keyOfGuardUse, keyOfGuardUse); }
};

} // anonymous namespace

SourceSite sourceSiteOf(const Program& program, FunctionID id, const Value& value)
{
    Origin origin = value.origin();
    FunctionID owner = origin.inlineFrame ? program.at(id).inlineFrames[origin.inlineFrame].callee : id;
    // Copies of one call of RegExp may have become different literals, and a cell holds the object of one literal.
    bool isRegExpLiteral = Value::opcodesWithPattern.contains(value.opcode);
    return {
        firstSourceOf(program, owner),
        origin.position,
        isRegExpLiteral ? value.pattern() : StringID(),
        isRegExpLiteral ? value.regExpFlags().toRaw() : 0u,
    };
}

static SlotAccessKey useKeyOf(const FieldAccessUse& use) { return use.access; }
static auto useKeyOf(const ShapeTestUse& use) { return std::tuple(use.tested, use.known); }
static auto useKeyOf(const GuardUse& use) { return keyOfGuardUse(use); }
static GuardSetKey useKeyOf(const GuardSetUse& use) { return { use.guards.span() }; }
static uint32_t useKeyOf(BuiltinCellKey key) { return key.encoded(); }
template<typename T> static const T& useKeyOf(const T& element) { return element; }

template<typename Use>
static bool isOrderedBefore(const Use& a, const Use& b)
{
    return useKeyOf(a) < useKeyOf(b);
}

template<typename Use>
static void foldEqualUses(Vector<Use>& sorted)
{
    size_t count = 0;
    for (Use& use : sorted) {
        if (!count || useKeyOf(sorted[count - 1]) != useKeyOf(use)) {
            if (&sorted[count++] != &use)
                sorted[count - 1] = WTF::move(use);
            continue;
        }
        if constexpr (requires { use.weight; })
            sorted[count - 1].weight = saturatedSum(sorted[count - 1].weight, use.weight);
        if constexpr (requires { use.sites; })
            sorted[count - 1].sites += use.sites;
    }
    sorted.shrink(count);
}

template<typename Use>
static void sortAndFold(Vector<Use>& uses)
{
    AOT::sortInLine(uses, isOrderedBefore<Use>);
    foldEqualUses(uses);
}

namespace {

class SiteScanner {
public:
    SiteScanner(const PlacementContext& context, FunctionID id, FunctionAnalyses* analyses)
        : m_context(context)
        , m_program(context.program)
        , m_id(id)
        , m_function(m_program.at(id))
        , m_weights(m_program, context.inputs.heat, id, analyses ? *analyses : m_ownAnalyses.emplace(AnalysisContext { context.build, m_program, &context.inputs.flow, nullptr, id }, m_function))
        , m_source(firstSourceOf(m_program, id))
    {
    }

    void run(PlacedFunctionSites& sites)
    {
        m_sites = &sites;
        sites.function = m_id;
        bool hasWeighedBlock = false;
        for (unsigned i = 0; i < m_function.blocks.size(); ++i) {
            const Block& block = m_function.blocks[i];
            std::optional<SiteWeight> weight;
            auto weightWhere = [&](bool isWeighed) -> SiteWeight {
                if (!isWeighed)
                    return 0;
                if (!weight)
                    weight = m_weights.weightOf(BlockID(i));
                hasWeighedBlock = true;
                return *weight;
            };
            for (uint32_t index = 0; index < block.values.size(); ++index) {
                const Value& value = m_function.at(block.values[index]);
                if (remembersKeyOrderStep(block, value))
                    sites.receiversOfStoresThatMayAdd.append(value.receivers());
                // Behind the first weight another costs less than the question.
                scanValue({ BlockID(i), index }, block.values[index], value, weightWhere(weighedOpcodes.contains(value.opcode) && (hasWeighedBlock || isWeightRead(value))));
            }
            scanTerminator(block.terminator, weightWhere(block.terminator.kind == TerminatorKind::SwitchOnShape));
            scanLayouts(block.terminator);
        }
        sortAndFold(sites.data);
        for (DataID id : sites.data) {
            forEachDataNodeInPreorder(m_program.at(id).root, [&](uint32_t, const DataNode& node) {
                switch (node.kind()) {
                case DataNode::Kind::Undefined:
                case DataNode::Kind::Null:
                case DataNode::Kind::Hole:
                case DataNode::Kind::Boolean:
                case DataNode::Kind::Number:
                case DataNode::Kind::String:
                case DataNode::Kind::Array:
                    return;
                case DataNode::Kind::BigInt:
                    sites.bigIntConstants.append(node.as<DataNode::BigInt>().digits);
                    return;
                case DataNode::Kind::Object:
                    scanDataObject(node.as<DataNode::Object>());
                    return;
                }
                RELEASE_ASSERT_NOT_REACHED();
            });
        }
        sortAndFold(sites.fieldAccesses);
        sortAndFold(sites.shapeTests);
        sortAndFold(sites.guards);
        sortAndFold(sites.guardSets);
        sortAndFold(sites.tableCalls);
        sortAndFold(sites.allocations);
        sortAndFold(sites.builtinCells);
        sortAndFold(sites.layouts);
        sortAndFold(sites.layoutsOfData);
        sortAndFold(sites.layoutSets);
    }

private:
    LayoutSetID knownSetOf(ValueID id) const
    {
        const Type& type = m_program.typeOf(m_function, id);
        if (!type.hasOnlyLayouts() || !type.layouts || !m_program.at(type.layouts).all())
            return { };
        return type.layouts;
    }

    bool isUnordered(const Value& allocation) const
    {
        LayoutID layout = allocation.layout();
        if (m_context.isEnvironment(layout))
            return false;
        if (AllocationClassID allocationClass = allocation.allocationClass())
            return m_context.inputs.flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
        return m_context.slots().layout(layout).keysAreNeverEnumerated;
    }

    bool isWeightRead(const Value&) const;

    void appendShapeTest(LayoutSetID tested, ValueID subject, SiteWeight weight)
    {
        if (tested)
            m_sites->shapeTests.append(ShapeTestUse { tested, knownSetOf(subject), weight, 1 });
    }

    void appendAllocation(LayoutID layout, bool keysAreNeverEnumerated, std::span<const FieldIndex> initialFields)
    {
        if (!m_sites->allocations.isEmpty()) {
            const AllocationUse& last = m_sites->allocations.last();
            if (last.layout == layout && last.keysAreNeverEnumerated == keysAreNeverEnumerated && std::ranges::equal(last.initialFields, initialFields))
                return;
        }
        m_sites->allocations.append(AllocationUse { layout, keysAreNeverEnumerated, Vector<FieldIndex>(initialFields) });
    }

    void appendFieldCreations(LayoutSetID receivers, std::span<const PropertyKeyID> keys)
    {
        const SlotPlan& slots = m_context.slots();
        for (LayoutID member : m_program.at(receivers).completeMembers()) {
            if (!m_context.isCompiledObject(member) || slots.layout(member).keysAreNeverEnumerated)
                continue;
            Vector<FieldIndex> fields;
            for (PropertyKeyID key : keys) {
                std::optional<FieldIndex> field = m_program.at(member).findField(key);
                if (field && slots.slot(member, *field).canBeCreatedByStore())
                    fields.append(*field);
            }
            if (!fields.isEmpty()) {
                m_sites->sourceFieldCreations.append(FieldCreationUse { member, fields });
                m_sites->fieldCreations.append(FieldCreationUse { member, WTF::move(fields) });
            }
        }
    }

    // `Object.defineProperty(exports, "name", { enumerable: true, get })` creates the field, positioned where the definition appears among the stores.
    void appendFieldCreationByDefinition(const Value& definition)
    {
        LayoutSetID receivers = knownSetOf(definition.objectOperand());
        ConstantKey constant = constantKeyOf(m_program, m_program.typeOf(m_function, definition.computedKeyOperand()));
        if (!receivers || constant.kind != ConstantKeyKind::Text)
            return;
        std::optional<PropertyKeyID> key = m_program.types.find(PropertyKey { .kind = PropertyKeyKind::Text, .text = constant.text });
        if (!key)
            return;
        const SlotPlan& slots = m_context.slots();
        for (LayoutID member : m_program.at(receivers).completeMembers()) {
            if (!m_context.isCompiledObject(member) || slots.layout(member).keysAreNeverEnumerated)
                continue;
            std::optional<FieldIndex> field = m_program.at(member).findField(*key);
            if (!field || !slots.slot(member, *field).hasStorage())
                continue;
            m_sites->sourceFieldCreations.append(FieldCreationUse { member, { *field } });
            if (slots.slot(member, *field).canBeCreatedByStore())
                m_sites->fieldCreations.append(FieldCreationUse { member, { *field } });
        }
    }

    void appendCell(BuiltinCellKey key)
    {
        if (key.hasCell(m_program.builtins()))
            m_sites->builtinCells.append(key);
    }

    void appendCellsOfMember(BuiltinID id)
    {
        for (BuiltinCellKey key : { BuiltinCellKey::object(id), BuiltinCellKey::getter(id), BuiltinCellKey::setter(id) })
            appendCell(key);
        if (BuiltinID holder = m_program.builtins().at(id).holder)
            appendCell(BuiltinCellKey::object(holder));
    }

    // The run time sets and clears the bit of one condition. A site with several conditions tests the bit of their set, which follows the bits of its members.
    void appendGuards(Vector<GuardUse, 4>&& guards, SiteWeight weight)
    {
        for (GuardUse guard : guards) {
            guard.weight = weight;
            m_sites->guards.append(guard);
        }
        if (guards.size() > 1)
            m_sites->guardSets.append(GuardSetUse { WTF::move(guards), weight });
    }

    // A private name of a class is no value, so it has no cell. That of the library has one, which only the run time is handed.
    void appendKeyCell(PropertyKeyID key)
    {
        if (!key || m_program.at(key).kind == PropertyKeyKind::PrivateName)
            return;
        ModuleID module = m_program.at(m_source).module;
        m_sites->keyCellUses.append(KeyCellUse { key, module ? module.index() : UINT32_MAX, m_source.index() });
    }

    void scanSlotRow(const Value& value, SiteWeight weight)
    {
        if (value.opcode == Opcode::PutSlots) {
            Vector<PropertyKeyID, 4> keysThatMayBeAdded;
            for (PlacedKey placedKey : value.placedKeys()) {
                m_sites->fieldAccesses.append(FieldAccessUse { { placedKey.key, value.receivers(), true }, weight, 1 });
                if (placedKey.mayAdd)
                    keysThatMayBeAdded.append(placedKey.key);
            }
            appendFieldCreations(value.receivers(), keysThatMayBeAdded.span());
            return;
        }
        m_sites->fieldAccesses.append(FieldAccessUse { { value.key(), value.receivers(), isSlotStore(value.opcode) }, weight, 1 });
        if (value.opcode == Opcode::PutSlotOrAdd) {
            PropertyKeyID key = value.key();
            appendFieldCreations(value.receivers(), singleElementSpan(key));
        }
    }

    void scanDataObject(const DataNode::Object& object)
    {
        if (object.layout)
            m_sites->layoutsOfData.append(object.layout);
        if (!object.layout || !m_context.isCompiledObject(object.layout))
            return;
        const Layout& layout = m_program.at(object.layout);
        Vector<FieldIndex, 16> fields;
        for (const DataMember& member : object.members) {
            if (std::optional<uint32_t> field = layout.findField(member.key))
                fields.append(*field);
        }
        appendAllocation(object.layout, m_context.slots().layout(object.layout).keysAreNeverEnumerated, fields.span());
    }

    // Nothing tests the bit of an entry of the method table: the run time keeps the entry as the bit says.
    void scanTableCall(const Value& value, SiteWeight weight)
    {
        m_sites->tableCalls.append({ selectorOf(m_program, value), value.receivers() });
        PropertyAccessContext access { m_program, &m_context.inputs.flow };
        MemberPartition partition = partitionMembers(access, m_program.at(value.receivers()).completeMembers(), value.key(), Access::Read);
        for (const MemberArm& arm : partition.arms) {
            if (arm.answer.kind == MemberKind::Method && !arm.answer.conditions.isEmpty())
                appendGuards(guardsOf(arm.answer.conditions.span()), weight);
        }
    }

    void scanTypes(const Value& value)
    {
        if (familyOf(value.opcode) != OperationFamily::RuntimeValidation) {
            Vector<TypeID>& types = value.opcode == Opcode::ReportMismatch ? m_sites->types : m_sites->typesTestedByCode;
            forEachType(value, [&](TypeID type) {
                types.append(type);
            });
            return;
        }
        // The test of a proof is code, so the run time needs no record of the type: only the cells the type names, and a record of what a container holds.
        const BuiltinTable& builtins = m_program.builtins();
        forEachType(value, [&](TypeID id) {
            const Type& type = m_program.at(id);
            for (BuiltinID builtin : type.builtins)
                appendCell(BuiltinCellKey::object(builtin));
            for (const ClassPart& part : type.classes) {
                if (!part.contentsAreProven)
                    continue;
                for (TypeID contents : part.writtenContents) {
                    bool everyBuiltinHasCell = contents && std::ranges::all_of(m_program.at(contents).builtins, [&](BuiltinID builtin) {
                        return BuiltinCellKey::object(builtin).hasCell(builtins);
                    });
                    if (everyBuiltinHasCell)
                        m_sites->types.append(contents);
                }
            }
        });
    }

    template<typename InstructionType>
    void scanLayouts(const InstructionType& instruction)
    {
        forEachLayout(instruction, [&](LayoutID layout) {
            if (layout && (m_sites->layouts.isEmpty() || m_sites->layouts.last() != layout))
                m_sites->layouts.append(layout);
        });
        forEachLayoutSet(instruction, [&](LayoutSetID set) {
            if (!set || (!m_sites->layoutSets.isEmpty() && m_sites->layoutSets.last() == set))
                return;
            if (TypeTable::isProvisional(set))
                m_sites->layouts.appendVector(m_program.at(set).written());
            else
                m_sites->layoutSets.append(set);
        });
    }

    void scanTerminator(const Terminator& terminator, SiteWeight weight)
    {
        switch (terminator.kind) {
        case TerminatorKind::SwitchOnShape:
            for (LayoutSetID tested : terminator.layoutSets())
                appendShapeTest(tested, terminator.subjectOperand(), weight);
            return;
        case TerminatorKind::TryCallInline:
            appendCellsOfMember(m_program.builtins().at(terminator.overload()).builtin);
            return;
        case TerminatorKind::Jump:
        case TerminatorKind::Branch:
        case TerminatorKind::Switch:
        case TerminatorKind::SwitchString:
        case TerminatorKind::Return:
        case TerminatorKind::ReturnCall:
        case TerminatorKind::Throw:
        case TerminatorKind::Rethrow:
        case TerminatorKind::Unreachable:
        case TerminatorKind::TestAndBranch:
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void scanValue(ProgramPoint point, ValueID id, const Value& value, SiteWeight weight)
    {
        static const OpcodeSet opcodesNamingLayouts = findOpcodesNaming<LayoutID, LayoutSetID>();
        const BuiltinTable& builtins = m_program.builtins();
        PlacedFunctionSites& sites = *m_sites;
        if (opcodesNamingLayouts.contains(value.opcode))
            scanLayouts(value);
        if (slotRowOpcodes.contains(value.opcode))
            scanSlotRow(value, weight);
        if (Value::opcodesWithData.contains(value.opcode))
            sites.data.append(value.data());
        if (Value::opcodesWithOverload.contains(value.opcode))
            appendCellsOfMember(builtins.at(value.overload()).builtin);
        sites.numberOfOperationsWithWordForSiteCache += Value::opcodesWithSiteCache.contains(value.opcode);
        if (opcodesNamingKeyCell.contains(value.opcode))
            appendKeyCell(value.key());

        switch (value.opcode) {
        case Opcode::ShapeTest:
            appendShapeTest(value.layouts(), value.objectOperand(), weight);
            break;
        case Opcode::TypeTest:
            appendShapeTest(m_program.at(value.testedType()).layouts, value.valueOperand(), weight);
            break;
        case Opcode::NewClosure:
            sites.maxClosureCaptures = std::max<unsigned>(sites.maxClosureCaptures, value.numberOfVariadicOperands());
            if (value.closureForm() != ClosureForm::Singleton)
                sites.allocatedClosures.append(value.function());
            break;
        case Opcode::MaterializeEnvironment:
            appendAllocation(value.layout(), false, { });
            break;
        case Opcode::NewObject:
            if (m_context.receivesShape(value.layout()))
                appendAllocation(value.layout(), isUnordered(value), value.fields());
            break;
        case Opcode::Data:
            if (value.encoding() == DataEncoding::CopyOnWriteArray) {
                bool isStorage = isStorageOfArray(m_program.at(value.data()).root.as<DataNode::Array>());
                (isStorage ? sites.arraysWithStorageInImage : sites.copyOnWriteArrays).append(value.data());
            } else if (value.encoding() == DataEncoding::MadeOnce)
                sites.dataMadeOnce.append(value.data());
            break;
        case Opcode::AreMembersUnchanged:
            appendGuards(guardsOf(value.conditions()), weight);
            break;
        case Opcode::CallTable:
            scanTableCall(value, weight);
            break;
        case Opcode::NewArray:
            if (hasArrayProfile(value.literalStorage()))
                sites.profiledArrayLiterals.append(sourceSiteOf(m_program, m_id, value));
            break;
        case Opcode::NewRegExp:
        case Opcode::SharedRegExpLiteral:
            sites.regExpLiterals.append(sourceSiteOf(m_program, m_id, value));
            break;
        case Opcode::NewTemplateObject:
            sites.templateObjects.append(sourceSiteOf(m_program, m_id, value));
            break;
        case Opcode::HasKeyOrder:
            sites.keyOrderTests.append(KeyOrderTestUse { value.layout(), Vector<FieldIndex>(value.fields()) });
            break;
        case Opcode::Builtin:
            appendCell(BuiltinCellKey::object(builtins.canonicalID(value.builtin())));
            break;
        case Opcode::InitializeBuiltin:
            appendCell(BuiltinCellKey::object(builtins.canonicalID(value.builtin())));
            appendCell(BuiltinCellKey::value(value.builtin()));
            break;
        case Opcode::IsBuiltinLocationUnchanged:
            appendCellsOfMember(value.member());
            break;
        case Opcode::GetBuiltinValue:
            appendCell(BuiltinCellKey::value(builtins.canonicalID(value.builtin())));
            break;
        case Opcode::BigInt:
            sites.bigIntConstants.append(value.digits());
            break;
        case Opcode::RequireModule:
        case Opcode::EvaluateRequiredModule:
            sites.requireEdges.append(RequireEdgeUse { { value.referrer(), value.module() }, value.specifier() });
            break;
        case Opcode::ModuleBuiltin:
            // Each run of a module that runs again has an `import.meta` of its own, so no word holds it for good.
            if (!m_program.at(value.module()).flags.contains(ModuleFlags::RunsAgain))
                sites.moduleBuiltins.append(ModuleBuiltinUse { value.builtin(), value.module() });
            break;
        case Opcode::ModuleNamespace:
            if (value.phase() == ImportPhase::Evaluation)
                sites.moduleNamespaces.append(value.module());
            break;
        case Opcode::DefineAccessors:
            for (PropertyKeyID key : value.keys())
                appendKeyCell(key);
            break;
        case Opcode::DefineOwnProperty:
            appendFieldCreationByDefinition(value);
            break;
        case Opcode::GetGlobalBinding:
            sites.globalBindingSites.append(GlobalBindingSite { { value.key(), value.seesScriptBindings(), false, value.isForTypeOf() }, m_id, point, id });
            break;
        case Opcode::SetGlobalBinding:
            sites.globalBindingSites.append(GlobalBindingSite { { value.key(), value.seesScriptBindings(), true }, m_id, point, id });
            break;
        case Opcode::PropSet:
            if (value.isFreeName())
                sites.globalBindingSites.append(GlobalBindingSite { { value.key(), true, true }, m_id, point, id, true });
            break;
        default:
            break;
        }

        scanTypes(value);
        forEachVariable(value, [&](VariableID variable) {
            sites.variables.append(variable);
        });
    }

    const PlacementContext& m_context;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    std::optional<FunctionAnalyses> m_ownAnalyses;
    FunctionSiteWeights m_weights;
    FunctionID m_source;
    PlacedFunctionSites* m_sites { nullptr };
};

bool SiteScanner::isWeightRead(const Value& value) const
{
    auto someReceiverLacksSlot = [&](PropertyKeyID key) {
        return !m_context.slots().agreementOf(m_program, key, value.receivers()).coversEveryMember();
    };
    switch (value.opcode) {
    case Opcode::ShapeTest:
        return !!value.layouts();
    case Opcode::TypeTest:
        return !!m_program.at(value.testedType()).layouts;
    case Opcode::PutSlots:
        return std::ranges::any_of(value.placedKeys(), [&](PlacedKey placedKey) {
            return someReceiverLacksSlot(placedKey.key);
        });
    default:
        return !slotRowOpcodes.contains(value.opcode) || someReceiverLacksSlot(value.key());
    }
}

} // anonymous namespace

template<typename Use>
static void mergeSiteList(const ParallelLoops& loops, FunctionSitesArray functions, PlacedSites& sites, Vector<Use> SiteLists::* list)
{
    unsigned number = KeptSites::numberOf<Use>(list);
    size_t size = 0;
    for (KeptSites* function : functions)
        size += function->list<Use>(number).size();
    Vector<Use>& all = sites.*list;
    all.reserveInitialCapacity(size);
    for (KeptSites* function : functions)
        all.appendVector(WTF::move(function->list<Use>(number)));
    constexpr bool isWeighted = requires(Use use) { use.weight; };
    // The merge rounds of a parallel sort copy every element into a default-constructed buffer.
    if constexpr (isWeighted || (std::is_trivially_copyable_v<Use> && std::is_default_constructible_v<Use>))
        parallelSort(loops, all.mutableSpan(), isOrderedBefore<Use>);
    else
        AOT::sortInLine(all, isOrderedBefore<Use>);
    foldEqualUses(all);
    if constexpr (isWeighted)
        all.shrinkToFit();
}

static bool classDeclares(const Layout& layout, ClassSide side, PropertyKeyID key)
{
    return std::ranges::any_of(layout.classFlow.members, [&](const ClassMember& member) {
        return member.side == side && member.key == key;
    }) || (side == ClassSide::Static && std::ranges::any_of(layout.staticFields, [&](const StaticField& field) {
        return field.key == key;
    }));
}

static bool isCreatedAtDefinition(const PlacementContext& context, const GuardUse& guard)
{
    const Vector<ClassMember>& members = context.program.at(guard.classLayout).classFlow.members;
    const Vector<MemberCreation>& creations = context.slots().classCreation(guard.classLayout).members;
    return std::ranges::any_of(members, [&](const ClassMember& member) {
        if (member.side != guard.side || member.key != guard.key)
            return false;
        size_t index = &member - members.begin();
        return index >= creations.size() || creations[index] == MemberCreation::AtDefinition;
    });
}

// The run time arms the guard of a member after its DefineMethod only while it knows that the class was evaluated once.
static void addEvaluationGuards(const PlacementContext& context, Vector<GuardUse>& guards)
{
    size_t numberOfSiteGuards = guards.size();
    for (size_t index = 0; index < numberOfSiteGuards; ++index) {
        GuardUse guard = guards[index];
        bool isProvenOnce = isEvaluatedOnce(context.program, context.program.at(guard.classLayout));
        if (guard.kind == MemberGuardKind::Member && !isProvenOnce && isCreatedAtDefinition(context, guard))
            guards.append(GuardUse { guard.classLayout, ClassSide::Static, MemberGuardKind::EvaluatedOnce, { }, guard.weight });
    }
    if (guards.size() != numberOfSiteGuards)
        sortAndFold(guards);
}

// The run time makes `name` and `length` of a class object when somebody asks for them, and follows neither. A condition without a bit never holds.
static bool speaksOfNameOrLengthMadeOnDemand(const Program& program, const GuardUse& guard)
{
    if (guard.side != ClassSide::Static || guard.kind != MemberGuardKind::Member)
        return false;
    if (program.nameClassOf(guard.key) != NameClass::Length && guard.key != program.keyOf(BuiltinNames::name))
        return false;
    return !classDeclares(program.at(guard.classLayout), ClassSide::Static, guard.key);
}

static bool declaresOrIsBornWith(const Program& program, const Layout& layout, ClassSide side, PropertyKeyID key)
{
    if (classDeclares(layout, side, key))
        return true;
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Prototype || nameClass == NameClass::Length || key == program.keyOf(BuiltinNames::name);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The guards of such a class hold from the start because its records say all there is to say. They do not say what a parent has that nobody knows. What the prototype object of a built-in parent has, the compiler knows.
static bool isLookedForAboveRecords(const PlacementContext& context, const GuardUse& guard)
{
    if ((guard.kind != MemberGuardKind::Member && guard.kind != MemberGuardKind::PassedByStores) || !context.slots().classCreation(guard.classLayout).guardsHoldWhileClear)
        return false;
    const Program& program = context.program;
    LayoutID current = guard.classLayout;
    for (size_t steps = 0; current && steps < program.layouts.size(); ++steps) {
        const Layout& layout = program.at(current);
        if (declaresOrIsBornWith(program, layout, guard.side, guard.key))
            return false;
        if (layout.flow.baseState == BaseState::BuiltinClass)
            return guard.kind != MemberGuardKind::Member || guard.side != ClassSide::Prototype;
        if (layout.flow.baseState == BaseState::Unknown && layout.has(LayoutFlags::HasHeritage) && !layout.base)
            return true;
        current = expectedBaseOf(layout);
    }
    return false;
}

template<typename IDType>
static void setBits(BitVector& bits, const Vector<IDType>& ids)
{
    for (IDType id : ids)
        bits.quickSet(id.index());
}

static void mergeBits(const Program& program, FunctionSitesArray functions, PlacedSites& sites)
{
    sites.variablesNamedByCode.ensureSize(program.variables.size());
    sites.typesInRunTimePayloads.ensureSize(program.types.size());
    sites.typesTestedByCode.ensureSize(program.types.size());
    sites.functionsWithAllocatedClosures.ensureSize(program.functions.size());
    unsigned variables = KeptSites::numberOf(&PlacedFunctionSites::variables);
    unsigned types = KeptSites::numberOf(&PlacedFunctionSites::types);
    unsigned typesTestedByCode = KeptSites::numberOf(&PlacedFunctionSites::typesTestedByCode);
    unsigned allocatedClosures = KeptSites::numberOf(&PlacedFunctionSites::allocatedClosures);
    for (const KeptSites* function : functions) {
        setBits(sites.variablesNamedByCode, function->list<VariableID>(variables));
        setBits(sites.typesInRunTimePayloads, function->list<TypeID>(types));
        setBits(sites.typesTestedByCode, function->list<TypeID>(typesTestedByCode));
        setBits(sites.functionsWithAllocatedClosures, function->list<FunctionID>(allocatedClosures));
        sites.maxClosureCaptures = std::max(sites.maxClosureCaptures, function->maxClosureCaptures);
    }
}

// The stores are numbered in their sequence, and the remainder of a number says where in the table the store's entries begin. A store whose remainder another store
// of one of its layouts has takes the next remainder that none of them has. Where there is none left the stores meet, and their entries name them.
static void numberStoresThatMayAdd(const Program& program, FunctionSitesArray functions, PlacedSites& sites)
{
    using Remainders = WTF::BitSet<numberOfKeyOrderStepCaches>;
    struct RemaindersOfLayout {
        Remainders taken;
        Remainders takenTwice;
        uint32_t numberTaken { 0 };
    };
    Vector<RemaindersOfLayout> remainders;
    Vector<uint32_t> indexPlusOneOfLayout;
    indexPlusOneOfLayout.fill(0, program.layouts.size());
    auto remaindersOf = [&](LayoutID layout) -> RemaindersOfLayout* {
        uint32_t indexPlusOne = indexPlusOneOfLayout[layout.index()];
        return indexPlusOne ? &remainders[indexPlusOne - 1] : nullptr;
    };

    sites.firstStoreThatMayAdd.fill(0, program.functions.size() + 1);
    unsigned receiversOfStoresThatMayAdd = KeptSites::numberOf(&PlacedFunctionSites::receiversOfStoresThatMayAdd);
    for (const KeptSites* function : functions)
        sites.firstStoreThatMayAdd[function->function.index() + 1] = function->list<LayoutSetID>(receiversOfStoresThatMayAdd).size();
    for (size_t index = 1; index < sites.firstStoreThatMayAdd.size(); ++index)
        sites.firstStoreThatMayAdd[index] += sites.firstStoreThatMayAdd[index - 1];
    sites.placesOfStoresThatMayAdd.grow(sites.firstStoreThatMayAdd.last());

    Vector<uint32_t> storesWithRemainder;
    storesWithRemainder.fill(0, numberOfKeyOrderStepCaches);
    auto forEachStore = [&](const Invocable<void(PlaceOfStoreThatMayAdd&, std::span<const LayoutID>)> auto& functor) {
        for (const KeptSites* function : functions) {
            uint32_t store = sites.firstStoreThatMayAdd[function->function.index()];
            for (LayoutSetID receivers : function->list<LayoutSetID>(receiversOfStoresThatMayAdd))
                functor(sites.placesOfStoresThatMayAdd[store++], program.at(receivers).completeMembers());
        }
    };
    uint32_t storesInFront = 0;
    forEachStore([&](PlaceOfStoreThatMayAdd& place, std::span<const LayoutID> members) {
        uint32_t remainder = storesInFront++ % numberOfKeyOrderStepCaches;
        bool isTaken = false;
        bool hasRemainderLeft = true;
        for (LayoutID member : members) {
            if (RemaindersOfLayout* ofMember = remaindersOf(member)) {
                isTaken |= ofMember->taken.get(remainder);
                hasRemainderLeft &= ofMember->numberTaken < numberOfKeyOrderStepCaches;
            }
        }
        if (isTaken && hasRemainderLeft) {
            Remainders taken;
            for (LayoutID member : members) {
                if (RemaindersOfLayout* ofMember = remaindersOf(member))
                    taken.merge(ofMember->taken);
            }
            size_t next = taken.findBit(remainder, false);
            if (next >= numberOfKeyOrderStepCaches)
                next = taken.findBit(0, false);
            if (next < numberOfKeyOrderStepCaches)
                remainder = next;
        }
        for (LayoutID member : members) {
            uint32_t& indexPlusOne = indexPlusOneOfLayout[member.index()];
            if (!indexPlusOne) {
                remainders.append(RemaindersOfLayout { });
                indexPlusOne = remainders.size();
            }
            RemaindersOfLayout& ofMember = remainders[indexPlusOne - 1];
            if (ofMember.taken.testAndSet(remainder))
                ofMember.takenTwice.set(remainder);
            else
                ++ofMember.numberTaken;
        }
        place.number = remainder + numberOfKeyOrderStepCaches * storesWithRemainder[remainder]++;
    });
    forEachStore([&](PlaceOfStoreThatMayAdd& place, std::span<const LayoutID> members) {
        place.meetsOtherStores = std::ranges::any_of(members, [&](LayoutID member) {
            return remaindersOf(member)->takenTwice.get(place.number % numberOfKeyOrderStepCaches);
        });
    });
}

static bool isMadeOnlyWhereItIsNamed(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.kind != LayoutKind::Record && layout.kind != LayoutKind::Instance && layout.kind != LayoutKind::Container)
        return false;
    return id.index() < program.numberOfLayoutsAsRead() && !layout.has(LayoutFlags::IsDerived) && !layout.has(LayoutFlags::IsOfCreatedObject) && !layout.flow.observedUses.contains(ObservedUse::ReachesTypedParse);
}

static BitVector findLayoutsOutOfUse(const Program& program, const BitVector& bodiesWithoutCode, FunctionSitesArray functions, std::span<const FunctionID> functionsWhereNothingWasFound)
{
    Vector<LayoutID> worklist;
    BitVector inUse(program.layouts.size());
    auto use = [&](LayoutID layout) {
        if (layout && !inUse.quickSet(layout.index()))
            worklist.append(layout);
    };
    BitVector layoutSets(program.types.numberOfLayoutSets());
    BitVector hasEntrypoint(program.functions.size());
    auto useWhatRecordNames = [&](FunctionID function) {
        use(program.at(function).constructedClass);
        if (const auto* entrypoint = program.at(function).creator.tryAs<CreatorRecord::Entrypoint>())
            hasEntrypoint.set(entrypoint->target.index());
    };
    unsigned layoutsOfData = KeptSites::numberOf(&PlacedFunctionSites::layoutsOfData);
    unsigned layouts = KeptSites::numberOf(&PlacedFunctionSites::layouts);
    unsigned layoutSetsOfFunction = KeptSites::numberOf(&PlacedFunctionSites::layoutSets);
    for (const KeptSites* function : functions) {
        for (LayoutID layout : function->list<LayoutID>(layoutsOfData))
            use(layout);
        if (bodiesWithoutCode.get(function->function.index()))
            continue;
        for (LayoutID layout : function->list<LayoutID>(layouts))
            use(layout);
        setBits(layoutSets, function->list<LayoutSetID>(layoutSetsOfFunction));
        useWhatRecordNames(function->function);
    }
    for (FunctionID function : functionsWhereNothingWasFound) {
        if (!bodiesWithoutCode.get(function.index()))
            useWhatRecordNames(function);
    }
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        FunctionID constructor = program.layouts[index].constructor;
        if (constructor && hasEntrypoint.get(constructor.index()))
            use(LayoutID(index));
    }
    for (size_t index : layoutSets) {
        for (LayoutID member : program.at(LayoutSetID(static_cast<uint32_t>(index))).written())
            use(member);
    }
    for (const Module& module : program.modules) {
        if (module.data)
            forEachLayout(program.at(module.data), use);
    }
    for (const ExpectedResult& expected : program.expectedResults)
        forEachLayout(expected, use);
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (!isMadeOnlyWhereItIsNamed(program, LayoutID(index)))
            use(LayoutID(index));
    }
    while (!worklist.isEmpty())
        forEachLayout(program.at(worklist.takeLast()), use);

    BitVector outOfUse(program.layouts.size());
    for (unsigned index = 0; index < program.layouts.size(); ++index)
        outOfUse.quickSet(index, !inUse.quickGet(index));
    return outOfUse;
}

LayoutID findClosureLayout(const Program& program)
{
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Closure)
            return LayoutID(index);
    }
    return { };
}

struct BodyScansForPlacement::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    // No operation names the layout of closures where numberAndEncode() has yet to make it.
    State(const Program& program, const PlacementInputs& inputs, BuildContext& build)
        : sizeClasses(build.options().sizeClassProgression)
        , context { program, inputs, sizeClasses, build, build.recordedOptions().useWideReferences(), findClosureLayout(program) }
        , scans(program.functions.size())
    {
        // Made here, by one thread: whoever compares with it has to find the same record.
        nothingFound();
    }

    // Most small functions have nothing that placement asks about. They share one record.
    static KeptSites* nothingFound()
    {
        static KeptSites* record = KeptSites::create({ });
        return record;
    }

    struct Scans {
        Scans(size_t size) { ofFunction.fill(nullptr, size); }
        Scans(Scans&&) = default;
        ~Scans()
        {
            for (KeptSites* scan : ofFunction) {
                if (scan && scan != nothingFound())
                    KeptSites::destroy(scan);
            }
        }

        Vector<KeptSites*> ofFunction;
    };

    CellSizeClasses sizeClasses;
    PlacementContext context;
    Scans scans;
    uint64_t numberOfOperationsWithWordForSiteCache { 0 };
};

BodyScansForPlacement::BodyScansForPlacement(const Program& program, const PlacementInputs& inputs, BuildContext& build)
    : m_state(makeUnique<State>(program, inputs, build))
{
}

BodyScansForPlacement::~BodyScansForPlacement() = default;

void BodyScansForPlacement::scan(FunctionID id, FunctionAnalyses* analyses)
{
    PlacedFunctionSites scan;
    SiteScanner(m_state->context, id, analyses).run(scan);
    KeptSites*& kept = m_state->scans.ofFunction[id.index()];
    if (kept && kept != State::nothingFound())
        KeptSites::destroy(kept);
    if (scan.hasFoundNothing()) {
        kept = State::nothingFound();
        return;
    }
    scan.shrinkToFit();
    kept = KeptSites::create(WTF::move(scan));
}

void BodyScansForPlacement::didAddFunctions()
{
    Vector<KeptSites*>& scans = m_state->scans.ofFunction;
    size_t size = m_state->context.program.functions.size();
    RELEASE_ASSERT(size >= scans.size());
    scans.insertFill(scans.size(), nullptr, size - scans.size());
}

uint64_t BodyScansForPlacement::numberOfOperationsWithWordForSiteCache() const
{
    return m_state->numberOfOperationsWithWordForSiteCache;
}

const Vector<KeyCellUse>& BodyScansForPlacement::keyCellUsesOf(FunctionID id) const
{
    RELEASE_ASSERT(m_state->scans.ofFunction[id.index()]);
    return m_state->scans.ofFunction[id.index()]->list(&PlacedFunctionSites::keyCellUses);
}

const Vector<GlobalBindingSite>& BodyScansForPlacement::globalBindingSitesOf(FunctionID id) const
{
    RELEASE_ASSERT(m_state->scans.ofFunction[id.index()]);
    return m_state->scans.ofFunction[id.index()]->list(&PlacedFunctionSites::globalBindingSites);
}

PlacedSites BodyScansForPlacement::takeSites(const PlacementContext& context)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    RELEASE_ASSERT(m_state->scans.ofFunction.size() == program.functions.size());
    Vector<KeptSites*> functionsWithBodies;
    Vector<FunctionID> functionsWhereNothingWasFound;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        KeptSites* scan = m_state->scans.ofFunction[index];
        RELEASE_ASSERT(scan);
        if (scan == State::nothingFound()) {
            functionsWhereNothingWasFound.append(FunctionID(index));
            continue;
        }
        functionsWithBodies.append(scan);
        m_state->numberOfOperationsWithWordForSiteCache += scan->numberOfOperationsWithWordForSiteCache;
    }
    FunctionSitesArray functions = functionsWithBodies.span();
    PlacedSites sites;
    auto mergeEach = [&](auto... lists) {
        (mergeSiteList(loops, functions, sites, lists), ...);
    };
    runTasksInParallel(loops, {
        [&] { mergeEach(&SiteLists::fieldAccesses); },
        [&] { mergeEach(&SiteLists::shapeTests); },
        [&] {
            unsigned sourceFieldCreations = KeptSites::numberOf(&PlacedFunctionSites::sourceFieldCreations);
            for (KeptSites* function : functions)
                sites.sourceFieldCreations.appendVector(WTF::move(function->list<FieldCreationUse>(sourceFieldCreations)));
            std::ranges::stable_sort(sites.sourceFieldCreations, { }, &FieldCreationUse::layout);
            mergeEach(&SiteLists::allocations, &SiteLists::keyOrderTests, &SiteLists::fieldCreations, &SiteLists::guards);
            sites.guards.removeAllMatching([&](const GuardUse& guard) {
                return speaksOfNameOrLengthMadeOnDemand(program, guard) || isLookedForAboveRecords(context, guard);
            });
            addEvaluationGuards(context, sites.guards);
            mergeEach(&SiteLists::guardSets, &SiteLists::tableCalls);
        },
        [&] { mergeEach(&SiteLists::profiledArrayLiterals, &SiteLists::regExpLiterals, &SiteLists::templateObjects, &SiteLists::copyOnWriteArrays, &SiteLists::arraysWithStorageInImage, &SiteLists::dataMadeOnce); },
        [&] { mergeEach(&SiteLists::builtinCells, &SiteLists::bigIntConstants, &SiteLists::moduleBuiltins, &SiteLists::moduleNamespaces, &SiteLists::requireEdges); },
        [&] { mergeBits(program, functions, sites); },
        [&] { numberStoresThatMayAdd(program, functions, sites); },
        [&] {
            sites.bodiesWithoutCode = findBodiesWithoutCode(program, context.build);
            sites.layoutsOutOfUse = findLayoutsOutOfUse(program, sites.bodiesWithoutCode, functions, functionsWhereNothingWasFound.span());
        },
    });
    return sites;
}

void BodyScansForPlacement::destroyLater()
{
    m_state->context.build.destroyLater(WTF::move(m_state->scans));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
