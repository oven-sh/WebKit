/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNarrowFieldTypes.h"

#if ENABLE(AOT)

#include "AOTAnalysisContext.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTCellFormat.h"
#include "AOTCellSizeClasses.h"
#include "AOTFlowSiteData.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTProgramCaches.h"
#include "AOTRepresentation.h"
#include "AOTSlotRepresentation.h"
#include "AOTSort.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTVector.h"
#include "AOTVisibleStores.h"
#include <algorithm>
#include <atomic>
#include <optional>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/MathExtras.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

using StoredKinds = OptionSet<StoredKind>;

constexpr Range inlineInt32s { minInlineInt32, INT32_MAX };

class FieldEvidence {
public:
    FieldEvidence(const Program&, const ParallelLoops&);

    bool hasValidFieldReferences() const { return m_hasValidFieldReferences; }
    const BitVector& functionsHandedOn() const { return m_functionsHandedOn; }

    StoredKinds visibleStores(FieldReference field) const { return m_visibleStores.kindsStoredIn(field.layout, field.field); }
    bool isUsedAsInteger(FieldReference field) const { return m_visibleStores.isUsedAsInteger(field.layout, field.field); }
    bool typesOfInitialValuesFitInt32Slot(FieldReference field) const { return m_visibleStores.typesOfInitialValuesFitInt32Slot(field.layout, field.field); }
    bool isKept(LayoutID layout) const { return m_layoutsKept.get(layout.index()); }
    StoredKinds visibleArguments(FunctionID function, unsigned parameter) const { return m_visibleStores.kindsPassedTo(function, parameter); }
    bool typesOfVisibleArgumentsLieInInt32(FunctionID function, unsigned parameter) const { return m_visibleStores.typesPassedLieInInt32(function, parameter); }
    StoredKinds constantData(FieldReference) const;

private:
    VisibleStores m_visibleStores;
    Vector<Vector<StoredKinds>> m_constantData;
    BitVector m_functionsHandedOn;
    BitVector m_layoutsKept;
    bool m_hasValidFieldReferences { true };
};

struct NarrowedFields {
    BitVector keys;
    uint64_t count { 0 };
};

} // anonymous namespace

TypeID slotTypeOf(const Program& program, const Field& field)
{
    // The slot of an accessor holds its GetterSetter. Its type is that of the getter, which every writer of the slot tests.
    if (field.form.contains(FieldForm::IsAccessor))
        return field.slotType ? field.slotType : TypeTable::anything;
    if (field.slotType && !field.declared.flags.contains(PositionFlags::IsExpected))
        return field.slotType;
    return program.provenTypeOf(field.declared);
}

bool slotHoldsUndefinedAndNull(const Program& program, FieldReference reference)
{
    const Type& type = program.at(slotTypeOf(program, fieldAt(program, reference)));
    bool hasMarkers = !type.isNever() && type.hasOnlyPrimitives(Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull);
    return hasMarkers && !flowOf(program, reference).stability.contains(FieldStability::IsNeverStoredWithOtherType);
}

bool holdsRawValueFromBirth(const Program& program, FieldReference reference)
{
    if (!isFilledWithValueAtBirth(program, reference))
        return false;
    const Type& type = program.at(slotTypeOf(program, fieldAt(program, reference)));
    return type.isNumber() || type.isBoolean();
}

static StoredKind storedKindOf(const DataNode& node)
{
    if (const DataNode::Number* number = node.tryAs<DataNode::Number>()) {
        std::optional<int32_t> integer = tryConvertToStrictInt32(number->value);
        return integer && *integer >= minInlineInt32 ? StoredKind::Int32 : StoredKind::Other;
    }
    if (node.is<DataNode::Undefined>())
        return StoredKind::Undefined;
    return node.is<DataNode::Null>() ? StoredKind::Null : StoredKind::Other;
}

static Vector<Vector<StoredKinds>> collectConstantDataKinds(const Program& program, const ParallelLoops& loops)
{
    struct KindInField {
        auto operator<=>(const KindInField&) const = default;

        uint32_t layout;
        uint32_t field;
        StoredKind kind;
    };
    constexpr size_t maxRepeatedEntries = 16384;
    auto removeRepeatedEntries = [](Vector<KindInField>& found) {
        AOT::sort(found);
        removeRepeatedElements(found);
    };
    // What the threads find is united, so it does not matter which thread looks at which data.
    Vector<Vector<KindInField>> foundByWorkers(loops.width());
    Vector<size_t> sizesWithoutRepeatedEntries(FillWith { }, loops.width(), size_t { 0 });
    forEachIndexInParallel(loops, program.data.size(), [&](unsigned index, unsigned worker) {
        Vector<KindInField>& found = foundByWorkers[worker];
        forEachDataNodeInPreorder(program.data[index].root, [&](uint32_t, const DataNode& node) {
            const DataNode::Object* object = node.tryAs<DataNode::Object>();
            if (!object || !object->layout)
                return;
            for (const DataMember& member : object->members) {
                std::optional<uint32_t> field = program.at(object->layout).findField(member.key);
                if (!field)
                    continue;
                FieldReference declaration = declarationOf(program, { object->layout, *field });
                found.append({ declaration.layout.index(), declaration.field, storedKindOf(member.value) });
            }
        });
        if (found.size() - sizesWithoutRepeatedEntries[worker] > maxRepeatedEntries) {
            removeRepeatedEntries(found);
            sizesWithoutRepeatedEntries[worker] = found.size();
        }
    });
    forEachIndexInParallel(loops, foundByWorkers.size(), [&](unsigned worker) {
        removeRepeatedEntries(foundByWorkers[worker]);
    });

    Vector<Vector<StoredKinds>> kinds(program.layouts.size());
    for (const Vector<KindInField>& found : foundByWorkers) {
        for (const KindInField& entry : found) {
            Vector<StoredKinds>& kindsByField = kinds[entry.layout];
            if (kindsByField.isEmpty())
                kindsByField.grow(program.layouts[entry.layout].fields.size());
            kindsByField[entry.field].add(entry.kind);
        }
    }
    return kinds;
}

namespace {

// What one walk over the bodies finds. Each thread has its own, and all of it is united, so the result does not depend on which thread saw which function.
struct FieldTypeScan {
    Vector<TypeID> typesHandedOn;
    Vector<LayoutSetID> layoutSetsKept;
    Vector<std::pair<FunctionID, LayoutSetID>> layoutSetsReturned;
    bool hasInvalidFieldReference { false };
};

struct ScannedType {
    LayoutSetID layouts;
    bool listsFunctions : 1 { false };
    bool listsOnlyFunctions : 1 { false };
};

class FieldTypeScanner {
public:
    FieldTypeScanner(const Program&, const ParallelLoops&);

    // Answers whether the field references of the function are valid.
    bool scan(FunctionID, unsigned worker);

    bool hasValidFieldReferences() const;
    BitVector findFunctionsHandedOn();
    BitVector findLayoutsKept(const BitVector& functionsHandedOn);

private:
    const ScannedType& typeOf(const Function& function, ValueID value) const { return m_types[function.at(value).type.index()]; }
    const ScannedType& expectedOrTypeOf(const Function& function, ValueID value) const { return m_types[function.at(value).expectedOrType().index()]; }
    bool namesItsCallees(const Function&, const Value& call) const;

    const Program& m_program;
    const ParallelLoops& m_loops;
    Vector<ScannedType> m_types;
    Vector<FieldTypeScan> m_scans;
    // The casts and the operations that retain an operand. No other value says anything here.
    OpcodeSet m_opcodesToLookAt;
};

} // anonymous namespace

static bool listsInitialValues(Opcode opcode)
{
    return opcode == Opcode::NewObject || opcode == Opcode::CloneAs;
}

FieldTypeScanner::FieldTypeScanner(const Program& program, const ParallelLoops& loops)
    : m_program(program)
    , m_loops(loops)
    , m_types(program.types.size())
    , m_scans(loops.width())
{
    for (unsigned index = 0; index < numberOfOpcodes; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        bool retainsOperand = false;
        for (uint32_t operand = 0, end = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode); operand < end; ++operand)
            retainsOperand |= operandInfoOf(opcode, operand).uses.contains(OperandUse::Retains);
        if (retainsOperand || isCast(opcode))
            m_opcodesToLookAt.add(opcode);
    }
    constexpr size_t typesPerTask = 1024;
    forEachIndexInParallel(loops, static_cast<unsigned>((m_types.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
        for (size_t index = task * typesPerTask, end = std::min(m_types.size(), index + typesPerTask); index < end; ++index) {
            const Type& type = program.at(TypeID(index));
            m_types[index].layouts = type.layouts;
            m_types[index].listsFunctions = !type.functions.isEmpty();
            m_types[index].listsOnlyFunctions = !type.functions.isEmpty() && !type.anyObject && type.classes.isEmpty() && !type.layouts && type.builtins.isEmpty();
        }
    });
}

bool FieldTypeScanner::namesItsCallees(const Function& function, const Value& call) const
{
    if (call.opcode == Opcode::CallDirect)
        return true;
    ValueID callee = call.opcode == Opcode::Call ? call.operand(OperandIndex::Call::callee) : ValueID();
    return callee && expectedOrTypeOf(function, callee).listsOnlyFunctions;
}

// The functions that the program hands on as values, to a callee or into memory that no type describes.
bool FieldTypeScanner::scan(FunctionID id, unsigned worker)
{
    const Function& function = m_program.at(id);
    FieldTypeScan& scan = m_scans[worker];
    for (const Value& value : function.values) {
        if (listsInitialValues(value.opcode) && value.fields().size() != value.numberOfVariadicOperands()) {
            scan.hasInvalidFieldReference = true;
            return false;
        }
    }

    Vector<LayoutSetID, 16> kept;
    Vector<LayoutSetID, 4> returned;
    // Of an operand that is retained.
    auto noteHandedOn = [&](ValueID operand, const OperandInfo& info) {
        TypeID type = function.at(operand).type;
        if (type && m_types[type.index()].listsFunctions && info.kind != OperandKind::Declared && info.retainedIn == RetainedIn::UnknownMemory)
            scan.typesHandedOn.append(type);
    };
    auto noteKept = [&](ValueID operand, auto& list) {
        if (LayoutSetID layouts = operand ? expectedOrTypeOf(function, operand).layouts : LayoutSetID())
            list.append(layouts);
    };
    for (const Block& block : function.blocks) {
        for (ValueID valueID : block.values) {
            const Value& value = function.at(valueID);
            if (!m_opcodesToLookAt.contains(value.opcode))
                continue;
            bool keepsNothing = namesItsCallees(function, value);
            bool isCastOfValue = !keepsNothing && isCast(value.opcode);
            if (isCastOfValue && !expectedOrTypeOf(function, valueID).layouts)
                noteKept(value.operand(0), kept);
            value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                const OperandInfo& info = operandInfoOf(value.opcode, operandIndex);
                if (!info.uses.contains(OperandUse::Retains))
                    return;
                noteHandedOn(operand, info);
                if (!keepsNothing && !isCastOfValue)
                    noteKept(operand, kept);
            });
        }
        const Terminator& terminator = block.terminator;
        terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
            const OperandInfo& info = operandInfoOf(terminator.kind, operandIndex);
            if (info.uses.contains(OperandUse::Retains)) {
                noteHandedOn(operand, info);
                noteKept(operand, kept);
            } else if (info.uses.contains(OperandUse::Returns))
                noteKept(operand, returned);
        });
    }
    AOT::sort(kept);
    removeRepeatedElements(kept);
    scan.layoutSetsKept.appendVector(kept);
    AOT::sort(returned);
    removeRepeatedElements(returned);
    for (LayoutSetID set : returned)
        scan.layoutSetsReturned.append({ id, set });
    return true;
}

bool FieldTypeScanner::hasValidFieldReferences() const
{
    return std::ranges::none_of(m_scans, &FieldTypeScan::hasInvalidFieldReference);
}

// What the lists of the threads hold, each id once, ascending.
template<typename IDType>
static Vector<IDType> uniteLists(const ParallelLoops& loops, Vector<FieldTypeScan>& scans, Vector<IDType> FieldTypeScan::* list)
{
    forEachIndexInParallel(loops, scans.size(), [&](unsigned worker) {
        AOT::sort(scans[worker].*list);
        removeRepeatedElements(scans[worker].*list);
    });
    Vector<IDType> united;
    for (FieldTypeScan& scan : scans)
        united.appendVector(std::exchange(scan.*list, { }));
    parallelSort(loops, united.mutableSpan(), std::less<IDType> { });
    removeRepeatedElements(united);
    return united;
}

// The union of what the functor sets for each index. Each thread sets bits of its own.
static BitVector uniteBits(const ParallelLoops& loops, size_t numberOfBits, size_t count, NOESCAPE const Invocable<void(size_t index, BitVector&)> auto& functor)
{
    constexpr size_t indicesPerTask = 16;
    Vector<BitVector> bitsOfWorkers(loops.width());
    forEachIndexInParallel(loops, static_cast<unsigned>((count + indicesPerTask - 1) / indicesPerTask), [&](unsigned task, unsigned worker) {
        BitVector& bits = bitsOfWorkers[worker];
        bits.ensureSize(numberOfBits);
        for (size_t index = task * indicesPerTask, end = std::min(count, index + indicesPerTask); index < end; ++index)
            functor(index, bits);
    });
    BitVector united(numberOfBits);
    for (const BitVector& bits : bitsOfWorkers)
        united.merge(bits);
    return united;
}

BitVector FieldTypeScanner::findFunctionsHandedOn()
{
    Vector<TypeID> types = uniteLists(m_loops, m_scans, &FieldTypeScan::typesHandedOn);
    return uniteBits(m_loops, m_program.functions.size(), types.size(), [&](size_t index, BitVector& functions) {
        for (FunctionID function : m_program.at(types[index]).functions)
            functions.quickSet(function.index());
    });
}

BitVector FieldTypeScanner::findLayoutsKept(const BitVector& functionsHandedOn)
{
    forEachIndexInParallel(m_loops, m_scans.size(), [&](unsigned worker) {
        FieldTypeScan& scan = m_scans[worker];
        for (auto [function, set] : scan.layoutSetsReturned) {
            if (functionsHandedOn.quickGet(function.index()))
                scan.layoutSetsKept.append(set);
        }
    });
    Vector<LayoutSetID> sets = uniteLists(m_loops, m_scans, &FieldTypeScan::layoutSetsKept);
    BitVector layouts = uniteBits(m_loops, m_program.layouts.size(), sets.size(), [&](size_t index, BitVector& members) {
        for (LayoutID member : m_program.at(sets[index]).members)
            members.quickSet(member.index());
    });
    // A field is narrowed where it is declared.
    for (size_t index : BitVector(layouts)) {
        for (const Field& field : m_program.layouts[index].fields) {
            if (field.originLayout)
                layouts.quickSet(field.originLayout.index());
        }
    }
    return layouts;
}

FieldEvidence::FieldEvidence(const Program& program, const ParallelLoops& loops)
{
    FieldTypeScanner scanner(program, loops);
    m_visibleStores = VisibleStores(program, loops, [&](FunctionID id, unsigned worker) {
        return scanner.scan(id, worker);
    });
    m_hasValidFieldReferences = scanner.hasValidFieldReferences();
    if (!m_hasValidFieldReferences)
        return;
    m_constantData = collectConstantDataKinds(program, loops);
    m_functionsHandedOn = scanner.findFunctionsHandedOn();
    m_layoutsKept = scanner.findLayoutsKept(m_functionsHandedOn);
}

StoredKinds FieldEvidence::constantData(FieldReference field) const
{
    const Vector<StoredKinds>& kindsByField = m_constantData[field.layout.index()];
    return kindsByField.isEmpty() ? StoredKinds { } : kindsByField[field.field];
}

static uint16_t nullishPrimitivesOf(StoredKinds kinds)
{
    return (kinds.contains(StoredKind::Undefined) ? Primitive::Undefined : 0) | (kinds.contains(StoredKind::Null) ? Primitive::Null : 0);
}

static bool showsOnlyInt32s(StoredKinds stored)
{
    return stored.contains(StoredKind::Int32) && !stored.contains(StoredKind::Other);
}

static bool isCheckedCode(const Function& function)
{
    return function.role != FunctionRole::Library && function.has(FunctionFlags::IsChecked);
}

static bool isDeclaredInCheckedCode(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    if (layout.constructor)
        return isCheckedCode(program.at(layout.constructor));
    if (!layout.module)
        return false;
    const Module& declaringModule = program.at(layout.module);
    return !declaringModule.flags.contains(ModuleFlags::IsLibrary) && declaringModule.body && isCheckedCode(program.at(declaringModule.body));
}

enum class IntegerEvidence : bool { TypesAndStores, Uses };

static std::optional<Type> findExpectedIntegers(const Program& program, const FieldEvidence& evidence, FieldReference reference, IntegerEvidence admitted)
{
    const Field& field = fieldAt(program, reference);
    const Type& slotType = program.at(slotTypeOf(program, field));
    if (!(slotType.primitives & Primitive::Integer) || slotType.primitives & Primitive::Absent)
        return std::nullopt;
    if (!slotType.isNumberOrNullish() && !slotType.isAnything())
        return std::nullopt;

    StoredKinds constants = evidence.constantData(reference);
    if (constants.contains(StoredKind::Other) || nullishPrimitivesOf(constants) & ~slotType.primitives)
        return std::nullopt;

    StoredKinds stored = evidence.visibleStores(reference);
    bool listsOnlyInt32s = !field.declared.flags.contains(PositionFlags::IsExpected) && program.at(field.declared.type).isInt32OrNullish();
    bool isDeclaredInt32 = listsOnlyInt32s || slotType.isInt32OrNullish() || field.declared.flags.contains(PositionFlags::IsInt32Promise);
    // A position in a text and an index of a table get numbers that nobody can bound: what a scanner counts up, what a caller passes.
    bool isCountedWith = admitted == IntegerEvidence::Uses && slotType.isNumberOrNullish() && stored.contains(StoredKind::Int32) && !stored.contains(StoredKind::NoInt32)
        && evidence.isUsedAsInteger(reference) && isDeclaredInCheckedCode(program, reference.layout);
    // An object that starts with a number of a wider type is made with a filler and gets the number by a tested store.
    bool isShownOnlyInt32s = showsOnlyInt32s(stored) && (evidence.isKept(reference.layout) || evidence.typesOfInitialValuesFitInt32Slot(reference));
    if (!isDeclaredInt32 && !isShownOnlyInt32s && !isCountedWith)
        return std::nullopt;

    Range integers = (listsOnlyInt32s ? program.at(field.declared.type) : slotType).integers.intersectWith(inlineInt32s);
    if (integers.isEmpty())
        return std::nullopt;

    uint16_t nullish = nullishPrimitivesOf(stored | constants);
    if (!field.flags.contains(FieldFlags::IsRequired))
        nullish |= Primitive::Undefined;
    return Type::integer(integers).withPrimitives(nullish & slotType.primitives);
}

static std::optional<Type> findIntegersToKeepInSlot(const Program& program, const FieldEvidence& evidence, FieldReference reference, IntegerEvidence admitted)
{
    if (!canBeStoredBehindAllocation(fieldAt(program, reference)))
        return std::nullopt;
    return findExpectedIntegers(program, evidence, reference, admitted);
}

static bool keepIntegersInSlot(Program& program, FieldReference reference, const Type& expected)
{
    TypeID type = program.types.intern(expected);
    DeclaredType& declared = program.at(reference.layout).fields[reference.field].declared;
    if (declared.type == type && !declared.flags.contains(PositionFlags::IsExpected))
        return false;
    declared.type = type;
    declared.flags.remove(PositionFlags::IsExpected);
    return true;
}

static bool isDeclaredObjectLayout(LayoutKind kind)
{
    return isObjectLayout(kind) && kind != LayoutKind::BuiltinResult;
}

static bool hasSlots(const Program& program, LayoutID id)
{
    return isDeclaredObjectLayout(program.at(id).kind) && hasOnlyCompiledBases(program, id);
}

static void forEachFieldDeclaredWithSlot(const Program& program, LayoutID layout, NOESCAPE const Invocable<void(FieldReference)> auto& functor)
{
    if (!hasSlots(program, layout))
        return;
    for (uint32_t fieldIndex = 0; fieldIndex < program.at(layout).fields.size(); ++fieldIndex) {
        if (!program.at(layout).fields[fieldIndex].flags.contains(FieldFlags::IsInherited))
            functor(FieldReference { layout, fieldIndex });
    }
}

// What the packer will most likely give a field of this type. It knows more by then: which fields need a state for "absent", which references must stay wide.
static uint32_t likelyWidthOfSlot(const Program& program, const Field& field)
{
    constexpr uint16_t markers = Primitive::UndefinedOrNull | Primitive::Absent;
    const Type& type = program.at(slotTypeOf(program, field));
    SlotRepresentation representation = defaultSlotRepresentationOf(type);
    if (representation == SlotRepresentation::JSValue && type.hasOnlyPrimitives(markers | Primitive::Boolean))
        return widthOf(SlotRepresentation::OptionalBoolean);
    if (representation == SlotRepresentation::OptionalInt64 && type.integers.isSubsetOf(inlineInt32s))
        return widthOf(SlotRepresentation::OptionalInt32);
    if (type.hasCells() && !(type.primitives & ~(markers | cellPrimitives)) && !type.opaque)
        return widthOf(SlotRepresentation::Cell32);
    return widthOf(representation);
}

// The uses are a guess, and a guess costs a test at every store and every read.
static Vector<FieldReference> findCountedFieldsThatMakeCellsSmaller(const Program& program, const FieldEvidence& evidence, const CellSizeClasses& sizeClasses, const ParallelLoops& loops)
{
    constexpr uint32_t saved = widthOf(SlotRepresentation::OptionalDouble) - widthOf(SlotRepresentation::OptionalInt32);
    constexpr uint32_t largestCellWithEveryField = 128;

    Vector<BitVector> candidates(program.layouts.size());
    std::atomic<bool> hasCandidates { false };
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned layoutIndex) {
        forEachFieldDeclaredWithSlot(program, LayoutID(layoutIndex), [&](FieldReference reference) {
            if (!canBeStoredBehindAllocation(fieldAt(program, reference)) || findExpectedIntegers(program, evidence, reference, IntegerEvidence::TypesAndStores))
                return;
            if (!findExpectedIntegers(program, evidence, reference, IntegerEvidence::Uses))
                return;
            candidates[layoutIndex].set(reference.field);
            hasCandidates.store(true, std::memory_order_relaxed);
        });
    });
    if (!hasCandidates.load(std::memory_order_relaxed))
        return { };

    Vector<FieldReference> heldByChoosingLayouts = collectInParallel<FieldReference>(loops, program.layouts.size(), [&](unsigned layoutIndex, Vector<FieldReference>& found) {
        const Layout& layout = program.layouts[layoutIndex];
        if (!hasSlots(program, LayoutID(layoutIndex)) || layout.flow.allocation == AllocationState::NeverAllocated)
            return;
        Vector<FieldReference, 8> held;
        uint32_t bytes = offsetOfFirstField;
        for (uint32_t fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
            FieldReference declaration = declarationOf(program, { LayoutID(layoutIndex), fieldIndex });
            bytes += likelyWidthOfSlot(program, fieldAt(program, declaration));
            if (candidates[declaration.layout.index()].get(declaration.field))
                held.append(declaration);
        }
        if (held.isEmpty() || bytes > largestCellWithEveryField || sizeClasses.cellSizeFor(bytes - saved * held.size()) == sizeClasses.cellSizeFor(bytes))
            return;
        found.appendVector(held);
    });
    Vector<BitVector> chosen(program.layouts.size());
    for (FieldReference declaration : heldByChoosingLayouts)
        chosen[declaration.layout.index()].set(declaration.field);

    Vector<FieldReference> fields;
    for (uint32_t layoutIndex = 0; layoutIndex < chosen.size(); ++layoutIndex) {
        for (size_t fieldIndex : chosen[layoutIndex])
            fields.append({ LayoutID(layoutIndex), static_cast<uint32_t>(fieldIndex) });
    }
    return fields;
}

static NarrowedFields narrowFields(Program& program, const FieldEvidence& evidence, const CellSizeClasses& sizeClasses, const ParallelLoops& loops)
{
    struct FieldToNarrow {
        FieldReference field;
        Type expected;
    };
    NarrowedFields narrowed;
    auto narrow = [&](FieldReference reference, const Type& expected) {
        if (!keepIntegersInSlot(program, reference, expected))
            return;
        narrowed.keys.set(fieldAt(program, reference).key.index());
        ++narrowed.count;
    };
    Vector<FieldToNarrow> fields = collectInParallel<FieldToNarrow>(loops, program.layouts.size(), [&](unsigned layoutIndex, Vector<FieldToNarrow>& found) {
        forEachFieldDeclaredWithSlot(program, LayoutID(layoutIndex), [&](FieldReference reference) {
            if (std::optional<Type> expected = findIntegersToKeepInSlot(program, evidence, reference, IntegerEvidence::TypesAndStores))
                found.append({ reference, WTF::move(*expected) });
        });
    });
    for (const FieldToNarrow& field : fields)
        narrow(field.field, field.expected);
    for (FieldReference reference : findCountedFieldsThatMakeCellsSmaller(program, evidence, sizeClasses, loops)) {
        if (std::optional<Type> expected = findIntegersToKeepInSlot(program, evidence, reference, IntegerEvidence::Uses))
            narrow(reference, *expected);
    }
    return narrowed;
}

static bool originCanBeResolved(const Program& program, const Field& field)
{
    bool isInherited = field.flags.contains(FieldFlags::IsInherited);
    if (!field.originLayout)
        return !isInherited;
    const Vector<Field>& originFields = program.at(field.originLayout).fields;
    if (!field.originField || *field.originField >= originFields.size())
        return false;
    return !isInherited || !originFields[*field.originField].flags.contains(FieldFlags::IsInherited);
}

static void updateInheritedFields(Program& program, const ParallelLoops& loops)
{
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned layoutIndex) {
        for (Field& field : program.layouts[layoutIndex].fields) {
            if (!field.flags.contains(FieldFlags::IsInherited) || !originCanBeResolved(program, field))
                continue;
            const Field& declaration = fieldAt(program, { field.originLayout, *field.originField });
            field.declared = declaration.declared;
            field.slotType = declaration.slotType;
            field.form = declaration.form;
            field.flags.set(FieldFlags::IsEnforced, declaration.flags.contains(FieldFlags::IsEnforced));
        }
    });
}

void markEnforcedFields(Program& program, BuildContext& build)
{
    const ParallelLoops& loops = build.parallelLoops();
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned layoutIndex) {
        Layout& layout = program.layouts[layoutIndex];
        if (!isDeclaredObjectLayout(layout.kind))
            return;
        bool hasSlots = hasOnlyCompiledBases(program, LayoutID(layoutIndex));
        for (Field& field : layout.fields) {
            if (!field.flags.contains(FieldFlags::IsInherited))
                field.flags.set(FieldFlags::IsEnforced, hasSlots && !field.declared.flags.contains(PositionFlags::IsExpected));
        }
    });
    updateInheritedFields(program, loops);
}

// The verifier runs after this pass and reports whatever is rejected here.
static bool hasResolvableFieldOrigins(const Program& program, const ParallelLoops& loops)
{
    std::atomic<bool> areResolvable { true };
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        for (const Field& field : program.layouts[index].fields) {
            if (!originCanBeResolved(program, field))
                areResolvable.store(false, std::memory_order_relaxed);
        }
    });
    return areResolvable.load(std::memory_order_relaxed);
}

bool canBeStoredBehindAllocation(const Field& field)
{
    constexpr OptionSet<FieldFlags> storedOnlyByAllocation { FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsHidden };
    return field.key && !field.flags.containsAny(storedOnlyByAllocation) && field.flow.location.stores == StoreCount::Unknown;
}

static constexpr OpcodeSet opcodesThatListInitialValues { Opcode::NewObject, Opcode::CloneAs };

// The values with one of the opcodes that the blocks of the function list, in the order of the blocks, until the functor answers true.
static bool findListedValue(const Program& program, FunctionID id, const OpcodeSet& opcodes, NOESCAPE const Invocable<bool(const Value&)> auto& functor)
{
    const Function& function = program.at(id);
    BodyIndex* index = program.bodyIndex();
    if (std::optional<RowOfBody> row = index ? index->rowOf(program, id) : std::nullopt) {
        if (!row->opcodes.intersects(opcodes))
            return false;
        return std::ranges::any_of(row->listedValues, [&](IndexedValue listed) {
            return opcodes.contains(listed.opcode()) && functor(function.at(listed.value()));
        });
    }
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID value) {
            return opcodes.contains(function.at(value).opcode) && functor(function.at(value));
        });
    });
}

static bool writesInitialValues(const Program& program, const Value& value)
{
    return listsInitialValues(value.opcode) && isObjectLayout(program.at(value.layout()).kind) && value.fields().size() == value.numberOfVariadicOperands();
}

static const Field& initializedField(const Program& program, const Value& allocation, uint32_t valueIndex)
{
    return fieldAt(program, declarationOf(program, { allocation.layout(), allocation.fields()[valueIndex] }));
}

static bool isUnprovenInitialValue(const Program& program, const Function& function, const Value& allocation, uint32_t valueIndex)
{
    const Field& field = initializedField(program, allocation, valueIndex);
    TypeID slotType = slotTypeOf(program, field);
    if (slotType == TypeTable::anything || field.flow.stability.contains(FieldStability::IsNeverStoredWithOtherType))
        return false;
    const Type& value = program.typeOf(function, allocation.variadicOperands()[valueIndex]);
    if (value.mayBeUndefinedOrNull() && slotHoldsUndefinedAndNull(program, { allocation.layout(), allocation.fields()[valueIndex] }))
        return !value.withoutNullish().isSubsetOf(program.at(slotType), program);
    return !value.isSubsetOf(program.at(slotType), program);
}

static bool hasUnprovenInitialValue(const Program& program, const Function& function, const Value& value)
{
    if (!writesInitialValues(program, value))
        return false;
    for (uint32_t valueIndex = 0; valueIndex < value.numberOfVariadicOperands(); ++valueIndex) {
        if (isUnprovenInitialValue(program, function, value, valueIndex))
            return true;
    }
    return false;
}

static bool hasUnprovenInitialValue(const Program& program, const Function& function)
{
    return std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID id) {
            return hasUnprovenInitialValue(program, function, function.at(id));
        });
    });
}

static bool hasUnprovenInitialValue(const Program& program, FunctionID id)
{
    return findListedValue(program, id, opcodesThatListInitialValues, [&](const Value& value) {
        return hasUnprovenInitialValue(program, program.at(id), value);
    });
}

std::optional<Value> tryCreateFiller(const Program& program, const Type& type)
{
    if (type.primitives & Primitive::Undefined)
        return IRBuilder::make(Opcode::Undefined);
    if (type.primitives & Primitive::Null)
        return IRBuilder::make(Opcode::Null);
    if (type.primitives & Primitive::Boolean) {
        Value boolean = IRBuilder::make(Opcode::Boolean);
        boolean.setBoolean(!(type.primitives & Primitive::False));
        return boolean;
    }
    if (type.primitives & Primitive::Integer && !type.integers.isEmpty()) {
        Value integer = IRBuilder::make(Opcode::Integer);
        integer.setInteger(std::clamp<int64_t>(0, type.integers.minimum, type.integers.maximum));
        return integer;
    }
    bool holdsAnyString = type.primitives & Primitive::String;
    if (!holdsAnyString && (!(type.primitives & Primitive::LiteralString) || type.stringLiterals.isEmpty()))
        return std::nullopt;
    Value string = IRBuilder::make(Opcode::String);
    string.setString(holdsAnyString ? program.strings.emptyString() : *type.stringLiterals.begin());
    return string;
}

// Something that fits the slot stands in for the value until the store.
bool canStoreInitialValueBehindAllocation(const Program& program, const Value& allocation, uint32_t valueIndex)
{
    const Field& field = initializedField(program, allocation, valueIndex);
    if (!canBeStoredBehindAllocation(field))
        return false;
    if (tryCreateFiller(program, program.at(slotTypeOf(program, field))))
        return true;
    for (uint32_t index = valueIndex + 1; index < allocation.numberOfVariadicOperands(); ++index) {
        if (!canBeStoredBehindAllocation(initializedField(program, allocation, index)))
            return false;
    }
    return true;
}

// A value outside the field's type that cannot be stored behind its allocation stays listed in it: the slot holds anything, and the type is only expected.
static void expectFieldsInitializedWithOtherValues(Program& program, const ParallelLoops& loops)
{
    Vector<FieldReference> fields = collectInParallel<FieldReference>(loops, program.functions.size(), [&](unsigned index, Vector<FieldReference>& found) {
        const Function& function = program.functions[index];
        findListedValue(program, FunctionID(index), opcodesThatListInitialValues, [&](const Value& allocation) {
            if (!writesInitialValues(program, allocation))
                return false;
            for (uint32_t valueIndex = 0; valueIndex < allocation.numberOfVariadicOperands(); ++valueIndex) {
                if (isUnprovenInitialValue(program, function, allocation, valueIndex) && !canStoreInitialValueBehindAllocation(program, allocation, valueIndex))
                    found.append(declarationOf(program, { allocation.layout(), allocation.fields()[valueIndex] }));
            }
            return false;
        });
    });
    if (fields.isEmpty())
        return;

    for (FieldReference reference : fields) {
        Field& field = program.at(reference.layout).fields[reference.field];
        field.declared.flags.add(PositionFlags::IsExpected);
        field.declared.flags.remove(PositionFlags::IsInt32Promise);
        field.flags.remove(FieldFlags::IsEnforced);
    }
    updateInheritedFields(program, loops);
}

static void reportInitialValueOutsideType(IRBuilder& builder, ProgramPoint allocation)
{
    const AnalysisContext& context = builder.context();
    context.build.diagnostics().add(InternalError {
        .kind = InternalErrorKind::StoredTypeMismatch,
        .function = context.id,
        .block = allocation.block,
        .value = builder.function().at(allocation.block).values[allocation.index],
        .pass = PassID::NarrowFieldTypes,
    });
}

static ProgramPoint programPointAfter(ProgramPoint point)
{
    return { point.block, point.index + 1 };
}

static uint32_t defineBehindAllocation(IRBuilder& builder, ProgramPoint point, uint32_t firstValueIndex)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    ValueID id = function.at(point.block).values[point.index];
    uint32_t numberOfValues = function.at(id).numberOfVariadicOperands();

    for (uint32_t valueIndex = firstValueIndex; valueIndex < numberOfValues; ++valueIndex) {
        Value definition = IRBuilder::make(Opcode::PropDefine, { id, function.at(id).variadicOperands()[valueIndex] });
        definition.setKey(initializedField(program, function.at(id), valueIndex).key);
        builder.insertBefore(programPointAfter(point), WTF::move(definition));
    }

    Value& allocation = function.edit(id);
    allocation.setFields(allocation.fields().first(firstValueIndex));
    allocation.setVariadicOperands(allocation.variadicOperands().first(firstValueIndex));
    return numberOfValues - firstValueIndex;
}

static uint32_t storeBehindAllocation(IRBuilder& builder, ProgramPoint point)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    ValueID id = function.at(point.block).values[point.index];
    builder.setOrigin(function.at(id).origin());

    uint32_t numberOfStores = 0;
    uint32_t numberOfValues = function.at(id).numberOfVariadicOperands();
    for (uint32_t valueIndex = 0; valueIndex < numberOfValues; ++valueIndex) {
        if (!isUnprovenInitialValue(program, function, function.at(id), valueIndex))
            continue;

        const Field& field = initializedField(program, function.at(id), valueIndex);
        if (!canStoreInitialValueBehindAllocation(program, function.at(id), valueIndex)) {
            reportInitialValueOutsideType(builder, point);
            continue;
        }
        std::optional<Value> filler = tryCreateFiller(program, program.at(slotTypeOf(program, field)));
        if (!filler)
            return numberOfStores + defineBehindAllocation(builder, point, valueIndex);

        Value store = IRBuilder::make(Opcode::PropSet, { id, function.at(id).variadicOperands()[valueIndex] });
        store.setKey(field.key);
        store.setMode(ECMAMode::Strict);
        ValueID fillerID = builder.insertBefore(point, WTF::move(*filler));
        function.edit(id).mutableVariadicOperands()[valueIndex] = fillerID;
        builder.insertBefore(programPointAfter(point), WTF::move(store));
        ++numberOfStores;
    }
    return numberOfStores;
}

static void storeUnprovenInitialValues(IRBuilder& builder)
{
    const Program& program = builder.context().program;
    Function& function = builder.function();
    uint64_t numberOfStores = 0;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            if (writesInitialValues(program, function.at(function.at(block).values[index])))
                numberOfStores += storeBehindAllocation(builder, { block, index });
        }
    }
    builder.applyInsertions();
    builder.context().build.statistics().count(StatisticID::InitialValuesStoredBehindAllocations, numberOfStores);
}

void storeUnprovenInitialValues(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    if (!hasUnprovenInitialValue(program, function))
        return;

    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    storeUnprovenInitialValues(builder);
}

// A parameter of type number that every call the program shows fills with an int32.
static void forEachCheckedNumberParameter(const Function& function, NOESCAPE const Invocable<void(ValueID check, unsigned entryParameterIndex)> auto& functor)
{
    if (!function.hasBody())
        return;
    std::span<const ValueID> parameters = function.at(entryBlock).parameters.span();
    if (parameters.size() != function.numberOfEntryParameters())
        return;
    unsigned first = function.firstParameterIndex();
    for (ValueID value : function.at(entryBlock).values) {
        const Value& check = function.at(value);
        if (check.opcode != Opcode::Check || check.castType() != TypeTable::number)
            continue;
        auto position = std::ranges::find(parameters, check.valueOperand());
        if (position == parameters.end())
            continue;
        unsigned index = static_cast<unsigned>(position - parameters.begin());
        if (function.entryParameterKind(index) != EntryParameter::Parameter || index - first >= function.numberOfPositionalParameters())
            continue;
        const DeclaredType& declared = *function.declaredEntryParameter(index);
        if (declared.type == TypeTable::number && declared.flags.contains(PositionFlags::IsExpected))
            functor(value, index);
    }
}

static void forEachNumberExpectedToBeInt32(const FieldEvidence& evidence, const BitVector& functionsHandedOn, FunctionID id, const Function& function, NOESCAPE const Invocable<void(ValueID check, unsigned entryParameterIndex, bool isCertain)> auto& functor)
{
    forEachCheckedNumberParameter(function, [&](ValueID check, unsigned index) {
        unsigned parameter = index - function.firstParameterIndex();
        if (evidence.visibleArguments(id, parameter) != StoredKinds { StoredKind::Int32 })
            return;
        functor(check, index, functionsHandedOn.quickGet(id.index()) || evidence.typesOfVisibleArgumentsLieInInt32(id, parameter));
    });
}

static bool hasNumberExpectedToBeInt32(const FieldEvidence& evidence, const BitVector& functionsHandedOn, FunctionID id, const Function& function)
{
    bool found = false;
    forEachNumberExpectedToBeInt32(evidence, functionsHandedOn, id, function, [&](ValueID, unsigned, bool isCertain) {
        found |= isCertain;
    });
    return found;
}

static void expectInt32(const Program& program, FunctionID id, Function& function, std::span<const std::pair<ValueID, unsigned>> checkedParameters, TypeArena& types, BuildContext& build)
{
    if (checkedParameters.empty())
        return;

    Vector<ValueID, 8> changed;
    for (auto [check, index] : checkedParameters) {
        function.edit(check, ValueEdit::Definition).setCastType(TypeTable::int32);
        function.declaredEntryParameter(index)->type = TypeTable::int32;
        changed.append(function.at(entryBlock).parameters[index]);
        changed.append(check);
    }
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    narrowValueTypes(TypePropagationInputs { program, id, function, analyses, types, build }, changed.span());
}

static void expectInt32Parameters(const Program& program, const FieldEvidence& evidence, const BitVector& functionsHandedOn, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    Vector<std::pair<ValueID, unsigned>, 4> found;
    forEachNumberExpectedToBeInt32(evidence, functionsHandedOn, id, function, [&](ValueID check, unsigned index, bool isCertain) {
        if (isCertain)
            found.append({ check, index });
    });
    expectInt32(program, id, function, found.span(), types, build);
}

static Vector<Int32ParameterCandidate> findInt32ParameterCandidates(const Program& program, const FieldEvidence& evidence, const BitVector& functionsHandedOn, const ParallelLoops& loops)
{
    return collectInParallel<Int32ParameterCandidate>(loops, program.functions.size(), [&](unsigned index, Vector<Int32ParameterCandidate>& candidates) {
        forEachNumberExpectedToBeInt32(evidence, functionsHandedOn, FunctionID(index), program.functions[index], [&](ValueID, unsigned parameterIndex, bool isCertain) {
            if (!isCertain)
                candidates.append({ FunctionID(index), parameterIndex });
        });
    });
}

void expectInt32ParametersWithRemainingChecks(Program& program, std::span<const Int32ParameterCandidate> candidates, BuildContext& build)
{
    if (candidates.empty())
        return;

    Vector<FunctionID> functions;
    for (const Int32ParameterCandidate& candidate : candidates) {
        if (functions.isEmpty() || functions.last() != candidate.function)
            functions.append(candidate.function);
    }
    ProgramEditor editor(program);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        auto [first, last] = std::ranges::equal_range(candidates, id, { }, &Int32ParameterCandidate::function);
        Vector<std::pair<ValueID, unsigned>, 4> found;
        forEachCheckedNumberParameter(function, [&](ValueID check, unsigned index) {
            bool isCandidate = std::ranges::any_of(first, last, [&](const Int32ParameterCandidate& candidate) {
                return candidate.entryParameterIndex == index;
            });
            if (isCandidate)
                found.append({ check, index });
        });
        expectInt32(program, id, function, found.span(), arenas.types, build);
    });
}

static bool accessesNarrowedField(const Value& value, const BitVector& narrowedKeys)
{
    return Value::opcodesWithKey.contains(value.opcode) && value.key() && narrowedKeys.get(value.key().index());
}

// Most functions that name the key of a narrowed field name it on other objects.
static bool hasAccessTypedOtherwise(const Program& program, FunctionID id, const BitVector& narrowedKeys)
{
    if (narrowedKeys.isEmpty())
        return false;
    const Function& function = program.at(id);
    std::optional<ScratchTypeArena> types;
    return findListedValue(program, id, Value::opcodesWithKey, [&](const Value& value) {
        if (!accessesNarrowedField(value, narrowedKeys))
            return false;
        if (!types)
            types.emplace(program.types);
        return resultTypeOf(program, function, value, *types) != value.type || expectedTypeOf(program, function, value, *types) != value.expected();
    });
}

static void retypeNarrowedFieldAccesses(const Program& program, const BitVector& narrowedKeys, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    Vector<ValueID> accesses;
    for (const Block& block : function.blocks) {
        for (ValueID value : block.values) {
            if (accessesNarrowedField(function.at(value), narrowedKeys))
                accesses.append(value);
        }
    }
    if (accesses.isEmpty())
        return;

    // The functions have just been typed, and of the records that the rules read only the narrowed fields have changed.
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    narrowValueTypes(TypePropagationInputs { program, id, function, analyses, types, build }, accesses.span());
}

static Vector<FunctionID> findAffectedFunctionsLargestFirst(const Program& program, BuildContext& build, const FieldEvidence& evidence, const BitVector& functionsHandedOn, const BitVector& narrowedKeys)
{
    const ParallelLoops& loops = build.parallelLoops();
    Vector<FunctionID> functions;
    TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        functions = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& affected) {
            const Function& function = program.functions[index];
            if (function.hasBody() && (hasAccessTypedOtherwise(program, FunctionID(index), narrowedKeys) || hasUnprovenInitialValue(program, FunctionID(index)) || hasNumberExpectedToBeInt32(evidence, functionsHandedOn, FunctionID(index), function)))
                affected.append(FunctionID(index));
        });
    }
    scratch.discard();
    sortLargestFirst(program, loops, functions);
    return functions;
}

Vector<Int32ParameterCandidate> narrowFieldTypes(Program& program, BuildContext& build)
{
    RELEASE_ASSERT(program.form() == IRForm::AsRead);
    const ParallelLoops& loops = build.parallelLoops();
    if (!hasResolvableFieldOrigins(program, loops))
        return { };

    FieldEvidence evidence(program, loops);
    if (!evidence.hasValidFieldReferences())
        return { };
    const BitVector& functionsHandedOn = evidence.functionsHandedOn();
    NarrowedFields narrowed = narrowFields(program, evidence, CellSizeClasses(build.options().sizeClassProgression), loops);
    markEnforcedFields(program, build);
    expectFieldsInitializedWithOtherValues(program, loops);
    if (narrowed.count)
        program.indexNames(loops);
    build.statistics().count(StatisticID::FieldsNarrowedToInt32, narrowed.count);

    Vector<Int32ParameterCandidate> candidates = findInt32ParameterCandidates(program, evidence, functionsHandedOn, loops);
    ProgramEditor editor(program);
    Vector<FunctionID> functions = findAffectedFunctionsLargestFirst(program, build, evidence, functionsHandedOn, narrowed.keys);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        expectInt32Parameters(program, evidence, functionsHandedOn, id, function, arenas.types, build);
        retypeNarrowedFieldAccesses(program, narrowed.keys, id, function, arenas.types, build);
        storeUnprovenInitialValues(program, id, function, arenas.types, build);
    });
    return candidates;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
