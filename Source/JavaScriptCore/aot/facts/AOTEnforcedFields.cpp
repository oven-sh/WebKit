/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnforcedFields.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTCellFormat.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTResultRules.h"
#include "MarkedSpace.h"
#include <algorithm>
#include <span>

namespace JSC { namespace AOT { namespace Typed {

static constexpr uint16_t rawValuesInEnforcedFields = Primitive::Number | Primitive::Boolean;


FamiliesOfInstances::FamiliesOfInstances(const Program& program, const Families& families)
    : m_families(families)
    , m_leadsToInstance(families.size())
{
    for (uint32_t index = 0; index < program.layouts.size(); ++index) {
        if (program.layouts[index].kind != LayoutKind::Instance)
            continue;
        if (FamilyID nearest = families.nearestFamilyOf(LayoutID(index)))
            m_leadsToInstance.quickSet(nearest.index());
    }
    for (uint32_t number = 0; number < families.size(); ++number) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(number));
        if (above && m_leadsToInstance.quickGet(number))
            m_leadsToInstance.quickSet(above.index());
    }
    for (uint32_t number = families.size(); number--;) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(number));
        if (above && m_leadsToInstance.quickGet(above.index()))
            m_leadsToInstance.quickSet(number);
    }
}

bool slotsOfLayoutMayBeHeldToTypes(const Program& program, const FamiliesOfInstances& familiesOfInstances, LayoutID id)
{
    LayoutKind kind = program.at(id).kind;
    return kind == LayoutKind::Instance || kind == LayoutKind::Record;
}

static bool hasRoomForHiddenWord(const Program& program, const LinkTables& linked, LayoutID id)
{
    constexpr OptionSet<LayoutChange> changesThatMakeExtraProperties { LayoutChange::GainsProperty, LayoutChange::GetsAccessor };
    if (id.index() >= linked.layouts.size() || linked.layout(id).possibleChanges.containsAny(changesThatMakeExtraProperties) || linked.numberOfAppendedKeysOfPlan(id))
        return true;
    return program.at(id).fields.size() % 2;
}

static bool isNarrowNumberInEnforcedFields(const Type& type)
{
    if (type.hasOnlyPrimitives(Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::Absent))
        return type.primitives & Primitive::Boolean;
    return (type.primitives & Primitive::Number) && !(type.primitives & Primitive::Double) && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent) && type.integers.isSubsetOf(Range::int32());
}

static bool isReferenceInEnforcedFields(const Type& type)
{
    return type.hasCells() && !type.hasUnboundedUnknown() && !(type.primitives & (Primitive::Number | Primitive::Boolean));
}

static constexpr size_t mostFieldsOfSmallRecord = 5;
static uint32_t fieldsThatMakeCellSmaller(const Program& program, const LinkTables& linked, TypeArena& types, LayoutID id)
{
    constexpr uint32_t widthOfNarrowSlot = sizeof(uint32_t);
    constexpr OptionSet<LayoutChange> changesThatMakeExtraProperties { LayoutChange::GainsProperty, LayoutChange::GetsAccessor };
    const auto& fields = program.at(id).fields;
    if (fields.isEmpty() || fields.size() > mostFieldsOfSmallRecord || id.index() >= linked.layouts.size() || linked.numberOfAppendedKeysOfPlan(id))
        return 0;
    bool hasHiddenWord = linked.layout(id).possibleChanges.containsAny(changesThatMakeExtraProperties);
    uint32_t end = offsetOfFirstField + (hasHiddenWord ? widthOfNarrowSlot : 0) + static_cast<uint32_t>(sizeof(uint64_t) * fields.size());
    uint32_t sizeWanted = WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(end);
    uint32_t chosen = 0;
    for (bool choosesNumbers : { true, false }) {
        for (uint32_t field = 0; field < fields.size(); ++field) {
            TypeID written = fieldAt(program, declarationOf(program, { id, field })).declared.written();
            if (!written || !(choosesNumbers ? isNarrowNumberInEnforcedFields(types.at(written)) : isReferenceInEnforcedFields(types.at(written))))
                continue;
            chosen |= 1u << field;
            end -= sizeof(uint64_t) - widthOfNarrowSlot;
            if (WTF::roundUpToMultipleOf<MarkedSpace::sizeStep>(end) < sizeWanted)
                return chosen;
        }
    }
    return 0;
}

static bool isHeldForSmallerCell(const Program& program, const LinkTables& linked, const Families& families, TypeArena& types, LayoutID id, PropertyKeyID key)
{
    constexpr uint32_t mostMembers = 32;
    FamilyID nearest = families.nearestFamilyOf(id);
    if (!nearest)
        return false;
    LayoutSetID members = families.layoutsOf(families.topmostFamilyAtOrAbove(nearest));
    uint32_t numberOfMembers = program.at(members).size();
    if (numberOfMembers < 2 || numberOfMembers > mostMembers)
        return false;
    bool someSmallRecordHasKey = program.at(members).anyOf([&](LayoutID member) {
        const Layout& record = program.at(member);
        return record.fields.size() <= mostFieldsOfSmallRecord && record.findField(key);
    });
    if (!someSmallRecordHasKey)
        return false;
    auto isTag = [&](LayoutID member, PropertyKeyID tag) {
        std::optional<uint32_t> field = program.at(member).kind == LayoutKind::Record ? program.at(member).findField(tag) : std::nullopt;
        TypeID written = field ? fieldAt(program, declarationOf(program, { member, *field })).declared.written() : TypeID();
        return written && types.at(written).hasOnlyPrimitives(Primitive::Integer) && isNarrowNumberInEnforcedFields(types.at(written));
    };
    bool hasTag = std::ranges::any_of(program.at(id).fields, [&](const Field& field) {
        bool everyMemberHasIt = field.key && isTag(id, field.key);
        program.at(members).forEach([&](LayoutID member) {
            everyMemberHasIt = everyMemberHasIt && isTag(member, field.key);
        });
        return everyMemberHasIt;
    });
    if (!hasTag)
        return false;
    bool isHeld = false;
    program.at(members).forEach([&](LayoutID member) {
        std::optional<uint32_t> field = program.at(member).findField(key);
        isHeld = isHeld || (field && *field < mostFieldsOfSmallRecord && fieldsThatMakeCellSmaller(program, linked, types, member) >> *field & 1);
    });
    return isHeld;
}

static bool holdsField(const Program& program, const LinkTables& linked, const Families& families, const FamiliesOfInstances& familiesOfInstances, TypeArena& types, FieldReference field, PropertyKeyID key, TypeID written)
{
    if (program.at(field.layout).kind == LayoutKind::Instance || familiesOfInstances.sharesFamilyWithInstance(field.layout))
        return true;
    Type type = types.at(written);
    if (key && (isNarrowNumberInEnforcedFields(type) || isReferenceInEnforcedFields(type)) && isHeldForSmallerCell(program, linked, families, types, field.layout, key))
        return true;
    bool isNumber = (type.primitives & Primitive::Number) && type.hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull | Primitive::Absent);
    return isNumber && key && linked.fieldsUsedInProducts.names(program, field.layout, key) && hasRoomForHiddenWord(program, linked, field.layout);
}

static bool moreIsStoredThanIsSeenInEnforcedFields(const LinkTables& linked, const LinkTables& held, LayoutID id)
{
    auto isFilledUnderComputedKeys = [&](const LinkTables& tables) {
        return id.index() < tables.layouts.size() && tables.layouts[id.index()].isFilledUnderComputedKeys;
    };
    return isFilledUnderComputedKeys(linked) || isFilledUnderComputedKeys(held);
}

TypeID contentsOfEnforcedSlot(TypeArena& types, TypeID slotType, bool holdsUndefinedAndNull)
{
    return slotType && holdsUndefinedAndNull ? types.withPrimitives(slotType, Primitive::UndefinedOrNull) : slotType;
}

bool enforcedTypeSparesTestOfStoresByName(const TypeArena& types, TypeID slotType)
{
    if (!slotType)
        return true;
    Type type = types.at(slotType);
    if (!type.hasOnlyPrimitives(rawValuesInEnforcedFields | Primitive::UndefinedOrNull))
        return false;
    uint16_t values = type.primitives & rawValuesInEnforcedFields;
    if (values == Primitive::Integer)
        return Range::int32().isSubsetOf(type.integers);
    return values == Primitive::Boolean || (values == Primitive::Number && type.integers == Range::full());
}

static bool holdsPlainValueInEnforcedFields(const Program& program, const Field& declaration)
{
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsBrand, FieldFlags::IsHidden };
    bool isUnderPrivateName = declaration.key && program.at(declaration.key).kind == PropertyKeyKind::PrivateName;
    return !declaration.form.contains(FieldForm::IsAccessor) && !declaration.flags.containsAny(filledByAllocationAlone) && !isUnderPrivateName;
}

static SourceOfSlotType sourceOfWrittenTypeInEnforcedFields(const DeclaredType& declared)
{
    return declared.isGuarded() ? SourceOfSlotType::Written : SourceOfSlotType::Nothing;
}

bool FieldsOfNumbers::names(const Program& program, LayoutID layout, PropertyKeyID key) const
{
    LayoutID declaring = declaredLayoutOf(program, layout);
    if (contains(layout, key) || contains(declaring, key))
        return true;
    return std::ranges::any_of(program.viewsAbove(declaring), [&](LayoutID view) {
        return contains(view, key);
    });
}

static PropertyKeyID keyOfAppendedFieldInEnforcedFields(const LinkTables& linked, LayoutID layout, size_t place)
{
    std::span<const PropertyKeyID> keys = linked.appendedKeysOf(layout);
    if (place < keys.size())
        return keys[place];
    std::span<const PropertyKeyID> keysOfOtherReading = linked.keysAppendedByHeldAloneOf(layout);
    place -= keys.size();
    return place < keysOfOtherReading.size() ? keysOfOtherReading[place] : PropertyKeyID();
}

static TypeID typeWrittenInFamilyInEnforcedFields(const Program& program, TypeArena& types, LayoutID layout, PropertyKeyID key)
{
    Vector<TypeID, 8> written;
    auto add = [&](LayoutID view, uint32_t field) {
        const Field& record = program.at(view).fields[field];
        if (record.form.contains(FieldForm::IsAccessor) || record.flags.contains(FieldFlags::IsMethod) || !record.declared.isGuarded())
            return false;
        written.append(record.declared.written());
        return true;
    };
    LayoutID declaring = declaredLayoutOf(program, layout);
    std::span<const LayoutID> views = program.viewsAbove(declaring);
    if (views.size() >= Program::fewestViewsIndexedByKey) {
        bool isAnswered = program.visitInheritedFields(declaring, key, [&](Program::FieldOfView listed) {
            return add(listed.view, listed.field);
        });
        if (!isAnswered)
            return { };
    } else {
        for (LayoutID view : views) {
            if (std::optional<uint32_t> field = program.at(view).findField(key); field && !add(view, *field))
                return { };
        }
    }
    TypeID united = written.isEmpty() ? TypeID() : types.unionOf(written.span());
    return united && !isNeverType(united) ? united : TypeID();
}

static TypeID typeInWordsOfTestsInEnforcedFields(const Families& families, TypeArena& types, TypeID type)
{
    if (!types.at(type).isJavaScriptValue() || types.at(type).primitives & Primitive::Unspecified)
        return { };
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(widenedToFamily(types, families, types.withoutPrimitives(type, Primitive::Absent))));
    if (types.at(tested).hasUnboundedUnknown())
        return { };
    if (types.at(tested).objects && !singleFunctionOf(types, types.at(types.withOnlyObjects(tested))))
        return { };
    for (const ClassPart& part : classPartsOf(types, types.at(tested))) {
        if (types.builtins().at(part.builtinClass).instanceTest != BuiltinInstanceTest::JSTypeEquals)
            return { };
    }
    return tested;
}

static TypeID widenedToAllThatFormHoldsInEnforcedFields(TypeArena& types, TypeID type, bool numbersAreInt32)
{
    Type narrow = types.at(type);
    if (narrow.primitives & Primitive::Number) {
        bool fitsInt32 = numbersAreInt32 || (!(narrow.primitives & Primitive::Double) && narrow.integers.isSubsetOf(Range::int32()));
        type = fitsInt32 ? types.withIntegers(types.withoutPrimitives(type, Primitive::Double), Range::int32()) : types.withPrimitives(type, Primitive::Number);
    }
    if (narrow.primitives & Primitive::Boolean)
        type = types.withPrimitives(type, Primitive::Boolean);
    return type;
}

static bool isDeclaredInCheckedCodeOfProgramInEnforcedFields(const Program& program, LayoutID id)
{
    auto isCheckedCode = [&](FunctionID function) {
        return function && !isLibraryCode(program, program.at(function)) && program.at(function).has(FunctionFlags::IsChecked);
    };
    const Layout& layout = program.at(id);
    if (layout.constructor)
        return isCheckedCode(layout.constructor);
    if (!layout.module)
        return false;
    const Module& declaringModule = program.at(layout.module);
    return !declaringModule.flags.contains(ModuleFlags::IsLibrary) && isCheckedCode(declaringModule.body);
}

static bool isInt32InInstancesOfFamily(const Program& program, const LinkTables& linked, const Families& families, LayoutID record, PropertyKeyID key)
{
    FamilyID nearest = families.nearestFamilyOf(record);
    if (!nearest || program.at(record).kind != LayoutKind::Record)
        return false;
    return program.at(families.layoutsOf(families.topmostFamilyAtOrAbove(nearest))).anyOf([&](LayoutID member) {
        const Layout& layout = program.at(member);
        return layout.kind == LayoutKind::Instance && layout.findField(key)
            && (linked.fieldsReadAsIntegers.names(program, member, key) || linked.fieldsStoredCounts.names(program, member, key));
    });
}

static bool numberIsInt32ByGuessInEnforcedFields(const Program& program, const LinkTables& linked, const LinkTables& held, const Families& families, TypeArena& types, FieldReference reference)
{
    const Layout& layout = program.at(reference.layout);
    bool isAppended = reference.field >= layout.fields.size();
    PropertyKeyID key = isAppended ? keyOfAppendedFieldInEnforcedFields(linked, reference.layout, reference.field - layout.fields.size()) : layout.fields[reference.field].key;
    if (!isAppended && layout.fields[reference.field].declared.has(PositionFlags::IsInt32Promise))
        return true;
    auto isInt32InFamily = [&] {
        return !isAppended && key && isInt32InInstancesOfFamily(program, linked, families, reference.layout, key);
    };
    if (const LinkedCell* seen = held.findCellOfField(reference); seen && seen->proven && !isNeverType(seen->proven)) {
        Type shown = types.at(seen->proven);
        bool showsSomething = !shown.hasUnboundedUnknown() && (shown.primitives & Primitive::All) != Primitive::All;
        bool showsNoInt32 = !!(shown.primitives & Primitive::FractionalNumber) || (shown.primitives & Primitive::Double && !(shown.primitives & Primitive::Integer));
        bool showsAnyNumber = shown.isNumberOrNullish() && (shown.primitives & Primitive::Number) == Primitive::Number && shown.integers == Range::full();
        bool showsOtherValues = shown.primitives & (Primitive::Boolean | Primitive::AnyString | Primitive::AnySymbol | Primitive::BigInt) || shown.hasObjects();
        if (showsSomething && (showsNoInt32 || showsOtherValues) && !(showsAnyNumber && isInt32InFamily()))
            return false;
    }
    if (!key || linked.keysStoredWhatIsNoInt32.get(key.index()))
        return false;
    auto namesField = [&](const FieldsOfNumbers& fields) {
        return fields.names(program, reference.layout, key);
    };
    if (namesField(linked.fieldsStoredWhatIsNoInt32))
        return false;
    if (!namesField(linked.fieldsReadAsIntegers) && !namesField(linked.fieldsStoredCounts) && !isInt32InFamily())
        return false;
    return isDeclaredInCheckedCodeOfProgramInEnforcedFields(program, reference.layout);
}

static bool fieldIsNamedWhereGuessesOpenInEnforcedFields(FieldReference)
{
    return true;
}

static bool everyStoreIsProvenToFitInEnforcedFields(const LinkTables& linked, TypeArena& types, FieldReference field, TypeID type)
{
    bool reachesUnseenCode = field.layout.index() >= linked.layouts.size() || linked.layouts[field.layout.index()].reachesUnseenCode;
    TypeID proven = linked.provenOfField(field);
    return !reachesUnseenCode && proven && types.isSubset(proven, type);
}

bool isLibraryLayout(const Program& program, LayoutID id)
{
    const Layout& layout = program.at(id);
    return layout.module && program.at(layout.module).flags.contains(ModuleFlags::IsLibrary);
}

static TypeID typeWrittenByOthersOfFamilyInEnforcedFields(const Program& program, const Families& families, TypeArena& types, LayoutID layout, PropertyKeyID key)
{
    FamilyID nearest = families.nearestFamilyOf(layout);
    if (!nearest || !key || program.at(layout).kind != LayoutKind::Instance)
        return { };
    Vector<TypeID, 8> written;
    program.at(families.layoutsOf(families.topmostFamilyAtOrAbove(nearest))).forEach([&](LayoutID member) {
        const Layout& other = program.at(member);
        std::optional<uint32_t> listed = member != layout && other.kind == LayoutKind::Instance ? other.findField(key) : std::nullopt;
        if (!listed || isLibraryLayout(program, member) != isLibraryLayout(program, layout))
            return;
        const Field& record = fieldAt(program, declarationOf(program, { member, *listed }));
        if (TypeID type = holdsPlainValueInEnforcedFields(program, record) && record.declared.isGuarded() ? record.declared.written() : TypeID(); type && type != TypeTable::anything && !written.contains(type))
            written.append(type);
    });
    TypeID united = written.isEmpty() ? TypeID() : types.unionOf(written.span());
    return united && !isNeverType(united) ? united : TypeID();
}

static bool holdsSeenStores(const LinkTables& held, TypeArena& types, FieldReference field, TypeID type, bool isExpectedOfUnknownValues)
{
    const LinkedCell* seen = held.findCellOfField(field);
    if (!seen || !seen->proven || isNeverType(seen->proven))
        return true;
    TypeID stored = types.withoutPrimitives(seen->proven, Primitive::UndefinedOrNull | Primitive::Absent);
    if (isNeverType(stored))
        return true;
    Type shown = types.at(stored);
    if (isExpectedOfUnknownValues)
        return shown.hasUnboundedUnknown() || (shown.primitives & Primitive::All) == Primitive::All || types.overlap(stored, type);

    constexpr uint16_t values = Primitive::All & ~Primitive::UndefinedOrNull;
    if ((shown.primitives & values) == values)
        return false;
    if (seen->leavesOutWhatArrives || shown.hasUnboundedUnknown())
        return types.at(type).hasObjects() || types.overlap(stored, type);
    if (types.at(type).primitives & Primitive::Number)
        stored = types.withoutPrimitives(stored, Primitive::BigInt);
    return types.isSubset(stored, type);
}

EnforcedField enforcedTypeOfField(const Program& program, const LinkTables& linked, const LinkTables& held, const Families& families, const FamiliesOfInstances& familiesOfInstances, TypeArena& types, FieldReference reference)
{
    if (!slotsOfLayoutMayBeHeldToTypes(program, familiesOfInstances, reference.layout))
        return { };
    if (const LinkedCell* cell = linked.findCellOfField(reference); cell && cell->isFilledByAccessorsAlone)
        return { };

    size_t numberDeclared = program.at(reference.layout).fields.size();
    bool isAppended = reference.field >= numberDeclared;
    FieldReference declaration = isAppended ? reference : declarationOf(program, reference);
    if (declaration.layout != reference.layout && !slotsOfLayoutMayBeHeldToTypes(program, familiesOfInstances, declaration.layout))
        return { };
    bool moreIsStoredThanIsSeen = moreIsStoredThanIsSeenInEnforcedFields(linked, held, reference.layout) || moreIsStoredThanIsSeenInEnforcedFields(linked, held, declaration.layout);

    TypeID written;
    SourceOfSlotType source = SourceOfSlotType::Nothing;
    if (isAppended) {
        PropertyKeyID key = keyOfAppendedFieldInEnforcedFields(linked, reference.layout, reference.field - numberDeclared);
        if (!key)
            return { };
        written = typeWrittenInFamilyInEnforcedFields(program, types, reference.layout, key);
        source = SourceOfSlotType::WrittenInFamily;
    } else {
        const Field& record = fieldAt(program, declaration);
        if (!holdsPlainValueInEnforcedFields(program, record))
            return { };
        source = sourceOfWrittenTypeInEnforcedFields(record.declared);
        if (source != SourceOfSlotType::Nothing)
            written = record.declared.written();
        bool isOnlyExpected = record.declared.isExpected() && !record.declared.writtenFlags().contains(PositionFlags::IsFromCompiler);
        if (isOnlyExpected || written == TypeTable::anything) {
            TypeID ofOthers = typeWrittenByOthersOfFamilyInEnforcedFields(program, families, types, declaration.layout, record.key);
            TypeID inWords = ofOthers ? typeInWordsOfTestsInEnforcedFields(families, types, ofOthers) : TypeID();
            TypeID expected = inWords && isOnlyExpected && record.declared.written() ? typeInWordsOfTestsInEnforcedFields(families, types, record.declared.written()) : TypeID();
            if (inWords && (!isOnlyExpected || (expected && types.isSubset(expected, inWords))) && holdsSeenStores(held, types, declaration, inWords, isOnlyExpected)) {
                written = isOnlyExpected ? record.declared.written() : ofOthers;
                source = SourceOfSlotType::WrittenInFamily;
            }
        }
    }

    if (!isAppended && !moreIsStoredThanIsSeen && (!written || written == TypeTable::anything)) {
        bool isFilledOnceByDefinition = fieldAt(program, declaration).form.containsAll({ FieldForm::IsReadOnly, FieldForm::IsNotConfigurable });
        if (const LinkedCell* seen = held.findCellOfField(declaration); seen && seen->proven && (!seen->leavesOutWhatArrives || isFilledOnceByDefinition) && !isNeverType(seen->proven)) {
            Type shown = types.at(seen->proven);
            if (!shown.hasUnboundedUnknown() && (shown.primitives & Primitive::All) != Primitive::All && !shown.hasOnlyPrimitives(rawValuesInEnforcedFields | Primitive::UndefinedOrNull)) {
                written = seen->proven;
                source = SourceOfSlotType::Seen;
            }
        }
    }

    EnforcedField answer;
    TypeID type;
    PropertyKeyID key = isAppended ? keyOfAppendedFieldInEnforcedFields(linked, reference.layout, reference.field - numberDeclared) : fieldAt(program, declaration).key;
    bool isHeld = written && holdsField(program, linked, families, familiesOfInstances, types, reference, key, written);
    if (isHeld && written != TypeTable::anything && !describesBagOfOptions(types, written))
        type = typeInWordsOfTestsInEnforcedFields(families, types, written);
    if (type) {
        Type words = types.at(type);
        bool holdsFractionsOrWideIntegers = words.primitives & Primitive::Double || !words.integers.isSubsetOf(Range::int32());
        answer.isInt32ByGuess = !moreIsStoredThanIsSeen && words.isNumberOrNullish() && holdsFractionsOrWideIntegers && numberIsInt32ByGuessInEnforcedFields(program, linked, held, families, types, declaration);
        if (answer.isInt32ByGuess && program.at(reference.layout).kind == LayoutKind::Record && !familiesOfInstances.sharesFamilyWithInstance(reference.layout))
            return { };
        type = widenedToAllThatFormHoldsInEnforcedFields(types, type, answer.isInt32ByGuess);
    }
    if (type && source != SourceOfSlotType::Compiler) {
        bool noReaderGains = !fieldIsNamedWhereGuessesOpenInEnforcedFields(declaration) && !enforcedTypeSparesTestOfStoresByName(types, type);
        if (noReaderGains) {
            EnforcedField takenBack;
            takenBack.wasTakenBack = true;
            return takenBack;
        }
    }

    if (!type)
        return { };

    if (!isAppended && !fieldAt(program, declaration).form.isEmpty() && types.at(type).hasOnlyPrimitives(rawValuesInEnforcedFields | Primitive::UndefinedOrNull))
        return { };

    if (types.at(type).hasOnlyPrimitives(rawValuesInEnforcedFields | Primitive::UndefinedOrNull))
        answer.holdsUndefinedAndNull = isAppended || !everyStoreIsProvenToFitInEnforcedFields(linked, types, declaration, type);
    else
        type = types.withPrimitives(type, Primitive::UndefinedOrNull);
    answer.type = type;
    answer.source = source;
    return answer;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
