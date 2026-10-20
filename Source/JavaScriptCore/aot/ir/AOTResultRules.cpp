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
#include "AOTIRProgram.h"
#include "AOTResultRulesInternal.h"
#include <wtf/StdLibExtras.h>

namespace JSC { namespace AOT { namespace Typed {

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
    case PropertyKeyKind::PrivateName:
        return Type::anything();
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static Type typeOfBuiltinEntry(const TypingContext& context, BuiltinID builtin)
{
    const BuiltinTable& builtins = context.program.builtins();
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
        return typeOfAllocatedLayout(context, node.as<DataNode::Object>().layout, false);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

Type typeOfAllocatedLayout(const TypingContext& context, LayoutID layout, bool keepsShape)
{
    if (context.program.at(layout).isEnvironment())
        return Type::forEnvironment(layout);
    if (keepsShape || context.predicts())
        return Type::forLayout(layout, context.types);
    return Type::object();
}

bool mayAllocateOtherLayout(const Program& program, const Value& allocation)
{
    if (allocation.opcode != Opcode::NewObject || !allocation.prototypeOperand() || allocation.keepsShape())
        return false;
    return program.prototypeMaySelectOtherClass(allocation.layout());
}

Type typeOfDeclaredLayout(const TypingContext& context, LayoutID layout)
{
    if (!context.predicts())
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
        // Unproven contents declare what may be stored later, so a string literal counts as any string.
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

static bool canLookUpField(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    switch (context.level) {
    case TypeLevel::Proven:
        return receiver.hasOnlyLayouts() && !receiver.mayBeUndefinedOrNull();
    case TypeLevel::Written:
        return hasWrittenField(context.program, receiver, key);
    case TypeLevel::Predicted:
        return !!receiver.layouts;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Type> tryTypeOfNamedRead(const TypingContext& context, const Type& receiver, PropertyKeyID key)
{
    if (receiver.isNever())
        return Type::never();
    if (!canLookUpField(context, receiver, key))
        return std::nullopt;
    return tryTypeOfFieldRead(context, receiver, key);
}

Type typeOfSlotRead(const TypingContext& context, const Value& read, std::span<const LayoutID> receivers)
{
    Type result = Type::never();
    for (LayoutID receiver : receivers) {
        std::optional<FieldReference> field = findField(context.program, receiver, read.key(), BaseChain::Proven);
        if (!field)
            return Type::anything();
        result = result.unionWith(context.typeOf(fieldAt(context.program, *field).declared), context.types);
    }
    if (read.opcode == Opcode::GetSlotOrUndefined)
        return result.withPrimitives(Primitive::Undefined);
    return result;
}

static Type typeOfSlotRead(const TypingContext& context, const Value& read)
{
    if (!read.receivers())
        return Type::anything();
    return typeOfSlotRead(context, read, context.types.at(read.receivers()).members.span());
}

static std::optional<Type> tryTypeOfAwait(const TypingContext& context, const Type& awaited)
{
    // Whether awaiting an object runs user code holds for a stretch, and an await names no guard.
    if (context.level == TypeLevel::Proven && awaited.hasObjects())
        return std::nullopt;
    return tryTypeOfAwaited(context, awaited);
}

// A position inside the length of an array without holes holds an element.
static bool findsElement(const Value& read, const Type& array)
{
    return read.opcode == Opcode::GetArrayElement && read.isInBounds() && pristineAspectsProvenBy(array, BuiltinClasses::Array).contains(PristineAspects::Dense);
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
    if (!element)
        return stored;
    return element->intersectWith(stored, context.types);
}

static ContentsSlot defaultContentsSlotOf(const BuiltinTable& builtins, const Type& container)
{
    for (const ClassPart& part : container.classes) {
        if (!builtins.findSlot(part.builtinClass, ContentsSlot::Element))
            return ContentsSlot::Value;
    }
    return ContentsSlot::Element;
}

// Only built-ins that leave the object as created get a shared literal, so nothing but its prototype chain could change what they find.
static Type typeOfSharedRegExpLiteral(const TypingContext& context, const Value& literal)
{
    Type type = Type::regExpLiteral(literal.pattern(), literal.regExpFlags());
    if (pristineTestOf(context.program.builtins(), BuiltinClasses::RegExp) == PristineTest::MayPass)
        type.classes[0].provenPristine = { PristineAspects::Storage, PristineAspects::Names, PristineAspects::AsCreated };
    return type;
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
    const Type& source = context.typeOf(value.operand(operandIndex));
    if (value.opcode != Opcode::Predict || !context.predicts())
        return source;

    const Type& predicted = context.types.at(value.predicted());
    Type narrowed = source.castTo(predicted, context.types);
    if (!narrowed.isNever())
        return narrowed;
    // Of two predictions that exclude each other the one nearer to the use stands.
    return context.types.at(context.function.at(value.operand(operandIndex)).type).castTo(predicted, context.types);
}

Type ResultRules::allocation(const Value& value, LayoutID layout)
{
    if (context.predicts() && mayAllocateOtherLayout(context.program, value)) {
        Vector<LayoutID, 8> layouts { layout };
        layouts.append(context.program.layoutsBelow(layout));
        return Type::forLayouts(layouts.span(), context.types);
    }
    return typeOfAllocatedLayout(context, layout, FlagMask::keepsShape(value.opcode) && value.keepsShape());
}

Type ResultRules::allocation(const Value& value, BuiltinClassID builtinClass)
{
    if (value.opcode == Opcode::NewRegExp)
        return Type::regExpLiteral(value.pattern(), value.regExpFlags());
    if (Value::opcodesWithData.contains(value.opcode))
        return typeOfCollectionFromData(context, builtinClass, context.program.at(value.data()).root);
    return Type::builtinClass(builtinClass);
}

Type ResultRules::allocation(const Value&, BuiltinClassID builtinClass, TypeID contents)
{
    return Type::builtinClass(builtinClass, singleElementSpan(contents));
}

Type ResultRules::allocation(const Value& value, BuiltinLayoutID layout)
{
    return typeOfAllocatedLayout(context, context.program.layoutOf(layout), value.keepsShape());
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
    // Only a definition stores a brand, and it stores the declared type: a read that does not throw gives that on any receiver.
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

Type ResultRules::declared(const Value& value, FunctionID function, uint32_t resultIndex)
{
    const Function& callee = context.program.at(function);
    bool readsCapture = value.opcode == Opcode::GetCapture;
    const Vector<DeclaredType>& positions = readsCapture ? callee.captures : callee.results;
    uint32_t index = readsCapture ? value.index() : resultIndex;
    if (index >= positions.size())
        return Type::anything();
    return context.typeOf(positions[index]);
}

Type ResultRules::declared(const Value& value, LayoutID layout)
{
    const Vector<LayoutID>& members = context.program.at(layout).flow.members;
    if (value.keepsShape() && !members.isEmpty())
        return Type::forLayouts(members.span(), context.types);
    return typeOfDeclaredLayout(context, layout);
}

Type ResultRules::declared(const Value& value, ModuleID module)
{
    LayoutID layout = context.program.at(module).namespaceLayout;
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
    // The step that finds no entry answers undefined, and its results stand in front of the test for an entry.
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

Type ResultRules::cast(const Value& value, uint32_t operandIndex, TypeID castType)
{
    return context.typeOf(value.operand(operandIndex)).castTo(context.types.at(castType), context.types);
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

std::optional<uint32_t> numberOfResultsOf(const Program& program, const Function&, const Value& value)
{
    if (builtinCallOpcodes.contains(value.opcode))
        return numberOfResultsOfOverload(program, value);

    switch (value.opcode) {
    case Opcode::Results:
        return value.numberOfVariadicOperands();
    case Opcode::CallDirect:
    case Opcode::CallGroup: {
        size_t numberOfResults = program.at(value.function()).results.size();
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

static TypeID narrowerTypeOf(const TypingContext& context, const Value& value, TypeID knownID)
{
    const Type& known = context.types.at(knownID);
    Type met = known.intersectWith(computeResultType(context, value), context.types);
    if (met.isNever() || met == known || !met.isSubsetOf(known, context.program))
        return { };
    return context.types.intern(met);
}

TypeID resultTypeOf(const Program& program, const Function& function, const Value& value, TypeArena& arena)
{
    return arena.intern(computeResultType({ program, function, arena, TypeLevel::Proven }, value));
}

TypeID writtenTypeOf(const Program& program, const Function& function, const Value& value, TypeArena& arena)
{
    return narrowerTypeOf({ program, function, arena, TypeLevel::Written }, value, value.type);
}

TypeID predictionOf(const Program& program, const Function& function, const Value& value, TypeArena& arena)
{
    return narrowerTypeOf({ program, function, arena, TypeLevel::Predicted }, value, value.writtenOrType());
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
