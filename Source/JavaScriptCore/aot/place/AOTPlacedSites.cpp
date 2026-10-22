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
#include "AOTPlacementCheck.h"
#include "AOTPropertyAccessMembers.h"
#include "AOTSchemaWalkers.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <tuple>
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet slotRowOpcodes = Value::opcodesWithFieldAccess | Value::opcodesWithPlacedKeys;
static constexpr OpcodeSet shapeTestOpcodes { Opcode::ShapeTest, Opcode::TypeTest };
static constexpr OpcodeSet weighedOpcodes = slotRowOpcodes | shapeTestOpcodes | Value::opcodesWithGuardBit | OpcodeSet { Opcode::CallTable };
static constexpr OpcodeSet literalSiteOpcodes = Value::opcodesWithStaticsOffset | Value::opcodesWithOrderWord;

static_assert(Value::opcodesWithGuardBit == OpcodeSet { Opcode::AreMembersUnchanged });

namespace {

struct RequireEdgeUse {
    RequireEdge edge;
    StringID specifier;

    friend auto operator<=>(const RequireEdgeUse&, const RequireEdgeUse&) = default;
};

struct FunctionSites {
    Vector<FieldAccessUse> fieldAccesses;
    Vector<ShapeTestUse> shapeTests;
    Vector<AllocationUse> allocations;
    Vector<KeyOrderTestUse> keyOrderTests;
    Vector<FieldCreationUse> fieldCreations;
    Vector<GuardUse> guards;
    Vector<GuardSetUse> guardSets;
    Vector<TableCallUse> tableCalls;
    Vector<SourceSite> profiledArrayLiterals;
    Vector<SourceSite> regExpLiterals;
    Vector<SourceSite> templateObjects;
    Vector<DataID> copyOnWriteArrays;
    Vector<DataID> arraysWithStorageInImage;
    Vector<BuiltinCellKey> builtinCells;
    Vector<StringID> bigIntConstants;
    Vector<RequireEdgeUse> requireEdges;
    Vector<ModuleBuiltinUse> moduleBuiltins;
    Vector<VariableID> variables;
    Vector<TypeID> types;
    Vector<TypeID> typesTestedByCode;
    Vector<FunctionID> allocatedClosures;
    Vector<DataID> data;
    unsigned maxClosureCaptures { 0 };
    FunctionID function;
    uint32_t numberOfStoresThatMayAdd { 0 };
};

// Of the functions with a body, in their order.
using FunctionSitesArray = std::span<FunctionSites* const>;

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

static void checkMemberList(PlacementFailureSink& sink, std::span<const LayoutID> members)
{
    for (size_t i = 1; i < members.size(); ++i) {
        if (members[i - 1] >= members[i])
            sink.fail(PlacementRule::MembersNotSorted, members[i]);
    }
}

static void checkLayoutInputs(PlacementFailureSink& sink, const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (!layout.isEnvironment())
        return;
    if (std::ranges::any_of(layout.flow.members, [&](LayoutID member) { return member != id; }))
        sink.fail(PlacementRule::EnvironmentHasMembers, id);
}

void checkPlacementInputs(const PlacementContext& context)
{
    const Program& program = context.program;
    PlacementFailureSink sink(context.build);
    for (unsigned i = 0; i < program.types.numberOfLayoutSets(); ++i)
        checkMemberList(sink, program.at(LayoutSetID(i)).members.span());
    for (unsigned i = 0; i < program.layouts.size(); ++i)
        checkLayoutInputs(sink, program, LayoutID(i));
    for (const Variable& variable : program.variables) {
        bool startsUndefined = variable.initialization == VariableInitialization::Undefined && variable.flow.initialization == InitializationState::Unknown;
        bool holdsOnlyDeclaredType = !variable.declared.flags.contains(PositionFlags::IsExpected);
        if (startsUndefined && holdsOnlyDeclaredType && !(program.at(variable.declared.type).primitives & Primitive::Undefined))
            sink.fail(PlacementRule::VariableLacksInitialValue);
    }
}

static SlotAccessKey keyOf(const FieldAccessUse& use) { return use.access; }
static auto keyOf(const ShapeTestUse& use) { return std::tuple(use.tested, use.known); }
static auto keyOf(const GuardUse& use) { return std::tuple(use.classLayout, use.side, use.kind, use.key); }

namespace {

struct GuardSetKey {
    std::span<const GuardUse> guards;

    friend bool operator==(GuardSetKey a, GuardSetKey b)
    {
        return std::ranges::equal(a.guards, b.guards, [](const GuardUse& guard, const GuardUse& other) {
            return keyOf(guard) == keyOf(other);
        });
    }
    friend bool operator<(GuardSetKey a, GuardSetKey b)
    {
        return std::ranges::lexicographical_compare(a.guards, b.guards, [](const GuardUse& guard, const GuardUse& other) {
            return keyOf(guard) < keyOf(other);
        });
    }
};

} // anonymous namespace

static GuardSetKey keyOf(const GuardSetUse& use) { return { use.guards.span() }; }

template<typename Use>
static bool isOrderedBefore(const Use& a, const Use& b)
{
    return keyOf(a) < keyOf(b);
}

template<typename Use>
static void foldEqualUses(Vector<Use>& sorted)
{
    size_t count = 0;
    for (const Use& use : sorted) {
        if (!count || keyOf(sorted[count - 1]) != keyOf(use)) {
            sorted[count++] = use;
            continue;
        }
        Use& folded = sorted[count - 1];
        folded.weight = saturatedSum(folded.weight, use.weight);
        if constexpr (requires { folded.sites; })
            folded.sites += use.sites;
    }
    sorted.shrink(count);
}

template<typename Use>
static void sortAndFold(Vector<Use>& uses)
{
    std::ranges::sort(uses, isOrderedBefore<Use>);
    foldEqualUses(uses);
}

template<typename T>
static void sortAndRemoveDuplicates(Vector<T>& list)
{
    std::ranges::sort(list, std::less<T> { });
    removeRepeatedElements(list);
}

static void sortAndRemoveDuplicates(Vector<BuiltinCellKey>& cells)
{
    std::ranges::sort(cells, { }, &BuiltinCellKey::encoded);
    removeRepeatedElements(cells);
}

namespace {

class SiteScanner {
public:
    SiteScanner(const PlacementContext&, FunctionID);

    FunctionSites run();

private:
    LayoutSetID knownSetOf(ValueID) const;
    bool isUnordered(const Value&) const;
    bool isWeightRead(const Value&) const;
    void appendShapeTest(LayoutSetID tested, ValueID subject, SiteWeight);
    void appendAllocation(LayoutID, bool keysAreNeverEnumerated, std::span<const FieldIndex> initialFields, bool holdsInnerObjects = false);
    void appendFieldCreations(LayoutSetID receivers, std::span<const PropertyKeyID> keys);
    void appendCell(BuiltinCellKey);
    void appendCellsOf(BuiltinOverloadID);
    void appendCellsOfMember(BuiltinID);

    void scanValue(const Value&, SiteWeight);
    void scanTerminator(const Terminator&, SiteWeight);
    void scanSlotRow(const Value&, SiteWeight);
    void scanShapeTests(const Value&, SiteWeight);
    void scanAllocation(const Value&);
    void scanData(const Value&);
    void scanDataTree(const DataNode& root);
    void scanDataObject(const DataNode::Object&);
    void scanGuards(const Value&, SiteWeight);
    void appendGuards(Vector<GuardUse, 4>&&, SiteWeight);
    void scanTableCall(const Value&, SiteWeight);
    void scanLiteralSite(const Value&);
    void scanConstants(const Value&);
    void scanTypes(const Value&);
    void scanValidatedTypes(const Value&);
    void scanVariables(const Value&);

    const PlacementContext& m_context;
    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    FunctionAnalyses m_analyses;
    FunctionSiteWeights m_weights;
    bool m_hasWeighedBlock { false };
    FunctionSites m_sites;
};

SiteScanner::SiteScanner(const PlacementContext& context, FunctionID id)
    : m_context(context)
    , m_program(context.program)
    , m_id(id)
    , m_function(m_program.at(id))
    , m_analyses({ context.build, m_program, &context.inputs.flow, nullptr, id }, m_function)
    , m_weights(m_program, context.inputs.heat, id, m_analyses)
{
}

LayoutSetID SiteScanner::knownSetOf(ValueID id) const
{
    const Type& type = m_program.typeOf(m_function, id);
    if (!type.hasOnlyLayouts(m_program) || !type.layouts || !m_program.at(type.layouts).isComplete)
        return { };
    return type.layouts;
}

bool SiteScanner::isUnordered(const Value& allocation) const
{
    LayoutID layout = allocation.layout();
    if (m_context.isEnvironment(layout))
        return false;
    if (AllocationClassID allocationClass = allocation.allocationClass())
        return m_context.inputs.flow.at(allocationClass).keys == KeyEnumeration::NeverEnumerated;
    return m_context.slots().layout(layout).keysAreNeverEnumerated;
}

// Of an operation with a weighed opcode. The shapes are numbered by the weights of the sites that test them, and an access tests shapes only where some receiver has no slot for the field:
// FieldAccess::Coverage::SomeMembers. The first weight of a function takes an estimate of all its blocks.
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

void SiteScanner::appendShapeTest(LayoutSetID tested, ValueID subject, SiteWeight weight)
{
    if (tested)
        m_sites.shapeTests.append(ShapeTestUse { tested, knownSetOf(subject), weight, 1 });
}

void SiteScanner::appendAllocation(LayoutID layout, bool keysAreNeverEnumerated, std::span<const FieldIndex> initialFields, bool holdsInnerObjects)
{
    if (!m_sites.allocations.isEmpty()) {
        const AllocationUse& last = m_sites.allocations.last();
        if (last.layout == layout && last.keysAreNeverEnumerated == keysAreNeverEnumerated && last.holdsInnerObjects == holdsInnerObjects && std::ranges::equal(last.initialFields, initialFields))
            return;
    }
    m_sites.allocations.append(AllocationUse { layout, keysAreNeverEnumerated, holdsInnerObjects, Vector<FieldIndex>(initialFields) });
}

void SiteScanner::appendFieldCreations(LayoutSetID receivers, std::span<const PropertyKeyID> keys)
{
    const SlotPlan& slots = m_context.slots();
    for (LayoutID member : m_program.at(receivers).members) {
        if (!m_context.isCompiledObject(member) || slots.layout(member).keysAreNeverEnumerated)
            continue;
        Vector<FieldIndex> fields;
        for (PropertyKeyID key : keys) {
            std::optional<FieldIndex> field = m_program.at(member).findField(key);
            if (field && slots.slot(member, *field).canBeCreatedByStore())
                fields.append(*field);
        }
        if (!fields.isEmpty())
            m_sites.fieldCreations.append(FieldCreationUse { member, WTF::move(fields) });
    }
}

void SiteScanner::appendCell(BuiltinCellKey key)
{
    if (key.hasCell(m_program.builtins()))
        m_sites.builtinCells.append(key);
}

void SiteScanner::appendCellsOf(BuiltinOverloadID overload)
{
    appendCellsOfMember(m_program.builtins().at(overload).builtin);
}

void SiteScanner::appendCellsOfMember(BuiltinID id)
{
    const BuiltinTable& builtins = m_program.builtins();
    for (BuiltinCellKey key : { BuiltinCellKey::object(id), BuiltinCellKey::getter(id), BuiltinCellKey::setter(id) })
        appendCell(key);
    if (BuiltinID holder = builtins.at(id).holder)
        appendCell(BuiltinCellKey::object(holder));
}

void SiteScanner::scanSlotRow(const Value& value, SiteWeight weight)
{
    if (value.opcode == Opcode::PutSlots) {
        Vector<PropertyKeyID, 4> keysThatMayBeAdded;
        for (PlacedKey placedKey : value.placedKeys()) {
            m_sites.fieldAccesses.append(FieldAccessUse { { placedKey.key, value.receivers(), true }, weight, 1 });
            if (placedKey.mayAdd)
                keysThatMayBeAdded.append(placedKey.key);
        }
        appendFieldCreations(value.receivers(), keysThatMayBeAdded.span());
        return;
    }
    m_sites.fieldAccesses.append(FieldAccessUse { { value.key(), value.receivers(), isSlotStore(value.opcode) }, weight, 1 });
    if (value.opcode == Opcode::PutSlotOrAdd) {
        PropertyKeyID key = value.key();
        appendFieldCreations(value.receivers(), singleElementSpan(key));
    }
}

void SiteScanner::scanShapeTests(const Value& value, SiteWeight weight)
{
    switch (value.opcode) {
    case Opcode::ShapeTest:
        appendShapeTest(value.layouts(), value.objectOperand(), weight);
        return;
    case Opcode::TypeTest:
        appendShapeTest(m_program.at(value.testedType()).layouts, value.valueOperand(), weight);
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void SiteScanner::scanAllocation(const Value& value)
{
    switch (value.opcode) {
    case Opcode::NewClosure:
        m_sites.maxClosureCaptures = std::max<unsigned>(m_sites.maxClosureCaptures, value.numberOfVariadicOperands());
        if (value.closureForm() != ClosureForm::Singleton)
            m_sites.allocatedClosures.append(value.function());
        return;
    case Opcode::MaterializeEnvironment:
        appendAllocation(value.layout(), false, { });
        return;
    case Opcode::NewObject:
        if (m_context.receivesShape(value.layout()))
            appendAllocation(value.layout(), isUnordered(value), value.fields(), takesCellWithInnerObjects(m_context.slots(), value));
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void SiteScanner::scanData(const Value& value)
{
    m_sites.data.append(value.data());
    if (value.opcode != Opcode::Data || value.encoding() != DataEncoding::CopyOnWriteArray)
        return;
    bool isStorage = isStorageOfArray(m_program.at(value.data()).root.as<DataNode::Array>());
    (isStorage ? m_sites.arraysWithStorageInImage : m_sites.copyOnWriteArrays).append(value.data());
}

void SiteScanner::scanDataObject(const DataNode::Object& object)
{
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

void SiteScanner::scanDataTree(const DataNode& root)
{
    forEachDataNodeInPreorder(root, [&](uint32_t, const DataNode& node) {
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
            m_sites.bigIntConstants.append(node.as<DataNode::BigInt>().digits);
            return;
        case DataNode::Kind::Object:
            scanDataObject(node.as<DataNode::Object>());
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

// The run time sets and clears the bit of one condition. A site with several conditions tests the bit of their set, which follows the bits of its members.
void SiteScanner::scanGuards(const Value& value, SiteWeight weight)
{
    appendGuards(guardsOf(value.conditions()), weight);
}

// Nothing tests the bit of an entry of the method table: the run time keeps the entry as the bit says.
void SiteScanner::scanTableCall(const Value& value, SiteWeight weight)
{
    m_sites.tableCalls.append({ selectorOf(m_program, value), value.receivers() });
    PropertyAccessContext access { m_program, &m_context.inputs.flow };
    MemberPartition partition = partitionMembers(access, m_program.at(value.receivers()).members.span(), value.key(), Access::Read);
    for (const MemberArm& arm : partition.arms) {
        if (arm.answer.kind == MemberKind::Method && !arm.answer.conditions.isEmpty())
            appendGuards(guardsOf(arm.answer.conditions.span()), weight);
    }
}

void SiteScanner::appendGuards(Vector<GuardUse, 4>&& guards, SiteWeight weight)
{
    for (GuardUse guard : guards) {
        guard.weight = weight;
        m_sites.guards.append(guard);
    }
    if (guards.size() > 1)
        m_sites.guardSets.append(GuardSetUse { WTF::move(guards), weight });
}

void SiteScanner::scanLiteralSite(const Value& value)
{
    switch (value.opcode) {
    case Opcode::NewArray:
        switch (value.literalStorage()) {
        case ArrayLiteralStorage::Fresh:
            return;
        case ArrayLiteralStorage::Profiled:
            m_sites.profiledArrayLiterals.append(sourceSiteOf(m_program, m_id, value));
            return;
        }
        RELEASE_ASSERT_NOT_REACHED();
    case Opcode::NewRegExp:
    case Opcode::SharedRegExpLiteral:
        m_sites.regExpLiterals.append(sourceSiteOf(m_program, m_id, value));
        return;
    case Opcode::NewTemplateObject:
        m_sites.templateObjects.append(sourceSiteOf(m_program, m_id, value));
        return;
    case Opcode::HasKeyOrder:
        m_sites.keyOrderTests.append(KeyOrderTestUse { value.layout(), Vector<FieldIndex>(value.fields()) });
        return;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

void SiteScanner::scanConstants(const Value& value)
{
    if (value.opcode == Opcode::Builtin)
        appendCell(BuiltinCellKey::object(m_program.builtins().canonicalID(value.builtin())));
    if (Value::opcodesWithOverload.contains(value.opcode))
        appendCellsOf(value.overload());
    if (value.opcode == Opcode::IsBuiltinLocationUnchanged)
        appendCellsOfMember(value.member());
    if (value.opcode == Opcode::GetBuiltinValue)
        appendCell(BuiltinCellKey::value(m_program.builtins().canonicalID(value.builtin())));
    if (value.opcode == Opcode::BigInt)
        m_sites.bigIntConstants.append(value.digits());
    if (value.opcode == Opcode::RequireModule)
        m_sites.requireEdges.append(RequireEdgeUse { { value.referrer(), value.module() }, value.specifier() });
    if (value.opcode == Opcode::ModuleBuiltin)
        m_sites.moduleBuiltins.append(ModuleBuiltinUse { value.builtin(), value.module() });
}

// The test of a proof is code, so the run time needs no record of the type: only the cells the type names, and a record of what a container holds.
void SiteScanner::scanValidatedTypes(const Value& value)
{
    const BuiltinTable& builtins = m_program.builtins();
    forEachType(value, [&](TypeID id) {
        const Type& type = m_program.at(id);
        for (BuiltinID builtin : type.builtins)
            appendCell(BuiltinCellKey::object(builtin));
        for (const ClassPart& part : type.classes) {
            if (!part.contentsAreProven)
                continue;
            for (TypeID contents : part.contents) {
                bool everyBuiltinHasCell = contents && std::ranges::all_of(m_program.at(contents).builtins, [&](BuiltinID builtin) {
                    return BuiltinCellKey::object(builtin).hasCell(builtins);
                });
                if (everyBuiltinHasCell)
                    m_sites.types.append(contents);
            }
        }
    });
}

void SiteScanner::scanTypes(const Value& value)
{
    if (familyOf(value.opcode) == OperationFamily::RuntimeValidation) {
        scanValidatedTypes(value);
        return;
    }
    // The one operation that hands a type to the run time.
    Vector<TypeID>& types = value.opcode == Opcode::ReportMismatch ? m_sites.types : m_sites.typesTestedByCode;
    forEachType(value, [&](TypeID type) {
        types.append(type);
    });
}

void SiteScanner::scanVariables(const Value& value)
{
    forEachVariable(value, [&](VariableID variable) {
        m_sites.variables.append(variable);
    });
}

void SiteScanner::scanValue(const Value& value, SiteWeight weight)
{
    if (slotRowOpcodes.contains(value.opcode))
        scanSlotRow(value, weight);
    if (shapeTestOpcodes.contains(value.opcode))
        scanShapeTests(value, weight);
    if (Value::opcodesWithAllocation.contains(value.opcode))
        scanAllocation(value);
    if (Value::opcodesWithData.contains(value.opcode))
        scanData(value);
    if (Value::opcodesWithGuardBit.contains(value.opcode))
        scanGuards(value, weight);
    if (value.opcode == Opcode::CallTable)
        scanTableCall(value, weight);
    if (literalSiteOpcodes.contains(value.opcode))
        scanLiteralSite(value);
    scanConstants(value);
    scanTypes(value);
    scanVariables(value);
}

void SiteScanner::scanTerminator(const Terminator& terminator, SiteWeight weight)
{
    switch (terminator.kind) {
    case TerminatorKind::SwitchOnShape:
        for (LayoutSetID tested : terminator.layoutSets())
            appendShapeTest(tested, terminator.subjectOperand(), weight);
        return;
    case TerminatorKind::TryCallInline:
        appendCellsOf(terminator.overload());
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

FunctionSites SiteScanner::run()
{
    m_sites.function = m_id;
    for (unsigned i = 0; i < m_function.blocks.size(); ++i) {
        const Block& block = m_function.blocks[i];
        std::optional<SiteWeight> weight;
        auto weightWhere = [&](bool isWeighed) -> SiteWeight {
            if (!isWeighed)
                return 0;
            if (!weight)
                weight = m_weights.weightOf(BlockID(i));
            m_hasWeighedBlock = true;
            return *weight;
        };
        for (ValueID id : block.values) {
            const Value& value = m_function.at(id);
            m_sites.numberOfStoresThatMayAdd += remembersKeyOrderStep(block, value);
            // Behind the first weight another costs less than the question.
            scanValue(value, weightWhere(weighedOpcodes.contains(value.opcode) && (m_hasWeighedBlock || isWeightRead(value))));
        }
        scanTerminator(block.terminator, weightWhere(block.terminator.kind == TerminatorKind::SwitchOnShape));
    }
    sortAndRemoveDuplicates(m_sites.data);
    for (DataID id : m_sites.data)
        scanDataTree(m_program.at(id).root);
    sortAndFold(m_sites.fieldAccesses);
    sortAndFold(m_sites.shapeTests);
    sortAndFold(m_sites.guards);
    sortAndFold(m_sites.guardSets);
    sortAndRemoveDuplicates(m_sites.tableCalls);
    sortAndRemoveDuplicates(m_sites.allocations);
    sortAndRemoveDuplicates(m_sites.builtinCells);
    return WTF::move(m_sites);
}

} // anonymous namespace

template<typename T>
static Vector<T> concatenate(FunctionSitesArray functions, Vector<T> FunctionSites::* list)
{
    size_t size = 0;
    for (FunctionSites* function : functions)
        size += (function->*list).size();
    Vector<T> all;
    all.reserveInitialCapacity(size);
    for (FunctionSites* function : functions)
        all.appendVector(WTF::move(function->*list));
    return all;
}

template<typename Use>
static Vector<Use> mergeWeightedUses(const ParallelLoops& loops, FunctionSitesArray functions, Vector<Use> FunctionSites::* list)
{
    Vector<Use> uses = concatenate(functions, list);
    parallelSort(loops, uses.mutableSpan(), isOrderedBefore<Use>);
    foldEqualUses(uses);
    uses.shrinkToFit();
    return uses;
}

template<typename T>
static Vector<T> mergeDistinct(const ParallelLoops& loops, FunctionSitesArray functions, Vector<T> FunctionSites::* list)
{
    Vector<T> all = concatenate(functions, list);
    // The merge rounds of a parallel sort copy every element into a default-constructed buffer.
    if constexpr (std::is_trivially_copyable_v<T> && std::is_default_constructible_v<T>)
        AOT::sortAndRemoveDuplicates(loops, all);
    else
        sortAndRemoveDuplicates(all);
    return all;
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
    const Layout& layout = program.at(guard.classLayout);
    bool isDeclared = std::ranges::any_of(layout.classFlow.members, [&](const ClassMember& member) {
        return member.side == ClassSide::Static && member.key == guard.key;
    }) || std::ranges::any_of(layout.staticFields, [&](const StaticField& field) {
        return field.key == guard.key;
    });
    return !isDeclared;
}

static bool declaresOrIsBornWith(const Program& program, const Layout& layout, ClassSide side, PropertyKeyID key)
{
    bool isMethod = std::ranges::any_of(layout.classFlow.members, [&](const ClassMember& member) {
        return member.side == side && member.key == key;
    });
    if (isMethod)
        return true;
    NameClass nameClass = program.nameClassOf(key);
    switch (side) {
    case ClassSide::Prototype:
        return nameClass == NameClass::Constructor;
    case ClassSide::Static:
        return nameClass == NameClass::Prototype || nameClass == NameClass::Length || key == program.keyOf(BuiltinNames::name) || std::ranges::any_of(layout.staticFields, [&](const StaticField& field) {
            return field.key == key;
        });
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The guards of such a class hold from the start because its records say all there is to say. They do not say what a built-in parent has, or a parent that nobody knows.
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
        if (layout.flow.baseState == BaseState::BuiltinClass || (layout.flow.baseState == BaseState::Unknown && layout.has(LayoutFlags::HasHeritage) && !layout.base))
            return true;
        current = expectedBaseOf(layout);
    }
    return false;
}

static void mergeLayoutSites(const PlacementContext& context, FunctionSitesArray functions, PlacedSites& sites)
{
    const ParallelLoops& loops = context.build.parallelLoops();
    sites.allocations = mergeDistinct(loops, functions, &FunctionSites::allocations);
    sites.keyOrderTests = mergeDistinct(loops, functions, &FunctionSites::keyOrderTests);
    sites.fieldCreations = mergeDistinct(loops, functions, &FunctionSites::fieldCreations);
    sites.guards = mergeWeightedUses(loops, functions, &FunctionSites::guards);
    sites.guards.removeAllMatching([&](const GuardUse& guard) {
        return speaksOfNameOrLengthMadeOnDemand(context.program, guard) || isLookedForAboveRecords(context, guard);
    });
    addEvaluationGuards(context, sites.guards);
    sites.guardSets = mergeWeightedUses(loops, functions, &FunctionSites::guardSets);
    sites.tableCalls = mergeDistinct(loops, functions, &FunctionSites::tableCalls);
}

static void mergeLiteralSites(const ParallelLoops& loops, FunctionSitesArray functions, PlacedSites& sites)
{
    sites.profiledArrayLiterals = mergeDistinct(loops, functions, &FunctionSites::profiledArrayLiterals);
    sites.regExpLiterals = mergeDistinct(loops, functions, &FunctionSites::regExpLiterals);
    sites.templateObjects = mergeDistinct(loops, functions, &FunctionSites::templateObjects);
    sites.copyOnWriteArrays = mergeDistinct(loops, functions, &FunctionSites::copyOnWriteArrays);
    sites.arraysWithStorageInImage = mergeDistinct(loops, functions, &FunctionSites::arraysWithStorageInImage);
}

static void mergeConstants(const ParallelLoops& loops, FunctionSitesArray functions, PlacedSites& sites)
{
    sites.builtinCells = mergeDistinct(loops, functions, &FunctionSites::builtinCells);
    sites.bigIntConstants = mergeDistinct(loops, functions, &FunctionSites::bigIntConstants);
    sites.moduleBuiltins = mergeDistinct(loops, functions, &FunctionSites::moduleBuiltins);
    for (const RequireEdgeUse& use : mergeDistinct(loops, functions, &FunctionSites::requireEdges)) {
        if (!sites.requireEdges.isEmpty() && sites.requireEdges.last() == use.edge)
            continue;
        sites.requireEdges.append(use.edge);
        sites.requireEdgeSpecifiers.append(use.specifier);
    }
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
    sites.firstStoreThatMayAdd.fill(0, program.functions.size() + 1);
    for (const FunctionSites* function : functions)
        sites.firstStoreThatMayAdd[function->function.index() + 1] = function->numberOfStoresThatMayAdd;
    for (size_t index = 1; index < sites.firstStoreThatMayAdd.size(); ++index)
        sites.firstStoreThatMayAdd[index] += sites.firstStoreThatMayAdd[index - 1];
    for (const FunctionSites* sitesOfFunction : functions) {
        const FunctionSites& function = *sitesOfFunction;
        setBits(sites.variablesNamedByCode, function.variables);
        setBits(sites.typesInRunTimePayloads, function.types);
        setBits(sites.typesTestedByCode, function.typesTestedByCode);
        setBits(sites.functionsWithAllocatedClosures, function.allocatedClosures);
        sites.maxClosureCaptures = std::max(sites.maxClosureCaptures, function.maxClosureCaptures);
    }
}

static PlacedSites mergeSites(const PlacementContext& context, FunctionSitesArray functions)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    PlacedSites sites;
    runTasksInParallel(loops, {
        [&] { sites.fieldAccesses = mergeWeightedUses(loops, functions, &FunctionSites::fieldAccesses); },
        [&] { sites.shapeTests = mergeWeightedUses(loops, functions, &FunctionSites::shapeTests); },
        [&] { mergeLayoutSites(context, functions, sites); },
        [&] { mergeLiteralSites(loops, functions, sites); },
        [&] { mergeConstants(loops, functions, sites); },
        [&] { mergeBits(program, functions, sites); },
    });
    return sites;
}

PlacedSites collectPlacedSites(const PlacementContext& context)
{
    const Program& program = context.program;
    const ParallelLoops& loops = context.build.parallelLoops();
    Vector<uint32_t> functionsWithBodies = findIndicesInParallel(loops, program.functions.size(), [&](unsigned index) {
        return program.functions[index].hasBody();
    });
    Vector<uint32_t> largestFirst;
    for (uint32_t large : largeFunctions(program, loops)) {
        auto found = std::ranges::lower_bound(functionsWithBodies, large);
        if (found != functionsWithBodies.end() && *found == large)
            largestFirst.append(static_cast<uint32_t>(found - functionsWithBodies.begin()));
    }
    ParallelArray<FunctionSites> sitesOfFunctions(loops, functionsWithBodies.size(), largestFirst.span(), [&](unsigned index) {
        return SiteScanner(context, FunctionID(functionsWithBodies[index])).run();
    });
    Vector<FunctionSites*> functions(sitesOfFunctions.size(), [&](size_t index) {
        return &sitesOfFunctions[index];
    });
    PlacedSites sites = mergeSites(context, functions.span());
    context.build.destroyLater(WTF::move(sitesOfFunctions));
    return sites;
}

static LayoutID findClosureLayout(const Program& program)
{
    for (unsigned index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind == LayoutKind::Closure)
            return LayoutID(index);
    }
    return { };
}

struct BodyScansForPlacement::State {
    WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(State);

    struct ScanOfBody {
        WTF_DEPRECATED_MAKE_STRUCT_FAST_ALLOCATED(ScanOfBody);

        FunctionSites sites;
        Vector<KeyCellUse> keyCellUses;
        Vector<GlobalBindingSite> globalBindingSites;
        uint32_t numberOfOperationsWithWordForSiteCache { 0 };
    };

    // No operation names the layout of closures where numberAndEncode() has yet to make it.
    State(const Program& program, const PlacementInputs& inputs, BuildContext& build)
        : sizeClasses(build.options().sizeClassProgression)
        , context { program, inputs, sizeClasses, build, build.recordedOptions().useWideReferences(), findClosureLayout(program) }
        , scans(program.functions.size())
    {
    }

    const ScanOfBody& of(FunctionID id) const
    {
        RELEASE_ASSERT(scans[id.index()]);
        return *scans[id.index()];
    }

    CellSizeClasses sizeClasses;
    PlacementContext context;
    Vector<std::unique_ptr<ScanOfBody>> scans;
    // Summed by takeSites().
    uint64_t numberOfOperationsWithWordForSiteCache { 0 };
};

BodyScansForPlacement::BodyScansForPlacement(const Program& program, const PlacementInputs& inputs, BuildContext& build)
    : m_state(makeUnique<State>(program, inputs, build))
{
}

BodyScansForPlacement::~BodyScansForPlacement() = default;

void BodyScansForPlacement::scan(FunctionID id)
{
    const Program& program = m_state->context.program;
    auto scan = makeUnique<State::ScanOfBody>();
    scan->sites = SiteScanner(m_state->context, id).run();
    collectKeyCellUses(program, id, scan->keyCellUses);
    collectGlobalBindingSites(program, id, scan->globalBindingSites);
    const Function& function = program.at(id);
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values)
            scan->numberOfOperationsWithWordForSiteCache += Value::opcodesWithSiteCache.contains(function.at(value).opcode);
    }
    m_state->scans[id.index()] = WTF::move(scan);
}

void BodyScansForPlacement::didAddFunctions()
{
    m_state->scans.grow(m_state->context.program.functions.size());
}

uint64_t BodyScansForPlacement::numberOfOperationsWithWordForSiteCache() const
{
    return m_state->numberOfOperationsWithWordForSiteCache;
}

const Vector<KeyCellUse>& BodyScansForPlacement::keyCellUsesOf(FunctionID id) const
{
    return m_state->of(id).keyCellUses;
}

const Vector<GlobalBindingSite>& BodyScansForPlacement::globalBindingSitesOf(FunctionID id) const
{
    return m_state->of(id).globalBindingSites;
}

PlacedSites BodyScansForPlacement::takeSites(const PlacementContext& context)
{
    const Program& program = context.program;
    RELEASE_ASSERT(m_state->scans.size() == program.functions.size());
    Vector<FunctionSites*> functions;
    for (unsigned index = 0; index < program.functions.size(); ++index) {
        if (!program.functions[index].hasBody())
            continue;
        RELEASE_ASSERT(m_state->scans[index]);
        functions.append(&m_state->scans[index]->sites);
        m_state->numberOfOperationsWithWordForSiteCache += m_state->scans[index]->numberOfOperationsWithWordForSiteCache;
    }
    return mergeSites(context, functions.span());
}

void BodyScansForPlacement::destroyLater()
{
    m_state->context.build.destroyLater(WTF::move(m_state->scans));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
