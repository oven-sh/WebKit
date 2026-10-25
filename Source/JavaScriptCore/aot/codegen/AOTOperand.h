/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTID.h"
#include "AOTOutput.h"
#include "AOTRepresentation.h"
#include "AOTSlotRepresentation.h"
#include "AOTVector.h"
#include "JSCJSValue.h"
#include <type_traits>

namespace JSC { namespace AOT { namespace Typed {

inline LType b3TypeOf(Representation representation)
{
    switch (representation) {
    case Representation::Void:
        return B3::Void;
    case Representation::Boolean:
    case Representation::Int32:
    case Representation::Cell32:
        return B3::Int32;
    case Representation::Int64:
    case Representation::OptionalInt64:
    case Representation::Cell:
    case Representation::JSValue:
    case Representation::LiteralString:
    case Representation::RawWord:
        return B3::Int64;
    case Representation::Double:
    case Representation::OptionalDouble:
        return B3::Double;
    case Representation::Unchosen:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<Representation chosenRepresentation>
class Lowered {
public:
    static constexpr Representation representation = chosenRepresentation;

    Lowered() = default;
    explicit Lowered(LValue value)
        : m_value(value)
    {
    }

    LValue value() const { return m_value; }
    explicit operator bool() const { return m_value; }

private:
    LValue m_value { nullptr };
};

using Condition = Lowered<Representation::Boolean>;
using Word32 = Lowered<Representation::Int32>;
using Integer = Lowered<Representation::Int64>;
using Number = Lowered<Representation::Double>;
using OptionalInteger = Lowered<Representation::OptionalInt64>;
using OptionalNumber = Lowered<Representation::OptionalDouble>;
using CellPointer = Lowered<Representation::Cell>;
using NarrowReference = Lowered<Representation::Cell32>;
using Boxed = Lowered<Representation::JSValue>;
using LiteralStringWord = Lowered<Representation::LiteralString>;
using Pointer = Lowered<Representation::RawWord>;

struct Operand {
    Operand() = default;
    Operand(Representation representation, LValue value)
        : representation(representation)
        , value(value)
    {
    }
    template<Representation chosenRepresentation>
    Operand(Lowered<chosenRepresentation> lowered)
        : representation(chosenRepresentation)
        , value(lowered.value())
    {
    }

    template<typename T>
    T as() const
    {
        RELEASE_ASSERT(representation == T::representation);
        return T(value);
    }

    template<typename T>
    bool is() const { return representation == T::representation; }

    Representation representation { Representation::Void };
    LValue value { nullptr };
};

class OperandJoin {
public:
    OperandJoin(Output& out, Representation representation)
        : m_out(out)
        , m_representation(representation)
        , m_continuation(out.newBlock())
    {
    }

    void add(Operand operand)
    {
        RELEASE_ASSERT(operand.representation == m_representation);
        if (m_representation != Representation::Void)
            m_values.append(m_out.anchor(operand.value));
        m_out.jump(m_continuation);
    }

    LBasicBlock continuation() const { return m_continuation; }

    Operand finish()
    {
        m_out.appendTo(m_continuation);
        if (m_representation == Representation::Void)
            return { };
        return Operand(m_representation, m_out.phi(b3TypeOf(m_representation), m_values));
    }

private:
    Output& m_out;
    Representation m_representation;
    LBasicBlock m_continuation;
    Vector<ValueFromBlock, 4> m_values;
};

template<typename T>
class Join {
public:
    explicit Join(Output& out)
        : m_join(out, T::representation)
    {
    }

    void add(T value) { m_join.add(value); }
    LBasicBlock continuation() const { return m_join.continuation(); }
    T finish() { return m_join.finish().template as<T>(); }

private:
    OperandJoin m_join;
};

static_assert(sizeof(Operand) == 16);
static_assert(std::is_trivially_copyable_v<Operand>);
static_assert(!absentCell32 && nullCell32 == JSValue::ValueNull && undefinedCell32 == JSValue::ValueUndefined);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
