/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyDecimalNumber.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyNumbers.h"
#include "PyObject.h"
#include "PyStr.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

class PyModule;

// The module _decimal, which Lib/decimal.py prefers to Lib/_pydecimal.py. (Modules/_decimal/_decimal.c in CPython is the reference for
// what it does.) The arithmetic is in PyDecimalNumber.h and knows nothing of Python. Here are the objects: PyDecimal.cpp (what the types
// share, the module), PyDecimalType.cpp (Decimal), PyDecimalContext.cpp (Context, the mappings of its signals, localcontext()).
namespace Decimal {

// ---- The objects ----

// It never changes. The limbs of the coefficient follow the cell.
class PyDecimal final : public PyObject {
public:
    PY_DECLARE_CELL(PyDecimal, PyObject, PyObjectType);

    static size_t allocationSize(int64_t length) { return sizeof(PyDecimal) + static_cast<size_t>(length) * sizeof(Limb); }

    // Null: there is no memory for it.
    static PyDecimal* tryCreate(VM&, Structure*, PyType*, const Number&);

    const Limb* limbs() const { return storageBehind<const Limb>(this); }
    int64_t digits() const { return m_length ? digitCount(limbs()[m_length - 1]) + (static_cast<int64_t>(m_length) - 1) * digitsPerLimb : 0; }
    // The number, for as long as the cell is alive: it reads the limbs where they are, and a pointer to them keeps no cell alive. So whoever
    // allocates cells or calls out before it has read them for the last time says ensureStillAliveHere() after that.
    void lend(Number& number) const { number.borrow(m_flags, m_exponent, digits(), m_length, limbs()); }

    uint8_t flags() const { return m_flags; }
    int64_t exponent() const { return m_exponent; }
    uint32_t length() const { return m_length; }
    bool isSpecial() const { return m_flags & Number::anySpecial; }
    bool isNaN() const { return m_flags & Number::anyNaN; }
    bool isZero() const { return !isSpecial() && !limbs()[m_length - 1]; }

    int64_t cachedHash { -1 }; // -1: not known.

private:
    PyDecimal(VM&, Structure*, PyType*, const Number&);

    Limb* limbs() { return storageBehind<Limb>(this); }

    int64_t m_exponent;
    uint32_t m_length;
    uint8_t m_flags;
};

// The traps and the flags of a context, as conditions. Never seen from Python. They are apart from the context because its two mappings read and
// write them as well: so those need not refer to the context, which refers to them, and a context is not part of a cycle. (In CPython
// the mappings point into the context without that counting as a reference.)
class PyDecimalSignals final : public PyObject {
public:
    PY_DECLARE_CELL(PyDecimalSignals, PyObject, PyObjectType);

    static PyDecimalSignals* create(VM&, Structure*, PyType*);

    uint32_t traps { invalidOperationSignal | DivisionByZero | Overflow };
    uint32_t status { 0 };

private:
    PyDecimalSignals(VM&, Structure*, PyType*);
};

class PyDecimalContext final : public PyObject {
public:
    PY_DECLARE_CELL(PyDecimalContext, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyDecimalContext* create(VM&, Structure*, PyType*);

    Context context;
    int capitals { 1 };
    WriteBarrier<PyDecimalSignals> signals;
    // The mappings context.traps and context.flags, which read and write `signals`.
    WriteBarrier<Unknown> traps;
    WriteBarrier<Unknown> flags;

private:
    PyDecimalContext(VM&, Structure*, PyType*);
};

// ---- What there is one of for each runtime ----

// The signals, in the order in which they are looked through and listed.
enum Signal : unsigned {
    SignalInvalidOperation,
    SignalFloatOperation,
    SignalDivisionByZero,
    SignalOverflow,
    SignalUnderflow,
    SignalSubnormal,
    SignalInexact,
    SignalRounded,
    SignalClamped,
    numberOfSignals
};
extern const uint32_t conditionsOfSignal[numberOfSignals];
extern const ASCIILiteral namesOfSignals[numberOfSignals];

// The conditions that are reported as InvalidOperation and have classes of their own, derived from it.
enum InvalidCondition : unsigned {
    InvalidConversionSyntax,
    InvalidDivisionImpossible,
    InvalidDivisionUndefined,
    InvalidInvalidContext,
    numberOfInvalidConditions
};

extern const ASCIILiteral namesOfRoundings[numberOfRoundings];

class State final : public PyObject {
public:
    PY_DECLARE_CELL(State, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static State* create(VM&, Structure*, PyType*);

    WriteBarrier<PyType> decimalType;
    WriteBarrier<PyType> contextType;
    WriteBarrier<PyType> contextManagerType;
    WriteBarrier<PyType> signalsType;
    WriteBarrier<PyType> signalDictMixinType;
    WriteBarrier<PyType> signalDictType;
    WriteBarrier<PyType> decimalTupleType;
    WriteBarrier<PyType> decimalException;
    WriteBarrier<PyType> signals[numberOfSignals];
    WriteBarrier<PyType> invalidConditions[numberOfInvalidConditions];
    WriteBarrier<PyTuple> signalTuple;
    WriteBarrier<JSString> roundings[numberOfRoundings];

    WriteBarrier<Structure> decimalStructure;
    WriteBarrier<Structure> contextStructure;
    WriteBarrier<Structure> signalsStructure;

    WriteBarrier<PyDecimalContext> defaultContext;
    WriteBarrier<PyDecimalContext> basicContext;
    WriteBarrier<PyDecimalContext> extendedContext;
    WriteBarrier<Unknown> contextVariable;
    WriteBarrier<Unknown> rational; // numbers.Rational
    WriteBarrier<Unknown> pythonDecimal; // _pydecimal.Decimal, once it has been needed.

    // What the context variable was found to hold when Runtime::contextVariablesVersion() was this.
    WriteBarrier<PyDecimalContext> cachedContext;
    uint64_t cachedContextVersion { 0 };

private:
    State(VM&, Structure*, PyType*);
};

State* createState(JSGlobalObject*); // Null: an exception.
// Once the module has been imported, which it has when there is an object of one of its types.
ALWAYS_INLINE State& stateOf(JSGlobalObject* globalObject)
{
    return *uncheckedDowncast<State>(runtime(globalObject).slot(Runtime::Slot::Decimal).asCell());
}

extern const TypeDef decimalTypeDef;
extern const TypeDef contextTypeDef;
extern const TypeDef contextManagerTypeDef;
extern const TypeDef signalDictMixinTypeDef;

// ---- Contexts ----

PyDecimalContext* currentContextSlow(JSGlobalObject*, State&);
// decimal.getcontext(). Null: an exception.
ALWAYS_INLINE PyDecimalContext* currentContext(JSGlobalObject* globalObject, State& state)
{
    if (state.cachedContextVersion == runtime(globalObject).contextVariablesVersion()) [[likely]] {
        if (auto* context = state.cachedContext.get()) [[likely]]
            return context;
    }
    return currentContextSlow(globalObject, state);
}
// decimal.setcontext(). False: an exception.
bool setCurrentContext(JSGlobalObject*, State&, JSValue context);
// A parameter `context=None`. Null: a TypeError.
PyDecimalContext* contextArgument(JSGlobalObject*, State&, JSValue);
// Context(), and context.copy(). Null: an exception.
PyDecimalContext* newContext(JSGlobalObject*, State&, PyType*);
PyDecimalContext* copyContext(JSGlobalObject*, State&, PyDecimalContext*);
// What Context(...) and localcontext(...) are given, each a value, None or empty. False: an exception.
struct ContextSettings {
    JSValue precision;
    JSValue rounding;
    JSValue emin;
    JSValue emax;
    JSValue capitals;
    JSValue clamp;
    JSValue flags;
    JSValue traps;
};
bool applyContextSettings(JSGlobalObject*, State&, PyDecimalContext*, const ContextSettings&);
// One of the ROUND_ strings. False: a TypeError.
bool roundingArgument(JSGlobalObject*, State&, JSValue, Rounding&);

// The conditions that an operation met become flags of the context. True: one of them is trapped, or there was no memory, and that has been raised.
bool addStatusSlow(JSGlobalObject*, State&, PyDecimalContext*, uint32_t status);
ALWAYS_INLINE bool addStatus(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, uint32_t status)
{
    PyDecimalSignals* signals = context->signals.get();
    signals->status |= status;
    if (status & (signals->traps | OutOfMemory)) [[unlikely]]
        return addStatusSlow(globalObject, state, context, status);
    return false;
}

// ---- Numbers and objects ----

// A Decimal, or an instance of the class derived from it. Empty: a MemoryError.
JSValue makeDecimal(JSGlobalObject*, State&, PyType*, const Number&);
ALWAYS_INLINE JSValue makeDecimal(JSGlobalObject* globalObject, State& state, const Number& number) { return makeDecimal(globalObject, state, state.decimalType.get(), number); }
// The result of an operation, once its conditions are in the context. Empty: an exception.
ALWAYS_INLINE JSValue finish(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, const Number& result, uint32_t status)
{
    if (addStatus(globalObject, state, context, status)) [[unlikely]]
        return { };
    return makeDecimal(globalObject, state, result);
}

// An int, within the limits of the context, and exactly: what is too long for any Decimal is a NaN then, and InvalidOperation.
void numberFromInt(Number&, JSValue exactInt, const Context&, uint32_t& status);
void numberFromIntExactly(Number&, JSValue exactInt, uint32_t& status);
// A float, exactly.
void numberFromDouble(Number&, double, uint32_t& status);

enum class Conversion : uint8_t { Done, NotImplemented, Raised };
// An operand: a Decimal or an int. `raises`: anything else is a TypeError.
Conversion convertOperandSlow(JSGlobalObject*, State&, PyDecimalContext*, JSValue, Number&, bool raises);
ALWAYS_INLINE Conversion convertOperand(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue value, Number& number, bool raises)
{
    if (auto* decimal = PyDecimal::tryFrom(value)) [[likely]] {
        decimal->lend(number);
        return Conversion::Done;
    }
    return convertOperandSlow(globalObject, state, context, value, number, raises);
}

// Decimal(value), which takes no notice of the limits of the context, and context.create_decimal(value), which does. An empty value is 0.
JSValue decimalFromObjectExactly(JSGlobalObject*, State&, PyType*, JSValue, PyDecimalContext*);
JSValue decimalFromObject(JSGlobalObject*, State&, JSValue, PyDecimalContext*);
// Decimal.from_float(value) and context.create_decimal_from_float(value): a float or an int.
JSValue decimalFromFloat(JSGlobalObject*, State&, JSValue, PyDecimalContext*, bool isExact);

// The number as text, between the two other texts. Null: a MemoryError.
JSString* decimalToStr(JSGlobalObject*, const Number&, unsigned textFlags, ASCIILiteral before = ""_s, ASCIILiteral after = ""_s);
ALWAYS_INLINE unsigned scientific(PyDecimalContext* context) { return TextScientific | (context->capitals ? TextUpperCase : 0); }

// ---- Operations on whatever can be an operand ----

using UnaryOperation = void (*)(Number&, const Number&, const Context&, uint32_t&);
using BinaryOperation = void (*)(Number&, const Number&, const Number&, const Context&, uint32_t&);

// These are for methods, where what is no operand is a TypeError.
template<UnaryOperation operation>
ALWAYS_INLINE JSValue applyUnary(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue x)
{
    Number a;
    if (convertOperand(globalObject, state, context, x, a, true) != Conversion::Done) [[unlikely]]
        return { };
    Number result;
    uint32_t status = 0;
    operation(result, a, context->context, status);
    return finish(globalObject, state, context, result, status);
}

template<BinaryOperation operation>
ALWAYS_INLINE JSValue applyBinary(JSGlobalObject* globalObject, State& state, PyDecimalContext* context, JSValue x, JSValue y)
{
    Number a;
    Number b;
    if (convertOperand(globalObject, state, context, x, a, true) != Conversion::Done || convertOperand(globalObject, state, context, y, b, true) != Conversion::Done) [[unlikely]]
        return { };
    Number result;
    uint32_t status = 0;
    operation(result, a, b, context->context, status);
    return finish(globalObject, state, context, result, status);
}

// Operations that take no notice of the context, made to look as if they did.
inline void copySignInContext(Number& result, const Number& a, const Number& b, const Context&, uint32_t& status) { copySign(result, a, b, status); }
inline void compareTotalInContext(Number& result, const Number& a, const Number& b, const Context&, uint32_t&) { compareTotal(result, a, b); }
inline void compareTotalMagnitudeInContext(Number& result, const Number& a, const Number& b, const Context&, uint32_t&) { compareTotalMagnitude(result, a, b); }

// What Decimal and Context both have.
JSValue applyFusedMultiplyAdd(JSGlobalObject*, State&, PyDecimalContext*, JSValue x, JSValue y, JSValue z);
JSValue applyDivMod(JSGlobalObject*, State&, PyDecimalContext*, JSValue x, JSValue y, bool raises);
JSValue applyPower(JSGlobalObject*, State&, PyDecimalContext*, JSValue base, JSValue exponent, JSValue modulus, bool raises);

} // namespace Decimal
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
