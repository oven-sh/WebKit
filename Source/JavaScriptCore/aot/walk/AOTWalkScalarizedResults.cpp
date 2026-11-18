/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTWalk.h"

#if ENABLE(AOT)

#include "AOTIRProgram.h"
#include "AOTPlan.h"
#include "AOTSlotPlan.h"
#include "AOTTypeArena.h"
#include "AOTWalkMergedObjects.h"

namespace JSC { namespace AOT { namespace Typed {

bool Walker::returnsScalars() const
{
    return plan().conventions().returnsScalars(unit());
}

bool Walker::isScalarOfResult(const Operation& record) const
{
    if (record.opcode != Opcode::ResultAt || m_code.isConstant(record.resultsOperand()))
        return false;
    const Operation& producer = m_code.at(record.resultsOperand());
    return producer.opcode == Opcode::CallDirect && plan().conventions().returnsScalars(plan().conventions().unitEnteredBy(producer));
}

Reference Walker::allocateScalarizedResult(ValueID call)
{
    const CallingConventions& conventions = plan().conventions();
    CodeUnit entered = conventions.unitEnteredBy(m_code.at(call));
    const ScalarizedResult& result = *conventions.findScalarizedResult(entered.function);
    Origin origin = m_code.at(call).origin();
    origin.isTailCall = false;
    Vector<Reference, 8> values;
    for (uint32_t index = 0; index < result.fields.size(); ++index) {
        Draft part(Opcode::ResultAt, origin);
        part.standsForNoValueOfInput = true;
        part.setResultsOperand(Reference::value(call));
        part.setIndex(index);
        part.representation = conventions.resultRepresentation(entered, index);
        values.append(build(WTF::move(part)));
        if (!values.last() || isUnreachable())
            return { };
    }
    Reference object = buildObjectOfScalars(result.layout, Vector<uint32_t, 8> { result.fields }, WTF::move(values), origin);
    if (object)
        count(StatisticID::ScalarizedCalls);
    return object;
}

Reference Walker::buildObjectOfScalars(LayoutID layout, Vector<uint32_t, 8>&& fields, Vector<Reference, 8>&& values, Origin origin)
{
    Draft allocation(Opcode::NewObject, origin);
    allocation.standsForNoValueOfInput = true;
    allocation.setLayout(layout);
    allocation.setFields(fields.span());
    allocation.setValuesOperands(values.span());
    chooseConservativeFields(allocation);
    Reference object = build(WTF::move(allocation));
    if (!object || isUnreachable())
        return { };
    noteFreshObject(*this, object, fields.size());
    return object;
}

static FieldReference fieldOfPlace(const ScalarizedParameter& parameter, uint32_t place)
{
    const PlaceOfScalar& scalar = parameter.places[place];
    if (scalar.held)
        return { scalar.held, scalar.fieldOfHeld };
    return { parameter.layout, scalar.field };
}

Reference Walker::readScalar(Reference object, FieldReference field, TypeID held, ProofSource source, Origin origin)
{
    if (program().at(field.layout).fields[field.field].flags.contains(FieldFlags::IsBrand))
        held = { };
    ValueID cell = object.isValue() ? underlyingCellOf(m_code, object.id()) : ValueID();
    Reference value = cell && !m_code.isConstant(cell) ? m_state.slot(Reference::value(cell), field) : Reference();
    TypeID known = value && value.isRecord() && !isClosureLeftUnmade(*this, value) ? typeOf(value) : TypeID();
    if (!known || (m_types.at(known).primitives & Primitive::Absent)) {
        Draft read(Opcode::GetSlot, origin);
        read.standsForNoValueOfInput = true;
        read.setObjectOperand(object);
        read.setKey(plan().slots().keyOf(program(), field));
        read.setReceivers(m_types.at(m_types.forLayout(field.layout)).layouts);
        read.setLevel(KnowledgeLevel::Proven);
        chooseConservativeFields(read);
        value = build(WTF::move(read));
    }
    if (!value || isUnreachable())
        return { };
    if (held && !m_types.isSubset(typeOf(value), held))
        value = proveOrTrap(value, held, source, origin);
    else if (held && value.isRecord() && !isShown(value, held))
        buildCheckOfProof(value, held, TypeWithoutProof::restingOnBelief(typeShown(value)), source, origin, true);
    return isUnreachable() ? Reference() : value;
}

void Walker::returnScalars(Reference returned, Origin origin)
{
    const ScalarizedResult& result = *plan().conventions().findScalarizedResult(unit().function);
    origin.isTailCall = false;
    if (TypeID held = m_types.forLayout(result.layout); !m_types.isSubset(typeOf(returned), held)) {
        returned = proveOrTrap(returned, held, ProofSource::ResultOfCall, origin);
        if (!returned || isUnreachable())
            return;
    }
    Vector<Reference, 8> values;
    for (FieldIndex field : result.fields) {
        values.append(readScalar(returned, { result.layout, field }, m_positions->scalarOfResult(typing().level, unit().function, values.size()), ProofSource::ResultOfCall, origin));
        if (!values.last())
            return;
    }
    endWithReturn(values.span(), origin);
}

Reference Walker::buildCallWithScalars(Draft&& call, const Effects* foundByCaller)
{
    const CallingConventions& conventions = plan().conventions();
    Vector<Reference, 16> operands(call.variadicOperands());
    Origin origin = call.origin();
    origin.isTailCall = false;
    for (const ScalarizedParameter& parameter : conventions.scalarizedParametersOf(call.function())) {
        TypeID held = m_types.forLayout(parameter.layout);
        Reference object = operands[parameter.entryParameter];
        if (object && !m_types.isSubset(typeOf(object), held))
            object = proveOrTrap(object, held, ProofSource::EntryParameter, origin);
        if (!object || isUnreachable())
            return { };
        RELEASE_ASSERT(operands.size() == parameter.firstScalar);
        Reference holder;
        for (uint32_t place = 0; place < parameter.places.size(); ++place) {
            const PlaceOfScalar& scalar = parameter.places[place];
            if (scalar.held && (!place || parameter.places[place - 1].field != scalar.field)) {
                holder = readScalar(object, { parameter.layout, scalar.field }, m_types.forLayout(scalar.held), ProofSource::EntryParameter, origin);
                if (!holder)
                    return { };
                count(StatisticID::ScalarizedHeldObjects);
            }
            operands.append(readScalar(scalar.held ? holder : object, fieldOfPlace(parameter, place), m_positions->scalarOfField(typing().level, fieldOfPlace(parameter, place).layout, fieldOfPlace(parameter, place).field, parameter.forms[place]), ProofSource::EntryParameter, origin));
            if (!operands.last())
                return { };
        }
        operands[parameter.entryParameter] = unspecifiedConstant(held);
        count(StatisticID::ScalarizedArguments);
    }
    call.setVariadicOperands(operands.span());
    return build(WTF::move(call), foundByCaller);
}

void Walker::addParametersOfScalars(BlockID entry)
{
    for (const ScalarizedParameter& parameter : plan().conventions().scalarizedParametersOf(unit().function)) {
        for (uint32_t place = 0; place < parameter.places.size(); ++place)
            addParameter(entry, m_positions->scalarOfField(typing().level, fieldOfPlace(parameter, place).layout, fieldOfPlace(parameter, place).field, parameter.forms[place]), parameter.forms[place]);
    }
}

Reference Walker::scalarReadBy(const Draft& read) const
{
    Reference object = read.objectOperand();
    if (read.level() != KnowledgeLevel::Proven || !object || !object.isValue())
        return { };
    ValueID cell = underlyingCellOf(m_code, object.id());
    for (const ObjectOfScalars& scalars : m_objectsOfScalars) {
        if (scalars.object != cell)
            continue;
        for (uint32_t place = 0; place < scalars.fields.size(); ++place) {
            if (plan().slots().keyOf(program(), { scalars.layout, scalars.fields[place] }) == read.key())
                return scalars.values[place];
        }
    }
    return { };
}

void Walker::allocateScalarizedParameters()
{
    BodyBlock input = m_frames[0].index->block(entryBlock);
    for (const ScalarizedParameter& parameter : plan().conventions().scalarizedParametersOf(unit().function)) {
        BlockID entry = m_code.blockOf(referenceBoundTo(input.parameter(parameter.entryParameter)).id());
        ObjectOfScalars object { { }, parameter.layout, parameter.fields, { } };
        for (uint32_t place = 0; place < parameter.places.size();) {
            const PlaceOfScalar& scalar = parameter.places[place];
            ObjectOfScalars held { { }, scalar.held, { }, { } };
            do {
                held.fields.append(parameter.places[place].fieldOfHeld);
                held.values.append(Reference::value(m_code.at(entry).parameters()[parameter.firstScalar + place]));
            } while (++place < parameter.places.size() && scalar.held && parameter.places[place].field == scalar.field);
            object.values.append(scalar.held ? allocateObjectOfScalars(WTF::move(held)) : held.values[0]);
            if (!object.values.last())
                return;
        }
        Reference allocated = allocateObjectOfScalars(WTF::move(object));
        if (!allocated)
            return;
        bind(input.parameter(parameter.entryParameter), allocated);
    }
}

Reference Walker::allocateObjectOfScalars(ObjectOfScalars&& scalars)
{
    Reference object = buildObjectOfScalars(scalars.layout, Vector<uint32_t, 8> { scalars.fields }, Vector<Reference, 8> { scalars.values }, m_origin);
    if (!object)
        return { };
    scalars.object = object.id();
    m_objectsOfScalars.append(WTF::move(scalars));
    return object;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
