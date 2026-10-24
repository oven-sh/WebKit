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
#include "AOTThreshold.h"
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

struct FieldEvidence {
    VisibleStores stores;
    Vector<Vector<StoredKinds>> constantData;
    BitVector functionsHandedOn;
    BitVector layoutsKept;
};

} // anonymous namespace

static constexpr OpcodeSet opcodesThatListInitialValues { Opcode::NewObject, Opcode::CloneAs };

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

static void sortAndRemoveRepeated(auto& list)
{
    AOT::sort(list);
    removeRepeatedElements(list);
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
    AOT_THRESHOLD(size_t, maxRepeatedEntries, 16384);
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
            sortAndRemoveRepeated(found);
            sizesWithoutRepeatedEntries[worker] = found.size();
        }
    });
    forEachIndexInParallel(loops, foundByWorkers.size(), [&](unsigned worker) {
        sortAndRemoveRepeated(foundByWorkers[worker]);
    });

    Vector<Vector<StoredKinds>> kinds(program.layouts.size());
    for (const Vector<KindInField>& found : foundByWorkers) {
        for (auto [layout, field, kind] : found) {
            if (kinds[layout].isEmpty())
                kinds[layout].grow(program.layouts[layout].fields.size());
            kinds[layout][field].add(kind);
        }
    }
    return kinds;
}

// Nothing where a field reference of some function is not valid.
static std::optional<FieldEvidence> collectFieldEvidence(const Program& program, const ParallelLoops& loops)
{
    struct ScannedType {
        LayoutSetID layouts;
        bool listsFunctions : 1 { false };
        bool listsOnlyFunctions : 1 { false };
    };
    // What one walk over the bodies finds. Each thread has its own, and all of it is united, so the result does not depend on which thread saw which function.
    struct Scan {
        Vector<TypeID> typesHandedOn;
        Vector<LayoutSetID> layoutSetsKept;
        Vector<std::pair<FunctionID, LayoutSetID>> layoutSetsReturned;
        bool hasInvalidFieldReference { false };
    };
    Vector<ScannedType> types(program.types.size());
    Vector<Scan> scans(loops.width());

    // The casts and the operations that retain an operand. No other value says anything here.
    OpcodeSet opcodesToLookAt;
    for (Opcode opcode : allMembersOf<Opcode>) {
        bool retainsOperand = false;
        for (uint32_t operand = 0, end = numberOfNamedOperandsOf(opcode) + hasVariadicOperand(opcode); operand < end; ++operand)
            retainsOperand |= operandInfoOf(opcode, operand).uses.contains(OperandUse::Retains);
        if (retainsOperand || isCast(opcode))
            opcodesToLookAt.add(opcode);
    }
    constexpr size_t typesPerTask = 1024;
    forEachIndexInParallel(loops, static_cast<unsigned>((types.size() + typesPerTask - 1) / typesPerTask), [&](unsigned task) {
        for (size_t index = task * typesPerTask, end = std::min(types.size(), index + typesPerTask); index < end; ++index) {
            const Type& type = program.at(TypeID(index));
            types[index].layouts = type.layouts;
            types[index].listsFunctions = !type.functions.isEmpty();
            types[index].listsOnlyFunctions = !type.functions.isEmpty() && !type.anyObject && type.classes.isEmpty() && !type.layouts && type.builtins.isEmpty();
        }
    });

    FieldEvidence evidence;
    // The functions that the program hands on as values, to a callee or into memory that no type describes.
    evidence.stores = VisibleStores(program, loops, [&](FunctionID id, unsigned worker) {
        const Function& function = program.at(id);
        Scan& scan = scans[worker];
        for (const Value& value : function.values) {
            if (opcodesThatListInitialValues.contains(value.opcode) && value.fields().size() != value.numberOfVariadicOperands()) {
                scan.hasInvalidFieldReference = true;
                return false;
            }
        }

        Vector<LayoutSetID, 16> kept;
        Vector<LayoutSetID, 4> returned;
        auto expectedOrTypeOf = [&](ValueID value) -> const ScannedType& {
            return types[function.at(value).expectedOrType().index()];
        };
        // Of an operand that is retained.
        auto noteHandedOn = [&](ValueID operand, const OperandInfo& info) {
            TypeID type = function.at(operand).type;
            if (type && types[type.index()].listsFunctions && info.kind != OperandKind::Declared && info.retainedIn == RetainedIn::UnknownMemory)
                scan.typesHandedOn.append(type);
        };
        auto noteKept = [&](ValueID operand, auto& list) {
            if (LayoutSetID layouts = operand ? expectedOrTypeOf(operand).layouts : LayoutSetID())
                list.append(layouts);
        };
        for (const Block& block : function.blocks) {
            for (ValueID valueID : block.values) {
                const Value& value = function.at(valueID);
                if (!opcodesToLookAt.contains(value.opcode))
                    continue;
                ValueID callee = value.opcode == Opcode::Call ? value.operand(OperandIndex::Call::callee) : ValueID();
                bool namesItsCallees = value.opcode == Opcode::CallDirect || (callee && expectedOrTypeOf(callee).listsOnlyFunctions);
                bool isCastOfValue = !namesItsCallees && isCast(value.opcode);
                if (isCastOfValue && !expectedOrTypeOf(valueID).layouts)
                    noteKept(value.operand(0), kept);
                value.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                    const OperandInfo& info = operandInfoOf(value.opcode, operandIndex);
                    if (!info.uses.contains(OperandUse::Retains))
                        return;
                    noteHandedOn(operand, info);
                    if (!namesItsCallees && !isCastOfValue)
                        noteKept(operand, kept);
                });
            }
            block.terminator.forEachOperand([&](ValueID operand, uint32_t operandIndex) {
                const OperandInfo& info = operandInfoOf(block.terminator.kind, operandIndex);
                if (info.uses.contains(OperandUse::Retains)) {
                    noteHandedOn(operand, info);
                    noteKept(operand, kept);
                } else if (info.uses.contains(OperandUse::Returns))
                    noteKept(operand, returned);
            });
        }
        sortAndRemoveRepeated(kept);
        scan.layoutSetsKept.appendVector(kept);
        sortAndRemoveRepeated(returned);
        for (LayoutSetID set : returned)
            scan.layoutSetsReturned.append({ id, set });
        return true;
    });
    if (std::ranges::any_of(scans, &Scan::hasInvalidFieldReference))
        return std::nullopt;
    evidence.constantData = collectConstantDataKinds(program, loops);

    // The members of what the lists of the threads hold. Each thread sets bits of its own.
    auto uniteMembers = [&]<typename IDType>(Vector<IDType> Scan::* list, size_t numberOfBits, const auto& membersOf) {
        forEachIndexInParallel(loops, scans.size(), [&](unsigned worker) {
            sortAndRemoveRepeated(scans[worker].*list);
        });
        Vector<IDType> united;
        for (Scan& scan : scans)
            united.appendVector(std::exchange(scan.*list, { }));
        sortAndRemoveDuplicates(loops, united);

        constexpr size_t indicesPerTask = 16;
        Vector<BitVector> bitsOfWorkers(loops.width());
        forEachIndexInParallel(loops, static_cast<unsigned>((united.size() + indicesPerTask - 1) / indicesPerTask), [&](unsigned task, unsigned worker) {
            bitsOfWorkers[worker].ensureSize(numberOfBits);
            for (size_t index = task * indicesPerTask, end = std::min(united.size(), index + indicesPerTask); index < end; ++index) {
                for (auto member : membersOf(united[index]))
                    bitsOfWorkers[worker].quickSet(member.index());
            }
        });
        BitVector members(numberOfBits);
        for (const BitVector& bits : bitsOfWorkers)
            members.merge(bits);
        return members;
    };
    evidence.functionsHandedOn = uniteMembers(&Scan::typesHandedOn, program.functions.size(), [&](TypeID type) -> const auto& {
        return program.at(type).functions;
    });
    forEachIndexInParallel(loops, scans.size(), [&](unsigned worker) {
        for (auto [function, set] : scans[worker].layoutSetsReturned) {
            if (evidence.functionsHandedOn.quickGet(function.index()))
                scans[worker].layoutSetsKept.append(set);
        }
    });
    evidence.layoutsKept = uniteMembers(&Scan::layoutSetsKept, program.layouts.size(), [&](LayoutSetID set) -> const auto& {
        return program.at(set).members;
    });
    // A field is narrowed where it is declared.
    for (size_t index : BitVector(evidence.layoutsKept)) {
        for (const Field& field : program.layouts[index].fields) {
            if (field.originLayout)
                evidence.layoutsKept.quickSet(field.originLayout.index());
        }
    }
    return evidence;
}

static uint16_t nullishPrimitivesOf(StoredKinds kinds)
{
    return (kinds.contains(StoredKind::Undefined) ? Primitive::Undefined : 0) | (kinds.contains(StoredKind::Null) ? Primitive::Null : 0);
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

static std::optional<Type> findIntegersToKeepInSlot(const Program& program, const FieldEvidence& evidence, FieldReference reference, IntegerEvidence admitted)
{
    const Field& field = fieldAt(program, reference);
    if (!canBeStoredBehindAllocation(field))
        return std::nullopt;
    const Type& slotType = program.at(slotTypeOf(program, field));
    if (!(slotType.primitives & Primitive::Integer) || slotType.primitives & Primitive::Absent)
        return std::nullopt;
    if (!slotType.isNumberOrNullish() && !slotType.isAnything())
        return std::nullopt;

    const Vector<StoredKinds>& constantsByField = evidence.constantData[reference.layout.index()];
    StoredKinds constants = constantsByField.isEmpty() ? StoredKinds { } : constantsByField[reference.field];
    if (constants.contains(StoredKind::Other) || nullishPrimitivesOf(constants) & ~slotType.primitives)
        return std::nullopt;

    StoredKinds stored = evidence.stores.kindsStoredIn(reference.layout, reference.field);
    bool listsOnlyInt32s = !field.declared.flags.contains(PositionFlags::IsExpected) && program.at(field.declared.type).isInt32OrNullish();
    bool isDeclaredInt32 = listsOnlyInt32s || slotType.isInt32OrNullish() || field.declared.flags.contains(PositionFlags::IsInt32Promise);
    // A position in a text and an index of a table get numbers that nobody can bound: what a scanner counts up, what a caller passes.
    bool isCountedWith = admitted == IntegerEvidence::Uses && slotType.isNumberOrNullish() && stored.contains(StoredKind::Int32) && !stored.contains(StoredKind::NoInt32)
        && evidence.stores.isUsedAsInteger(reference.layout, reference.field) && isDeclaredInCheckedCode(program, reference.layout);
    // An object that starts with a number of a wider type is made with a filler and gets the number by a tested store.
    bool isShownOnlyInt32s = stored.contains(StoredKind::Int32) && !stored.contains(StoredKind::Other)
        && (evidence.layoutsKept.get(reference.layout.index()) || evidence.stores.typesOfInitialValuesFitInt32Slot(reference.layout, reference.field));
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

static bool isDeclaredObjectLayout(LayoutKind kind)
{
    return isObjectLayout(kind) && kind != LayoutKind::BuiltinResult;
}

static bool hasSlots(const Program& program, LayoutID id)
{
    return isDeclaredObjectLayout(program.at(id).kind) && hasOnlyCompiledBases(program, id);
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

bool canBeStoredBehindAllocation(const Field& field)
{
    constexpr OptionSet<FieldFlags> storedOnlyByAllocation { FieldFlags::IsMethod, FieldFlags::IsPrivate, FieldFlags::IsImmutable, FieldFlags::IsBrand, FieldFlags::IsHidden };
    return field.key && !field.flags.containsAny(storedOnlyByAllocation) && field.flow.location.stores == StoreCount::Unknown;
}

static bool writesInitialValues(const Program& program, const Value& value)
{
    return opcodesThatListInitialValues.contains(value.opcode) && isObjectLayout(program.at(value.layout()).kind) && value.fields().size() == value.numberOfVariadicOperands();
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

void storeUnprovenInitialValues(const Program& program, FunctionID id, Function& function, TypeArena& types, BuildContext& build)
{
    bool hasUnprovenValue = std::ranges::any_of(function.blocks, [&](const Block& block) {
        return std::ranges::any_of(block.values, [&](ValueID value) {
            return hasUnprovenInitialValue(program, function, function.at(value));
        });
    });
    if (!hasUnprovenValue)
        return;

    AnalysisContext context { build, program, nullptr, nullptr, id };
    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    auto storeBehindAllocation = [&](ProgramPoint point) -> uint32_t {
        ValueID allocationID = function.at(point.block).values[point.index];
        ProgramPoint behind { point.block, point.index + 1 };
        builder.setOrigin(function.at(allocationID).origin());

        uint32_t numberOfStores = 0;
        uint32_t numberOfValues = function.at(allocationID).numberOfVariadicOperands();
        for (uint32_t valueIndex = 0; valueIndex < numberOfValues; ++valueIndex) {
            if (!isUnprovenInitialValue(program, function, function.at(allocationID), valueIndex))
                continue;

            const Field& field = initializedField(program, function.at(allocationID), valueIndex);
            if (!canStoreInitialValueBehindAllocation(program, function.at(allocationID), valueIndex)) {
                builder.context().build.diagnostics().add(InternalError {
                    .kind = InternalErrorKind::StoredTypeMismatch,
                    .function = builder.context().id,
                    .block = point.block,
                    .value = allocationID,
                    .pass = PassID::NarrowFieldTypes,
                });
                continue;
            }
            std::optional<Value> filler = tryCreateFiller(program, program.at(slotTypeOf(program, field)));
            if (!filler) {
                for (uint32_t index = valueIndex; index < numberOfValues; ++index) {
                    Value definition = IRBuilder::make(Opcode::PropDefine, { allocationID, function.at(allocationID).variadicOperands()[index] });
                    definition.setKey(initializedField(program, function.at(allocationID), index).key);
                    builder.insertBefore(behind, WTF::move(definition));
                }
                Value& allocation = function.edit(allocationID);
                allocation.setFields(allocation.fields().first(valueIndex));
                allocation.setVariadicOperands(allocation.variadicOperands().first(valueIndex));
                return numberOfStores + numberOfValues - valueIndex;
            }

            Value store = IRBuilder::make(Opcode::PropSet, { allocationID, function.at(allocationID).variadicOperands()[valueIndex] });
            store.setKey(field.key);
            store.setMode(ECMAMode::Strict);
            ValueID fillerID = builder.insertBefore(point, WTF::move(*filler));
            function.edit(allocationID).mutableVariadicOperands()[valueIndex] = fillerID;
            builder.insertBefore(behind, WTF::move(store));
            ++numberOfStores;
        }
        return numberOfStores;
    };
    uint64_t numberOfStores = 0;
    for (uint32_t blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        uint32_t numberOfValues = static_cast<uint32_t>(function.at(block).values.size());
        for (uint32_t index = 0; index < numberOfValues; ++index) {
            if (writesInitialValues(program, function.at(function.at(block).values[index])))
                numberOfStores += storeBehindAllocation({ block, index });
        }
    }
    builder.applyInsertions();
    build.statistics().count(StatisticID::InitialValuesStoredBehindAllocations, numberOfStores);
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

static void forEachNumberExpectedToBeInt32(const FieldEvidence& evidence, FunctionID id, const Function& function, NOESCAPE const Invocable<void(ValueID check, unsigned entryParameterIndex, bool isCertain)> auto& functor)
{
    forEachCheckedNumberParameter(function, [&](ValueID check, unsigned index) {
        unsigned parameter = index - function.firstParameterIndex();
        if (evidence.stores.kindsPassedTo(id, parameter) == StoredKinds { StoredKind::Int32 })
            functor(check, index, evidence.functionsHandedOn.quickGet(id.index()) || evidence.stores.typesPassedLieInInt32(id, parameter));
    });
}

static void narrowTypesFrom(const Program& program, FunctionID id, Function& function, std::span<const ValueID> changed, TypeArena& types, BuildContext& build)
{
    FunctionAnalyses analyses(AnalysisContext { build, program, nullptr, nullptr, id }, function);
    narrowValueTypes(TypePropagationInputs { program, id, function, analyses, types, build }, changed);
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
    narrowTypesFrom(program, id, function, changed.span(), types, build);
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
        auto candidatesOfFunction = std::ranges::equal_range(candidates, id, { }, &Int32ParameterCandidate::function);
        Vector<std::pair<ValueID, unsigned>, 4> found;
        forEachCheckedNumberParameter(function, [&](ValueID check, unsigned index) {
            if (std::ranges::contains(candidatesOfFunction, index, &Int32ParameterCandidate::entryParameterIndex))
                found.append({ check, index });
        });
        expectInt32(program, id, function, found.span(), arenas.types, build);
    });
}

static bool accessesNarrowedField(const Value& value, const BitVector& narrowedKeys)
{
    return Value::opcodesWithKey.contains(value.opcode) && value.key() && narrowedKeys.get(value.key().index());
}

static Vector<FunctionID> findAffectedFunctionsLargestFirst(const Program& program, BuildContext& build, const Vector<bool>& hasInt32Parameter, const BitVector& narrowedKeys)
{
    const ParallelLoops& loops = build.parallelLoops();
    // Most functions that name the key of a narrowed field name it on other objects.
    auto hasAccessTypedOtherwise = [&](FunctionID id) {
        if (narrowedKeys.isEmpty())
            return false;
        std::optional<ScratchTypeArena> types;
        return anyListedValue(program, id, Value::opcodesWithKey, [&](ValueID, const Value& value) {
            if (!accessesNarrowedField(value, narrowedKeys))
                return false;
            if (!types)
                types.emplace(program.types);
            return resultTypeOf(program, program.at(id), value, *types) != value.type || expectedTypeOf(program, program.at(id), value, *types) != value.expected();
        });
    };
    auto hasUnprovenValue = [&](FunctionID id) {
        return anyListedValue(program, id, opcodesThatListInitialValues, [&](ValueID, const Value& value) {
            return hasUnprovenInitialValue(program, program.at(id), value);
        });
    };
    Vector<FunctionID> functions;
    TypeInterningPhase scratch(program.types, 0u, TypeInterningPhase::Scratch);
    {
        ProgramCaches caches(program, build);
        functions = collectInParallel<FunctionID>(loops, program.functions.size(), [&](unsigned index, Vector<FunctionID>& affected) {
            FunctionID id(index);
            if (program.at(id).hasBody() && (hasAccessTypedOtherwise(id) || hasUnprovenValue(id) || hasInt32Parameter[index]))
                affected.append(id);
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
    // The verifier runs after this pass and reports whatever is rejected here.
    std::atomic<bool> hasResolvableFieldOrigins { true };
    forEachIndexInParallel(loops, program.layouts.size(), [&](unsigned index) {
        for (const Field& field : program.layouts[index].fields) {
            if (!originCanBeResolved(program, field))
                hasResolvableFieldOrigins.store(false, std::memory_order_relaxed);
        }
    });
    if (!hasResolvableFieldOrigins.load(std::memory_order_relaxed))
        return { };
    std::optional<FieldEvidence> evidence = collectFieldEvidence(program, loops);
    if (!evidence)
        return { };

    BitVector narrowedKeys;
    uint64_t numberOfNarrowedFields = 0;
    auto narrow = [&](FieldReference reference, const Type& expected) {
        TypeID type = program.types.intern(expected);
        Field& field = program.at(reference.layout).fields[reference.field];
        if (field.declared.type == type && !field.declared.flags.contains(PositionFlags::IsExpected))
            return;
        field.declared.type = type;
        field.declared.flags.remove(PositionFlags::IsExpected);
        narrowedKeys.set(field.key.index());
        ++numberOfNarrowedFields;
    };
    using FieldToNarrow = std::pair<FieldReference, Type>;
    Vector<BitVector> countedFields(program.layouts.size());
    std::atomic<bool> hasCountedFields { false };
    Vector<FieldToNarrow> fields = collectInParallel<FieldToNarrow>(loops, program.layouts.size(), [&](unsigned layoutIndex, Vector<FieldToNarrow>& found) {
        if (!hasSlots(program, LayoutID(layoutIndex)))
            return;
        for (uint32_t fieldIndex = 0; fieldIndex < program.layouts[layoutIndex].fields.size(); ++fieldIndex) {
            FieldReference reference { LayoutID(layoutIndex), fieldIndex };
            if (fieldAt(program, reference).flags.contains(FieldFlags::IsInherited))
                continue;
            if (std::optional<Type> expected = findIntegersToKeepInSlot(program, *evidence, reference, IntegerEvidence::TypesAndStores))
                found.append({ reference, WTF::move(*expected) });
            else if (findIntegersToKeepInSlot(program, *evidence, reference, IntegerEvidence::Uses)) {
                countedFields[layoutIndex].set(fieldIndex);
                hasCountedFields.store(true, std::memory_order_relaxed);
            }
        }
    });
    for (const auto& [reference, expected] : fields)
        narrow(reference, expected);
    // The uses are a guess, and a guess costs a test at every store and every read.
    if (hasCountedFields.load(std::memory_order_relaxed)) {
        constexpr uint32_t saved = widthOf(SlotRepresentation::OptionalDouble) - widthOf(SlotRepresentation::OptionalInt32);
        AOT_THRESHOLD(uint32_t, largestCellWithEveryField, 128);
        CellSizeClasses sizeClasses(build.options().sizeClassProgression);
        Vector<FieldReference> fieldsThatMakeCellsSmaller = collectInParallel<FieldReference>(loops, program.layouts.size(), [&](unsigned layoutIndex, Vector<FieldReference>& found) {
            const Layout& layout = program.layouts[layoutIndex];
            if (!hasSlots(program, LayoutID(layoutIndex)) || layout.flow.allocation == AllocationState::NeverAllocated)
                return;
            Vector<FieldReference, 8> held;
            uint32_t bytes = offsetOfFirstField;
            for (uint32_t fieldIndex = 0; fieldIndex < layout.fields.size(); ++fieldIndex) {
                FieldReference declaration = declarationOf(program, { LayoutID(layoutIndex), fieldIndex });
                bytes += likelyWidthOfSlot(program, fieldAt(program, declaration));
                if (countedFields[declaration.layout.index()].get(declaration.field))
                    held.append(declaration);
            }
            if (held.isEmpty() || bytes > largestCellWithEveryField || sizeClasses.cellSizeFor(bytes - saved * held.size()) == sizeClasses.cellSizeFor(bytes))
                return;
            found.appendVector(held);
        });
        sortAndRemoveRepeated(fieldsThatMakeCellsSmaller);
        for (FieldReference reference : fieldsThatMakeCellsSmaller) {
            if (std::optional<Type> expected = findIntegersToKeepInSlot(program, *evidence, reference, IntegerEvidence::Uses))
                narrow(reference, *expected);
        }
    }
    markEnforcedFields(program, build);
    // A value outside the field's type that cannot be stored behind its allocation stays listed in it: the slot holds anything, and the type is only expected.
    Vector<FieldReference> fieldsInitializedWithOtherValues = collectInParallel<FieldReference>(loops, program.functions.size(), [&](unsigned index, Vector<FieldReference>& found) {
        forEachListedValue(program, FunctionID(index), opcodesThatListInitialValues, [&](BlockID, uint32_t, ValueID, const Value& allocation) {
            if (!writesInitialValues(program, allocation))
                return;
            for (uint32_t valueIndex = 0; valueIndex < allocation.numberOfVariadicOperands(); ++valueIndex) {
                if (isUnprovenInitialValue(program, program.functions[index], allocation, valueIndex) && !canStoreInitialValueBehindAllocation(program, allocation, valueIndex))
                    found.append(declarationOf(program, { allocation.layout(), allocation.fields()[valueIndex] }));
            }
        });
    });
    for (FieldReference reference : fieldsInitializedWithOtherValues) {
        Field& field = program.at(reference.layout).fields[reference.field];
        field.declared.flags.add(PositionFlags::IsExpected);
        field.declared.flags.remove(PositionFlags::IsInt32Promise);
        field.flags.remove(FieldFlags::IsEnforced);
    }
    if (!fieldsInitializedWithOtherValues.isEmpty())
        updateInheritedFields(program, loops);
    if (numberOfNarrowedFields)
        program.indexNames(loops);
    build.statistics().count(StatisticID::FieldsNarrowedToInt32, numberOfNarrowedFields);

    Vector<bool> hasInt32Parameter(FillWith { }, program.functions.size(), false);
    Vector<Int32ParameterCandidate> candidates = collectInParallel<Int32ParameterCandidate>(loops, program.functions.size(), [&](unsigned index, Vector<Int32ParameterCandidate>& found) {
        forEachNumberExpectedToBeInt32(*evidence, FunctionID(index), program.functions[index], [&](ValueID, unsigned parameterIndex, bool isCertain) {
            if (isCertain)
                hasInt32Parameter[index] = true;
            else
                found.append({ FunctionID(index), parameterIndex });
        });
    });
    ProgramEditor editor(program);
    Vector<FunctionID> functions = findAffectedFunctionsLargestFirst(program, build, hasInt32Parameter, narrowedKeys);
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned, FunctionID id, Function& function, TaskArenas& arenas) {
        Vector<std::pair<ValueID, unsigned>, 4> checkedParameters;
        forEachNumberExpectedToBeInt32(*evidence, id, function, [&](ValueID check, unsigned index, bool isCertain) {
            if (isCertain)
                checkedParameters.append({ check, index });
        });
        expectInt32(program, id, function, checkedParameters.span(), arenas.types, build);

        Vector<ValueID> accesses;
        for (const Block& block : function.blocks) {
            for (ValueID value : block.values) {
                if (accessesNarrowedField(function.at(value), narrowedKeys))
                    accesses.append(value);
            }
        }
        // The functions have just been typed, and of the records that the rules read only the narrowed fields have changed.
        if (!accesses.isEmpty())
            narrowTypesFrom(program, id, function, accesses.span(), arenas.types, build);
        storeUnprovenInitialValues(program, id, function, arenas.types, build);
    });
    return candidates;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
