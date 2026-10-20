/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTPropertyAccessElements.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectsOfAccesses.h"
#include "AOTPrimitiveKeyTexts.h"
#include "AOTTypeArena.h"
#include <algorithm>
#include <array>
#include <wtf/CommaPrinter.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

struct ElementOperation {
    Opcode byKey;
    Opcode byName;
};

struct NumberKey {
    KnowledgeLevel level { KnowledgeLevel::Unknown };
    bool isIntegral { false };
    bool isArrayIndex { false };
};

enum class ElementReceiverKind : uint8_t { Unknown, String, Union, Class };

struct ElementReceiver {
    ElementReceiverKind kind { ElementReceiverKind::Unknown };
    const ClassPart* part { nullptr };
    bool isPredicted { false };
};

} // anonymous namespace

static constexpr std::array<ElementOperation, 5> elementOperations { {
    { Opcode::ElemGet, Opcode::PropGet },
    { Opcode::ElemSet, Opcode::PropSet },
    { Opcode::ElemDefine, Opcode::PropDefine },
    { Opcode::ElemHas, Opcode::PropHas },
    { Opcode::ElemDelete, Opcode::PropDelete },
} };

static_assert(accessOf(Opcode::PropDelete) == Access::None);

static const ElementOperation* findElementOperation(Opcode byKey)
{
    for (const ElementOperation& operation : elementOperations) {
        if (operation.byKey == byKey)
            return &operation;
    }
    return nullptr;
}

std::optional<Access> elementAccessOf(Opcode opcode)
{
    const ElementOperation* operation = findElementOperation(opcode);
    if (!operation || accessOf(operation->byName) == Access::None)
        return std::nullopt;
    return accessOf(operation->byName);
}

Opcode namedOperationFor(Opcode elementOperation)
{
    const ElementOperation* operation = findElementOperation(elementOperation);
    RELEASE_ASSERT(operation);
    return operation->byName;
}

static Opcode namedOperationWith(Access access)
{
    for (const ElementOperation& operation : elementOperations) {
        if (accessOf(operation.byName) == access)
            return operation.byName;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Opcode> elementOperationFor(ElementAccessForm form, Access access)
{
    switch (form) {
    case ElementAccessForm::Array:
        switch (access) {
        case Access::Read:
            return Opcode::GetArrayElement;
        case Access::Store:
        case Access::Define:
            return Opcode::PutArrayElement;
        case Access::Has:
            return Opcode::HasArrayElement;
        case Access::None:
            break;
        }
        break;
    case ElementAccessForm::TypedArray:
        switch (access) {
        case Access::Read:
            return Opcode::GetTypedArrayElement;
        case Access::Store:
            return Opcode::PutTypedArrayElement;
        case Access::Define:
        case Access::Has:
            return std::nullopt;
        case Access::None:
            break;
        }
        break;
    case ElementAccessForm::ByKey:
    case ElementAccessForm::NamedKey:
    case ElementAccessForm::String:
    case ElementAccessForm::Split:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

ConstantKey constantKeyOf(const Program& program, const Type& key)
{
    if (StringID text = key.singleStringLiteral()) {
        if (mayBeElementKey(program.nameClass(text)))
            return { };
        return { ConstantKeyKind::Text, text, { } };
    }
    if (key.hasOnlyPrimitives(Primitive::LiteralSymbol) && key.symbolLiterals.size() == 1)
        return { ConstantKeyKind::Symbol, { }, key.symbolLiterals[0] };
    for (auto [primitive, text] : primitiveKeyTexts) {
        if (key.isNever() || !key.hasOnlyPrimitives(primitive))
            continue;
        if (StringID string = program.strings.find(byteCast<uint8_t>(text.span())))
            return { ConstantKeyKind::Text, string, { } };
    }
    return { };
}

PropertyKeyID internKey(TypeArena& types, const ConstantKey& key)
{
    switch (key.kind) {
    case ConstantKeyKind::None:
        return { };
    case ConstantKeyKind::Text:
        return types.textKey(key.text);
    case ConstantKeyKind::Symbol:
        return key.symbol;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void becomeNamedAccess(Value& access, PropertyKeyID key)
{
    Opcode namedOperation = namedOperationFor(access.opcode);
    ASSERT(!OperandPosition::value(access.opcode) == !OperandPosition::value(namedOperation));
    ASSERT(!PayloadWord::mode(access.opcode) == !PayloadWord::mode(namedOperation));

    ValueID object = access.objectOperand();
    ValueID stored = OperandPosition::value(access.opcode) ? access.valueOperand() : ValueID();
    std::optional<ECMAMode> mode;
    if (PayloadWord::mode(access.opcode))
        mode = access.mode();
    AccessSiteID accessSite = access.accessSite();
    LayoutSetID receivers = access.receivers();

    access.become(namedOperation);
    access.setObjectOperand(object);
    if (stored)
        access.setValueOperand(stored);
    if (mode)
        access.setMode(*mode);
    access.setKey(key);
    access.setAccessSite(accessSite);
    access.setReceivers(receivers);
}

static ElementAccessPlan planWithoutElementOperation(const ElementAccessQuery& query, ElementAccessForm form, KnowledgeLevel level)
{
    ElementAccessPlan plan;
    plan.form = form;
    plan.level = level;
    plan.keyIsIntegral = query.key.isNumberWithoutFraction();
    return plan;
}

static ElementAccessPlan planByKey(const ElementAccessQuery& query, UnprovenCause cause = UnprovenCause::Unknown)
{
    ElementAccessPlan plan = planWithoutElementOperation(query, ElementAccessForm::ByKey, KnowledgeLevel::Unknown);
    plan.cause = cause;
    return plan;
}

static ElementAccessPlan planNamedKey(ConstantKey constantKey, Opcode operation)
{
    ElementAccessPlan plan;
    plan.form = ElementAccessForm::NamedKey;
    plan.level = KnowledgeLevel::Proven;
    plan.operation = operation;
    plan.constantKey = constantKey;
    return plan;
}

static ElementAccessPlan finishElementPlan(ElementAccessPlan plan, ElementAccessForm form, Opcode operation)
{
    plan.form = form;
    plan.operation = operation;
    plan.level = plan.needsNoTest() ? KnowledgeLevel::Proven : KnowledgeLevel::Predicted;
    return plan;
}

TypeID testedNumberTypeContaining(const Type& predictedNumber)
{
    RELEASE_ASSERT(predictedNumber.isNumber());
    return predictedNumber.isInt32() ? TypeTable::int32 : TypeTable::number;
}

static Type numbersThatPassTest(const Type& value, const Type& predictedNumber)
{
    Type numbers = value.withoutObjects().withoutPrimitives(static_cast<uint16_t>(~Primitive::Number));
    if (testedNumberTypeContaining(predictedNumber) == TypeTable::number)
        return numbers;
    return Type::integer(numbers.integers.intersectWith(Range::int32()));
}

static NumberKey numberKeyOf(const Program& program, const ElementAccessQuery& query)
{
    if (query.key.isNumber())
        return { KnowledgeLevel::Proven, query.key.isNumberWithoutFraction(), isArrayIndex(query.key) };
    if (!query.keyPrediction)
        return { };

    Type predicted = query.keyPrediction->withoutNullish();
    if (!predicted.isNumber() || !query.key.overlaps(predicted, program))
        return { };
    Type tested = numbersThatPassTest(query.key, predicted);
    return { KnowledgeLevel::Predicted, tested.isNumberWithoutFraction(), isArrayIndex(tested) };
}

static bool mayHaveElementForm(UnionPartKind kind)
{
    switch (kind) {
    case UnionPartKind::String:
    case UnionPartKind::BuiltinClass:
        return true;
    case UnionPartKind::OtherObjects:
    case UnionPartKind::Nullish:
    case UnionPartKind::Boolean:
    case UnionPartKind::Number:
    case UnionPartKind::Symbol:
    case UnionPartKind::BigInt:
    case UnionPartKind::Absent:
    case UnionPartKind::Layouts:
    case UnionPartKind::Functions:
    case UnionPartKind::Builtins:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool canSplitReceiver(const Type& receiver)
{
    if (receiver.anyObject)
        return false;

    UnionParts parts = unionPartsOf(receiver);
    return parts.size() > 1 && isClosedUnion(parts) && std::ranges::any_of(parts, [](const UnionPart& part) {
        return mayHaveElementForm(part.kind);
    });
}

static TypedArrayType typedArrayTypeOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    BuiltinClassID typedArrayClass = builtins.typedArrayClassOf(builtinClass);
    return typedArrayClass ? builtins.at(typedArrayClass).typedArrayType : NotTypedArray;
}

// What flow could not follow is in a prediction with every primitive and with the objects made of them, beside the array that it did see.
static const ClassPart* findOnlyClassWithElements(const BuiltinTable& builtins, const Type& prediction)
{
    const ClassPart* found = nullptr;
    for (const ClassPart& part : prediction.classes) {
        if (part.builtinClass != BuiltinClasses::Array && !isTypedView(typedArrayTypeOf(builtins, part.builtinClass)))
            continue;
        if (found)
            return nullptr;
        found = &part;
    }
    return found;
}

static ElementReceiver elementReceiverOf(const Program& program, const ElementAccessQuery& query)
{
    if (const ClassPart* part = query.receiver.singleBuiltinClass())
        return { ElementReceiverKind::Class, part, false };
    if (query.receiver.isAnyString())
        return { ElementReceiverKind::String };
    if (canSplitReceiver(query.receiver))
        return { ElementReceiverKind::Union };
    if (!query.receiverPrediction)
        return { };

    const ClassPart* predictedClass = findOnlyClassWithElements(program.builtins(), *query.receiverPrediction);
    if (predictedClass && (query.receiver.anyObject || query.receiver.findClassPart(predictedClass->builtinClass)))
        return { ElementReceiverKind::Class, predictedClass, true };

    Type predicted = query.receiverPrediction->withoutNullish();
    if (predicted.isAnyString() && query.receiver.overlaps(predicted, program))
        return { ElementReceiverKind::String, nullptr, true };
    return { };
}

static ElementAccessPlan planCharacterRead(const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver)
{
    if (!receiver.isPredicted && key.level == KnowledgeLevel::Proven)
        return planWithoutElementOperation(query, ElementAccessForm::String, KnowledgeLevel::Proven);

    ElementAccessPlan plan = planWithoutElementOperation(query, ElementAccessForm::ByKey, KnowledgeLevel::Predicted);
    plan.testsReceiverIsString = receiver.isPredicted;
    plan.testsKeyIsNumber = key.level != KnowledgeLevel::Proven;
    return plan;
}

// An array of numbers has one of three kinds by the numbers it has met. An array that is said to hold none is contiguous whoever made it.
static ArrayStorage storageOfElementsWithoutNumbers(const Program& program, const ClassPart& part)
{
    TypeID contents = part.contentsAt(program.builtins(), ContentsSlot::Element);
    if (!contents)
        return ArrayStorage::Other;
    const Type& elements = program.at(contents);
    if (elements.isAnything() || elements.primitives & Primitive::Number)
        return ArrayStorage::Other;
    return arrayStorageHolding(elements);
}

static ArrayStorage expectedArrayStorageOf(const Program& program, const ElementAccessQuery& query, const ClassPart& part)
{
    if (part.arrayStorage != ArrayStorage::Other)
        return part.arrayStorage;
    const ClassPart* predicted = query.receiverPrediction ? query.receiverPrediction->findClassPart(part.builtinClass) : nullptr;
    if (predicted && predicted->arrayStorage != ArrayStorage::Other)
        return predicted->arrayStorage;
    ArrayStorage storage = storageOfElementsWithoutNumbers(program, part);
    if (storage == ArrayStorage::Other && predicted)
        storage = storageOfElementsWithoutNumbers(program, *predicted);
    return storage;
}

static ElementAccessPlan planArrayAccess(const Program& program, const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver, ElementAccessPlan plan)
{
    const ClassPart& part = *receiver.part;
    plan.requiredAspects = PristineAspects::Storage;
    // -1 and 1.5 name properties, and an accessor on the array or on its prototypes may answer to such a name.
    if (!key.isArrayIndex)
        plan.requiredAspects.add(PristineAspects::Names);
    if (!receiver.isPredicted)
        plan.requiredAspects = plan.requiredAspects - part.provenPristine;

    ArrayStorage expected = expectedArrayStorageOf(program, query, part);
    if (hasArrayStorageForm(expected)) {
        bool isProven = !receiver.isPredicted && part.contentsAreProven && part.arrayStorage == expected;
        plan.arrayStorage = expected;
        plan.testsArrayStorage = !isProven;
    }
    return finishElementPlan(plan, ElementAccessForm::Array, *elementOperationFor(ElementAccessForm::Array, query.access));
}

static bool fitsContentType(const Type& stored, TypedArrayContentType contentType)
{
    switch (contentType) {
    case TypedArrayContentType::Number:
        return stored.isNumber();
    case TypedArrayContentType::BigInt:
        return !stored.isNever() && stored.hasOnlyPrimitives(Primitive::BigInt);
    case TypedArrayContentType::None:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// The conversion of any other value can run code.
static KnowledgeLevel storedFitOf(const Program& program, const ElementAccessQuery& query, TypedArrayContentType contentType)
{
    if (!query.stored)
        return KnowledgeLevel::Unknown;
    if (fitsContentType(*query.stored, contentType))
        return KnowledgeLevel::Proven;
    if (!query.storedPrediction)
        return KnowledgeLevel::Unknown;

    Type predicted = query.storedPrediction->withoutNullish();
    if (!fitsContentType(predicted, contentType) || !query.stored->overlaps(predicted, program))
        return KnowledgeLevel::Unknown;
    return KnowledgeLevel::Predicted;
}

static ElementAccessPlan planTypedArrayAccess(const Program& program, const ElementAccessQuery& query, TypedArrayType typedArrayType, ElementAccessPlan plan)
{
    std::optional<Opcode> operation = elementOperationFor(ElementAccessForm::TypedArray, query.access);
    if (!operation)
        return planByKey(query);
    if (query.access == Access::Store) {
        KnowledgeLevel storedFit = storedFitOf(program, query, contentType(typedArrayType));
        if (storedFit == KnowledgeLevel::Unknown)
            return planByKey(query);
        plan.testsStoredValue = storedFit == KnowledgeLevel::Predicted;
    }

    plan.typedArrayType = typedArrayType;
    return finishElementPlan(plan, ElementAccessForm::TypedArray, *operation);
}

static ElementAccessPlan planClassAccess(const Program& program, const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver)
{
    ElementAccessPlan plan;
    plan.builtinClass = receiver.part->builtinClass;
    plan.keyIsIntegral = key.isIntegral;
    plan.testsKeyIsNumber = key.level != KnowledgeLevel::Proven;
    plan.testsReceiverClass = receiver.isPredicted;

    if (plan.builtinClass == BuiltinClasses::Array)
        return planArrayAccess(program, query, key, receiver, plan);

    TypedArrayType typedArrayType = typedArrayTypeOf(program.builtins(), plan.builtinClass);
    if (isTypedView(typedArrayType))
        return planTypedArrayAccess(program, query, typedArrayType, plan);
    return planByKey(query);
}

ElementAccessPlan planElementAccess(const Program& program, const ElementAccessQuery& query)
{
    RELEASE_ASSERT(query.access != Access::None);
    if (ConstantKey constantKey = constantKeyOf(program, query.key))
        return planNamedKey(constantKey, namedOperationWith(query.access));

    NumberKey key = numberKeyOf(program, query);
    if (key.level == KnowledgeLevel::Unknown)
        return planByKey(query);

    ElementReceiver receiver = elementReceiverOf(program, query);
    // The test of a predicted receiver fails for undefined and null.
    if (query.receiver.mayBeUndefinedOrNull() && !receiver.isPredicted)
        return planByKey(query);

    switch (receiver.kind) {
    case ElementReceiverKind::Unknown:
        return planByKey(query, query.receiver.anyObject ? UnprovenCause::UnknownObject : UnprovenCause::Unknown);
    case ElementReceiverKind::String:
        if (query.access != Access::Read)
            return planByKey(query);
        return planCharacterRead(query, key, receiver);
    case ElementReceiverKind::Union:
        return planWithoutElementOperation(query, ElementAccessForm::Split, KnowledgeLevel::Proven);
    case ElementReceiverKind::Class:
        return planClassAccess(program, query, key, receiver);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static const Type* findPredictedType(const Program& program, const Value& value)
{
    return value.prediction() ? &program.at(value.prediction()) : nullptr;
}

std::optional<ElementAccessPlan> planElementAccessOf(const Program& program, const Function& function, const Value& access)
{
    if (!findElementOperation(access.opcode))
        return std::nullopt;

    const Value& key = function.at(access.propertyOperand());
    std::optional<Access> kind = elementAccessOf(access.opcode);
    if (!kind) {
        const Type& keyType = program.at(key.type);
        if (ConstantKey constantKey = constantKeyOf(program, keyType))
            return planNamedKey(constantKey, namedOperationFor(access.opcode));
        ElementAccessPlan byKey;
        byKey.keyIsIntegral = keyType.isNumberWithoutFraction();
        return byKey;
    }

    const Value& receiver = function.at(access.objectOperand());
    bool hasStoredValue = *kind == Access::Store || *kind == Access::Define;
    return planElementAccess(program, {
        .access = *kind,
        .receiver = program.at(receiver.type),
        .receiverPrediction = findPredictedType(program, receiver),
        .key = program.at(key.type),
        .keyPrediction = findPredictedType(program, key),
        .stored = hasStoredValue ? &program.typeOf(function, access.valueOperand()) : nullptr,
        .storedPrediction = hasStoredValue ? findPredictedType(program, function.at(access.valueOperand())) : nullptr,
    });
}

static ASCIILiteral nameOf(ElementAccessForm form)
{
    switch (form) {
    case ElementAccessForm::ByKey:
        return "ByKey"_s;
    case ElementAccessForm::NamedKey:
        return "NamedKey"_s;
    case ElementAccessForm::Array:
        return "Array"_s;
    case ElementAccessForm::TypedArray:
        return "TypedArray"_s;
    case ElementAccessForm::String:
        return "String"_s;
    case ElementAccessForm::Split:
        return "Split"_s;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void dumpTests(PrintStream& out, const ElementAccessPlan& plan)
{
    out.print(" tests=[");
    CommaPrinter comma;
    if (plan.testsReceiverClass)
        out.print(comma, "class");
    if (plan.testsReceiverIsString)
        out.print(comma, "string");
    if (plan.testsKeyIsNumber)
        out.print(comma, "key");
    if (plan.testsStoredValue)
        out.print(comma, "stored");
    if (!plan.requiredAspects.isEmpty()) {
        out.print(comma, "pristine(");
        CommaPrinter plus("+"_s);
        for (PristineAspects aspect : allMembersOf<PristineAspects>) {
            if (plan.requiredAspects.contains(aspect))
                out.print(plus, nameOf(aspect));
        }
        out.print(")");
    }
    if (plan.testsArrayStorage)
        out.print(comma, "storage");
    if (plan.testsInBounds)
        out.print(comma, "bounds");
    out.print("]");
}

void ElementAccessPlan::dump(PrintStream& out) const
{
    out.print("form=", nameOf(form), " level=", nameOf(level));
    if (isResolved())
        out.print(" operation=", nameOf(operation));
    if (builtinClass)
        out.print(" class=C", builtinClass.index());
    if (form == ElementAccessForm::Array)
        out.print(" storage=", nameOf(arrayStorage));
    if (form == ElementAccessForm::TypedArray)
        out.print(" typedArray=", typedArrayType);
    if (keyIsIntegral)
        out.print(" integralKey");
    if (!needsNoTest())
        dumpTests(out, *this);
    if (cause != UnprovenCause::Unknown)
        out.print(" cause=", nameOf(cause));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
