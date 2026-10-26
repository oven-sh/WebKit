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
#include <wtf/EnumTraits.h>

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
    bool isExpected { false };
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

static Opcode arrayOperationFor(Access access)
{
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
    return { .form = form, .level = level, .keyIsIntegral = query.key.isNumberWithoutFraction() };
}

static ElementAccessPlan planByKey(const ElementAccessQuery& query, UnprovenCause cause = UnprovenCause::Unknown)
{
    ElementAccessPlan plan = planWithoutElementOperation(query, ElementAccessForm::ByKey, KnowledgeLevel::Unknown);
    plan.cause = cause;
    return plan;
}

static ElementAccessPlan planNamedKey(ConstantKey constantKey, Opcode operation)
{
    return { .form = ElementAccessForm::NamedKey, .level = KnowledgeLevel::Proven, .operation = operation, .constantKey = constantKey };
}

static ElementAccessPlan finishElementPlan(ElementAccessPlan plan, ElementAccessForm form, Opcode operation)
{
    plan.form = form;
    plan.operation = operation;
    plan.level = plan.needsNoTest() ? KnowledgeLevel::Proven : KnowledgeLevel::Expected;
    return plan;
}

TypeID testedNumberTypeContaining(const Type& expectedNumber)
{
    RELEASE_ASSERT(expectedNumber.isNumber());
    return expectedNumber.isInt32() ? TypeTable::int32 : TypeTable::number;
}

static TypedArrayType typedArrayTypeOf(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    BuiltinClassID typedArrayClass = builtins.typedArrayClassOf(builtinClass);
    return typedArrayClass ? builtins.at(typedArrayClass).typedArrayType : NotTypedArray;
}

static bool hasIndexedElements(const BuiltinTable& builtins, BuiltinClassID builtinClass)
{
    return builtinClass == BuiltinClasses::Array || isTypedView(typedArrayTypeOf(builtins, builtinClass));
}

static NumberKey testedNumberKey(const Type& key, const Type& expectedNumber)
{
    Type tested = key.withoutObjects().withoutPrimitives(static_cast<uint16_t>(~Primitive::Number));
    if (testedNumberTypeContaining(expectedNumber) != TypeTable::number)
        tested = Type::integer(tested.integers.intersectWith(Range::int32()));
    return { KnowledgeLevel::Expected, tested.isNumberWithoutFraction(), isArrayIndex(tested) };
}

static NumberKey expectedIndexKey(const Program& program, const ElementAccessQuery& query)
{
    const ClassPart* part = query.receiver.singleBuiltinClass();
    if (!part || !hasIndexedElements(program.builtins(), part->builtinClass))
        return { };

    const Type& testedType = program.at(TypeTable::int32);
    if (!query.key.overlaps(testedType, program) || isClosedUnion(unionPartsOf(query.key)))
        return { };
    return testedNumberKey(query.key, testedType);
}

static NumberKey numberKeyOf(const Program& program, const ElementAccessQuery& query)
{
    if (query.key.isNumber())
        return { KnowledgeLevel::Proven, query.key.isNumberWithoutFraction(), isArrayIndex(query.key) };
    if (!query.keyExpected)
        return expectedIndexKey(program, query);

    Type expected = query.keyExpected->withoutNullish();
    if (!expected.isNumber() || !query.key.overlaps(expected, program))
        return { };
    return testedNumberKey(query.key, expected);
}

static bool canSplitReceiver(const Type& receiver)
{
    if (receiver.anyObject)
        return false;

    UnionParts parts = unionPartsOf(receiver);
    return parts.size() > 1 && isClosedUnion(parts) && std::ranges::any_of(parts, [](const UnionPart& part) {
        return part.kind == UnionPartKind::String || part.kind == UnionPartKind::BuiltinClass;
    });
}

static const ClassPart* findOnlyClassWithElements(const BuiltinTable& builtins, const Type& expected)
{
    const ClassPart* found = nullptr;
    for (const ClassPart& part : expected.classes) {
        if (!hasIndexedElements(builtins, part.builtinClass))
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
    if (!query.receiverExpected)
        return { };

    const ClassPart* expectedClass = findOnlyClassWithElements(program.builtins(), *query.receiverExpected);
    if (expectedClass && (query.receiver.anyObject || query.receiver.findClassPart(expectedClass->builtinClass)))
        return { ElementReceiverKind::Class, expectedClass, true };

    Type expected = query.receiverExpected->withoutNullish();
    if (expected.isAnyString() && query.receiver.overlaps(expected, program))
        return { ElementReceiverKind::String, nullptr, true };
    return { };
}

static ElementAccessPlan planCharacterRead(const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver)
{
    if (!receiver.isExpected && key.level == KnowledgeLevel::Proven)
        return planWithoutElementOperation(query, ElementAccessForm::String, KnowledgeLevel::Proven);

    ElementAccessPlan plan = planWithoutElementOperation(query, ElementAccessForm::ByKey, KnowledgeLevel::Expected);
    plan.testsReceiverIsString = receiver.isExpected;
    plan.testsKeyIsNumber = key.level != KnowledgeLevel::Proven;
    return plan;
}

static ArrayStorage expectedArrayStorageOf(const Program& program, const ElementAccessQuery& query, const ClassPart& part)
{
    if (part.arrayStorage != ArrayStorage::Other)
        return part.arrayStorage;
    const ClassPart* expected = query.receiverExpected ? query.receiverExpected->findClassPart(part.builtinClass) : nullptr;
    if (expected && expected->arrayStorage != ArrayStorage::Other)
        return expected->arrayStorage;
    ArrayStorage storage = arrayStorageDeclaredBy(program, part);
    if (storage == ArrayStorage::Other && expected)
        storage = arrayStorageDeclaredBy(program, *expected);
    return storage;
}

static ElementAccessPlan planArrayAccess(const Program& program, const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver, ElementAccessPlan plan)
{
    const ClassPart& part = *receiver.part;
    plan.requiredAspects = PristineAspects::Storage;
    if (!key.isArrayIndex)
        plan.requiredAspects.add(PristineAspects::Names);
    if (!receiver.isExpected)
        plan.requiredAspects = plan.requiredAspects - part.provenPristine;

    ArrayStorage expected = expectedArrayStorageOf(program, query, part);
    if (hasArrayStorageForm(expected)) {
        bool isProven = !receiver.isExpected && part.contentsAreProven && part.arrayStorage == expected;
        plan.arrayStorage = expected;
        plan.testsArrayStorage = !isProven;
    }
    return finishElementPlan(plan, ElementAccessForm::Array, arrayOperationFor(query.access));
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

static KnowledgeLevel storedFitOf(const Program& program, const ElementAccessQuery& query, TypedArrayContentType contentType)
{
    if (!query.stored)
        return KnowledgeLevel::Unknown;
    if (fitsContentType(*query.stored, contentType))
        return KnowledgeLevel::Proven;
    if (!query.storedExpected)
        return KnowledgeLevel::Unknown;

    Type expected = query.storedExpected->withoutNullish();
    if (!fitsContentType(expected, contentType) || !query.stored->overlaps(expected, program))
        return KnowledgeLevel::Unknown;
    return KnowledgeLevel::Expected;
}

static ElementAccessPlan planTypedArrayAccess(const Program& program, const ElementAccessQuery& query, TypedArrayType typedArrayType, ElementAccessPlan plan)
{
    plan.typedArrayType = typedArrayType;
    switch (query.access) {
    case Access::Read:
        return finishElementPlan(plan, ElementAccessForm::TypedArray, Opcode::GetTypedArrayElement);
    case Access::Store: {
        KnowledgeLevel storedFit = storedFitOf(program, query, contentType(typedArrayType));
        if (storedFit == KnowledgeLevel::Unknown)
            return planByKey(query);
        plan.testsStoredValue = storedFit == KnowledgeLevel::Expected;
        return finishElementPlan(plan, ElementAccessForm::TypedArray, Opcode::PutTypedArrayElement);
    }
    case Access::Define:
    case Access::Has:
        return planByKey(query);
    case Access::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ElementAccessPlan planClassAccess(const Program& program, const ElementAccessQuery& query, NumberKey key, const ElementReceiver& receiver)
{
    ElementAccessPlan plan;
    plan.builtinClass = receiver.part->builtinClass;
    plan.keyIsIntegral = key.isIntegral;
    plan.testsKeyIsNumber = key.level != KnowledgeLevel::Proven;
    plan.testsReceiverClass = receiver.isExpected;
    plan.checksNullish = !receiver.isExpected && query.receiver.mayBeUndefinedOrNull();

    if (plan.builtinClass == BuiltinClasses::Array)
        return planArrayAccess(program, query, key, receiver, plan);

    TypedArrayType typedArrayType = typedArrayTypeOf(program.builtins(), plan.builtinClass);
    if (isTypedView(typedArrayType))
        return planTypedArrayAccess(program, query, typedArrayType, plan);
    if (plan.builtinClass == BuiltinClasses::Arguments && query.access == Access::Read && plan.needsNoTest())
        return planWithoutElementOperation(query, ElementAccessForm::ArgumentsElement, KnowledgeLevel::Proven);
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
    if (query.receiver.mayBeUndefinedOrNull() && !receiver.isExpected && receiver.kind != ElementReceiverKind::Class)
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

static const Type* findExpectedType(const Program& program, const Value& value)
{
    return value.expected().predicted() ? &program.at(value.expected().predicted()) : nullptr;
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
        return ElementAccessPlan { .keyIsIntegral = keyType.isNumberWithoutFraction() };
    }

    const Value& receiver = function.at(access.objectOperand());
    bool hasStoredValue = *kind == Access::Store || *kind == Access::Define;
    return planElementAccess(program, {
        .access = *kind,
        .receiver = program.at(receiver.type),
        .receiverExpected = findExpectedType(program, receiver),
        .key = program.at(key.type),
        .keyExpected = findExpectedType(program, key),
        .stored = hasStoredValue ? &program.typeOf(function, access.valueOperand()) : nullptr,
        .storedExpected = hasStoredValue ? findExpectedType(program, function.at(access.valueOperand())) : nullptr,
    });
}

void ElementAccessPlan::dump(PrintStream& out) const
{
    out.print("form=", enumName(form), " level=", nameOf(level));
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
    if (isPresent)
        out.print(" present");
    if (readsHoleAsNaN)
        out.print(" holeIsNaN");
    if (isInsideView)
        out.print(" insideView");
    if (!needsNoTest()) {
        out.print(" tests=[");
        CommaPrinter comma;
        if (testsReceiverClass)
            out.print(comma, "class");
        if (checksNullish)
            out.print(comma, "nullish");
        if (testsReceiverIsString)
            out.print(comma, "string");
        if (testsKeyIsNumber)
            out.print(comma, "key");
        if (testsStoredValue)
            out.print(comma, "stored");
        if (!requiredAspects.isEmpty()) {
            out.print(comma, "pristine(");
            CommaPrinter plus("+"_s);
            for (PristineAspects aspect : allMembersOf<PristineAspects>) {
                if (requiredAspects.contains(aspect))
                    out.print(plus, nameOf(aspect));
            }
            out.print(")");
        }
        if (testsArrayStorage)
            out.print(comma, "storage");
        if (testsInBounds)
            out.print(comma, "bounds");
        if (testsPresence)
            out.print(comma, "presence");
        out.print("]");
    }
    if (cause != UnprovenCause::Unknown)
        out.print(" cause=", nameOf(cause));
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
