/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEnforcedFields.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include "AOTIRProgram.h"
#include "AOTLinkTables.h"
#include "AOTResultRules.h"
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
    // A family has a lower number than every family that holds it. Upward: what holds a family with an instance has the instance.
    for (uint32_t number = 0; number < families.size(); ++number) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(number));
        if (above && m_leadsToInstance.quickGet(number))
            m_leadsToInstance.quickSet(above.index());
    }
    // Downward: the members of a family are members of every family that holds it.
    for (uint32_t number = families.size(); number--;) {
        FamilyID above = families.nearestFamilyAbove(FamilyID(number));
        if (above && m_leadsToInstance.quickGet(above.index()))
            m_leadsToInstance.quickSet(number);
    }
}

bool slotsOfLayoutMayBeHeldToTypes(const Program& program, const LinkTables& linked, const LinkTables& held, const FamiliesOfInstances& familiesOfInstances, LayoutID id)
{
    // The engine fills the results of its built-ins and the elements of a container by itself, and a value that does not fit an environment has nowhere to go.
    LayoutKind kind = program.at(id).kind;
    // THE OBJECTS OF CLASSES AND OF CONSTRUCTOR FUNCTIONS, AND THE LITERALS THAT LIE AT THEIR PLACES. A literal fills its fields where it is made, with whatever stands there: `{ a: i, b: 1 }`. Held to what is written, each value that is not proven
    // is tested in front of the allocation, the arms meet, and an object that never leaves, of which nothing was left, is made after all. One freeze (11-03 23:04:16), nothing held | literals too | instances alone:
    // the application's build 346.6 | 351.8 | 346.6 G, its code 142.09 | 145.05 | 142.02 MB, the TypeScript-compiler benchmark 40.49 | 40.31 | 40.25 G.
    // `{ kind, pos, end, .. } as SourceFile`: the literal is a member of the family of the nodes and lies at their places, and what lies at one place is held to one type or to none.
    // Left out, the one layout took the type from `kind`, `id` and `flags` of every node of the TypeScript-compiler benchmark (held-slots, 11-04 00:2x).
    if (kind != LayoutKind::Instance && (kind != LayoutKind::Record || !familiesOfInstances.sharesFamilyWithInstance(id)))
        return false;
    // `counts[word] = 1`: a store under a key that is no name asks the type again each time it comes to a field.
    auto isFilledUnderComputedKeys = [&](const LinkTables& tables) {
        return id.index() < tables.layouts.size() && tables.layouts[id.index()].isFilledUnderComputedKeys;
    };
    return !isFilledUnderComputedKeys(linked) && !isFilledUnderComputedKeys(held);
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

// The slot of an accessor holds its functions and a brand the token of its class, a store by name creates a field that is declared with other attributes beside its slot, and only an allocation fills a hidden field.
// UNDER A PRIVATE NAME THE SLOT ITSELF SAYS WHO HAS THE NAME. A value that does not fit has nowhere to go: moved out, the empty slot would say that the name is not there. So the walk builds no typed form under a name
// that some store may move out (buildAccessUnderPrivateNameForTypedAccess()), and what is closed there is a call of the run time. `#raw: number` with `this.#raw = raw`: 2,088 > 5,082 M (whole/quality/classes/fields-with-arrows.ts).
// Where every store is proven to fit, linked() says so to every read, and a held type adds nothing.
static bool holdsPlainValueInEnforcedFields(const Program& program, const Field& declaration)
{
    constexpr OptionSet<FieldFlags> filledByAllocationAlone { FieldFlags::IsBrand, FieldFlags::IsHidden };
    bool isUnderPrivateName = declaration.key && program.at(declaration.key).kind == PropertyKeyKind::PrivateName;
    return declaration.form.isEmpty() && !declaration.flags.containsAny(filledByAllocationAlone) && !isUnderPrivateName;
}

// THE FRONT END ALONE DECIDES WHAT IS GUARDED: a checked file, a trusted shape, what literals can bear. `.js`, `.d.ts`, an untrusted alias and a type without a required field arrive expected.
static SourceOfSlotType sourceOfWrittenTypeInEnforcedFields(const DeclaredType& declared)
{
    return declared.isGuarded() ? SourceOfSlotType::Written : SourceOfSlotType::Nothing;
}

// As the plan numbers them: what linked() appends, then what held() alone appends.
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

// `interface Type { id: number }   function Type(this: Type, flags) { this.flags = flags; }   made.id = next;`
// An appended field has no record. The types that the objects are written as say what lies under its name, where each that lists it is guarded. Null: nothing is written, or some view only expects.
static TypeID typeWrittenInFamilyInEnforcedFields(const Program& program, TypeArena& types, LayoutID layout, PropertyKeyID key)
{
    Vector<TypeID, 8> written;
    // False: what is written of an accessor is what its getter answers, and a method lies above the objects.
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
        for (const Program::FieldWrittenAbove& listed : program.fieldsWrittenAbove(declaring, key)) {
            if (!add(listed.view, listed.field))
                return { };
        }
    } else {
        for (LayoutID view : views) {
            if (std::optional<uint32_t> field = program.at(view).findField(key); field && !add(view, *field))
                return { };
        }
    }
    TypeID united = written.isEmpty() ? TypeID() : types.unionOf(written.span());
    return united && !isNeverType(united) ? united : TypeID();
}

// In the words of the run time's test, which is all that a door asks (appendTypeRecord() of image/AOTTypeTables.cpp), and of the middle: one layout or one family. No contents, no site, nothing that a value can lose while it lies there.
// Null: it says too little to be worth a test at each store.
static TypeID typeInWordsOfTestsInEnforcedFields(const Families& families, TypeArena& types, TypeID type)
{
    if (!types.at(type).isJavaScriptValue() || types.at(type).primitives & Primitive::Unspecified)
        return { };
    TypeID tested = types.testablePartOf(types.withLiteralStringsWidened(widenedToFamily(types, families, types.withoutPrimitives(type, Primitive::Absent))));
    if (types.at(tested).hasUnboundedUnknown())
        return { };
    // The closures of one function are found by one compare, and whoever reads one asks LinkTables::mayBeMentionedBy() before he names its code. A built-in has a cell only where linked() has it in reach.
    if (types.at(tested).objects && !singleFunctionOf(types, types.at(types.withOnlyObjects(tested))))
        return { };
    // A stub that stores tests a shape, a cell type, a callable and the closures of one function by itself and leaves every other test to the run time (branchUnlessTypeAccepts()).
    // `timer: Timeout`, a class of the host that is known by its class info: each store by a stub was a call of the run time: express, once a request: +1.0% on every route.
    for (const ClassPart& part : classPartsOf(types, types.at(tested))) {
        if (types.builtins().at(part.builtinClass).instanceTest != BuiltinInstanceTest::JSTypeEquals)
            return { };
    }
    return tested;
}

// `kind: SyntaxKind.SourceFile` is held to every int32 and `done: true` to both booleans: a type that takes all that its form holds spares each store by name the test, and what the one number was is for linked() to tell a read.
// The few integers that a narrow form keeps for its marks are the form's to leave out: whoever gives the slot that form writes the form's own type.
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

// A position in a text and an index of a table get numbers that nobody can bound: what a scanner counts up, what a caller passes. What the text does with them says what they are.
// The guess picks a form inside a written type, and the tests at the stores make it true: a fraction that arrives after all takes the field out of its slot, object by object. So it asks for a use that only an integer has, and gives way to one written fraction.
static bool numberIsInt32ByGuessInEnforcedFields(const Program& program, const LinkTables& linked, const LinkTables& held, TypeArena& types, FieldReference reference)
{
    const Layout& layout = program.at(reference.layout);
    bool isAppended = reference.field >= layout.fields.size();
    PropertyKeyID key = isAppended ? keyOfAppendedFieldInEnforcedFields(linked, reference.layout, reference.field - layout.fields.size()) : layout.fields[reference.field].key;
    if (!isAppended && layout.fields[reference.field].declared.has(PositionFlags::IsInt32Promise))
        return true;
    // A fraction, or what is no number, has been seen on its way into the field: the guess would fail where the program runs as it is written.
    if (const LinkedCell* seen = held.findCellOfField(reference); seen && seen->proven && !isNeverType(seen->proven)) {
        Type shown = types.at(seen->proven);
        // Every primitive at once is what comes out of code that nobody sees: it shows nothing.
        bool showsSomething = !shown.hasUnboundedUnknown() && (shown.primitives & Primitive::All) != Primitive::All;
        // `number` is what Link makes of a sum or a count: it shows no fraction. Fractions without an integer among them do.
        bool showsFractionsAlone = shown.primitives & Primitive::Double && !(shown.primitives & Primitive::Integer);
        if (showsSomething && (showsFractionsAlone || shown.primitives & (Primitive::Boolean | Primitive::AnyString | Primitive::AnySymbol | Primitive::BigInt) || shown.hasObjects()))
            return false;
    }
    if (!key || linked.keysStoredWhatIsNoInt32.get(key.index()))
        return false;
    // The text names the field by what is written of the receiver: the class itself, or one of the types that its objects are written as.
    auto namesField = [&](const FieldsOfNumbers& fields) {
        return fields.names(program, reference.layout, key);
    };
    if (!(namesField(linked.fieldsReadAsIntegers) || namesField(linked.fieldsStoredCounts)) || namesField(linked.fieldsStoredWhatIsNoInt32))
        return false;
    return isDeclaredInCheckedCodeOfProgramInEnforcedFields(program, reference.layout);
}

// Some body that may open a guess names the key on a receiver that may be the layout. Nobody counts that in the live tree (UsesOfField of fields-and-objects-2 is out of it), so every field counts as named and nothing is taken back for want of a reader.
static bool fieldIsNamedWhereGuessesOpenInEnforcedFields(FieldReference)
{
    return true;
}

// No door is ever passed, and every store of compiled code brings a value of the type. So no marker is ever needed beside it.
static bool everyStoreIsProvenToFitInEnforcedFields(const LinkTables& linked, TypeArena& types, FieldReference field, const LinkedCell* proven, TypeID type)
{
    bool reachesUnseenCode = field.layout.index() >= linked.layouts.size() || linked.layouts[field.layout.index()].reachesUnseenCode;
    return !reachesUnseenCode && proven && proven->everyWriterIsKnown && proven->proven && types.isSubset(proven->proven, type);
}

EnforcedField enforcedTypeOfField(const Program& program, const LinkTables& linked, const LinkTables& held, const Families& families, const FamiliesOfInstances& familiesOfInstances, TypeArena& types, FieldReference reference)
{
    if (!slotsOfLayoutMayBeHeldToTypes(program, linked, held, familiesOfInstances, reference.layout))
        return { };
    // `{ get x() { .. } }`: the slot holds the pair of functions.
    if (const LinkedCell* cell = linked.findCellOfField(reference); cell && cell->isFilledByAccessorsAlone)
        return { };

    size_t numberDeclared = program.at(reference.layout).fields.size();
    bool isAppended = reference.field >= numberDeclared;
    // What a layout has from the layout above is one field with what is declared there: the constructor above stores into objects of either.
    FieldReference declaration = isAppended ? reference : declarationOf(program, reference);
    if (declaration.layout != reference.layout && !slotsOfLayoutMayBeHeldToTypes(program, linked, held, familiesOfInstances, declaration.layout))
        return { };

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
    }

    EnforcedField answer;
    TypeID type;
    if (written && written != TypeTable::anything && !describesBagOfOptions(types, written))
        type = typeInWordsOfTestsInEnforcedFields(families, types, written);
    if (type) {
        Type words = types.at(type);
        bool holdsFractionsOrWideIntegers = words.primitives & Primitive::Double || !words.integers.isSubsetOf(Range::int32());
        answer.isInt32ByGuess = words.isNumberOrNullish() && holdsFractionsOrWideIntegers && numberIsInt32ByGuessInEnforcedFields(program, linked, held, types, declaration);
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

    const LinkedCell* proven = linked.findCellOfField(declaration);
    if (!type)
        return { };

    // EVERY HELD TYPE LETS undefined AND null LIE IN THE SLOT: a class's field holds undefined until its definition runs, an optional member is left out, and every form that a field can leave has a mark for each.
    // Beside numbers and booleans they are markers, which the record says by a bit. A type that holds a reference lists them: the front end writes both into it, so only a record by hand lacks one.
    if (types.at(type).hasOnlyPrimitives(rawValuesInEnforcedFields | Primitive::UndefinedOrNull))
        answer.holdsUndefinedAndNull = isAppended || !everyStoreIsProvenToFitInEnforcedFields(linked, types, declaration, proven, type);
    else
        type = types.withPrimitives(type, Primitive::UndefinedOrNull);
    answer.type = type;
    answer.source = source;
    return answer;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
