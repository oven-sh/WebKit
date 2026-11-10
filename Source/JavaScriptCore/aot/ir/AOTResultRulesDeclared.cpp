/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRulesInternal.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinMembers.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTIRProgram.h"
#include "AOTSchemaOperandPositions.h"
#include "AOTSlotRepresentation.h"
#include "AOTTypeNarrowing.h"
#include "AOTTypeRelations.h"
#include <algorithm>
#include <wtf/ASCIICType.h>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static bool holdsOnlyArraysBesideNullish(TypeReader reader, const Type& type)
{
    if (!type.hasOnlyClassObjects())
        return false;
    std::span<const ClassPart> parts = classPartsOf(reader, type);
    return parts.size() == 1 && parts[0].builtinClass == BuiltinClasses::Array;
}

namespace {

struct FieldReadUnion {
    const TypingContext& context;
    Vector<TypeID, 16> parts { };
    bool hasUninformativePart { false };

    ALWAYS_INLINE bool add(TypeID part)
    {
        if (part && !parts.isEmpty() && parts.last() == part)
            return true;
        bool isGuess = context.level == TypeLevel::Expected;
        if (!part || (isGuess && context.at(part).isAnything())) {
            hasUninformativePart = true;
            return isGuess;
        }
        if (parts.isEmpty() || parts.last() != part)
            parts.append(part);
        return true;
    }

    TypeID finish() const
    {
        TypeID united = parts.size() == 1 ? parts[0] : context.types.unionOf(parts.span());
        if (!hasUninformativePart)
            return united;
        bool isStillWorthGuessing = context.level == TypeLevel::Expected && holdsOnlyArraysBesideNullish(context.types, context.at(united));
        return isStillWorthGuessing ? united : TypeID();
    }
};

} // anonymous namespace

static std::span<const LayoutID> layoutsOfSetInResultRules(const TypingContext& context, LayoutSetID set, LayoutID& onlyMember)
{
    if (!holdsOneMember(set))
        return membersOf(context.types, set);
    onlyMember = LayoutID::fromBits(positionOf(set));
    return singleElementSpan(onlyMember);
}

static ALWAYS_INLINE TypeID tryTypeHeldInSlot(const TypingContext& context, FieldReference reference)
{
    if (TypeID known = context.positions.slot(context.level, reference.layout, reference.field))
        return known;

    if (reference.field >= context.program.at(reference.layout).fields.size())
        return { };

    const Field& field = fieldAt(context.program, reference);
    if (field.form.contains(FieldForm::IsAccessor))
        return { };
    if (context.level == TypeLevel::Expected)
        return context.typeIDOf(field.declared);
    if (!field.declared.isEstablished())
        return { };

    TypeID established = context.typeIDOf(field.declared);
    Type type = context.at(established);
    bool hasMarkers = isObjectLayout(context.program.at(reference.layout).kind) && !type.isNever()
        && type.hasOnlyPrimitives(Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull);
    return hasMarkers ? context.types.withPrimitives(established, Primitive::UndefinedOrNull) : established;
}

TypeID tryTypeWrittenAbove(const TypingContext& context, LayoutID layout, PropertyKeyID key)
{
    if (context.level != TypeLevel::Expected)
        return { };

    const Program& program = context.program;
    FieldReadUnion written { context };
    bool isWritten = false;
    auto add = [&](LayoutID view, uint32_t field) ALWAYS_INLINE_LAMBDA {
        const Field& record = program.at(view).fields[field];
        if (record.form.contains(FieldForm::IsAccessor) || record.flags.contains(FieldFlags::IsMethod))
            return false;
        isWritten = true;
        written.add(context.typeIDOf(record.declared));
        return true;
    };
    LayoutID declaring = declaredLayoutOf(program, layout);
    std::span<const LayoutID> views = program.viewsAbove(declaring);
    if (views.size() >= Program::fewestViewsIndexedByKey) {
        for (const Program::FieldWrittenAbove& listed : program.fieldsWrittenAbove(declaring, key)) {
            if (!add(listed.view, listed.field))
                return { };
        }
        return isWritten ? written.finish() : TypeID();
    }
    for (LayoutID view : views) {
        if (std::optional<uint32_t> field = program.at(view).findField(key); field && !add(view, *field))
            return { };
    }
    return isWritten ? written.finish() : TypeID();
}

static TypeID tryTypeReadFromSlot(const TypingContext& context, FieldReference reference, PropertyKeyID key)
{
    TypeID held = tryTypeHeldInSlot(context, reference);
    if (!held && reference.field >= context.program.at(reference.layout).fields.size())
        return tryTypeWrittenAbove(context, reference.layout, key);
    TypeID arriving = held ? context.positions.readOfSlot(context.level, reference.layout, reference.field) : TypeID();
    if (!arriving)
        return held;
    TypeID found = context.types.intersectionOf(held, arriving);
    return isNeverType(found) || !context.types.isSubset(found, held) ? held : found;
}

static ALWAYS_INLINE std::optional<FieldReference> findFieldOfSlotAccess(const TypingContext& context, LayoutID layout, PropertyKeyID key)
{
    if (std::optional<FieldReference> declared = findField(context.program, layout, key))
        return declared;
    if (std::optional<uint32_t> appended = context.positions.appendedField(layout, key))
        return FieldReference { layout, *appended };
    return std::nullopt;
}

static TypeID askEachReceiverForSlotContents(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    FieldReadUnion read { context };
    bool someReceiverHasField = false;
    LayoutID onlyReceiver;
    Vector<LayoutID, 16> asked;
    bool asksSomeReceivers = !holdsOneMember(receivers) && context.positions.collectQueriedLayouts(key, receivers, false, asked);
    for (LayoutID receiver : asksSomeReceivers ? asked.span() : layoutsOfSetInResultRules(context, receivers, onlyReceiver)) {
        std::optional<FieldReference> field = findFieldOfSlotAccess(context, receiver, key);
        if (!field)
            continue;
        someReceiverHasField = true;
        if (!read.add(tryTypeReadFromSlot(context, *field, key)))
            return { };
    }
    return someReceiverHasField ? read.finish() : TypeID();
}

TypeID tryTypeOfSlotContents(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    if (!receivers)
        return { };
    if (holdsOneMember(receivers))
        return askEachReceiverForSlotContents(context, receivers, key);
    return context.positions.askedOfEachLayout(PositionTypes::QuestionOfEachLayout::ContentsOfSlot, context.level, receivers, key, scopedLambda<TypeID()>([&] {
        return askEachReceiverForSlotContents(context, receivers, key);
    }));
}

TypeID tryTypeOfSetterInSlot(const TypingContext& context, LayoutSetID receivers, PropertyKeyID)
{
    if (!receivers)
        return { };
    return context.types.function(UnknownSeed::ContentsOfEscapedObject);
}

TypeID tryTypeOfProvenHalfInSlot(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key, bool isSetter)
{
    if (!receivers || !holdsOneMember(receivers))
        return { };
    for (LayoutID receiver : membersOf(context.types, receivers)) {
        if (std::optional<FieldReference> field = findFieldOfSlotAccess(context, receiver, key))
            return context.positions.halfOfAccessorOfField(context.level, field->layout, field->field, isSetter);
    }
    return { };
}

TypeID tryTypeOfDeferringSlotContents(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    return tryTypeOfSlotContents(context, receivers, key);
}

TypeID tryTypeAcceptedBySlots(const TypingContext& context, LayoutSetID receivers, PropertyKeyID key)
{
    TypeID accepted;
    LayoutID onlyReceiver;
    for (LayoutID receiver : layoutsOfSetInResultRules(context, receivers, onlyReceiver)) {
        std::optional<FieldReference> field = findFieldOfSlotAccess(context, receiver, key);
        if (!field)
            return { };
        TypeID held = tryTypeHeldInSlot(context, *field);
        if (!held || isNeverType(held))
            continue;
        accepted = accepted ? context.types.intersectionOf(accepted, held) : held;
    }
    return accepted;
}

static bool isAccessorOfLayout(const Program& program, LayoutID layout, PropertyKeyID key)
{
    std::optional<FieldReference> field = layout ? findField(program, layout, key) : std::nullopt;
    return field && fieldAt(program, *field).form.contains(FieldForm::IsAccessor);
}

static ALWAYS_INLINE TypeID tryProvenTypeFoundAboveObjects(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    auto askEachLayout = [&]() -> TypeID {
        TypeID foundByAll;
        LayoutID onlyLayout;
        for (LayoutID layout : layoutsOfSetInResultRules(context, receiver.layouts, onlyLayout)) {
            TypeID found = context.positions.foundAboveObjectsOf(context.level, layout, key);
            if (!found)
                return { };
            if (isNeverType(found))
                continue;
            if (foundByAll && foundByAll != found)
                return { };
            foundByAll = found;
        }
        return foundByAll;
    };
    if (holdsOneMember(receiver.layouts))
        return askEachLayout();
    return context.positions.askedOfEachLayout(PositionTypes::QuestionOfEachLayout::FoundAboveObjects, context.level, receiver.layouts, key, scopedLambda<TypeID()>(askEachLayout));
}

static ALWAYS_INLINE TypeID tryProvenTypeReadOffLayouts(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    auto askEachLayout = [&]() -> TypeID {
        FieldReadUnion read { context };
        LayoutID onlyLayout;
        for (LayoutID layout : layoutsOfSetInResultRules(context, receiver.layouts, onlyLayout)) {
            if (!read.add(context.positions.readByNameOffObjectsOf(context.level, layout, key)))
                return { };
        }
        return read.finish();
    };
    if (holdsOneMember(receiver.layouts))
        return askEachLayout();
    return context.positions.askedOfEachLayout(PositionTypes::QuestionOfEachLayout::ReadByNameOffObjects, context.level, receiver.layouts, key, scopedLambda<TypeID()>(askEachLayout));
}

static ALWAYS_INLINE TypeID tryProvenTypeFoundAbovePrimitives(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    const BuiltinTable& builtins = context.program.builtins();
    BuiltinClassID builtinClass = receiver.hasObjects() || liesInThread(key) ? BuiltinClassID() : primitiveClassOf(static_cast<uint16_t>(receiver.primitives & ~Primitive::UndefinedOrNull));
    BuiltinName name = builtinClass ? context.program.builtinNameOf(key) : BuiltinName();
    if (!name)
        return { };
    if (BuiltinID own = builtins.findOwnPropertyOfInstances(builtinClass, name)) {
        BuiltinOperandTypes passed;
        passed.receiver = typeOfValuesThatLookUpAsInstancesOf(context.types, builtinClass);
        TypeID written = resultTypeOfReachableOverloads({ context.program, context.types, &context.positions, context.level }, own, BuiltinCallMode::Get, passed);
        return written && !isNeverType(written) && !context.at(written).isAnything() ? written : TypeID();
    }
    BuiltinID prototype;
    for (BuiltinClassID current = builtinClass; current && !prototype; current = builtins.at(current).parent)
        prototype = builtins.at(current).prototype;
    ObjectID object = prototype ? context.types.objects().objectOf(builtins.canonicalID(prototype)) : ObjectID();
    return object ? context.positions.property(context.level, object, key) : TypeID();
}

static ALWAYS_INLINE TypeID tryProvenTypeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    if (!receiver.isJavaScriptValue())
        return { };
    if (receiver.primitives & ~Primitive::UndefinedOrNull)
        return tryProvenTypeFoundAbovePrimitives(context, receiver, key);
    if (receiver.hasOnlyLayouts() && !receiver.traitsOfNamed.contains(ObjectTrait::MayHaveLayoutBelow)) {
        if (TypeID found = tryProvenTypeFoundAboveObjects(context, receiver, key))
            return found;
        if (TypeID found = tryProvenTypeReadOffLayouts(context, receiver, key))
            return found;
    }
    if (!receiver.hasOnlyNamedObjects() || receiver.namesMergedObjects())
        return { };

    return context.positions.askedOfEachObject(PositionTypes::QuestionOfEachObject::Property, context.level, receiver.objects, key.bits(), scopedLambda<TypeID()>([&] {
        const ObjectTable& objects = context.types.objects();
        Vector<TypeID, 16> typesFound;
        bool everyObjectAnswers = context.types.at(receiver.objects).allOf([&](ObjectID object) {
            if (isAccessorOfLayout(context.program, objects.layoutOf(object), key))
                return false;
            TypeID found = context.positions.property(context.level, object, key);
            if (!found)
                return false;
            if (typesFound.isEmpty() || typesFound.last() != found)
                typesFound.append(found);
            return true;
        });
        return everyObjectAnswers ? context.types.unionOf(typesFound.span()) : TypeID();
    }));
}

TypeID typeOfSlotContentsInReceiver(const TypingContext& context, TypeID contents, TypeID receiver, PropertyKeyID key)
{
    if (context.level != TypeLevel::Proven || !contents || !receiver)
        return contents;
    if (context.at(contents).primitives & (Primitive::Absent | Primitive::Unspecified))
        return contents;
    TypeID found = tryProvenTypeOfFieldRead(context, context.at(receiver), key);
    if (!found || context.types.isSubset(contents, found))
        return contents;
    TypeID held = context.types.intersectionOf(contents, found);
    return isNeverType(held) || !context.types.isSubset(held, contents) ? contents : held;
}

static TypeID tryTypeFoundOnPrototypeOfInstances(const TypingContext& context, LayoutID layout, PropertyKeyID key)
{
    if (context.program.at(layout).kind != LayoutKind::Instance)
        return { };
    return context.positions.property(context.level, context.types.objects().prototypeOf(layout), key);
}

static TypeID expectedTypeOfFieldReadByName(const TypingContext& context, LayoutID receiver, FieldReference reference, PropertyKeyID key)
{
    TypeID expected = context.positions.slot(context.level, reference.layout, reference.field);
    bool mayBeLeftOut = true;
    if (reference.field < context.program.at(reference.layout).fields.size()) {
        const Field& field = fieldAt(context.program, reference);
        if (!expected)
            expected = context.typeIDOf(field.declared);
        mayBeLeftOut = !field.form.contains(FieldForm::IsAccessor) && !field.flags.contains(FieldFlags::IsRequired);
    } else if (!expected)
        expected = tryTypeWrittenAbove(context, reference.layout, key);
    if (!expected || !mayBeLeftOut)
        return expected;
    TypeID above = tryTypeFoundOnPrototypeOfInstances(context, receiver, key);
    return context.types.unionOf(expected, above ? above : TypeTable::undefined);
}

static TypeID askEachReceiverForExpectedTypeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    FieldReadUnion read { context };
    bool isFoundSomewhere = false;
    bool isMissingSomewhere = false;
    auto expectedOfField = [&](LayoutID layout) -> std::optional<TypeID> {
        std::optional<FieldReference> field = findFieldOfSlotAccess(context, layout, key);
        if (field)
            return expectedTypeOfFieldReadByName(context, layout, *field, key);
        if (TypeID written = tryTypeWrittenAbove(context, layout, key))
            return context.types.withPrimitives(written, Primitive::Undefined);
        return std::nullopt;
    };
    auto addField = [&](LayoutID layout) {
        std::optional<TypeID> expected = expectedOfField(layout);
        if (!expected) {
            isMissingSomewhere = true;
            return;
        }
        isFoundSomewhere = true;
        read.add(*expected);
    };

    Vector<LayoutID, 8> layoutsOfSilentCells;
    Vector<LayoutID, 8> layoutsOfCellsThatAnswer;
    auto noteLayout = [](Vector<LayoutID, 8>& layouts, LayoutID layout) {
        if (layouts.isEmpty() || layouts.last() != layout)
            layouts.append(layout);
    };
    auto dropRepeats = [](Vector<LayoutID, 8>& layouts) {
        std::ranges::sort(layouts);
        layouts.shrink(layouts.size() - std::ranges::unique(layouts).size());
    };

    const ObjectTable& objects = context.types.objects();
    context.types.at(receiver.objects).forEach([&](ObjectID object) {
        TypeID found = context.positions.property(context.level, object, key);
        LayoutID layout = objects.layoutOf(object);
        if (!found) {
            if (layout)
                noteLayout(layoutsOfSilentCells, layout);
            else
                read.add(found);
            return;
        }
        isFoundSomewhere = true;
        read.add(found);
        if (layout)
            noteLayout(layoutsOfCellsThatAnswer, layout);
    });
    dropRepeats(layoutsOfSilentCells);
    for (LayoutID layout : layoutsOfSilentCells)
        addField(layout);
    dropRepeats(layoutsOfCellsThatAnswer);
    for (LayoutID layout : layoutsOfCellsThatAnswer) {
        std::optional<TypeID> ofField = expectedOfField(layout);
        if (ofField && *ofField && !context.at(*ofField).isAnything())
            read.add(*ofField);
    }
    std::span<const LayoutID> unnamedLayouts = membersOf(context.types, receiver.unnamedLayouts);
    Vector<LayoutID, 16> asked;
    if (receiver.unnamedLayouts && !holdsOneMember(receiver.unnamedLayouts) && context.positions.collectQueriedLayouts(key, receiver.unnamedLayouts, true, asked)) {
        isMissingSomewhere |= asked.size() < unnamedLayouts.size();
        unnamedLayouts = asked.span();
    }
    for (LayoutID layout : unnamedLayouts)
        addField(layout);

    if (!isFoundSomewhere)
        return { };
    if (isMissingSomewhere)
        read.add(TypeTable::undefined);
    return read.finish();
}

static TypeID tryExpectedTypeOfFieldRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    if (!receiver.objects && receiver.unnamedLayouts && !holdsOneMember(receiver.unnamedLayouts)) {
        return context.positions.askedOfEachLayout(PositionTypes::QuestionOfEachLayout::FieldReadByName, context.level, receiver.unnamedLayouts, key, scopedLambda<TypeID()>([&] {
            return askEachReceiverForExpectedTypeOfFieldRead(context, receiver, key);
        }));
    }
    if (!receiver.objects || receiver.unnamedLayouts)
        return askEachReceiverForExpectedTypeOfFieldRead(context, receiver, key);
    return context.positions.askedOfEachObject(PositionTypes::QuestionOfEachObject::Property, context.level, receiver.objects, key.bits(), scopedLambda<TypeID()>([&] {
        return askEachReceiverForExpectedTypeOfFieldRead(context, receiver, key);
    }));
}

TypeID tryTypeOfFieldRead(const TypingContext& context, TypeID receiverID, PropertyKeyID key)
{
    if (!receiverID)
        return { };
    Type receiver = context.at(receiverID);
    if (receiver.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return TypeTable::never;

    switch (context.level) {
    case TypeLevel::Proven:
        return tryProvenTypeOfFieldRead(context, receiver, key);
    case TypeLevel::Expected:
        return tryExpectedTypeOfFieldRead(context, receiver, key);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID tryTypeOfGlobalBindingRead(const TypingContext& context, PropertyKeyID key, bool mayBeShadowed)
{
    if (mayBeShadowed && context.level == TypeLevel::Proven)
        return { };

    return tryTypeOfFieldRead(context, typeOfBuiltinObject(context, Builtins::GlobalThis), key);
}

static TypeID tryTypeOfTypedArrayContents(const BuiltinTable& builtins, BuiltinClassID builtinClass, ContentsSlot slot)
{
    if (slot != ContentsSlot::Element || !builtinClass)
        return { };
    TypedArrayType typedArrayType = builtins.typedArrayTypeOf(builtinClass);
    return typedArrayType == NotTypedArray ? TypeID() : elementTypeOf(typedArrayType);
}

static TypeID tryWrittenContentsOfClassPart(const BuiltinTable& builtins, const ClassPart& part, ContentsSlot slot)
{
    std::optional<uint8_t> position = positionInTypeOf(builtins, part.builtinClass, slot);
    return position ? part.writtenContents[*position] : TypeID();
}

TypeID tryTypeOfContentsRead(const TypingContext& context, TypeID containerID, ContentsSlot slot)
{
    if (!containerID)
        return { };
    Type container = context.at(containerID);
    if (container.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return TypeTable::never;
    if (!container.isJavaScriptValue() || !container.hasOnlyClassObjects())
        return { };

    bool isProof = context.level == TypeLevel::Proven;
    const BuiltinTable& builtins = context.types.builtins();
    const ObjectTable& objects = context.types.objects();
    Vector<TypeID, 16> typesOfContents;
    for (const ClassPart& part : context.types.at(container.unnamedClasses)) {
        TypeID contents = tryTypeOfTypedArrayContents(builtins, part.builtinClass, slot);
        if (!contents && !isProof)
            contents = tryWrittenContentsOfClassPart(builtins, part, slot);
        if (!contents)
            return { };
        typesOfContents.append(contents);
    }
    if (!container.objects)
        return context.types.unionOf(typesOfContents.span());
    TypeID contentsOfObjects = context.positions.askedOfEachObject(PositionTypes::QuestionOfEachObject::Contents, context.level, container.objects, static_cast<uint32_t>(slot), scopedLambda<TypeID()>([&] {
        Vector<TypeID, 16> typesInObjects;
        bool everyObjectAnswers = context.types.at(container.objects).allOf([&](ObjectID object) {
            TypeID contents = tryTypeOfTypedArrayContents(builtins, objects.classOf(object), slot);
            if (!contents)
                return false;
            if (typesInObjects.isEmpty() || typesInObjects.last() != contents)
                typesInObjects.append(contents);
            return true;
        });
        return everyObjectAnswers ? context.types.unionOf(typesInObjects.span()) : TypeID();
    }));
    if (!contentsOfObjects)
        return { };
    typesOfContents.append(contentsOfObjects);
    return context.types.unionOf(typesOfContents.span());
}

TypeID typeOfContentsRead(const TypingContext& context, TypeID container, ContentsSlot slot)
{
    TypeID known = tryTypeOfContentsRead(context, container, slot);
    return known ? known : context.types.anything(UnknownSeed::ContentsOfEscapedObject);
}

static_assert(static_cast<uint32_t>(SetNextResult::HasEntry) == static_cast<uint32_t>(MapNextResult::HasEntry));

static std::optional<ContentsSlot> partOfEntryInResultOfStep(Opcode step, uint32_t resultIndex)
{
    if (step == Opcode::MapNext) {
        if (resultIndex == static_cast<uint32_t>(MapNextResult::Key))
            return ContentsSlot::Key;
        if (resultIndex == static_cast<uint32_t>(MapNextResult::Value))
            return ContentsSlot::Value;
        return std::nullopt;
    }
    if (step == Opcode::SetNext && resultIndex == static_cast<uint32_t>(SetNextResult::Element))
        return ContentsSlot::Element;
    return std::nullopt;
}

TypeID narrowResultByOtherResult(const TypingContext& context, Opcode producer, OperandTypes operandsOfProducer, uint32_t testedResult, uint32_t narrowedResult, TypeID source, EdgeSide side)
{
    if (side != EdgeSide::Taken || !source || testedResult != static_cast<uint32_t>(MapNextResult::HasEntry))
        return { };
    std::optional<ContentsSlot> part = partOfEntryInResultOfStep(producer, narrowedResult);
    if (!part)
        return { };
    std::optional<unsigned> collection = producer == Opcode::SetNext ? OperandPosition::set(producer) : OperandPosition::map(producer);
    RELEASE_ASSERT(collection);
    TypeID contents = tryTypeOfContentsRead(context, operandsOfProducer[*collection], *part);
    if (!contents)
        return { };
    return context.types.isSubset(contents, source) ? contents : context.types.intersectionOf(source, contents);
}

static bool hasIndexedElements(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    BuiltinSlotID slot = builtinClass ? builtins.findSlot(builtinClass, ContentsSlot::Element) : BuiltinSlotID();
    if (!slot)
        return false;
    AbstractHeap heap = builtins.at(slot).heap;
    return heap == AbstractHeap::Elements || heap == AbstractHeap::TypedArrayElements;
}

static bool hasOnlyClassesWithIndexedElements(const TypingContext& context, const Type& container)
{
    return std::ranges::all_of(classPartsOf(context.types, container), [&](const ClassPart& part) {
        return hasIndexedElements(context.types.builtins(), part.builtinClass);
    });
}

static bool isPositionInsideEveryText(const TypingContext& context, const Type& string, const Type& position)
{
    if ((string.primitives & Primitive::AnyString) != Primitive::LiteralString || !position.isInteger() || position.integers.minimum < 0)
        return false;
    return context.types.at(string.stringLiterals).allOf([&](StringID literal) {
        std::span<const uint8_t> text = context.program.string(literal);
        return static_cast<uint64_t>(position.integers.maximum) < text.size() && std::ranges::all_of(text, isASCII<uint8_t>);
    });
}

static TypeID tryTypeOfElementReadOffContainer(const TypingContext& context, TypeID containerID, TypeID keyID)
{
    if (!containerID || !keyID)
        return { };
    Type container = context.at(containerID);
    Type key = context.at(keyID);
    if (key.isNever() || container.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return TypeTable::never;
    if (!key.isNumber())
        return { };
    if (container.isAnyStringOrNullish())
        return isPositionInsideEveryText(context, container, key) ? TypeTable::string : TypeArena::fromPrimitives(Primitive::String | Primitive::Undefined);
    if (!hasOnlyClassesWithIndexedElements(context, container))
        return { };

    TypeID element = tryTypeOfContentsRead(context, containerID, ContentsSlot::Element);
    if (!element)
        return { };
    return context.types.withPrimitives(element, Primitive::Undefined);
}

TypeID tryTypeOfElementRead(const TypingContext& context, TypeID container, TypeID key)
{
    TypeID known = tryTypeOfElementReadOffContainer(context, container, key);
    return known ? known : context.positions.resultOfReadBeingTyped(context.level);
}

TypeID typeOfElementRead(const TypingContext& context, TypeID container, TypeID key)
{
    TypeID known = tryTypeOfElementRead(context, container, key);
    return known ? known : context.types.anything(UnknownSeed::ContentsOfEscapedObject);
}

TypeID tryTypeOfArrayElementRead(const TypingContext& context, TypeID array, ArrayStorage storage, bool admitsInt32Storage, bool findsElement, bool readsHoleAsNaN)
{
    TypeID element = tryTypeOfContentsRead(context, array, ContentsSlot::Element);
    if (element && !findsElement)
        element = context.types.withPrimitives(element, Primitive::Undefined);
    if (!element)
        element = context.positions.resultOfReadBeingTyped(context.level);

    TypeID stored = typeOfElementRead(storage);
    if (context.at(stored).isAnything())
        return element;
    if (findsElement)
        stored = context.types.withoutPrimitives(stored, Primitive::Undefined);
    TypeID result = element ? context.types.intersectionOf(element, stored) : stored;
    if (context.level == TypeLevel::Proven && (storage == ArrayStorage::Int32 || admitsInt32Storage))
        result = context.types.unionOf(result, TypeTable::int32);
    bool mayBeFraction = context.at(result).holdsFractionsWithoutBounds();
    if (!readsHoleAsNaN)
        return mayBeFraction && isNumericStorage(storage) ? context.types.withPrimitives(result, Primitive::Number) : result;
    return context.types.withPrimitives(result, mayBeFraction ? Primitive::Number : Primitive::WholeNumber);
}

static bool mayHoldInstancesOfClassesBelowPromise(const TypingContext& context, const Type& type)
{
    return std::ranges::any_of(membersOf(context.types, type), [&](LayoutID layout) {
        BuiltinClassID above = builtinClassWrittenAbove(context.types, layout);
        return above && context.types.builtins().isSameOrSubclassOf(above, BuiltinClasses::Promise);
    });
}

static TypeID tryExpectedTypeOfAwaitedObjects(const TypingContext& context, TypeID awaitedID, const Type& awaited)
{
    if (awaited.anyObject || mayHoldInstancesOfClassesBelowPromise(context, awaited))
        return { };

    const ObjectTable& objects = context.types.objects();
    TypeID fulfillment = TypeTable::never;
    bool holdsPromises = false;
    auto addFulfillment = [&](TypeID value) {
        holdsPromises = true;
        if (value)
            fulfillment = context.types.unionOf(fulfillment, value);
        return !!value;
    };
    bool namesNoPromise = context.types.at(awaited.objects).allOf([&](ObjectID object) {
        return objects.classOf(object) != BuiltinClasses::Promise;
    });
    if (!namesNoPromise)
        return { };
    for (const ClassPart& part : context.types.at(awaited.unnamedClasses)) {
        if (part.builtinClass != BuiltinClasses::Promise)
            continue;
        if (!addFulfillment(tryWrittenContentsOfClassPart(context.types.builtins(), part, ContentsSlot::Value)))
            return { };
    }
    if (!holdsPromises)
        return awaitedID;

    TypeID others = context.types.differenceOf(awaitedID, context.types.builtinClass(BuiltinClasses::Promise));
    return context.types.unionOf(others, fulfillment);
}

TypeID tryTypeOfAwaited(const TypingContext& context, TypeID awaitedID)
{
    if (!awaitedID)
        return { };
    Type awaited = context.at(awaitedID);
    if (!awaited.hasObjects())
        return awaitedID;

    switch (context.level) {
    case TypeLevel::Proven:
        return { };
    case TypeLevel::Expected:
        return tryExpectedTypeOfAwaitedObjects(context, awaitedID, awaited);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

TypeID typeOfAwaited(const TypingContext& context, TypeID awaited)
{
    TypeID known = tryTypeOfAwaited(context, awaited);
    return known ? known : context.types.anything(UnknownSeed::ResultOfUnknownCallee);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
