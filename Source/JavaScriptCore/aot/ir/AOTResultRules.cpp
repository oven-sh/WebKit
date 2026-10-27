/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTResultRules.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBuiltinCalls.h"
#include "AOTBuiltinPristine.h"
#include "AOTBuiltinTable.h"
#include "AOTBuiltinTypes.h"
#include "AOTFieldValueNarrowing.h"
#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include "AOTTypeRelations.h"
#include "AOTVector.h"
#include <algorithm>
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

static const Function& recordOf(const Program& program, FunctionID function)
{
    RELEASE_ASSERT(!program.isBeingReplaced(function));
    return program.at(function);
}

static constexpr OpcodeSet arrayElementReads { Opcode::GetArrayElement, Opcode::GetContiguousIndex };

static Type typeOfKeyConstant(const TypingContext& context, PropertyKeyID id)
{
    const PropertyKey& key = context.types.at(id);
    switch (key.kind) {
    case PropertyKeyKind::Text:
        return Type::stringLiteral(key.text);
    case PropertyKeyKind::WellKnownSymbol:
    case PropertyKeyKind::CreatedSymbol:
    case PropertyKeyKind::RegisteredSymbol:
        return Type::symbolLiteral(id);
    case PropertyKeyKind::EnginePrivateName:
    case PropertyKeyKind::ModulePrivateName:
    case PropertyKeyKind::PrivateName:
        return Type::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfBuiltinEntry(const TypingContext& context, BuiltinID builtin)
{
    const BuiltinTable& builtins = context.program.builtins();
    if (compiledModuleWithObject(context.program, builtins.canonicalID(builtin)))
        return Type::object();
    if (builtins.at(builtin).kind != BuiltinKind::DataProperty)
        return typeOfBuiltinConstant(builtins, builtin);

    Type result = Type::never();
    for (const BuiltinOverload& overload : builtins.overloadsOf(builtin)) {
        if (overload.mode == BuiltinCallMode::Get)
            result = result.unionWith(typeOfBuiltinType({ context }, overload.result.type), context.types);
    }
    return result.isNever() ? Type::anything() : result;
}

Type typeOfConstant(const TypingContext& context, const Value& value)
{
    switch (value.opcode) {
    case Opcode::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case Opcode::Null:
        return Type::fromPrimitives(Primitive::Null);
    case Opcode::Absent:
        return Type::fromPrimitives(Primitive::Absent);
    case Opcode::Boolean:
        return Type::fromPrimitives(value.boolean() ? Primitive::True : Primitive::False);
    case Opcode::Integer:
        if (!Range::full().contains(value.integer()))
            return Type::fromPrimitives(Primitive::WholeNumber);
        return Type::integer(Range::one(value.integer()));
    case Opcode::Double:
        return Type::number(value.number());
    case Opcode::String:
        return Type::stringLiteral(value.string());
    case Opcode::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case Opcode::Builtin:
    case Opcode::ModuleBuiltin:
        return typeOfBuiltinEntry(context, value.builtin());
    case Opcode::Data:
        return typeOfData(context, context.program.at(value.data()).root);
    case Opcode::NewClosure:
        return Type::forFunction(value.function());
    case Opcode::KeyConstant:
        return typeOfKeyConstant(context, value.key());
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type typeOfData(const TypingContext& context, const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
        return Type::fromPrimitives(Primitive::Undefined);
    case DataNode::Kind::Null:
        return Type::fromPrimitives(Primitive::Null);
    case DataNode::Kind::Hole:
        return Type::fromPrimitives(Primitive::Absent);
    case DataNode::Kind::Boolean:
        return Type::fromPrimitives(node.as<DataNode::Boolean>().value ? Primitive::True : Primitive::False);
    case DataNode::Kind::Number:
        return Type::number(node.as<DataNode::Number>().value);
    case DataNode::Kind::String:
        return Type::stringLiteral(node.as<DataNode::String>().value);
    case DataNode::Kind::BigInt:
        return Type::fromPrimitives(Primitive::BigInt);
    case DataNode::Kind::Array:
        return Type::builtinClass(BuiltinClasses::Array, singleElementSpan(node.as<DataNode::Array>().contents));
    case DataNode::Kind::Object:
        if (!node.as<DataNode::Object>().layout)
            return Type::object();
        return typeOfAllocatedLayout(context, node.as<DataNode::Object>().layout);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type typeOfAllocatedLayout(const TypingContext& context, LayoutID layout)
{
    if (context.program.at(layout).isEnvironment())
        return Type::forEnvironment(layout);
    return Type::forLayout(layout, context.types);
}

bool mayAllocateOtherLayout(const Program& program, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject || !allocation.prototypeOperand())
        return false;
    return program.prototypeMaySelectOtherClass(allocation.layout());
}

Type typeOfDeclaredLayout(const TypingContext& context, LayoutID layout)
{
    if (context.level == TypeLevel::Proven)
        return Type::object();
    const Vector<LayoutID>& members = context.program.at(layout).flow.members;
    if (!members.isEmpty())
        return Type::forLayouts(members.span(), context.types);
    return Type::forLayoutSet(context.program.declaredMembersOf(layout));
}

static const DataNode* findPairMember(const DataNode& entry, size_t index)
{
    const auto* pair = entry.tryAs<DataNode::Array>();
    if (!pair || pair->elements.size() != 2)
        return nullptr;
    return &pair->elements[index];
}

static const DataNode* findEntryPart(const DataNode& entry, ContentsSlot slot)
{
    switch (slot) {
    case ContentsSlot::Element:
        return &entry;
    case ContentsSlot::Key:
        return findPairMember(entry, 0);
    case ContentsSlot::Value:
        return findPairMember(entry, 1);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfEntryParts(const TypingContext& context, const DataNode::Array& entries, ContentsSlot slot)
{
    Type result = Type::never();
    for (const DataNode& entry : entries.elements) {
        const DataNode* part = findEntryPart(entry, slot);
        if (!part)
            return Type::anything();
        result = result.unionWith(typeOfData(context, *part).withLiteralStringsWidened(), context.types);
    }
    return result;
}

static Type typeOfCollectionFromData(const TypingContext& context, BuiltinClassID builtinClass, const DataNode& root)
{
    const auto* entries = root.tryAs<DataNode::Array>();
    if (!entries)
        return Type::builtinClass(builtinClass);

    const BuiltinTable& builtins = context.program.builtins();
    Vector<TypeID, 2> contents(FillWith { }, builtins.numberOfSlotsInType(builtinClass), TypeTable::anything);
    for (unsigned indexInType = 0; indexInType < contents.size(); ++indexInType) {
        std::optional<ContentsSlot> slot = builtins.at(builtins.slotInType(builtinClass, indexInType)).contentsSlot;
        if (!slot)
            continue;
        Type parts = typeOfEntryParts(context, *entries, *slot);
        if (!parts.isNever())
            contents[indexInType] = context.types.intern(parts);
    }
    return Type::builtinClass(builtinClass, contents.span());
}

static const Type& receiverTypeOf(const TypingContext& context, const Value& access)
{
    std::optional<uint8_t> receiver = receiverOf(access.opcode);
    RELEASE_ASSERT(receiver);
    return context.typeOf(access.operand(*receiver));
}

static bool canLookUpField(const TypingContext& context, const Type& receiver)
{
    switch (context.level) {
    case TypeLevel::Proven:
        return receiver.hasOnlyLayouts(context.program);
    case TypeLevel::Expected:
        return !!receiver.layouts;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Type> tryTypeOfNamedRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    if (receiver.hasOnlyPrimitives(Primitive::UndefinedOrNull))
        return Type::never();
    if (!canLookUpField(context, receiver))
        return std::nullopt;
    return tryTypeOfFieldRead(context, receiver, key);
}

static Type typeOfSlotRead(const Value& read, std::optional<Type>&& contents)
{
    if (!contents)
        return Type::anything();
    if (read.opcode == Opcode::GetSlotOrUndefined || read.opcode == Opcode::GetDeferredClosureSlotOrUndefined)
        return contents->withPrimitives(Primitive::Undefined);
    if ((read.opcode == Opcode::GetSlot || read.opcode == Opcode::GetDeferredClosureSlot) && read.holdsDeclaredType())
        return contents->withoutNullish();
    return WTF::move(*contents);
}

static std::optional<Type> tryTypeOfContentsOfReadSlot(const TypingContext& context, const Value& read, auto receivers)
{
    if (read.opcode == Opcode::GetDeferredClosureSlot || read.opcode == Opcode::GetDeferredClosureSlotOrUndefined)
        return tryTypeOfDeferringSlotContents(context, receivers, read.key());
    if (read.opcode == Opcode::GetSlotSetter)
        return tryTypeOfSetterInSlot(context, receivers, read.key());
    return tryTypeOfSlotContents(context, receivers, read.key());
}

Type typeOfSlotRead(const TypingContext& context, const Value& read, std::span<const LayoutID> receivers)
{
    return typeOfSlotRead(read, tryTypeOfContentsOfReadSlot(context, read, receivers));
}

static Type typeOfSlotRead(const TypingContext& context, const Value& read)
{
    if (!read.receivers())
        return Type::anything();
    return typeOfSlotRead(read, tryTypeOfContentsOfReadSlot(context, read, read.receivers()));
}

static std::optional<Type> tryTypeOfAwait(const TypingContext& context, const Type& awaited)
{
    if (context.level == TypeLevel::Proven && awaited.hasObjects())
        return std::nullopt;
    return tryTypeOfAwaited(context, awaited);
}

static bool findsElement(const Value& read, const Type& array)
{
    if (read.opcode != Opcode::GetArrayElement)
        return false;
    if (read.isPresent())
        return true;
    return read.isInBounds() && (read.readsHoleAsNaN() || pristineAspectsProvenBy(array, BuiltinClasses::Array).contains(PristineAspects::Dense));
}

static std::optional<Type> tryTypeOfArrayElementRead(const TypingContext& context, const Value& read, const Type& array)
{
    bool mayFindNothing = !findsElement(read, array);
    std::optional<Type> element = tryTypeOfContentsRead(context, array, ContentsSlot::Element);
    if (element && mayFindNothing)
        element = element->withPrimitives(Primitive::Undefined);
    if (read.opcode != Opcode::GetArrayElement)
        return element;

    Type stored = typeOfElementRead(read.arrayStorage());
    if (stored.isAnything())
        return element;
    if (!mayFindNothing)
        stored = stored.withoutPrimitives(Primitive::Undefined);
    Type result = element ? element->intersectWith(stored, context.types) : WTF::move(stored);
    if (read.readsHoleAsNaN())
        result = result.withPrimitives(Primitive::WholeNumber);
    return result;
}

static ContentsSlot defaultContentsSlotOf(const BuiltinTable& builtins, const Type& container)
{
    for (const ClassPart& part : container.classes) {
        if (!builtins.findSlot(part.builtinClass, ContentsSlot::Element))
            return ContentsSlot::Value;
    }
    return ContentsSlot::Element;
}

static Type typeOfSharedRegExpLiteral(const TypingContext& context, const Value& literal)
{
    Type type = Type::regExpLiteral(literal.pattern(), literal.regExpFlags());
    if (pristineTestOf(context.program.builtins(), BuiltinClasses::RegExp) == PristineTest::MayPass)
        type.classes[0].provenPristine = { PristineAspects::Storage, PristineAspects::Names, PristineAspects::AsCreated };
    return type;
}

static bool isAssertedInt32(const Program& program, const DeclaredType& declared)
{
    return declared.has(PositionFlags::IsInt32Promise) && declared.site && program.at(declared.site).kind == TypeSiteKind::Assertion;
}

TypeID targetOfConvert(const TypingContext& context, const DeclaredType& declared)
{
    if (!isAssertedInt32(context.program, declared))
        return declared.expected().predicted();
    return context.types.withPrimitives(declared.expected().predicted(), Primitive::Number);
}

static bool hasInstanceTest(const BuiltinTable& builtins, const ClassPart& part)
{
    return builtins.at(part.builtinClass).instanceTest != BuiltinInstanceTest::None;
}

static bool hasProofs(const ClassPart& part)
{
    return part.contentsAreProven || !part.provenPristine.isEmpty() || part.arrayStorage != ArrayStorage::Other;
}

static TypeID testablePartOf(const TypingContext& context, TypeID target)
{
    const Type& type = context.types.at(target);
    bool hasMembersWithoutLayout = type.layouts && context.types.at(type.layouts).mayHoldObjectsWithoutLayout();
    if (type.classes.isEmpty() && type.builtins.isEmpty() && !hasMembersWithoutLayout)
        return target;

    const BuiltinTable& builtins = context.program.builtins();
    auto isUntestableBuiltin = [&](BuiltinID builtin) {
        return !isTestableBuiltin(builtins, builtin);
    };
    bool namesOnlyWhatTestsFind = !hasMembersWithoutLayout && type.regExpLiteralSet.isEmpty() && std::ranges::none_of(type.builtins, isUntestableBuiltin) && std::ranges::all_of(type.classes, [&](const ClassPart& part) {
        return hasInstanceTest(builtins, part) && !hasProofs(part);
    });
    if (namesOnlyWhatTestsFind)
        return target;

    Type testable = type;
    if (hasMembersWithoutLayout) {
        const Vector<LayoutID>& members = context.types.at(type.layouts).written();
        testable.layouts = members.isEmpty() ? LayoutSetID() : context.types.intern(LayoutSet::complete(Vector<LayoutID>(members)));
    }
    testable.classes.removeAllMatching([&](const ClassPart& part) {
        return !hasInstanceTest(builtins, part);
    });
    removeAllMatching(testable.builtins, isUntestableBuiltin);
    return context.types.intern(testable.withoutClassProofs());
}

static Type typeOfCheck(const TypingContext& context, const Type& source, TypeID castType)
{
    const Type& target = context.types.at(castType);
    Type passed = source.castTo(target, context.types);
    if (!passed.isNever() || source.isNever())
        return passed;
    return target.isNever() ? source : target;
}

ConvertResolution resolveConvert(const TypingContext& context, const Type& proven, const DeclaredType& declared)
{
    if (declared.isExpected())
        return { };

    TypeID targetID = targetOfConvert(context, declared);
    // Nothing tests a claim. It stays although the value has the type by now: the position or the check that gives the type may go.
    if (declared.has(PositionFlags::IsClaim))
        return { CastResolution::Assume, targetID };
    const Type& target = context.types.at(targetID);
    if (!proven.isSubsetOf(target, context.program))
        return { CastResolution::Check, testablePartOf(context, targetID) };
    Type cast = proven.castTo(target, context.types);
    if (cast == proven)
        return { };
    if (proven.anyFunction && !proven.functions.isEmpty()) {
        cast.functions = proven.functions;
        if (cast == proven)
            return { };
    }
    return { CastResolution::Upcast, targetID };
}

Type typeOfConvert(const TypingContext& context, const Type& source, const Type& proven, const DeclaredType& declared)
{
    TypeArena& types = context.types;
    if (declared.isExpected()) {
        if (context.level == TypeLevel::Proven)
            return source;
        const Type& expected = types.at(declared.expected().predicted());
        Type narrowed = source.castTo(expected, types);
        return narrowed.isNever() ? proven.castTo(expected, types) : narrowed;
    }

    TypeID target = targetOfConvert(context, declared);
    TypeID testable = testablePartOf(context, target);
    if (testable != target && proven.isSubsetOf(types.at(target), context.program))
        return source.castTo(types.at(target), types);
    return typeOfCheck(context, source, testable);
}

Type ResultRules::none(const Instruction&)
{
    return Type::fromPrimitives(Primitive::Undefined);
}

Type ResultRules::constant(const Value& value)
{
    return typeOfConstant(context, value);
}

Type ResultRules::natural(const Value& value, const NaturalType& upperBound)
{
    if (value.opcode == Opcode::SharedRegExpLiteral)
        return typeOfSharedRegExpLiteral(context, value);
    return refineFixedNatural(context, value, naturalTypeFrom(upperBound));
}

Type ResultRules::natural(const Value& value, NaturalTable table)
{
    return naturalTypeOfValue(context, value, table);
}

Type ResultRules::sameAs(const Value& value, uint32_t operandIndex)
{
    ValueID operand = value.operand(operandIndex);
    const Type& source = context.typeOf(operand);
    if (value.opcode != Opcode::Convert)
        return source;
    return typeOfConvert(context, source, context.types.at(context.function.at(operand).type), value.declared());
}

Type ResultRules::allocation(const Value& value, LayoutID layout)
{
    if (!mayAllocateOtherLayout(context.program, value))
        return typeOfAllocatedLayout(context, layout);
    if (context.level == TypeLevel::Proven)
        return Type::object();
    Vector<LayoutID, 8> layouts { layout };
    layouts.append(context.program.layoutsExpectedBelow(layout));
    return Type::forLayouts(layouts.span(), context.types);
}

Type ResultRules::allocation(const Value& value, BuiltinClassID builtinClass)
{
    if (value.opcode == Opcode::NewRegExp)
        return Type::regExpLiteral(value.pattern(), value.regExpFlags());
    if (Value::opcodesWithData.contains(value.opcode))
        return typeOfCollectionFromData(context, builtinClass, context.program.at(value.data()).root);
    return Type::builtinClass(builtinClass);
}

Type ResultRules::allocation(const Value& value, BuiltinClassID builtinClass, TypeID contents)
{
    Type type = Type::builtinClass(builtinClass, singleElementSpan(contents));
    if (value.opcode == Opcode::NewArray)
        type.classes[0].arrayStorage = value.arrayStorage();
    return type;
}

Type ResultRules::allocation(const Value&, BuiltinLayoutID layout)
{
    return typeOfAllocatedLayout(context, context.program.layoutOf(layout));
}

Type ResultRules::declared(const Value& value, VariableID variable)
{
    Type declared = context.typeOf(context.program.at(variable).declared);
    if (value.opcode != Opcode::GetEncodedModuleVariable || !(declared.primitives & Primitive::Undefined))
        return declared;
    return declared.withoutPrimitives(Primitive::Undefined).withPrimitives(Primitive::Absent);
}

Type ResultRules::declared(const Value& value, LayoutID layout, uint32_t field)
{
    const Vector<Field>& fields = context.program.at(layout).fields;
    if (field >= fields.size())
        return Type::anything();
    if (!receiverOf(value.opcode) || fields[field].flags.contains(FieldFlags::IsBrand))
        return context.typeOf(fields[field].declared);
    return provenOrUnknown(tryTypeOfNamedRead(context, receiverTypeOf(context, value), fields[field].key));
}

Type ResultRules::declared(const Value& value, PropertyKeyID key)
{
    if (familyOf(value.opcode) == OperationFamily::SlotAccess)
        return typeOfSlotRead(context, value);
    return provenOrUnknown(tryTypeOfNamedRead(context, receiverTypeOf(context, value), key));
}

static bool returnsDeclaredResultWhenCalledByValue(const Function& function)
{
    if (!canBeCalledByValueOf(function.role) || function.isUnsplitCoroutine() || function.results.size() != 1)
        return false;
    switch (function.kind) {
    case FunctionKind::Plain:
    case FunctionKind::Arrow:
    case FunctionKind::Method:
    case FunctionKind::Getter:
    case FunctionKind::Setter:
        return true;
    case FunctionKind::BaseConstructor:
    case FunctionKind::DerivedConstructor:
    case FunctionKind::Initializer:
    case FunctionKind::Module:
    case FunctionKind::CommonJsModule:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type ResultRules::declared(const Value& value, FunctionID function, uint32_t resultIndex)
{
    const Function& callee = recordOf(context.program, function);
    bool readsCapture = value.opcode == Opcode::GetCapture;
    const Vector<DeclaredType>& positions = readsCapture ? callee.captures : callee.results;
    uint32_t index = readsCapture ? value.index() : resultIndex;
    if (index >= positions.size())
        return Type::anything();
    return context.types.adopt(context.typeOf(positions[index]));
}

Type ResultRules::declared(const Value&, LayoutID layout)
{
    return typeOfDeclaredLayout(context, layout);
}

Type ResultRules::declared(const Value& value, ModuleID module)
{
    LayoutID layout = context.program.layoutOfWholeNamespace(module);
    if (!layout || value.phase() == ImportPhase::Defer)
        return Type::object();
    return Type::forLayout(layout, context.types);
}

Type ResultRules::declared(const Value&, TypeID type)
{
    return context.types.at(type);
}

Type ResultRules::declared(const Value& value, BuiltinOverloadID overload, uint32_t resultIndex)
{
    if (builtinCallOpcodes.contains(value.opcode))
        return resultTypeOfOverload(context, value, overload, resultIndex);
    return typeOfBuiltinType({ context }, context.program.builtins().at(overload).result.type);
}

Type ResultRules::declared(const Terminator& terminator, BuiltinOverloadID overload, uint32_t resultIndex)
{
    return resultTypeOfOverload(context, terminator, overload, resultIndex);
}

Type ResultRules::declaredContents(const Value& value, uint32_t operandIndex, std::optional<ContentsSlot> slot)
{
    const Type& container = context.typeOf(value.operand(operandIndex));
    if (container.isNever())
        return Type::never();
    if (value.opcode == Opcode::ElemGet)
        return provenOrUnknown(tryTypeOfElementRead(context, container, context.typeOf(value.propertyOperand())));
    if (protocolsOf(value.opcode, operandIndex).contains(Protocol::Thenable))
        return provenOrUnknown(tryTypeOfAwait(context, container));
    if (arrayElementReads.contains(value.opcode))
        return provenOrUnknown(tryTypeOfArrayElementRead(context, value, container));
    ContentsSlot readSlot = slot ? *slot : defaultContentsSlotOf(context.program.builtins(), container);
    std::optional<Type> contents = tryTypeOfContentsRead(context, container, readSlot);
    if (contents && (value.opcode == Opcode::MapNext || value.opcode == Opcode::SetNext))
        contents = contents->withPrimitives(Primitive::Undefined);
    return provenOrUnknown(WTF::move(contents));
}

Type ResultRules::declaredResult(const Value& value, uint32_t operandIndex, uint32_t index)
{
    const Value& producer = context.function.at(value.operand(operandIndex));
    if (!numberOfResultsOf(context.program, context.function, producer))
        return Type::anything();
    return computeResultType(context, producer, index);
}

Type ResultRules::declaredParameter(const Value& value)
{
    return context.types.at(context.typeIDOf(value));
}

Type ResultRules::unknown(const Instruction&)
{
    return Type::anything();
}

FunctionID onlyCalleeOf(const Program& program, const Type& callee)
{
    if (callee.functions.size() != 1)
        return { };
    FunctionID only = *callee.functions.begin();
    bool isNothingElse = !callee.primitives && !callee.anyObject && !callee.anyFunction && !callee.opaque && callee.integers == Range() && !callee.environment && !callee.layouts
        && callee.stringLiterals.isEmpty() && callee.symbolLiterals.isEmpty() && callee.classes.isEmpty() && callee.builtins.isEmpty() && callee.regExpLiteralSet.isEmpty();
    ASSERT(isNothingElse == (callee == Type::forFunction(only)));
    if (!isNothingElse)
        return { };

    return returnsDeclaredResultWhenCalledByValue(recordOf(program, only)) ? only : FunctionID();
}

Type ResultRules::unknown(const Value& value)
{
    static_assert(OperandIndex::Call::callee == OperandIndex::CallResolved::callee);
    if (value.opcode == Opcode::Construct || value.opcode == Opcode::ConstructList)
        return context.level == TypeLevel::Expected ? Type::object() : Type::anything();
    if (value.opcode != Opcode::Call && value.opcode != Opcode::CallResolved)
        return Type::anything();
    const Type& callee = context.typeOf(value.operand(OperandIndex::Call::callee));
    if (!callee.hasObjects() && callee.isJavaScriptValue())
        return Type::never();
    if (FunctionID only = onlyCalleeOf(context.program, callee))
        return declared(value, only, 0);
    return Type::anything();
}

Type ResultRules::cast(const Value& value, uint32_t operandIndex, TypeID castType)
{
    return context.typeOf(value.operand(operandIndex)).castTo(context.types.at(castType), context.types);
}

// The one place where what is expected of a value becomes what is proven of it: a check tests. An expectation that nothing tests proves nothing.
Type ResultRules::cast(const Value& value, uint32_t operandIndex, ExpectedType expected)
{
    ValueID operand = value.operand(operandIndex);
    const Type& source = context.typeOf(operand);
    if (value.opcode == Opcode::Check)
        return typeOfCheck(context, source, expected.predicted());
    RELEASE_ASSERT(value.opcode == Opcode::Expect);
    if (context.level == TypeLevel::Proven)
        return source;
    const Type& predicted = context.types.at(expected.predicted());
    Type narrowed = source.castTo(predicted, context.types);
    return narrowed.isNever() ? context.types.at(context.function.at(operand).type).castTo(predicted, context.types) : narrowed;
}

Type ResultRules::cast(const Value& value, uint32_t operandIndex)
{
    const Type& source = context.typeOf(value.operand(operandIndex));
    switch (value.opcode) {
    case Opcode::CheckInitialized:
        return source.withoutPrimitives(Primitive::Absent);
    case Opcode::RequireObjectCoercible:
        return source.withoutNullish();
    case Opcode::RequireObject:
        return source.withOnlyObjects();
    case Opcode::NarrowByField:
        return narrowHolderByFieldValue(context, source, value.key(), context.typeOf(value.readOperand()));
    default:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type ResultRules::resultOperand(const Value& value, uint32_t resultIndex)
{
    return context.typeOf(value.variadicOperands()[resultIndex]);
}

Type ResultRules::provenOrUnknown(std::optional<Type>&& type)
{
    if (type)
        return WTF::move(*type);
    if (context.level == TypeLevel::Proven)
        wasUnproven = true;
    return Type::anything();
}

static_assert((opcodesThatMayHaveSeveralResults | builtinCallOpcodes) == opcodesThatMayHaveSeveralResults);

std::optional<uint32_t> numberOfResultsOf(const Program& program, const Function&, const Value& value)
{
    if (!opcodesThatMayHaveSeveralResults.contains(value.opcode))
        return std::nullopt;
    if (builtinCallOpcodes.contains(value.opcode))
        return numberOfResultsOfOverload(program, value);

    switch (value.opcode) {
    case Opcode::Results:
        return value.numberOfVariadicOperands();
    case Opcode::CallDirect:
    case Opcode::CallTable: {
        size_t numberOfResults = recordOf(program, value.function()).results.size();
        if (numberOfResults == 1)
            return std::nullopt;
        return static_cast<uint32_t>(numberOfResults);
    }
    case Opcode::DestructureIterable:
        return value.count();
    default:
        return numberOfDeclaredResultsOf(value.opcode);
    }
}

Type computeResultType(const TypingContext& context, const Value& producer, uint32_t resultIndex)
{
    if (resultIndex >= numberOfResultsOf(context.program, context.function, producer).value_or(1))
        return Type::anything();

    ResultRules rules { context };
    Type result = applyResultRule(rules, producer, resultIndex, ResultSide::Proven);
    if (!rules.wasUnproven)
        return result;
    return applyResultRule(rules, producer, resultIndex, ResultSide::Unproven);
}

Type computeResultType(const TypingContext& context, const Value& value)
{
    if (numberOfResultsOf(context.program, context.function, value))
        return Type::fromPrimitives(Primitive::Undefined);
    return computeResultType(context, value, 0);
}

namespace {

struct ResultRulesWithoutType {
    using Result = TypeID;

    const TypingContext& context;

    enum class Key : uint32_t { Opcode = UINT32_MAX - 15, Constant, Closure, Layout, NamedRead, SlotRead, CastToType, CastOfOperand };

    TypeID rememberedResult(Key key, SharedAnswers answers, const Value& value, NOESCAPE const auto& addWords)
    {
        if (!context.memo)
            return { };
        auto addKeyWords = [&](auto& hasher) {
            hasher.addNonIDWords(static_cast<uint32_t>(key), static_cast<uint32_t>(value.opcode));
            addWords(hasher);
        };
        return rememberedType(context, answers, addKeyWords, [&] {
            return context.types.intern(context.types.adopt(computeResultType(context, value, 0)));
        });
    }

    uint32_t level() const { return static_cast<uint32_t>(context.level); }

    TypeID idInArena(TypeID id)
    {
        if (!TypeTable::isProvisional(id))
            return id;
        if (!context.memo)
            return context.types.adopt(id);
        if (TypeID known = context.memo->idsInArena.find(id.index()))
            return known;
        TypeID result = context.types.adopt(id);
        context.memo->idsInArena.add(id.index(), result);
        return result;
    }

    TypeID none(const Instruction&) { return TypeTable::undefined; }
    TypeID unknown(const Instruction&) { return TypeTable::anything; }
    TypeID unknown(const Value& value)
    {
        if (value.opcode == Opcode::Construct || value.opcode == Opcode::ConstructList)
            return context.level == TypeLevel::Expected ? TypeTable::object : TypeTable::anything;
        if (value.opcode != Opcode::Call && value.opcode != Opcode::CallResolved)
            return TypeTable::anything;
        const Type& callee = context.typeOf(value.operand(OperandIndex::Call::callee));
        if (!callee.hasObjects() && callee.isJavaScriptValue())
            return TypeTable::never;
        FunctionID only = onlyCalleeOf(context.program, callee);
        return only ? declared(value, only, 0) : TypeTable::anything;
    }

    TypeID constant(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Undefined:
            return TypeTable::undefined;
        case Opcode::Null:
            return TypeTable::null;
        case Opcode::Absent:
            return TypeTable::absent;
        case Opcode::Builtin:
        case Opcode::ModuleBuiltin:
            return { };
        case Opcode::NewClosure:
            return rememberedResult(Key::Closure, SharedAnswers::AboutTypes, value, [&](auto& hasher) {
                hasher.add(value.function().index());
            });
        default:
            return rememberedResult(Key::Constant, value.opcode == Opcode::Data ? SharedAnswers::AboutProgram : SharedAnswers::AboutTypes, value, [&](auto& hasher) {
                hasher.addPayloadOf(value);
            });
        }
    }

    TypeID natural(const Value& value, const NaturalType&)
    {
        if (opcodesWithNaturalTypeRefinedByValue.contains(value.opcode))
            return { };
        return rememberedResult(Key::Opcode, SharedAnswers::AboutTypes, value, [](auto&) { });
    }

    TypeID natural(const Value&, NaturalTable) { return { }; }

    TypeID sameAs(const Value& value, uint32_t operandIndex)
    {
        if (value.opcode == Opcode::Convert)
            return { };
        return idInArena(context.typeIDOf(value.operand(operandIndex)));
    }

    TypeID declared(const Value&, TypeID type) { return idInArena(type); }

    TypeID declared(const Value& value, VariableID variable)
    {
        TypeID declared = context.typeIDOf(context.program.at(variable).declared);
        if (value.opcode != Opcode::GetEncodedModuleVariable || !(context.types.at(declared).primitives & Primitive::Undefined))
            return idInArena(declared);
        return { };
    }

    TypeID declared(const Value& value, PropertyKeyID key)
    {
        if (familyOf(value.opcode) == OperationFamily::SlotAccess) {
            if (!value.receivers())
                return TypeTable::anything;
            return rememberedResult(Key::SlotRead, SharedAnswers::AboutProgram, value, [&](auto& hasher) {
                hasher.add(value.flags, level());
                hasher.add(value.receivers().index(), key.index());
            });
        }

        std::optional<uint8_t> receiverIndex = receiverOf(value.opcode);
        RELEASE_ASSERT(receiverIndex);
        TypeID receiverID = context.typeIDOf(value.operand(*receiverIndex));
        const Type& receiver = context.types.at(receiverID);
        if (receiver.hasOnlyPrimitives(Primitive::UndefinedOrNull))
            return TypeTable::never;
        if (!canLookUpField(context, receiver))
            return TypeTable::anything;
        return rememberedResult(Key::NamedRead, SharedAnswers::AboutProgram, value, [&](auto& hasher) {
            hasher.add(receiverID.index(), key.index());
            hasher.add(level());
        });
    }

    TypeID declared(const Value& value, FunctionID function, uint32_t resultIndex)
    {
        const Function& callee = recordOf(context.program, function);
        bool readsCapture = value.opcode == Opcode::GetCapture;
        const Vector<DeclaredType>& positions = readsCapture ? callee.captures : callee.results;
        uint32_t index = readsCapture ? value.index() : resultIndex;
        if (index >= positions.size())
            return TypeTable::anything;
        return idInArena(context.typeIDOf(positions[index]));
    }

    TypeID declared(const Value& value, LayoutID layout, uint32_t field)
    {
        const Vector<Field>& fields = context.program.at(layout).fields;
        if (field >= fields.size())
            return TypeTable::anything;
        if (!receiverOf(value.opcode) || fields[field].flags.contains(FieldFlags::IsBrand))
            return idInArena(context.typeIDOf(fields[field].declared));
        return { };
    }
    TypeID declared(const Value&, LayoutID) { return { }; }
    TypeID declared(const Value&, ModuleID) { return { }; }
    TypeID declared(const Value&, BuiltinOverloadID, uint32_t) { return { }; }
    TypeID declaredParameter(const Value& value) { return idInArena(context.typeIDOf(value)); }
    TypeID resultOperand(const Value& value, uint32_t resultIndex) { return idInArena(context.typeIDOf(value.variadicOperands()[resultIndex])); }

    TypeID allocation(const Value& value, LayoutID layout)
    {
        if (mayAllocateOtherLayout(context.program, value))
            return { };
        return rememberedResult(Key::Layout, SharedAnswers::AboutProgram, value, [&](auto& hasher) {
            hasher.add(layout.index());
        });
    }

    TypeID allocation(const Value&, BuiltinClassID) { return { }; }
    TypeID allocation(const Value&, BuiltinClassID, TypeID) { return { }; }
    TypeID allocation(const Value&, BuiltinLayoutID) { return { }; }

    TypeID cast(const Value& value, uint32_t operandIndex, TypeID castType)
    {
        return rememberedResult(Key::CastToType, SharedAnswers::AboutTypes, value, [&](auto& hasher) {
            hasher.add(context.typeIDOf(value.operand(operandIndex)).index(), castType.index());
        });
    }

    TypeID cast(const Value& value, uint32_t operandIndex, ExpectedType expected)
    {
        if (value.opcode == Opcode::Check)
            return cast(value, operandIndex, expected.predicted());
        ValueID operand = value.operand(operandIndex);
        if (context.level == TypeLevel::Proven)
            return context.typeIDOf(operand);
        return rememberedResult(Key::CastToType, SharedAnswers::AboutTypes, value, [&](auto& hasher) {
            hasher.add(context.typeIDOf(operand).index(), context.function.at(operand).type.index());
            hasher.add(expected.predicted());
        });
    }

    TypeID cast(const Value& value, uint32_t operandIndex)
    {
        TypeID sourceID = context.typeIDOf(value.operand(operandIndex));
        const Type& source = context.types.at(sourceID);
        uint16_t removed = 0;
        switch (value.opcode) {
        case Opcode::CheckInitialized:
            removed = Primitive::Absent;
            break;
        case Opcode::RequireObjectCoercible:
            removed = Primitive::UndefinedOrNull;
            break;
        case Opcode::RequireObject:
            if (!source.isJavaScriptValue())
                return TypeTable::never;
            removed = Primitive::Defined;
            break;
        default:
            return { };
        }
        if (!(source.primitives & removed))
            return idInArena(sourceID);
        return rememberedResult(Key::CastOfOperand, SharedAnswers::AboutTypes, value, [&](auto& hasher) {
            hasher.add(sourceID.index());
        });
    }

    TypeID declaredContents(auto&&...) { return { }; }
    TypeID declaredResult(auto&&...) { return { }; }
};

struct AnswerSharingRules {
    using Result = std::optional<SharedAnswers>;

    Result none(auto&&...) { return SharedAnswers::AboutTypes; }
    Result natural(auto&&...) { return SharedAnswers::AboutTypes; }
    Result declaredContents(auto&&...) { return SharedAnswers::AboutTypes; }
    Result resultOperand(auto&&...) { return SharedAnswers::AboutTypes; }
    Result allocation(auto&&...) { return SharedAnswers::AboutProgram; }
    Result declared(auto&&...) { return SharedAnswers::AboutProgram; }
    Result declared(const Value&, FunctionID, uint32_t) { return std::nullopt; }
    Result declaredResult(auto&&...) { return std::nullopt; }
    Result declaredParameter(auto&&...) { return std::nullopt; }

    Result constant(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Builtin:
        case Opcode::ModuleBuiltin:
        case Opcode::Data:
            return SharedAnswers::AboutProgram;
        default:
            return SharedAnswers::AboutTypes;
        }
    }

    Result sameAs(const Value& value, uint32_t) { return value.opcode == Opcode::Convert ? SharedAnswers::AboutProgram : SharedAnswers::AboutTypes; }
    Result cast(const Value&, uint32_t, TypeID) { return SharedAnswers::AboutTypes; }
    Result cast(const Value&, uint32_t, ExpectedType) { return SharedAnswers::AboutTypes; }
    Result cast(const Value& value, uint32_t) { return value.opcode == Opcode::NarrowByField ? SharedAnswers::AboutProgram : SharedAnswers::AboutTypes; }

    Result unknown(const Value& value)
    {
        if (value.opcode == Opcode::Call || value.opcode == Opcode::CallResolved)
            return std::nullopt;
        return SharedAnswers::AboutTypes;
    }
};

struct RulesThatReadNarrowedRecords {
    using Result = bool;

    Result none(auto&&...) { return false; }
    Result natural(auto&&...) { return false; }
    Result constant(auto&&...) { return false; }
    Result sameAs(auto&&...) { return false; }
    Result cast(auto&&...) { return false; }
    Result allocation(auto&&...) { return false; }
    Result declaredContents(auto&&...) { return false; }
    Result resultOperand(auto&&...) { return false; }
    Result declared(auto&&...) { return false; }
    Result declared(const Value&, VariableID) { return true; }
    Result declared(const Value&, FunctionID, uint32_t) { return true; }
    Result declaredResult(auto&&...) { return true; }
    Result declaredParameter(auto&&...) { return true; }
    Result unknown(const Value& value) { return value.opcode == Opcode::Call || value.opcode == Opcode::CallResolved; }
};

} // anonymous namespace

const OpcodeSet& opcodesTypedFromNarrowedRecords()
{
    static const OpcodeSet opcodes = [] {
        OpcodeSet result;
        RulesThatReadNarrowedRecords rules;
        for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
            Value value(static_cast<Opcode>(index));
            if (value.opcode == Opcode::Parameter)
                continue;
            if (applyResultRule(rules, value, 0, ResultSide::Proven) || applyResultRule(rules, value, 0, ResultSide::Unproven))
                result.add(value.opcode);
        }
        return result;
    }();
    return opcodes;
}

static std::optional<SharedAnswers> answerSharingFor(const Value& value)
{
    AnswerSharingRules rules;
    std::optional<SharedAnswers> proven = applyResultRule(rules, value, 0, ResultSide::Proven);
    std::optional<SharedAnswers> unproven = applyResultRule(rules, value, 0, ResultSide::Unproven);
    if (!proven || !unproven)
        return std::nullopt;
    return proven == unproven ? proven : SharedAnswers::AboutProgram;
}

static constexpr OpcodeSet opcodesTypedAlikeAtBothLevels = []() consteval {
    OpcodeSet opcodes;
    for (unsigned index = 0; index < numberOfEnumerators<Opcode>; ++index) {
        Opcode opcode = static_cast<Opcode>(index);
        switch (resultRuleOf(opcode)) {
        case ResultRule::Constant:
        case ResultRule::Natural:
        case ResultRule::Allocation:
        case ResultRule::Cast:
            opcodes.add(opcode);
            break;
        case ResultRule::None:
        case ResultRule::Declared:
        case ResultRule::Unknown:
        case ResultRule::Results:
            break;
        }
    }
    return opcodes;
}();

static constexpr OpcodeSet opcodesWhoseRuleReadsTheLevel { Opcode::Builtin, Opcode::ModuleBuiltin, Opcode::Convert, Opcode::NewObject, Opcode::NarrowByField };

static void addKeyOfResult(auto& hasher, const TypingContext& context, const Value& value)
{
    bool readsLevel = !opcodesTypedAlikeAtBothLevels.contains(value.opcode) || opcodesWhoseRuleReadsTheLevel.contains(value.opcode);
    bool isExpected = readsLevel && context.level == TypeLevel::Expected;

    hasher.add(static_cast<uint32_t>(value.opcode) << 1 | isExpected, value.numberOfOperands());
    hasher.addPayloadOf(value);
    hasher.addForEachOperandOf(value, [&](ValueID operand) {
        return (operand ? context.typeIDOf(operand) : TypeID()).index();
    });
    if (value.opcode == Opcode::Convert)
        hasher.add(context.function.at(value.valueOperand()).type);
}

TypeID computeResultTypeID(const TypingContext& context, const Value& value)
{
    if (numberOfResultsOf(context.program, context.function, value))
        return TypeTable::undefined;

    ResultRulesWithoutType rulesWithoutType { context };
    if (TypeID type = applyResultRule(rulesWithoutType, value, 0, ResultSide::Proven))
        return type;
    if (!context.memo || value.opcode == Opcode::ResultAt)
        return context.types.intern(context.types.adopt(computeResultType(context, value, 0)));

    auto addWords = [&](auto& hasher) {
        addKeyOfResult(hasher, context, value);
    };
    WordHasher hasher;
    addWords(hasher);
    if (TypeID known = context.memo->results.find(hasher.hash()))
        return known;
    TypeID type = sharedOrComputedType(context, answerSharingFor(value), hasher.hash(), addWords, [&] {
        return context.types.intern(context.types.adopt(computeResultType(context, value, 0)));
    });
    context.memo->results.add(hasher.hash(), type);
    return type;
}

TypeID resultTypeOf(const Program& program, const Function& function, const Value& value, TypeArena& arena, TypingMemo* memo)
{
    return computeResultTypeID({ program, function, arena, TypeLevel::Proven, memo }, value);
}

TypeID expectedTypeOf(const Program& program, const Function& function, const Value& value, TypeArena& arena, TypingMemo* memo)
{
    TypeID expected = computeResultTypeID({ program, function, arena, TypeLevel::Expected, memo }, value);
    if (expected == value.type)
        return { };
    const Type& type = arena.at(value.type);
    Type met = type.intersectWith(arena.at(expected), arena);
    if (met.isNever() || met == type || !met.isSubsetOf(type, program))
        return { };
    return arena.intern(met);
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
