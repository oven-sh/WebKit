/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTFlowEdges.h"

#if ENABLE(AOT)

#include "AOTBuiltinTable.h"
#include <algorithm>
#include <array>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet appendOperations { Opcode::AppendElement, Opcode::AppendSpread, Opcode::AppendHole };

static_assert(OperandIndex::AppendElement::array == OperandIndex::AppendSpread::array);
static_assert(OperandIndex::AppendElement::array == OperandIndex::AppendHole::array);

namespace {

struct PrimitiveWrapper {
    uint16_t primitives;
    TypeID type;
    BuiltinClassID wrapperClass;
};

} // anonymous namespace

static constexpr std::array<PrimitiveWrapper, 3> primitiveWrappers { {
    { Primitive::AnyString, TypeTable::string, BuiltinClasses::String },
    { Primitive::Number, TypeTable::number, BuiltinClasses::Number },
    { Primitive::Boolean, TypeTable::boolean, BuiltinClasses::Boolean },
} };

static bool isAppendedTo(const Function& function, const UseLists& uses, ValueID array)
{
    return std::ranges::any_of(uses.usesOf(array), [&](const Use& use) {
        return use.kind == Use::Kind::Operand
            && use.operandIndex == OperandIndex::AppendElement::array
            && appendOperations.contains(function.at(use.user).opcode);
    });
}

static bool isCompletedByPieces(const Function& function, const UseLists& uses, ValueID literal)
{
    bool isFinished = false;
    bool hasPieces = false;
    for (const Use& use : uses.usesOf(literal)) {
        if (use.kind != Use::Kind::Operand || use.operandIndex != literalOperandOfPiece)
            continue;
        Opcode opcode = function.at(use.user).opcode;
        isFinished |= opcode == Opcode::FinishObject;
        hasPieces |= literalPieceOperations.contains(opcode);
    }
    return isFinished && hasPieces;
}

static FlowExternalNode elementPartOf(uint32_t site, size_t index, uint16_t numberOfPositions, BuiltinSlotID elementSlot)
{
    if (index < numberOfPositions)
        return FlowExternalNode::sitePosition(site, static_cast<uint32_t>(index));
    return FlowExternalNode::siteSlot(site, elementSlot);
}

static FlowLocalID addUndefinedTemporary(FlowSummaryAssembler& assembler)
{
    FlowLocalID local = assembler.addTemporary();
    assembler.seed(FlowSeed::primitives(local, Primitive::Undefined));
    return local;
}

static FlowSlotAccess createElementStore(ValueID origin, FlowLocalID array, FlowLocalID element)
{
    return { .kind = FlowAccessKind::Store, .origin = origin, .receiver = array, .contentsSlot = ContentsSlot::Element, .value = element };
}

static FlowPropertyAccess createPropertyDefinition(ValueID origin, FlowLocalID object, PropertyKeyID key, FlowLocalID value)
{
    return { .kind = FlowAccessKind::Define, .origin = origin, .receiver = object, .key = key, .value = value };
}

static BuiltinClassID instanceClassOf(Opcode opcode)
{
    switch (opcode) {
    case Opcode::NewRegExp:
        return BuiltinClasses::RegExp;
    case Opcode::NewPromise:
        return BuiltinClasses::Promise;
    case Opcode::NewContiguousArray:
    case Opcode::NewDoubleArray:
    case Opcode::NewArrayLike:
    case Opcode::NewArrayWithCapacity:
        return BuiltinClasses::Array;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static BuiltinID findArrayIteratorMethod(const BuiltinTable& builtins)
{
    BuiltinMemberLookup lookup = builtins.findForInstance(BuiltinClasses::Array, BuiltinNames::iteratorSymbol);
    if (lookup.outcome != BuiltinMemberLookup::Outcome::Found)
        return { };
    BuiltinID method = builtins.canonicalID(lookup.member);
    return isObjectKind(builtins.at(method).kind) ? method : BuiltinID();
}

void FlowSummaryBuilder::addNewObject(ValueID id, const Value& value)
{
    FlowSiteDescription description { .kind = FlowSiteKind::Layout, .origin = FlowSiteOrigin::Operation, .isCompletedByPieces = isCompletedByPieces(m_function, m_uses, id), .layout = value.layout() };
    uint32_t site;
    if (FlowLocalID prototype = localOf(value.prototypeOperand())) {
        description.value = id;
        site = m_assembler.addSite(description);
        m_assembler.add(FlowAllocation { id, site, prototype, isInCycle() });
    } else
        site = addSiteFor(id, description).site;

    std::span<const uint32_t> fields = value.fields();
    std::span<const ValueID> values = value.variadicOperands();
    RELEASE_ASSERT(fields.size() == values.size());
    for (size_t index = 0; index < fields.size(); ++index)
        storeIntoSite(id, FlowExternalNode::siteField(site, fields[index]), localOf(values[index]));
}

void FlowSummaryBuilder::addNewArray(ValueID id, const Value& value)
{
    std::span<const ValueID> elements = value.variadicOperands();
    bool hasHoles = !value.holes().empty();
    uint16_t numberOfPositions = 0;
    if (!hasHoles && !isAppendedTo(m_function, m_uses, id))
        numberOfPositions = numberOfPositionsFor(elements.size());

    FlowNewSite created = addInstanceSiteFor(id, BuiltinClasses::Array, numberOfPositions, !hasHoles);
    for (size_t index = 0; index < elements.size(); ++index)
        storeIntoSite(id, elementPartOf(created.site, index, numberOfPositions, BuiltinSlots::ArrayElement), localOf(elements[index]));
    if (hasHoles)
        storeIntoSite(id, FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArrayElement), addUndefinedTemporary(m_assembler));
}

void FlowSummaryBuilder::addAppendElement(ValueID id, const Value& value)
{
    m_assembler.add(createElementStore(id, localOf(value.arrayOperand()), localOf(value.valueOperand())));
}

void FlowSummaryBuilder::addAppendSpread(ValueID id, const Value& value)
{
    FlowLocalID state = m_assembler.addTemporary();
    FlowLocalID element = m_assembler.addTemporary();
    FlowLocalID thrown = thrownTarget();

    m_assembler.add(FlowIteration { .step = FlowIterationStep::Open, .origin = id, .iterable = localOf(value.iterableOperand()), .state = state, .thrown = thrown });
    m_assembler.add(FlowIteration { .step = FlowIterationStep::Resume, .origin = id, .state = state, .value = element, .thrown = thrown });
    m_assembler.add(createElementStore(id, localOf(value.arrayOperand()), element));
}

void FlowSummaryBuilder::addAppendHole(ValueID id, const Value& value)
{
    FlowSlotAccess store = createElementStore(id, localOf(value.arrayOperand()), addUndefinedTemporary(m_assembler));
    store.mayLeaveHole = true;
    m_assembler.add(store);
}

void FlowSummaryBuilder::addNewInstance(ValueID id, const Value& value)
{
    BuiltinClassID builtinClass = instanceClassOf(value.opcode);
    FlowNewSite created = addInstanceSiteFor(id, builtinClass);
    if (builtinClass == BuiltinClasses::Array)
        storeIntoSite(id, FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArrayElement), addUndefinedTemporary(m_assembler));
    addOperandUsesFromSchema(id, value, { });
}

void FlowSummaryBuilder::addCreatePromise(ValueID id, const Value& value)
{
    FlowLocalID read = m_assembler.addTemporary();
    FlowLocalID prototype = m_assembler.addTemporary();

    addPrototypePropertyRead(id, value.constructorOperand(), read);
    filter(read, prototype, TypeTable::object);
    m_assembler.add(FlowConditionalSeed { read, TypeTable::object, FlowFilterKind::Outside, id, FlowSeed::builtin(prototype, Builtins::PromisePrototype) });

    uint32_t site = m_assembler.addSite({ .kind = FlowSiteKind::Instance, .origin = FlowSiteOrigin::Operation, .value = id, .builtinClass = BuiltinClasses::Promise });
    m_assembler.add(FlowAllocation { id, site, prototype, isInCycle() });
}

void FlowSummaryBuilder::addNewTemplateObject(ValueID id, const Value&)
{
    FlowNewSite created = addSiteFor(id, { .kind = FlowSiteKind::Opaque, .origin = FlowSiteOrigin::Operation, .builtinClass = BuiltinClasses::Array });

    FlowLocalID cookedString = addUndefinedTemporary(m_assembler);
    m_assembler.seed(FlowSeed::typeAsRead(cookedString, TypeTable::string));
    storeIntoSite(id, FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArrayElement), cookedString);

    FlowLocalID rawStrings = m_assembler.addTemporary();
    m_assembler.seed(FlowSeed::typeAsRead(rawStrings, TypeTable::object));
    storeIntoSite(id, FlowExternalNode::siteUndeclaredPart(created.site), rawStrings);
}

void FlowSummaryBuilder::addNewError(ValueID id, const Value& value)
{
    if (value.opcode == Opcode::NewRuntimeError) {
        m_assembler.add(FlowOperandUse { .origin = id, .result = localOf(id) });
        return;
    }

    RELEASE_ASSERT(value.opcode == Opcode::NewSuppressedError);
    FlowNewSite created = addSiteFor(id, { .kind = FlowSiteKind::Opaque, .origin = FlowSiteOrigin::Operation, .builtinClass = BuiltinClasses::Error });
    storeIntoSite(id, FlowExternalNode::siteUndeclaredPart(created.site), localOf(value.errorOperand()));
    storeIntoSite(id, FlowExternalNode::siteUndeclaredPart(created.site), localOf(value.suppressedOperand()));
}

void FlowSummaryBuilder::addCreateArguments(ValueID id, const Value& value)
{
    std::span<const ValueID> parameters = value.variadicOperands();
    uint16_t numberOfPositions = numberOfPositionsFor(parameters.size());

    FlowLocalID prototype = m_assembler.addTemporary();
    m_assembler.seed(FlowSeed::builtin(prototype, Builtins::ObjectPrototype));
    FlowNewSite created = addSiteFor(id, { .kind = FlowSiteKind::Instance, .origin = FlowSiteOrigin::Operation, .numberOfPositions = numberOfPositions, .builtinClass = BuiltinClasses::Arguments }, prototype);

    for (size_t index = 0; index < parameters.size(); ++index)
        storeIntoSite(id, elementPartOf(created.site, index, numberOfPositions, BuiltinSlots::ArgumentsElement), localOf(parameters[index]));

    FlowLocalID surplusArgument = m_assembler.addTemporary();
    m_assembler.add(FlowSlotAccess { .kind = FlowAccessKind::Load, .origin = id, .receiver = localOf(value.restOperand()), .contentsSlot = ContentsSlot::Element, .value = surplusArgument });
    storeIntoSite(id, FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArgumentsElement), surplusArgument);

    m_assembler.add(createPropertyDefinition(id, created.local, m_program.keyOf(BuiltinNames::length), localOf(value.countOperand())));

    FlowLocalID iteratorMethod = m_assembler.addTemporary();
    if (BuiltinID arrayIteratorMethod = findArrayIteratorMethod(m_program.builtins()))
        m_assembler.seed(FlowSeed::builtin(iteratorMethod, arrayIteratorMethod));
    else
        m_assembler.seed(FlowSeed::typeAsRead(iteratorMethod, TypeTable::function));
    m_assembler.add(createPropertyDefinition(id, created.local, m_program.keyOf(BuiltinNames::iteratorSymbol), iteratorMethod));

    if (value.opcode == Opcode::CreateMappedArguments)
        m_assembler.add(createPropertyDefinition(id, created.local, m_program.keyOf(BuiltinNames::callee), localOf(value.calleeOperand())));
}

void FlowSummaryBuilder::addMappedArgumentAccess(ValueID id, const Value& value)
{
    FlowSlotAccess access { .origin = id, .receiver = localOf(value.argumentsOperand()), .contentsSlot = ContentsSlot::Element, .position = value.index() };
    if (value.opcode == Opcode::GetMappedArgument) {
        access.kind = FlowAccessKind::Load;
        access.value = localOf(id);
    } else {
        RELEASE_ASSERT(value.opcode == Opcode::SetMappedArgument);
        access.kind = FlowAccessKind::Store;
        access.value = localOf(value.valueOperand());
    }
    m_assembler.add(access);
}

void FlowSummaryBuilder::addCreateIteratorResult(ValueID id, const Value& value)
{
    LayoutID layoutID = m_program.layoutOf(BuiltinLayouts::IteratorResult);
    const Layout& layout = m_program.at(layoutID);
    std::optional<uint32_t> valueField = layout.findField(m_program.keyOf(BuiltinNames::value));
    std::optional<uint32_t> doneField = layout.findField(m_program.keyOf(BuiltinNames::done));
    RELEASE_ASSERT(valueField && doneField);

    FlowNewSite created = addSiteFor(id, { .kind = FlowSiteKind::Layout, .origin = FlowSiteOrigin::Operation, .layout = layoutID });
    storeIntoSite(id, FlowExternalNode::siteField(created.site, *valueField), localOf(value.valueOperand()));
    storeIntoSite(id, FlowExternalNode::siteField(created.site, *doneField), localOf(value.doneOperand()));
}

void FlowSummaryBuilder::addArraySpeciesCreate(ValueID id, const Value& value)
{
    FlowNewSite created = addInstanceSiteFor(id, BuiltinClasses::Array);
    storeIntoSite(id, FlowExternalNode::siteSlot(created.site, BuiltinSlots::ArrayElement), addUndefinedTemporary(m_assembler));
    addOperandUsesFromSchema(id, value, { });
}

void FlowSummaryBuilder::addToObject(ValueID id, const Value& value)
{
    ValueID operand = value.valueOperand();
    uint16_t primitives = typeOf(operand).primitives;
    FlowLocalID from = localOf(operand);

    filter(from, localOf(id), TypeTable::object);
    for (uint32_t ordinal = 0; ordinal < primitiveWrappers.size(); ++ordinal) {
        const PrimitiveWrapper& wrapper = primitiveWrappers[ordinal];
        if (primitives & wrapper.primitives)
            addWrapperSeed(id, from, wrapper.type, wrapper.wrapperClass, ordinal);
    }

    if (!(primitives & (Primitive::AnySymbol | Primitive::BigInt)))
        return;

    FlowLocalID remaining = from;
    auto exclude = [&](TypeID type) {
        FlowLocalID outside = m_assembler.addTemporary();
        filter(remaining, outside, type, FlowFilterKind::Outside);
        remaining = outside;
    };
    exclude(TypeTable::object);
    for (const PrimitiveWrapper& wrapper : primitiveWrappers)
        exclude(wrapper.type);
    exclude(TypeTable::nullish);

    uint32_t site = m_assembler.addSite({ .kind = FlowSiteKind::Opaque, .origin = FlowSiteOrigin::Operation, .value = id, .ordinal = static_cast<uint32_t>(primitiveWrappers.size()) });
    m_assembler.add(FlowConditionalSeed { remaining, TypeTable::never, FlowFilterKind::Outside, id, FlowSeed::site(localOf(id), site) });
}

void FlowSummaryBuilder::addToThis(ValueID id, const Value& value)
{
    addToObject(id, value);

    ValueID operand = value.valueOperand();
    if (!typeOf(operand).mayBeUndefinedOrNull())
        return;
    m_assembler.add(FlowConditionalSeed { localOf(operand), TypeTable::nullish, FlowFilterKind::Inside, id, FlowSeed::builtin(localOf(id), Builtins::GlobalThis) });
}

void FlowSummaryBuilder::addWrapperSeed(ValueID id, FlowLocalID from, TypeID primitiveType, BuiltinClassID wrapperClass, uint32_t ordinal)
{
    uint32_t site = m_assembler.addSite({ .kind = FlowSiteKind::Instance, .origin = FlowSiteOrigin::Operation, .value = id, .builtinClass = wrapperClass, .ordinal = ordinal });
    m_assembler.add(FlowConditionalSeed { from, primitiveType, FlowFilterKind::Inside, id, FlowSeed::site(localOf(id), site) });
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
