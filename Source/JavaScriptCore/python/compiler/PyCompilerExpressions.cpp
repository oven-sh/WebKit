/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCompilerUnit.h"

#include "JSCInlines.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyRuntime.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTuple.h"
#include <wtf/text/MakeString.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

using namespace AST;

// More items than this in a display or a call are added one at a time, rather than held in as many registers.
static constexpr unsigned maxItemsInRegisters = 64;
// CPython's compiler builds a longer display item by item, and then makes no constant of it.
static constexpr unsigned maxItemsCPythonHasOnItsStack = 30;

// ---- Constants ----

// The values are made by functions of the run time, which can raise, if only for lack of memory. What they raise stays pending until
// Compiler::compile() returns. Until then every constant is None, and nothing above has to know.
JSValue Unit::constantValue(const ConstantValue& value)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    if (scope.exception()) [[unlikely]]
        return jsUndefined();
    JSValue result = makeConstantValue(value);
    return scope.exception() ? jsUndefined() : result;
}

JSValue Unit::makeConstantValue(const ConstantValue& value)
{
    auto scope = DECLARE_THROW_SCOPE(vm());
    switch (value.kind) {
    case ConstantValue::Kind::None:
        return jsUndefined();
    case ConstantValue::Kind::True:
        return jsBoolean(true);
    case ConstantValue::Kind::False:
        return jsBoolean(false);
    case ConstantValue::Kind::Ellipsis:
        return runtime().ellipsis();
    case ConstantValue::Kind::Str:
        return g.addStringConstant(*c.name(String(value.string)));
    case ConstantValue::Kind::Bytes:
        RELEASE_AND_RETURN(scope, PyBytes::create(globalObject(), std::span<const uint8_t> { value.bytes.data, value.bytes.size }));
    case ConstantValue::Kind::Int:
        RELEASE_AND_RETURN(scope, makeInt(globalObject(), value.integer));
    case ConstantValue::Kind::BigInt: {
        JSValue result = intFromDigits(globalObject(), StringView(std::span<const char> { value.digits.data, value.digits.size }), value.radix);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, value.isNegative ? intNegate(globalObject(), result) : result);
    }
    case ConstantValue::Kind::Float:
        RELEASE_AND_RETURN(scope, makeFloat(globalObject(), value.number));
    case ConstantValue::Kind::Complex:
        RELEASE_AND_RETURN(scope, PyComplex::create(globalObject(), value.complex.real, value.complex.imaginary));
    case ConstantValue::Kind::Tuple: {
        PyTuple* tuple = PyTuple::create(globalObject(), static_cast<unsigned>(value.items.size));
        RETURN_IF_EXCEPTION(scope, { });
        for (size_t i = 0; i < value.items.size; ++i) {
            JSValue item = makeConstantValue(value.items.data[i]);
            RETURN_IF_EXCEPTION(scope, { });
            tuple->initializeAt(vm(), i, item);
        }
        return tuple;
    }
    case ConstantValue::Kind::FrozenSet: {
        PySet* set = PySet::createFrozen(globalObject());
        RETURN_IF_EXCEPTION(scope, { });
        for (size_t i = 0; i < value.items.size; ++i) {
            JSValue item = makeConstantValue(value.items.data[i]);
            RETURN_IF_EXCEPTION(scope, { });
            set->add(globalObject(), item);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return set;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Literals, signed numbers, complex numbers written as a sum, and tuples of those.
bool Unit::tryConstantValue(Expr* node, JSValue& result)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    // As in constantValue().
    auto made = [&](JSValue value) {
        result = scope.exception() ? jsUndefined() : value;
        return true;
    };
    switch (node->kind) {
    case ExprKind::Constant:
        return made(constantValue(node->as<Constant>()->value));
    case ExprKind::UnaryOp: {
        auto* unary = node->as<UnaryOp>();
        if (unary->op != AST::UnaryKind::USub && unary->op != AST::UnaryKind::UAdd)
            return false;
        auto* operand = unary->operand->tryAs<Constant>();
        if (!operand)
            return false;
        bool negate = unary->op == AST::UnaryKind::USub;
        switch (operand->value.kind) {
        case ConstantValue::Kind::Int:
        case ConstantValue::Kind::BigInt:
            result = constantValue(operand->value);
            if (negate && !scope.exception())
                return made(intNegate(globalObject(), result));
            return true;
        case ConstantValue::Kind::Float:
            return made(makeFloat(globalObject(), negate ? -operand->value.number : operand->value.number));
        case ConstantValue::Kind::Complex:
            return made(negate ? PyComplex::create(globalObject(), -operand->value.complex.real, -operand->value.complex.imaginary) : constantValue(operand->value));
        default:
            return false;
        }
    }
    case ExprKind::BinOp: {
        auto* binary = node->as<BinOp>();
        if (binary->op != Operator::Add && binary->op != Operator::Sub)
            return false;
        auto* right = binary->right->tryAs<Constant>();
        if (!right || right->value.kind != ConstantValue::Kind::Complex || binary->left->is<BinOp>())
            return false;
        JSValue left;
        if (!tryConstantValue(binary->left, left))
            return false;
        double real;
        if (isSmallInt(left))
            real = static_cast<double>(smallIntValue(left));
        else if (isFloat(left))
            real = floatValue(left);
        else
            return false;
        double sign = binary->op == Operator::Add ? 1 : -1;
        return made(PyComplex::create(globalObject(), real + sign * right->value.complex.real, sign * right->value.complex.imaginary));
    }
    case ExprKind::Tuple: {
        auto* tuple = node->as<Tuple>();
        if (tuple->ctx != ExprContext::Load)
            return false;
        Vector<JSValue, 8> items;
        for (Expr* item : tuple->elts) {
            JSValue value;
            if (!tryConstantValue(item, value))
                return false;
            items.append(value);
        }
        return made(PyTuple::create(globalObject(), items.span()));
    }
    default:
        return false;
    }
}

// To CPython's compiler.
bool Unit::areConstants(Seq<Expr*> nodes)
{
    return !!tupleOfConstants(nodes);
}

// Null: they are not all constants to CPython's compiler.
PyTuple* Unit::tupleOfConstants(Seq<Expr*> nodes)
{
    if (!isSafeToRecurse())
        return nullptr;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    MarkedArgumentBuffer values;
    for (Expr* node : nodes) {
        JSValue value;
        if (!tryConstantValue(node, value))
            value = valueCPythonsCompilerWorksOut(node);
        if (!value) {
            auto* tuple = node->tryAs<Tuple>();
            if (!tuple || tuple->ctx != ExprContext::Load || tuple->elts.size() > maxItemsCPythonHasOnItsStack)
                return nullptr;
            value = tupleOfConstants(tuple->elts);
            if (!value)
                return nullptr;
        }
        values.append(value);
    }
    // As in constantValue().
    if (scope.exception() || values.hasOverflowed())
        return runtime().emptyTuple();
    PyTuple* result = PyTuple::create(globalObject(), values.size());
    for (unsigned i = 0; i < values.size(); ++i)
        result->initializeAt(vm(), i, values.at(i));
    return result;
}

// Null: it cannot be made. Making the set fails again when the code is run.
PySet* Unit::frozensetOf(PyTuple* tuple)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    PySet* set = PySet::createFrozen(globalObject());
    for (unsigned i = 0; !scope.exception() && i < tuple->length(); ++i)
        set->add(globalObject(), tuple->at(i));
    if (scope.exception()) {
        scope.clearException();
        return nullptr;
    }
    return set;
}

// A list or a set that is only iterated over or searched. As CPython's compiler does, this makes one constant of constants, a tuple or a
// frozenset, however few, and a tuple of anything else that would be a list.
RegisterID* Unit::exprToIterateOverOrSearch(Expr* node, RegisterID* dst)
{
    Seq<Expr*> items;
    if (auto* list = node->tryAs<List>())
        items = list->elts;
    else if (auto* set = node->tryAs<Set>())
        items = set->elts;
    if (items.isEmpty())
        return expr(node, dst);
    if (PyTuple* tuple = tupleOfConstants(items)) {
        JSValue value = tuple;
        if (node->is<Set>()) {
            value = frozensetOf(tuple);
            if (!value)
                return expr(node, dst);
        }
        line(node->range);
        noteFoldedConstant(node, value);
        return load(dst, value);
    }
    bool hasStar = false;
    for (Expr* item : items)
        hasStar |= item->is<Starred>();
    if (node->is<List>() && !hasStar && items.size() <= maxItemsCPythonHasOnItsStack)
        return sequence(SequenceKind::Tuple, items, dst, node->range);
    return expr(node, dst);
}

std::optional<bool> Unit::constantTruth(Expr* node)
{
    if (auto* name = node->tryAs<NameExpr>()) {
        if (name->id->impl() == names().debug->impl())
            return !c.m_options.optimize;
        return std::nullopt;
    }
    if (auto* unary = node->tryAs<UnaryOp>(); unary && unary->op == AST::UnaryKind::Not) {
        if (!isSafeToRecurse())
            return std::nullopt;
        auto truth = constantTruth(unary->operand);
        return truth ? std::optional(!*truth) : std::nullopt;
    }
    if (auto* boolOp = node->tryAs<BoolOp>()) {
        if (!isSafeToRecurse())
            return std::nullopt;
        bool decides = boolOp->op == BoolOperator::Or;
        for (Expr* value : boolOp->values) {
            auto truth = constantTruth(value);
            if (!truth)
                return std::nullopt;
            if (*truth == decides)
                return decides;
        }
        return !decides;
    }
    auto* constant = node->tryAs<Constant>();
    if (!constant)
        return std::nullopt;
    auto& value = constant->value;
    switch (value.kind) {
    case ConstantValue::Kind::None:
    case ConstantValue::Kind::False:
        return false;
    case ConstantValue::Kind::True:
    case ConstantValue::Kind::Ellipsis:
    case ConstantValue::Kind::BigInt:
        return true;
    case ConstantValue::Kind::Str:
        return !!value.string->length();
    case ConstantValue::Kind::Bytes:
        return !!value.bytes.size;
    case ConstantValue::Kind::Int:
        return !!value.integer;
    case ConstantValue::Kind::Float:
        return !!value.number;
    case ConstantValue::Kind::Complex:
        return value.complex.real || value.complex.imaginary;
    case ConstantValue::Kind::Tuple:
    case ConstantValue::Kind::FrozenSet:
        return !!value.items.size;
    }
    return std::nullopt;
}

// What CPython's compiler makes of operations on constants. Empty: it leaves them alone, because one of them fails or has a large result (fold_const_binop() and
// safe_multiply() and the like, in its flowgraph.c).
JSValue Unit::valueCPythonsCompilerWorksOut(Expr* node)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    if (scope.exception()) [[unlikely]]
        return { };

    Vector<BinOp*, 8> operations;
    while (auto* operation = node->tryAs<BinOp>()) {
        operations.append(operation);
        node = operation->left;
    }
    auto finish = [&](JSValue result) {
        if (scope.exception()) {
            scope.clearException();
            return JSValue();
        }
        return result;
    };

    JSValue value;
    if (auto* constant = node->tryAs<Constant>())
        value = constantValue(constant->value);
    else if (auto* unary = node->tryAs<UnaryOp>(); unary && isSafeToRecurse()) {
        JSValue operand = valueCPythonsCompilerWorksOut(unary->operand);
        if (operand && (unary->op != AST::UnaryKind::Invert || !operand.isBoolean()))
            value = finish(unaryOperation(globalObject(), static_cast<Python::UnaryOperator>(unary->op), operand));
    }

    while (value && !operations.isEmpty()) {
        BinOp* operation = operations.takeLast();
        JSValue right = isSafeToRecurse() ? valueCPythonsCompilerWorksOut(operation->right) : JSValue();
        if (!right)
            return { };
        value = valueCPythonsCompilerWorksOut(operation->op, value, right);
    }
    return value;
}

JSValue Unit::valueCPythonsCompilerWorksOut(Operator op, JSValue value, JSValue right)
{
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm());
    static constexpr uint64_t maxIntBits = 128;
    static constexpr uint64_t maxStrSize = 4096;
    {
        bool areInts = isInt(value) && isInt(right);
        auto sizeOf = [&](JSValue sequence) -> std::optional<uint64_t> {
            if (isStr(sequence))
                return strLength(globalObject(), strValue(sequence));
            if (auto* bytes = PyBytes::tryFrom(sequence))
                return bytes->length();
            return std::nullopt;
        };
        switch (op) {
        case Operator::MatMult:
            return { };
        case Operator::Mult:
            if (areInts) {
                if (intBitLength(intValue(value)) + intBitLength(intValue(right)) > maxIntBits)
                    return { };
                break;
            }
            for (auto [sequence, count] : { std::pair { value, right }, std::pair { right, value } }) {
                auto size = sizeOf(sequence);
                if (!size || !isInt(count) || !*size)
                    continue;
                auto times = tryInt64(intValue(count));
                if (!times || *times < 0 || static_cast<uint64_t>(*times) > maxStrSize / *size)
                    return { };
            }
            break;
        case Operator::Pow:
            if (areInts && intSign(intValue(value)) && intSign(intValue(right)) > 0) {
                auto exponent = tryUInt64(intValue(right));
                if (!exponent || *exponent > maxIntBits / intBitLength(intValue(value)))
                    return { };
            }
            break;
        case Operator::LShift:
            if (areInts && intSign(intValue(value)) && intSign(intValue(right))) {
                auto shift = tryUInt64(intValue(right));
                if (!shift || *shift > maxIntBits || intBitLength(intValue(value)) > maxIntBits - *shift)
                    return { };
            }
            break;
        case Operator::Mod:
            if (sizeOf(value))
                return { };
            break;
        default:
            break;
        }
    }
    // (A tree that a program made may have tuples for constants, for which CPython has other limits.)
    if (scope.exception() || PyTuple::is(value) || PyTuple::is(right)) {
        scope.clearException();
        return { };
    }
    value = binaryOperation(globalObject(), static_cast<BinaryOperator>(op), value, right);
    if (scope.exception()) {
        scope.clearException();
        return { };
    }
    return value;
}

// ---- Dispatch ----

void Unit::exprForEffect(Expr* node)
{
    if (node->is<Constant>())
        return line(node->range);
    if (auto* yielding = node->tryAs<Yield>(); yielding && isSafeToRecurse()) {
        Reg ignored = yield(yielding, nullptr, true);
        return;
    }
    Reg ignored = expr(node);
}

RegisterID* Unit::expr(Expr* node, RegisterID* dst)
{
    if (!isSafeToRecurse())
        return g.finalDestination(dst);

    // Of anything else, what comes first is a part of it.
    if (node->is<NameExpr>() || node->is<Constant>())
        line(node->range);
    if (node->range.lineno != node->range.endLineno && !m_sameLineDepth && (node->is<BinOp>() || node->is<UnaryOp>()) && valueCPythonsCompilerWorksOut(node)) [[unlikely]] {
        // (CPython's compiler works out `not` by another way than the rest, which leaves something on the line of the operand.)
        Vector<Expr*, 4> operandsOfNot;
        for (auto* unary = node->tryAs<UnaryOp>(); unary && unary->op == AST::UnaryKind::Not; unary = unary->operand->tryAs<UnaryOp>())
            operandsOfNot.append(unary->operand);
        while (!operandsOfNot.isEmpty())
            line(operandsOfNot.takeLast()->range);
        line(node->range);
        SameLine sameLine(*this);
        return expr(node, dst);
    }
    switch (node->kind) {
    case ExprKind::BoolOp:
        return boolOp(node->as<BoolOp>(), dst);
    case ExprKind::NamedExpr:
        return namedExpr(node->as<NamedExpr>(), dst);
    case ExprKind::BinOp:
    case ExprKind::Attribute:
    case ExprKind::Subscript:
        return chain(node, dst);
    case ExprKind::UnaryOp:
        return unaryOp(node->as<UnaryOp>(), dst);
    case ExprKind::Lambda:
        return lambda(node->as<Lambda>(), dst);
    case ExprKind::IfExp:
        return ifExp(node->as<IfExp>(), dst);
    case ExprKind::Dict:
        return dict(node->as<Dict>(), dst);
    case ExprKind::Set:
        return sequence(SequenceKind::Set, node->as<Set>()->elts, dst, node->range);
    case ExprKind::ListComp:
        return comprehension(node, ComprehensionKind::List, node->as<ListComp>()->generators, node->as<ListComp>()->elt, nullptr, dst);
    case ExprKind::SetComp:
        return comprehension(node, ComprehensionKind::Set, node->as<SetComp>()->generators, node->as<SetComp>()->elt, nullptr, dst);
    case ExprKind::DictComp:
        return comprehension(node, ComprehensionKind::Dict, node->as<DictComp>()->generators, node->as<DictComp>()->key, node->as<DictComp>()->value, dst);
    case ExprKind::GeneratorExp:
        return comprehension(node, ComprehensionKind::Generator, node->as<GeneratorExp>()->generators, node->as<GeneratorExp>()->elt, nullptr, dst);
    case ExprKind::Await:
        return await(node->as<Await>(), dst);
    case ExprKind::Yield:
        return yield(node->as<Yield>(), dst);
    case ExprKind::YieldFrom:
        return yieldFrom(node->as<YieldFrom>(), dst);
    case ExprKind::Compare:
        return compare(node->as<Compare>(), dst);
    case ExprKind::Call:
        return call(node->as<Call>(), dst);
    case ExprKind::FormattedValue:
        return formattedValue(node->as<FormattedValue>(), dst);
    case ExprKind::Interpolation:
        return interpolation(node->as<Interpolation>(), dst);
    case ExprKind::JoinedStr:
        return joinedStr(node->as<JoinedStr>(), dst);
    case ExprKind::TemplateStr:
        return templateStr(node->as<TemplateStr>(), dst);
    case ExprKind::Constant: {
        JSValue value = constantValue(node->as<Constant>()->value);
        noteConstant(value);
        return load(dst, value);
    }
    case ExprKind::Starred:
        fail(node->range, node->as<Starred>()->ctx == ExprContext::Store ? "starred assignment target must be in a list or tuple"_s : "can't use starred expression here"_s);
        return g.finalDestination(dst);
    case ExprKind::Name: {
        auto* name = node->as<NameExpr>();
        if (name->id->impl() == names().debug->impl())
            return load(dst, jsBoolean(!c.m_options.optimize));
        return loadName(name->id, dst, name->range);
    }
    case ExprKind::List:
        return sequence(SequenceKind::List, node->as<List>()->elts, dst, node->range);
    case ExprKind::Tuple: {
        JSValue value;
        if (tryConstantValue(node, value)) {
            line(node->range);
            noteFoldedConstant(node, value);
            return load(dst, value);
        }
        return sequence(SequenceKind::Tuple, node->as<Tuple>()->elts, dst, node->range);
    }
    case ExprKind::Slice:
        return slice(node->as<Slice>(), dst);
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// ---- Operators ----

RegisterID* Unit::boolOp(BoolOp* node, RegisterID* dst)
{
    // Not in `dst` until the end: the operands may read the variable it is, and may throw.
    Reg result = g.tempDestination(dst);
    Ref<Label> end = g.newLabel();
    unsigned last = node->values.size() - 1;
    bool isAnd = node->op == BoolOperator::And;
    for (unsigned i = 0; i < last; ++i)
        boolOperand(node->values[i], result.get(), isAnd ? nullptr : end.ptr(), isAnd ? end.ptr() : nullptr, node->range, node->range, end.get(), nullptr);
    // (`b` in `0 and b`.)
    if (!m_cannotBeReached && cannotGetHere())
        deadExpr(node->values[last]);
    else
        expr(node->values[last], result.get());
    if (end->isBound())
        g.emitLabel(end.get());
    return dst ? g.move(dst, result.get()) : result.get();
}

// The value goes in `result`, and then, by its truth, control to one of the labels. One of them is null: straight on.
// No object is asked twice whether it is true: in `a and b or c`, an `a` that is false is the value of `a and b`, known to be false.
// In CPython that is a jump to a jump, which its compiler has go where the second one goes, and then it is of the place of the second: `rangeOfTest`. What drops the
// value, if control goes straight on, is of the expression that the operand is written in: `rangeOfOperation`.
// A constant is not asked. If it settles the expression it is written in, the rest of that is never run, and it goes to the end of that (`whenConstantSettles`), where it is
// tested like any other value if the expression is an operand itself: so CPython has it, for which whatever comes after such a test can be reached.
// `endOfNested`: the operand is the last of an expression that is an operand itself, which ends here, before the test.
void Unit::boolOperand(Expr* node, RegisterID* result, Label* ifTrue, Label* ifFalse, const Range& rangeOfTest, const Range& rangeOfOperation, Label& whenConstantSettles, Label* endOfNested)
{
    ASSERT(!ifTrue != !ifFalse);
    bool isDead = !m_cannotBeReached && cannotGetHere();
    if (isDead) {
        deadExpr(node);
        if (!endOfNested || !endOfNested->isBound())
            return;
    }
    auto* nested = isDead ? nullptr : node->tryAs<BoolOp>();
    if (!nested || !isSafeToRecurse()) {
        if (!isDead)
            expr(node, result);
        if (endOfNested && endOfNested->isBound())
            g.emitLabel(*endOfNested);
        line(rangeOfOperation);
        // (What CPython's compiler works out, `not 1` or `()`, it has worked out too late for this.)
        bool isWrittenAsConstant = node->is<Constant>() || node->is<NameExpr>();
        if (auto truth = endOfNested || !isWrittenAsConstant ? std::nullopt : constantTruth(node)) {
            if (*truth == !!ifTrue)
                jump(whenConstantSettles);
            return;
        }
        loc(rangeOfTest);
        if (ifTrue)
            g.emitPyJumpIfTrue(result, *ifTrue);
        else
            g.emitPyJumpIfFalse(result, *ifFalse);
        line(rangeOfOperation);
        return;
    }
    Ref<Label> next = g.newLabel();
    Ref<Label> ownEnd = g.newLabel();
    Label* end = endOfNested ? endOfNested : ownEnd.ptr();
    unsigned last = nested->values.size() - 1;
    bool isAnd = nested->op == BoolOperator::And;
    // What settles the nested expression early settles the test as well.
    Label* whenSettled = isAnd ? ifFalse : ifTrue;
    const Range& rangeOfNestedTest = whenSettled ? rangeOfTest : nested->range;
    if (!whenSettled)
        whenSettled = next.ptr();
    for (unsigned i = 0; i < last; ++i)
        boolOperand(nested->values[i], result, isAnd ? nullptr : whenSettled, isAnd ? whenSettled : nullptr, rangeOfNestedTest, nested->range, *end, nullptr);
    boolOperand(nested->values[last], result, ifTrue, ifFalse, rangeOfTest, rangeOfOperation, whenConstantSettles, end);
    if (next->isBound()) {
        g.emitLabel(next.get());
        line(rangeOfOperation);
    }
}


// super().name, super(type, object).name. As in CPython, this is one step if nothing in the source says that `super` may be something else
// than the built-in; and it takes the class and the first parameter as any other code would, so that what goes wrong with them does so
// as for any other code.
bool Unit::isAttributeOfSuper(Attribute* node)
{
    auto* call = node->value->tryAs<Call>();
    if (!call || !call->keywords.isEmpty() || node->ctx != ExprContext::Load)
        return false;
    auto* function = call->func->tryAs<NameExpr>();
    if (!function || function->id->impl() != names().super->impl() || node->attr->impl() == names().classCell->impl())
        return false;
    Symbol* symbol = (m_overlays.isEmpty() ? m_scope : m_overlays.last())->find(function->id);
    if (!symbol || symbol->binding != Binding::GlobalImplicit || c.m_symbols->top()->find(function->id))
        return false;
    if (call->args.size() == 2)
        return !call->args[0]->is<Starred>() && !call->args[1]->is<Starred>();
    if (!call->args.isEmpty() || !isFunctionLike() || !m_info.argCount)
        return false;
    Symbol* classCell = m_scope->find(names().classCell);
    return classCell && classCell->binding == Binding::Free;
}

// `self`, unless null: the attribute is to be called, and this is for what to pass it first: see emitPyGetMethod().
RegisterID* Unit::attributeOfSuper(Attribute* node, RegisterID* dst, RegisterID* self)
{
    auto* call = node->value->as<Call>();
    bool isWrittenWithoutArguments = call->args.isEmpty();
    Arguments operands(*this, 4);
    expr(call->func, operands[0]);
    if (isWrittenWithoutArguments) {
        loadName(names().classCell, operands[1], call->range);
        loadName(m_scope->parameters[0], operands[2], call->range);
    } else {
        expr(call->args[0], operands[1]);
        expr(call->args[1], operands[2]);
    }
    g.move(operands[3], usedName(mangle(node->attr)));
    loc(node->range);
    if (!self)
        dst = callRuntime(dst, isWrittenWithoutArguments ? RuntimeFunction::LoadZeroSuperAttr : RuntimeFunction::LoadSuperAttr, operands);
    else {
        callRuntime(dst, isWrittenWithoutArguments ? RuntimeFunction::LoadZeroSuperMethod : RuntimeFunction::LoadSuperMethod, operands);
        g.move(self, operands[2]);
    }
    // (CPython has an instruction that does nothing here, on the line of the name.)
    line(rangeOfAttributeName(node));
    return dst;
}

// a + b + c, a.b.c and a[b][c] nest to the left, as deep as they are long. So that is a loop: down to what is evaluated first,
// and back up.
RegisterID* Unit::chain(Expr* node, RegisterID* dst)
{
    unsigned temporaryOfWhole = takeTemporary(node);
    Vector<Expr*, 8> chain;
    JSValue constant;
    while (true) {
        Expr* first = nullptr;
        switch (node->kind) {
        case ExprKind::BinOp:
            if (!tryConstantValue(node, constant))
                first = node->as<BinOp>()->left;
            break;
        case ExprKind::Attribute:
            if (!isAttributeOfSuper(node->as<Attribute>()))
                first = node->as<Attribute>()->value;
            break;
        case ExprKind::Subscript:
            checkSubscripter(node->as<Subscript>()->value);
            checkIndex(node->as<Subscript>()->value, node->as<Subscript>()->slice);
            first = node->as<Subscript>()->value;
            break;
        default:
            break;
        }
        if (!first)
            break;
        chain.append(node);
        node = first;
    }
    // Operations on constants are worked out now, from the left, for as long as they are on constants: 60 * 60 * x.
    Expr* workedOut = node->is<BinOp>() ? node : nullptr;
    if (!workedOut)
        constant = !chain.isEmpty() && chain.last()->is<BinOp>() && (node->is<Constant>() || node->is<UnaryOp>()) ? valueCPythonsCompilerWorksOut(node) : JSValue();
    while (constant && !chain.isEmpty() && chain.last()->is<BinOp>()) {
        auto* operation = chain.last()->as<BinOp>();
        JSValue right = valueCPythonsCompilerWorksOut(operation->right);
        JSValue value = right ? valueCPythonsCompilerWorksOut(operation->op, constant, right) : JSValue();
        if (!value)
            break;
        constant = value;
        workedOut = operation;
        chain.removeLast();
    }
    if (workedOut) {
        line(workedOut->range);
        noteFoldedConstant(workedOut, constant);
        if (chain.isEmpty())
            return load(dst, constant);
    }

    if (chain.isEmpty()) {
        Reg result = g.finalDestination(dst);
        return attributeOfSuper(node->as<Attribute>(), result.get(), nullptr);
    }

    // The number of what `result` holds, if the next in the chain is its one use and a subscript.
    unsigned temporary = chain.last()->is<Subscript>() ? beginTemporary(node) : 0;
    Reg result;
    if (workedOut)
        result = load(nullptr, constant);
    else if (node->is<Attribute>()) {
        result = newTemporary();
        attributeOfSuper(node->as<Attribute>(), result.get(), nullptr);
    } else
        result = expr(node);
    while (!chain.isEmpty()) {
        node = chain.takeLast();
        RegisterID* finalDst = chain.isEmpty() ? dst : nullptr;
        unsigned used = std::exchange(temporary, 0);
        switch (node->kind) {
        case ExprKind::BinOp: {
            Reg right = expr(node->as<BinOp>()->right);
            loc(node->range);
            result = g.emitPyBinary(g.finalDestination(finalDst, result.get()), result.get(), right.get(), static_cast<BinaryOperator>(node->as<BinOp>()->op));
            break;
        }
        case ExprKind::Attribute:
            loc(rangeOfAttributeName(node->as<Attribute>()));
            result = g.emitPyGetAttr(g.finalDestination(finalDst, result.get()), result.get(), usedName(mangle(node->as<Attribute>()->attr)));
            break;
        case ExprKind::Subscript: {
            Reg key = expr(node->as<Subscript>()->slice);
            loc(node->range);
            markTemporaryUsed(used, TemporaryUse::SubscriptBase, nullptr, true);
            if (chain.isEmpty())
                markTemporaryMade(temporaryOfWhole);
            else if (chain.last()->is<Subscript>()) {
                temporary = ++m_temporaryCount;
                markTemporaryMade(temporary);
            }
            result = g.emitPyGetItem(g.finalDestination(finalDst, result.get()), result.get(), key.get());
            break;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }
    return result.get();
}

RegisterID* Unit::unaryOp(UnaryOp* node, RegisterID* dst)
{
    JSValue value;
    if (!tryConstantValue(node, value))
        value = valueCPythonsCompilerWorksOut(node);
    if (value) {
        line(node->range);
        noteFoldedConstant(node, value);
        return load(dst, value);
    }
    Reg operand = expr(node->operand);
    loc(node->range);
    static_assert(static_cast<unsigned>(AST::UnaryKind::Invert) == static_cast<unsigned>(Python::UnaryOperator::Invert));
    static_assert(static_cast<unsigned>(AST::UnaryKind::USub) == static_cast<unsigned>(Python::UnaryOperator::Negative));
    return g.emitPyUnary(g.finalDestination(dst, operand.get()), operand.get(), static_cast<Python::UnaryOperator>(node->op));
}

RegisterID* Unit::ifExp(IfExp* node, RegisterID* dst)
{
    Reg result = g.finalDestination(dst);
    Ref<Label> orelse = g.newLabel();
    Ref<Label> end = g.newLabel();
    jumpIf(node->test, orelse.get(), false);
    if (!m_cannotBeReached && cannotGetHere())
        deadExpr(node->body);
    else
        expr(node->body, result.get());
    jump(end.get());
    if (orelse->isBound()) {
        g.emitLabel(orelse.get());
        expr(node->orelse, result.get());
    } else
        deadExpr(node->orelse);
    if (end->isBound())
        g.emitLabel(end.get());
    return result.get();
}

RegisterID* Unit::namedExpr(NamedExpr* node, RegisterID* dst)
{
    Reg value = g.tempDestination(dst);
    expr(node->value, value.get());
    store(node->target, value.get());
    return dst ? g.move(dst, value.get()) : value.get();
}

ASCIILiteral Unit::inferTypeName(Expr* node)
{
    switch (node->kind) {
    case ExprKind::Tuple:
        return "tuple"_s;
    case ExprKind::List:
    case ExprKind::ListComp:
        return "list"_s;
    case ExprKind::Dict:
    case ExprKind::DictComp:
        return "dict"_s;
    case ExprKind::Set:
    case ExprKind::SetComp:
        return "set"_s;
    case ExprKind::GeneratorExp:
        return "generator"_s;
    case ExprKind::Lambda:
        return "function"_s;
    case ExprKind::TemplateStr:
    case ExprKind::Interpolation:
        return "string.templatelib.Template"_s;
    case ExprKind::JoinedStr:
    case ExprKind::FormattedValue:
        return "str"_s;
    case ExprKind::Constant:
        switch (node->as<Constant>()->value.kind) {
        case ConstantValue::Kind::None:
            return "NoneType"_s;
        case ConstantValue::Kind::True:
        case ConstantValue::Kind::False:
            return "bool"_s;
        case ConstantValue::Kind::Ellipsis:
            return "ellipsis"_s;
        case ConstantValue::Kind::Str:
            return "str"_s;
        case ConstantValue::Kind::Bytes:
            return "bytes"_s;
        case ConstantValue::Kind::Int:
        case ConstantValue::Kind::BigInt:
            return "int"_s;
        case ConstantValue::Kind::Float:
            return "float"_s;
        case ConstantValue::Kind::Complex:
            return "complex"_s;
        case ConstantValue::Kind::Tuple:
            return "tuple"_s;
        case ConstantValue::Kind::FrozenSet:
            return "frozenset"_s;
        }
        return { };
    default:
        return { };
    }
}

// `x is 1` is most likely a mistake.
void Unit::checkCompare(Compare* node)
{
    auto isFine = [](Expr* operand) {
        if (auto* tuple = operand->tryAs<Tuple>()) {
            for (Expr* item : tuple->elts) {
                if (!item->is<Constant>())
                    return true;
            }
            return false;
        }
        auto* constant = operand->tryAs<Constant>();
        if (!constant)
            return true;
        auto kind = constant->value.kind;
        return kind == ConstantValue::Kind::None || kind == ConstantValue::Kind::True || kind == ConstantValue::Kind::False || kind == ConstantValue::Kind::Ellipsis;
    };
    Expr* leftExpr = node->left;
    bool left = isFine(leftExpr);
    for (unsigned i = 0; i < node->ops.size(); ++i) {
        Expr* rightExpr = node->comparators[i];
        bool right = isFine(rightExpr);
        auto op = node->ops[i];
        if ((op == AST::CompareKind::Is || op == AST::CompareKind::IsNot) && (!left || !right)) {
            if (c.m_warned.add(node).isNewEntry) {
                ASCIILiteral type = inferTypeName(!left ? leftExpr : rightExpr);
                warn(node->range, op == AST::CompareKind::Is ? makeString("\"is\" with '"_s, type, "' literal. Did you mean \"==\"?"_s) : makeString("\"is not\" with '"_s, type, "' literal. Did you mean \"!=\"?"_s));
            }
            return;
        }
        left = right;
        leftExpr = rightExpr;
    }
}

static bool isNoneLiteral(Expr* node)
{
    auto* constant = node->tryAs<Constant>();
    return constant && constant->value.kind == ConstantValue::Kind::None;
}

static bool yieldsBoolean(AST::CompareKind op)
{
    return op >= AST::CompareKind::Is;
}

RegisterID* Unit::emitCompare(RegisterID* dst, RegisterID* left, RegisterID* right, AST::CompareKind op, Expr* rightExpr)
{
    if (rightExpr && isNoneLiteral(rightExpr) && (op == AST::CompareKind::Is || op == AST::CompareKind::IsNot)) {
        g.emitPyIsNone(dst, left);
        if (op == AST::CompareKind::IsNot)
            g.emitPyBooleanNot(dst, dst);
        return dst;
    }
    return g.emitPyCompare(dst, left, right, static_cast<Python::CompareOperator>(op));
}

RegisterID* Unit::compare(Compare* node, RegisterID* dst)
{
    checkCompare(node);
    unsigned last = node->ops.size() - 1;
    Reg left = expr(node->left);
    if (!last) {
        bool searches = node->ops[0] == AST::CompareKind::In || node->ops[0] == AST::CompareKind::NotIn;
        Reg right = isNoneLiteral(node->comparators[0]) ? Reg(constant(jsUndefined())) : Reg(searches ? exprToIterateOverOrSearch(node->comparators[0]) : expr(node->comparators[0]));
        if (isNoneLiteral(node->comparators[0]))
            line(node->comparators[0]->range);
        loc(node->range);
        return emitCompare(g.finalDestination(dst, left.get()), left.get(), right.get(), node->ops[0], node->comparators[0]);
    }

    // a < b < c: b is evaluated once, and c not at all if a < b is false.
    Reg result = g.tempDestination(dst);
    Ref<Label> end = g.newLabel();
    for (unsigned i = 0; i <= last; ++i) {
        // (What is compared with the next is not only searched.)
        bool searches = i == last && (node->ops[i] == AST::CompareKind::In || node->ops[i] == AST::CompareKind::NotIn);
        Reg right = searches ? exprToIterateOverOrSearch(node->comparators[i]) : expr(node->comparators[i]);
        loc(node->range);
        emitCompare(result.get(), left.get(), right.get(), node->ops[i], node->comparators[i]);
        if (i < last)
            g.emitPyJumpIfFalse(result.get(), end.get());
        left = WTF::move(right);
    }
    g.emitLabel(end.get());
    return dst ? g.move(dst, result.get()) : result.get();
}

// What the last branch of a condition is about, if it ends with one.
static Expr* testedLast(Expr* node)
{
    while (true) {
        if (auto* boolOp = node->tryAs<BoolOp>())
            node = boolOp->values.last();
        else if (auto* unaryOp = node->tryAs<UnaryOp>(); unaryOp && unaryOp->op == AST::UnaryKind::Not)
            node = unaryOp->operand;
        else
            return node->is<IfExp>() ? nullptr : node;
    }
}

void Unit::jumpIf(Expr* node, Label& target, bool condition)
{
    // `b` in `a and 0 and b`. A branch would make it look as if its target could be reached from here.
    if (!m_cannotBeReached && cannotGetHere()) [[unlikely]]
        return deadExpr(node);
    if (&target != m_branchTarget.label) [[likely]]
        return jumpIfTo(node, target, condition);
    if (node->is<BoolOp>() || node->is<IfExp>() || (node->is<UnaryOp>() && node->as<UnaryOp>()->op == AST::UnaryKind::Not))
        return jumpIfTo(node, target, condition);
    // (It never goes there.)
    if (auto truth = constantTruth(node); truth && *truth != condition)
        return jumpIfTo(node, target, condition);
    BranchTarget branchTarget = std::exchange(m_branchTarget, { });
    if (branchTarget.loop) {
        Ref<Label> goesOn = g.newLabel();
        jumpIfTo(node, goesOn.get(), !condition);
        if (!cannotGetHere())
            emitImplicitContinue(*branchTarget.loop);
        if (goesOn->isBound())
            g.emitLabel(goesOn.get());
    } else if (branchTarget.loopOfComprehension) {
        Ref<Label> goesOn = g.newLabel();
        jumpIfTo(node, goesOn.get(), !condition);
        if (!cannotGetHere()) {
            line(branchTarget.placeOfJumpThere);
            jump(*branchTarget.loopOfComprehension);
        }
        if (goesOn->isBound()) {
            g.emitLabel(goesOn.get());
            line(branchTarget.placeOfJumpThere);
            // (CPython has a jump here, over what drops the operand of a comparison before the last that fails.)
            if (auto* compare = node->tryAs<Compare>(); compare && compare->ops.size() > 1)
                line(node->range);
        }
    } else if (branchTarget.returns) {
        jumpIfTo(node, labelForLine(*branchTarget.returns, node->range.lineno), condition);
    } else {
        jumpIfTo(node, target, condition);
        line(branchTarget.placeOfJumpThere);
    }
    m_branchTarget = branchTarget;
}

void Unit::jumpIfTo(Expr* node, Label& target, bool condition)
{
    if (!isSafeToRecurse())
        return;

    // (Not what is made up of constants, `0 or 1`: each of them may be on a line of its own, which the frame is on when it gets there.)
    bool isMadeUp = node->is<BoolOp>() || node->is<IfExp>() || (node->is<UnaryOp>() && node->as<UnaryOp>()->op == AST::UnaryKind::Not);
    if (auto truth = isMadeUp ? std::nullopt : constantTruth(node)) {
        line(node->range);
        noteWhatConstantIsMadeOf(node);
        if (*truth == condition)
            jump(target);
        return;
    }

    switch (node->kind) {
    case ExprKind::UnaryOp:
        if (node->as<UnaryOp>()->op == AST::UnaryKind::Not)
            return jumpIf(node->as<UnaryOp>()->operand, target, !condition);
        break;
    case ExprKind::BoolOp: {
        auto* boolOp = node->as<BoolOp>();
        unsigned last = boolOp->values.size() - 1;
        bool shortCircuitOn = boolOp->op == BoolOperator::Or;
        if (shortCircuitOn == condition) {
            for (unsigned i = 0; i <= last; ++i)
                jumpIf(boolOp->values[i], target, condition);
            return;
        }
        Ref<Label> skip = g.newLabel();
        for (unsigned i = 0; i < last; ++i)
            jumpIf(boolOp->values[i], skip.get(), shortCircuitOn);
        jumpIf(boolOp->values[last], target, condition);
        if (skip->isBound())
            g.emitLabel(skip.get());
        return;
    }
    case ExprKind::IfExp: {
        auto* ifExp = node->as<IfExp>();
        Ref<Label> orelse = g.newLabel();
        Ref<Label> end = g.newLabel();
        jumpIf(ifExp->test, orelse.get(), false);
        jumpIf(ifExp->body, target, condition);
        // (Of the line of what was tested last, wherever the branch is said to be.)
        if (Expr* tested = cannotGetHere() ? nullptr : testedLast(ifExp->body))
            line(tested->range);
        jump(end.get());
        if (orelse->isBound())
            g.emitLabel(orelse.get());
        jumpIf(ifExp->orelse, target, condition);
        if (end->isBound())
            g.emitLabel(end.get());
        return;
    }
    case ExprKind::Compare: {
        auto* compare = node->as<Compare>();
        checkCompare(compare);
        unsigned last = compare->ops.size() - 1;
        Ref<Label> isFalse = g.newLabel();
        Reg left = expr(compare->left);
        Reg result = newTemporary();
        for (unsigned i = 0; i <= last; ++i) {
            Expr* rightExpr = compare->comparators[i];
            auto op = compare->ops[i];
            bool isLast = i == last;
            // Where to go, and on what, once this comparison is made.
            Label& destination = isLast ? target : isFalse.get();
            bool jumpOn = isLast ? condition : false;
            if (isNoneLiteral(rightExpr) && (op == AST::CompareKind::Is || op == AST::CompareKind::IsNot)) {
                // The line `None` is written on is one the frame has been on.
                line(rightExpr->range);
                loc(compare->range);
                g.emitPyJumpIfNone(left.get(), destination, (op == AST::CompareKind::Is) == jumpOn);
                left = constant(jsUndefined());
                continue;
            }
            bool searches = isLast && (op == AST::CompareKind::In || op == AST::CompareKind::NotIn);
            Reg right = searches ? exprToIterateOverOrSearch(rightExpr) : expr(rightExpr);
            loc(compare->range);
            if (yieldsBoolean(op)) {
                g.emitPyCompare(result.get(), left.get(), right.get(), static_cast<Python::CompareOperator>(op));
                if (jumpOn)
                    g.emitPyJumpIfTrue(result.get(), destination);
                else
                    g.emitPyJumpIfFalse(result.get(), destination);
            } else
                g.emitPyCompareAndJump(left.get(), right.get(), static_cast<Python::CompareOperator>(op), jumpOn, destination);
            left = WTF::move(right);
        }
        if (last && !condition) {
            // A comparison before the last that is false makes the whole false.
            Ref<Label> end = g.newLabel();
            jump(end.get());
            g.emitLabel(isFalse.get());
            jump(target);
            g.emitLabel(end.get());
        } else
            g.emitLabel(isFalse.get());
        return;
    }
    default:
        break;
    }

    Reg value = expr(node);
    loc(node->range);
    if (condition)
        g.emitPyJumpIfTrue(value.get(), target);
    else
        g.emitPyJumpIfFalse(value.get(), target);
}

// ---- Attributes and items ----

Range Unit::rangeOfAttributeName(Attribute* node)
{
    // For an access that spans lines, where the name is.
    Range range = node->range;
    if (range.lineno != range.endLineno) {
        range.lineno = range.endLineno;
        int length = static_cast<int>(node->attr->string().length());
        range.colOffset = length <= range.endColOffset ? range.endColOffset - length : 0;
    }
    return range;
}

void Unit::checkSubscripter(Expr* node)
{
    switch (node->kind) {
    case ExprKind::Constant:
        switch (node->as<Constant>()->value.kind) {
        case ConstantValue::Kind::Str:
        case ConstantValue::Kind::Bytes:
            return;
        default:
            break;
        }
        break;
    case ExprKind::Set:
    case ExprKind::SetComp:
    case ExprKind::GeneratorExp:
    case ExprKind::TemplateStr:
    case ExprKind::Interpolation:
    case ExprKind::Lambda:
        break;
    default:
        return;
    }
    if (c.m_warned.add(node).isNewEntry)
        warn(node->range, makeString('\'', inferTypeName(node), "' object is not subscriptable; perhaps you missed a comma?"_s));
}

void Unit::checkIndex(Expr* value, Expr* index)
{
    ASCIILiteral indexType = inferTypeName(index);
    if (indexType.isNull() || indexType == "int"_s || indexType == "bool"_s)
        return;
    switch (value->kind) {
    case ExprKind::Constant:
        if (!value->as<Constant>()->value.isString() && !value->as<Constant>()->value.isBytes())
            return;
        break;
    case ExprKind::Tuple:
    case ExprKind::List:
    case ExprKind::ListComp:
    case ExprKind::JoinedStr:
    case ExprKind::FormattedValue:
        break;
    default:
        return;
    }
    if (c.m_warned.add(value).isNewEntry)
        warn(value->range, makeString(inferTypeName(value), " indices must be integers or slices, not "_s, indexType, "; perhaps you missed a comma?"_s));
}

RegisterID* Unit::slice(Slice* node, RegisterID* dst)
{
    JSValue parts[3];
    Expr* expressions[3] = { node->lower, node->upper, node->step };
    bool isConstant = true;
    for (unsigned i = 0; i < 3 && isConstant; ++i) {
        if (!expressions[i])
            parts[i] = jsUndefined();
        else
            isConstant = !expressions[i]->is<Tuple>() && tryConstantValue(expressions[i], parts[i]);
    }
    if (isConstant)
        return load(dst, PySlice::create(globalObject(), parts[0], parts[1], parts[2]));

    Reg result = g.finalDestination(dst);
    Arguments arguments(*this, 3);
    for (unsigned i = 0; i < 3; ++i) {
        if (expressions[i])
            expr(expressions[i], arguments[i]);
        else
            load(arguments[i], jsUndefined());
    }
    loc(node->range);
    return callRuntime(result.get(), RuntimeFunction::BuildSlice, arguments);
}

// ---- Targets ----

void Unit::unpack(Seq<Expr*> targets, RegisterID* value, const Range& range)
{
    unsigned count = targets.size();
    int star = -1;
    for (unsigned i = 0; i < count; ++i) {
        if (!targets[i]->is<Starred>())
            continue;
        if (star >= 0) {
            fail(range, "multiple starred expressions in assignment"_s);
            return;
        }
        if (i >= 256 || count - i - 1 >= (std::numeric_limits<int>::max() >> 8)) {
            fail(range, "too many expressions in star-unpacking assignment"_s);
            return;
        }
        star = i;
    }
    Reg held = value;
    auto targetAt = [&](unsigned i) { return targets[i]->is<Starred>() ? targets[i]->as<Starred>()->value : targets[i]; };

    // Variables in registers that follow one another are a run of registers: the items go where they belong. (The instruction stores
    // nothing unless it has all of them. If the targets are on several lines, a tool that is told of the lines sees them assigned to one by one.)
    RegisterID* firstVariable = nullptr;
    for (unsigned i = 0; i < count && range.lineno == range.endLineno; ++i) {
        auto* name = targetAt(i)->tryAs<NameExpr>();
        Access access = name ? resolve(mangle(name->id)) : Access { Access::Kind::Name };
        if (access.kind != Access::Kind::Fast || (i && access.variable->reg->virtualRegister().offset() != firstVariable->virtualRegister().offset() - static_cast<int>(i))) {
            firstVariable = nullptr;
            break;
        }
        if (!i)
            firstVariable = access.variable->reg;
    }

    Arguments items(*this, firstVariable ? 0 : count);
    RegisterID* first = firstVariable ? firstVariable : items.first();
    loc(range);
    if (star < 0)
        g.emitPyUnpack(first ? first : value, value, count, -1);
    else
        g.emitPyUnpack(first, value, star, count - star - 1);
    for (unsigned i = 0; i < count; ++i)
        store(targetAt(i), firstVariable ? resolve(mangle(targetAt(i)->as<NameExpr>()->id)).variable->reg : items[i]);
}

void Unit::store(Expr* target, RegisterID* value)
{
    if (!isSafeToRecurse())
        return;
    Reg held = value;
    line(target->range);
    switch (target->kind) {
    case ExprKind::Name:
        return storeName(target->as<NameExpr>()->id, value, target->range);
    case ExprKind::Attribute: {
        auto* attribute = target->as<Attribute>();
        // `self.x = ...` in a method: the class lists x in __static_attributes__.
        if (auto* name = attribute->value->tryAs<NameExpr>(); name && name->id->string() == "self"_s) {
            for (Unit* unit = m_parent; unit; unit = unit->m_parent) {
                if (unit->m_kind != UnitKind::Class)
                    continue;
                if (!unit->m_staticAttributes.containsIf([&](Name existing) { return existing->impl() == attribute->attr->impl(); }))
                    unit->m_staticAttributes.append(attribute->attr);
                break;
            }
        }
        Reg base = expr(attribute->value);
        loc(rangeOfAttributeName(attribute));
        g.emitPySetAttr(base.get(), usedName(mangle(attribute->attr)), value);
        return;
    }
    case ExprKind::Subscript: {
        auto* subscript = target->as<Subscript>();
        unsigned temporary = std::exchange(m_storedTemporary, 0);
        Reg base = expr(subscript->value);
        Reg key = expr(subscript->slice);
        loc(subscript->range);
        markTemporaryUsed(temporary, TemporaryUse::StoredValue, base.get(), true);
        g.emitPySetItem(base.get(), key.get(), value);
        return;
    }
    case ExprKind::List:
        return unpack(target->as<List>()->elts, value, target->range);
    case ExprKind::Tuple:
        return unpack(target->as<Tuple>()->elts, value, target->range);
    case ExprKind::Starred:
        fail(target->range, "starred assignment target must be in a list or tuple"_s);
        return;
    default:
        c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, "invalid node type for an assignment target"_s);
    }
}

void Unit::deleteTarget(Expr* target)
{
    switch (target->kind) {
    case ExprKind::Name:
        return deleteName(target->as<NameExpr>()->id, target->range);
    case ExprKind::Attribute: {
        auto* attribute = target->as<Attribute>();
        Reg base = expr(attribute->value);
        loc(rangeOfAttributeName(attribute));
        callRuntimeForEffect(RuntimeFunction::DeleteAttr, { base.get(), usedName(mangle(attribute->attr)) });
        return;
    }
    case ExprKind::Subscript: {
        auto* subscript = target->as<Subscript>();
        Reg base = expr(subscript->value);
        Reg key = expr(subscript->slice);
        loc(subscript->range);
        callRuntimeForEffect(RuntimeFunction::DeleteItem, { base.get(), key.get() });
        return;
    }
    case ExprKind::List:
        for (Expr* item : target->as<List>()->elts)
            deleteTarget(item);
        return;
    case ExprKind::Tuple:
        for (Expr* item : target->as<Tuple>()->elts)
            deleteTarget(item);
        return;
    case ExprKind::Starred:
        fail(target->range, "can't use starred expression here"_s);
        return;
    default:
        c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, "invalid node type for a deletion target"_s);
    }
}

// ---- Displays ----

RegisterID* Unit::sequence(SequenceKind kind, Seq<Expr*> items, RegisterID* dst, const Range& range, std::span<RegisterID* const> pushed, Name injected)
{
    unsigned count = items.size();
    bool hasStar = false;
    for (Expr* item : items)
        hasStar |= item->is<Starred>();
    unsigned total = pushed.size() + count + (injected ? 1 : 0);

    // CPython's compiler makes one constant of three constants or more, and the list or the set from that, all on the first line.
    // It makes constants of more tuples than tryConstantValue() does.
    std::optional<SameLine> sameLine;
    if ((kind == SequenceKind::Tuple || count >= 3) && count <= maxItemsCPythonHasOnItsStack && total == count) {
        PyTuple* tuple = kind != SequenceKind::List || (range.lineno != range.endLineno && !m_sameLineDepth) ? tupleOfConstants(items) : nullptr;
        // The order of a set shows how it was filled: all at once from a frozenset, not item by item.
        JSValue constantToUse;
        if (tuple && kind == SequenceKind::Tuple)
            constantToUse = tuple;
        else if (PySet* set = tuple && kind == SequenceKind::Set ? frozensetOf(tuple) : nullptr)
            constantToUse = set;
        if (constantToUse) {
            line(range);
            for (Expr* item : items)
                noteWhatConstantIsMadeOf(item);
            noteConstantMade(constantToUse);
            if (kind == SequenceKind::Tuple)
                return load(dst, constantToUse);
            Reg result = g.finalDestination(dst);
            loc(range);
            callRuntime(result.get(), RuntimeFunction::BuildSet);
            callRuntimeForEffect(RuntimeFunction::SetUpdate, { result.get(), constant(constantToUse) });
            return result.get();
        }
        if (tuple) {
            line(range);
            sameLine.emplace(*this);
        }
    }

    if (!hasStar && total <= maxItemsInRegisters) {
        // The items are where they are taken from before anything is written.
        Reg result = g.finalDestination(dst);
        Arguments values(*this, total);
        unsigned next = 0;
        for (RegisterID* value : pushed)
            g.move(values[next++], value);
        // CPython adds the items of a long display one by one, which is of the line the display begins on.
        bool isLong = total > maxItemsCPythonHasOnItsStack;
        if (isLong)
            line(range);
        for (Expr* item : items) {
            expr(item, values[next++]);
            if (isLong)
                line(range);
        }
        if (injected)
            loadName(injected, values[next++], range);
        loc(range);
        switch (kind) {
        case SequenceKind::List:
            g.emitPyNewList(result.get(), values.first(), total);
            break;
        case SequenceKind::Tuple:
            callRuntime(result.get(), RuntimeFunction::BuildTuple, values);
            break;
        case SequenceKind::Set:
            callRuntime(result.get(), RuntimeFunction::BuildSet, values);
            break;
        }
        return result.get();
    }

    // A tuple is built as a list. As in CPython's compiler, what comes before the first star is collected first, unless the display is long:
    // whatever makes the list, adds to it or extends it is of the line the display begins on.
    Reg result = g.tempDestination(dst);
    bool isSet = kind == SequenceKind::Set;
    unsigned collected = 0;
    if (total <= maxItemsCPythonHasOnItsStack) {
        while (collected < count && !items[collected]->is<Starred>())
            ++collected;
    }
    if (isSet) {
        ASSERT(pushed.empty());
        Arguments values(*this, collected);
        for (unsigned i = 0; i < collected; ++i)
            expr(items[i], values[i]);
        loc(range);
        callRuntime(result.get(), RuntimeFunction::BuildSet, values);
    } else {
        Vector<Reg, 8> values;
        for (unsigned i = 0; i < collected; ++i)
            values.append(expr(items[i]));
        loc(range);
        // (A list that is made of its items has room for no more, and more are to come.)
        g.emitPyNewList(result.get(), nullptr, 0);
        for (RegisterID* value : pushed)
            g.emitPyListAppend(result.get(), value);
        for (auto& value : values)
            g.emitPyListAppend(result.get(), value.get());
    }
    auto add = [&](RegisterID* value) {
        if (isSet)
            callRuntimeForEffect(RuntimeFunction::SetAdd, { result.get(), value });
        else
            g.emitPyListAppend(result.get(), value);
    };
    for (unsigned i = collected; i < count; ++i) {
        Expr* item = items[i];
        if (auto* starred = item->tryAs<Starred>()) {
            Reg value = expr(starred->value);
            loc(range);
            callRuntimeForEffect(isSet ? RuntimeFunction::SetUpdate : RuntimeFunction::ListExtend, { result.get(), value.get() });
        } else {
            Reg value = expr(item);
            loc(range);
            add(value.get());
        }
    }
    if (injected) {
        Reg value = loadName(injected, nullptr, range);
        add(value.get());
    }
    if (kind == SequenceKind::Tuple)
        callRuntime(result.get(), RuntimeFunction::ListToTuple, { result.get() });
    return dst ? g.move(dst, result.get()) : result.get();
}

RegisterID* Unit::dict(Dict* node, RegisterID* dst)
{
    unsigned count = node->values.size();
    bool hasUnpacking = false;
    for (Expr* key : node->keys)
        hasUnpacking |= !key;

    // CPython's compiler collects the keys and values between two mappings, at most this many pairs at a time, and makes a dict of them. If
    // they are more than it wants to have on its stack, it adds them one by one instead. Whatever makes a dict or adds to one is of the
    // line the display begins on.
    static constexpr unsigned maxPairsAtATime = maxItemsCPythonHasOnItsStack / 2 + 2;
    auto endOfPairs = [&](unsigned begin) {
        unsigned end = begin;
        while (end < count && node->keys[end] && end - begin < maxPairsAtATime)
            ++end;
        return end;
    };
    auto collect = [&](unsigned begin, unsigned end, Arguments& items, unsigned next) {
        bool areAddedOneByOne = (end - begin) * 2 > maxItemsCPythonHasOnItsStack;
        if (areAddedOneByOne)
            line(node->range);
        for (unsigned i = begin; i < end; ++i) {
            expr(node->keys[i], items[next++]);
            expr(node->values[i], items[next++]);
            if (areAddedOneByOne)
                line(node->range);
        }
    };

    if (!hasUnpacking && count * 2 <= maxItemsInRegisters) {
        Reg result = g.finalDestination(dst);
        Arguments items(*this, count * 2);
        for (unsigned i = 0; i < count;) {
            unsigned end = endOfPairs(i);
            collect(i, end, items, i * 2);
            line(node->range);
            i = end;
        }
        loc(node->range);
        return callRuntime(result.get(), RuntimeFunction::BuildMap, items);
    }

    Reg result = g.tempDestination(dst);
    bool isMade = false;
    for (unsigned i = 0; i < count;) {
        if (!node->keys[i]) {
            if (!std::exchange(isMade, true)) {
                loc(node->range);
                callRuntime(result.get(), RuntimeFunction::BuildMap);
            }
            Reg mapping = expr(node->values[i++]);
            loc(node->range);
            callRuntimeForEffect(RuntimeFunction::DictUpdate, { result.get(), mapping.get() });
            continue;
        }
        unsigned end = endOfPairs(i);
        unsigned pairs = end - i;
        Arguments items(*this, 1 + pairs * 2);
        collect(i, end, items, 1);
        i = end;
        loc(node->range);
        if (!std::exchange(isMade, true)) {
            g.emitPyRuntime(result.get(), RuntimeFunction::BuildMap, items[1], pairs * 2);
            continue;
        }
        g.move(items[0], result.get());
        for (unsigned pair = 0; pair < pairs; ++pair) {
            if (pair) {
                g.move(items[1], items[1 + pair * 2]);
                g.move(items[2], items[2 + pair * 2]);
            }
            g.emitPyRuntime(items[1], RuntimeFunction::MapAdd, items[0], 3);
        }
    }
    return dst ? g.move(dst, result.get()) : result.get();
}

// ---- Strings ----

// Only a tree that a program has made can have another.
bool Unit::checkConversion(int conversion)
{
    if (conversion == -1 || conversion == 's' || conversion == 'r' || conversion == 'a')
        return true;
    c.failWithoutLocation(SyntaxErrorInfo::Type::SystemError, makeString("Unrecognized conversion character "_s, conversion));
    return false;
}

RegisterID* Unit::formattedValue(FormattedValue* node, RegisterID* dst)
{
    if (!checkConversion(node->conversion))
        return emptyValue();
    Reg result = g.finalDestination(dst);
    if (node->conversion < 0 && !node->formatSpec) {
        Reg value = expr(node->value);
        loc(node->range);
        return callRuntime(result.get(), RuntimeFunction::FormatSimple, { value.get() });
    }
    Arguments arguments(*this, 3);
    expr(node->value, arguments[0]);
    int conversion = node->conversion < 0 ? 0 : node->conversion;
    load(arguments[1], jsNumber(conversion));
    // The value is converted before what is in the format spec is looked at: f'{x!r:{width}}'.
    auto* spec = node->formatSpec ? node->formatSpec->tryAs<JoinedStr>() : nullptr;
    bool specRunsCode = node->formatSpec && !spec;
    if (spec) {
        for (Expr* part : spec->values)
            specRunsCode |= !part->is<Constant>();
    }
    if (conversion && specRunsCode) {
        loc(node->range);
        g.emitPyRuntime(arguments[0], RuntimeFunction::ConvertValue, arguments[0], 2);
        load(arguments[1], jsNumber(0));
    }
    if (node->formatSpec)
        expr(node->formatSpec, arguments[2]);
    else
        g.move(arguments[2], emptyValue());
    loc(node->range);
    return callRuntime(result.get(), RuntimeFunction::FormatValue, arguments);
}

RegisterID* Unit::joinedStr(JoinedStr* node, RegisterID* dst)
{
    unsigned count = node->values.size();
    if (!count)
        return load(dst, g.addStringConstant(vm().propertyNames->emptyIdentifier));
    if (count == 1)
        return expr(node->values[0], dst);

    if (count <= maxItemsInRegisters) {
        Reg result = g.finalDestination(dst);
        Arguments parts(*this, count);
        for (unsigned i = 0; i < count; ++i)
            expr(node->values[i], parts[i]);
        loc(node->range);
        return callRuntime(result.get(), RuntimeFunction::BuildString, parts);
    }
    // In runs.
    Reg result = g.tempDestination(dst);
    load(result.get(), g.addStringConstant(vm().propertyNames->emptyIdentifier));
    for (unsigned start = 0; start < count; start += maxItemsInRegisters - 1) {
        unsigned runLength = std::min(count - start, maxItemsInRegisters - 1);
        Arguments parts(*this, runLength + 1);
        g.move(parts[0], result.get());
        for (unsigned i = 0; i < runLength; ++i)
            expr(node->values[start + i], parts[i + 1]);
        loc(node->range);
        callRuntime(result.get(), RuntimeFunction::BuildString, parts);
    }
    return dst ? g.move(dst, result.get()) : result.get();
}

RegisterID* Unit::interpolation(Interpolation* node, RegisterID* dst)
{
    if (!checkConversion(node->conversion))
        return emptyValue();
    Reg result = g.finalDestination(dst);
    Arguments arguments(*this, 4);
    expr(node->value, arguments[0]);
    load(arguments[1], constantValue(node->str));
    load(arguments[2], jsNumber(node->conversion < 0 ? 0 : node->conversion));
    if (node->formatSpec)
        expr(node->formatSpec, arguments[3]);
    else
        g.move(arguments[3], emptyValue());
    loc(node->range);
    return callRuntime(result.get(), RuntimeFunction::BuildInterpolation, arguments);
}

RegisterID* Unit::templateStr(TemplateStr* node, RegisterID* dst)
{
    // The strings around the interpolations, one more than there are of those; then the interpolations.
    Reg result = g.finalDestination(dst);
    Arguments arguments(*this, 2);
    JSValue emptyString = g.addStringConstant(vm().propertyNames->emptyIdentifier);

    g.emitPyNewList(arguments[0], nullptr, 0);
    auto addString = [&](RegisterID* value) {
        g.emitPyListAppend(arguments[0], value);
    };
    bool lastWasInterpolation = true;
    for (Expr* value : node->values) {
        if (value->is<Interpolation>()) {
            if (lastWasInterpolation)
                addString(constant(emptyString));
            lastWasInterpolation = true;
        } else {
            Reg string = expr(value);
            addString(string.get());
            lastWasInterpolation = false;
        }
    }
    if (lastWasInterpolation)
        addString(constant(emptyString));
    callRuntime(arguments[0], RuntimeFunction::ListToTuple, { arguments[0] });

    g.emitPyNewList(arguments[1], nullptr, 0);
    for (Expr* value : node->values) {
        if (!value->is<Interpolation>())
            continue;
        Reg item = expr(value);
        g.emitPyListAppend(arguments[1], item.get());
    }
    callRuntime(arguments[1], RuntimeFunction::ListToTuple, { arguments[1] });

    loc(node->range);
    return callRuntime(result.get(), RuntimeFunction::BuildTemplate, arguments);
}

// ---- Calls ----

void Unit::validateKeywords(Seq<Keyword*> keywords)
{
    for (unsigned i = 0; i < keywords.size(); ++i) {
        if (!keywords[i]->arg)
            continue;
        for (unsigned j = i + 1; j < keywords.size(); ++j) {
            if (keywords[j]->arg && keywords[j]->arg->impl() == keywords[i]->arg->impl()) {
                fail(keywords[j]->range, makeString("keyword argument repeated: "_s, keywords[i]->arg->string()));
                return;
            }
        }
    }
}

void Unit::checkCaller(Expr* node)
{
    switch (node->kind) {
    case ExprKind::Constant:
    case ExprKind::Tuple:
    case ExprKind::List:
    case ExprKind::ListComp:
    case ExprKind::Dict:
    case ExprKind::DictComp:
    case ExprKind::Set:
    case ExprKind::SetComp:
    case ExprKind::GeneratorExp:
    case ExprKind::JoinedStr:
    case ExprKind::TemplateStr:
    case ExprKind::FormattedValue:
    case ExprKind::Interpolation:
        if (c.m_warned.add(node).isNewEntry)
            warn(node->range, makeString('\'', inferTypeName(node), "' object is not callable; perhaps you missed a comma?"_s));
        return;
    default:
        return;
    }
}

// Whether the name is bound by an import at the top of the module: what is called through it is a function, not a method.
bool Unit::looksLikeModule(Expr* node)
{
    auto* name = node->tryAs<NameExpr>();
    return name && (c.m_symbols->top()->flagsOf(name->id) & DefImport);
}

// Point, _Node, collections.OrderedDict
bool Unit::looksLikeClass(Expr* node)
{
    Name name = nullptr;
    if (auto* variable = node->tryAs<NameExpr>())
        name = variable->id;
    else if (auto* attribute = node->tryAs<Attribute>(); attribute && looksLikeModule(attribute->value))
        name = attribute->attr;
    if (!name)
        return false;
    StringView text = name->string();
    unsigned i = 0;
    while (i < text.length() && text[i] == '_')
        ++i;
    return i < text.length() && isASCIIUpper(text[i]);
}

unsigned Unit::beginTemporary(Expr* node)
{
    if (!makesTemporary(node))
        return 0;
    m_temporaryNode = node;
    m_temporaryNumber = ++m_temporaryCount;
    return m_temporaryNumber;
}

unsigned Unit::takeTemporary(Expr* node)
{
    if (m_temporaryNode != node)
        return 0;
    m_temporaryNode = nullptr;
    return m_temporaryNumber;
}

void Unit::markTemporaryMade(unsigned number)
{
    if (number)
        g.addPythonMark(PythonMark::TemporaryMade, number);
}

void Unit::markTemporaryUsed(unsigned number, TemporaryUse::Kind kind, RegisterID* reg, bool isLast)
{
    if (number)
        g.addPythonMark(PythonMark::TemporaryUsed, number, TemporaryUse::encode(kind, (reg ? reg->virtualRegister() : VirtualRegister()).offset(), isLast));
}

RegisterID* Unit::emitSimpleCall(RegisterID* dst, RegisterID* callee, std::initializer_list<RegisterID*> args, const Range& range, unsigned temporary, unsigned temporaryToGoThrough)
{
    Reg heldCallee = callee;
    Vector<Reg, 4> held;
    for (RegisterID* argument : args)
        held.append(argument);
    CallArguments arguments(g, nullptr, args.size());
    load(arguments.thisRegister(), jsUndefined());
    unsigned i = 0;
    for (RegisterID* argument : args)
        g.move(arguments.argumentRegister(i++), argument);
    loc(range);
    markTemporaryUsed(temporaryToGoThrough, TemporaryUse::ForHeader, nullptr, true);
    markTemporaryMade(temporary);
    return g.emitPyCall(dst, callee, arguments, args.size());
}

RegisterID* Unit::emitMethodCall(RegisterID* dst, RegisterID* callee, RegisterID* self, std::initializer_list<RegisterID*> args, const Range& range)
{
    Reg heldCallee = callee;
    Reg heldSelf = self;
    Vector<Reg, 4> held;
    for (RegisterID* argument : args)
        held.append(argument);
    unsigned count = args.size();
    CallArguments arguments(g, nullptr, count + 1);
    load(arguments.thisRegister(), jsUndefined());
    g.move(arguments.argumentRegister(0), self);
    unsigned next = 1;
    for (RegisterID* argument : args)
        g.move(arguments.argumentRegister(next++), argument);

    Ref<Label> notMethod = g.newLabel();
    Ref<Label> done = g.newLabel();
    g.emitPyJumpIfEmpty(self, notMethod.get());
    loc(range);
    g.emitPyCall(dst, callee, arguments, count + 1);
    hiddenJump(done.get());
    g.emitLabel(notMethod.get());
    for (unsigned i = 0; i < count; ++i)
        g.move(arguments.argumentRegister(i), arguments.argumentRegister(i + 1));
    loc(range);
    g.emitPyCall(dst, callee, arguments, count);
    g.emitLabel(done.get());
    return dst;
}

static bool isSimpleCall(Seq<Expr*> args, Seq<Keyword*> keywords, unsigned extra)
{
    if (args.size() + keywords.size() + extra > maxItemsInRegisters)
        return false;
    for (Expr* argument : args) {
        if (argument->is<Starred>())
            return false;
    }
    for (Keyword* keyword : keywords) {
        if (!keyword->arg)
            return false;
    }
    return true;
}

RegisterID* Unit::emitCall(RegisterID* dst, RegisterID* callee, std::span<RegisterID* const> pushed, Seq<Expr*> args, Seq<Keyword*> keywords, Name injected, const Range& range, unsigned temporary)
{
    validateKeywords(keywords);
    Reg heldCallee = callee;
    Reg result = g.finalDestination(dst);

    if (isSimpleCall(args, keywords, pushed.size() + 1)) {
        unsigned count = pushed.size() + args.size() + (injected ? 1 : 0) + keywords.size();
        CallArguments arguments(g, nullptr, count);
        unsigned next = 0;
        for (RegisterID* value : pushed)
            g.move(arguments.argumentRegister(next++), value);
        Vector<unsigned, 8> temporaries;
        for (Expr* argument : args) {
            temporaries.append(beginTemporary(argument));
            expr(argument, arguments.argumentRegister(next++));
        }
        if (injected)
            loadName(injected, arguments.argumentRegister(next++), range);
        for (Keyword* keyword : keywords) {
            temporaries.append(beginTemporary(keyword->value));
            expr(keyword->value, arguments.argumentRegister(next++));
        }
        load(arguments.thisRegister(), keywordNames(keywords));
        loc(range);
        for (unsigned number : temporaries)
            markTemporaryUsed(number, TemporaryUse::Argument, callee, true);
        markTemporaryMade(temporary);
        return g.emitPyCall(result.get(), callee, arguments, count);
    }

    // callee(*positional, **named)
    bool hasOnlyMapping = keywords.size() == 1 && !keywords[0]->arg;
    Arguments operands(*this, hasOnlyMapping ? 4 : 3);
    g.move(operands[0], callee);
    if (pushed.empty() && !injected && args.size() == 1 && args[0]->is<Starred>())
        expr(args[0]->as<Starred>()->value, operands[1]);
    else
        sequence(SequenceKind::List, args, operands[1], range, pushed, injected);

    if (keywords.isEmpty())
        g.move(operands[2], emptyValue());
    else if (hasOnlyMapping) {
        // It is only read: callArguments(). In CPython a dict is made here, on the line the call begins on.
        line(range);
        g.move(operands[2], emptyValue());
        expr(keywords[0]->value, operands[3]);
    } else {
        RegisterID* named = operands[2];
        bool haveDict = false;
        for (unsigned i = 0; i < keywords.size();) {
            if (!keywords[i]->arg) {
                if (!haveDict) {
                    loc(range);
                    callRuntime(named, RuntimeFunction::BuildMap);
                    haveDict = true;
                }
                Reg mapping = expr(keywords[i]->value);
                loc(range);
                callRuntimeForEffect(RuntimeFunction::DictMerge, { named, mapping.get(), callee });
                ++i;
                continue;
            }
            unsigned end = i;
            while (end < keywords.size() && keywords[end]->arg && end - i < maxItemsInRegisters / 2)
                ++end;
            Arguments items(*this, (end - i) * 2);
            // In CPython the names, and whatever makes a dict or adds to one, are of the line the call begins on.
            for (unsigned j = i; j < end; ++j) {
                line(range);
                g.move(items[(j - i) * 2], nameConstant(keywords[j]->arg));
                expr(keywords[j]->value, items[(j - i) * 2 + 1]);
            }
            loc(range);
            if (!haveDict) {
                callRuntime(named, RuntimeFunction::BuildMap, items);
                haveDict = true;
            } else {
                // A name that a mapping also gave is for DictMerge to find.
                callRuntime(items[0], RuntimeFunction::BuildMap, items);
                callRuntimeForEffect(RuntimeFunction::DictMerge, { named, items[0], callee });
            }
            i = end;
        }
    }
    loc(range);
    Reg array = newTemporary();
    callRuntime(array.get(), RuntimeFunction::CallArguments, operands);
    Reg thisValue = newTemporary();
    if (keywords.isEmpty())
        load(thisValue.get(), jsUndefined());
    else
        callRuntime(thisValue.get(), RuntimeFunction::TakeKeywordNames);
    markTemporaryMade(temporary);
    return g.emitPyCallVarargs(result.get(), callee, thisValue.get(), array.get());
}

// What a call passes as `this`.
JSValue Unit::keywordNames(Seq<Keyword*> keywords)
{
    if (keywords.isEmpty())
        return jsUndefined();
    auto* names = PyTuple::createUninitialized(vm(), runtime().keywordNamesStructure(), runtime().tupleType(), keywords.size());
    for (unsigned i = 0; i < keywords.size(); ++i)
        names->initializeAt(vm(), i, g.addStringConstant(*keywords[i]->arg));
    return names;
}

RegisterID* Unit::call(Call* node, RegisterID* dst, RegisterID* calleeRegister)
{
    unsigned temporary = takeTemporary(node);
    validateKeywords(node->keywords);

    // object.name(...): if name turns out to be a function of the class, it is called with the object as its first argument,
    // and no bound method is made.
    auto* method = node->func->tryAs<Attribute>();
    if (method && !looksLikeModule(method->value) && isSimpleCall(node->args, node->keywords, 2)) {
        Reg result = g.finalDestination(dst);
        Reg callee = calleeRegister ? Reg(calleeRegister) : newTemporary();
        unsigned count = node->args.size() + node->keywords.size();
        CallArguments arguments(g, nullptr, count + 1);
        bool isOfSuper = isAttributeOfSuper(method);
        unsigned receiver = isOfSuper ? 0 : beginTemporary(method->value);
        RegisterID* name = nullptr;
        if (isOfSuper)
            attributeOfSuper(method, callee.get(), arguments.argumentRegister(0));
        else {
            Reg base = expr(method->value);
            loc(rangeOfAttributeName(method));
            name = usedName(mangle(method->attr));
            g.emitPyGetMethod(callee.get(), arguments.argumentRegister(0), base.get(), name);
        }
        unsigned next = 1;
        Vector<unsigned, 8> temporaries;
        for (Expr* argument : node->args) {
            temporaries.append(beginTemporary(argument));
            expr(argument, arguments.argumentRegister(next++));
        }
        for (Keyword* keyword : node->keywords) {
            temporaries.append(beginTemporary(keyword->value));
            expr(keyword->value, arguments.argumentRegister(next++));
        }
        load(arguments.thisRegister(), keywordNames(node->keywords));
        auto markCall = [&](bool isLast) {
            markTemporaryUsed(receiver, TemporaryUse::Receiver, name, isLast);
            for (unsigned number : temporaries)
                markTemporaryUsed(number, TemporaryUse::Argument, callee.get(), isLast);
            markTemporaryMade(temporary);
        };

        Range range = node->range;
        if (range.lineno != method->range.endLineno) {
            Range nameRange = rangeOfAttributeName(method);
            range.lineno = nameRange.lineno;
            range.colOffset = nameRange.colOffset;
        }
        loc(range);

        Ref<Label> notMethod = g.newLabel();
        Ref<Label> done = g.newLabel();
        g.emitPyJumpIfEmpty(arguments.argumentRegister(0), notMethod.get());
        loc(range);
        markCall(false);
        g.emitPyCall(result.get(), callee.get(), arguments, count + 1);
        hiddenJump(done.get());
        g.emitLabel(notMethod.get());
        for (unsigned i = 0; i < count; ++i)
            g.move(arguments.argumentRegister(i), arguments.argumentRegister(i + 1));
        loc(range);
        markCall(true);
        g.emitPyCall(result.get(), callee.get(), arguments, count);
        g.emitLabel(done.get());
        return result.get();
    }

    // Name(...): by its name, a class. If it turns out to be one whose instances are made in the common way, the code makes the
    // instance and calls __init__ as it calls any function, and the call does not leave the interpreter.
    if (looksLikeClass(node->func) && isSimpleCall(node->args, node->keywords, 2)) {
        Reg result = g.finalDestination(dst);
        Reg callee = calleeRegister ? Reg(calleeRegister) : newTemporary();
        expr(node->func, callee.get());
        // The frame of the callee begins right after the arguments: what outlives the call comes before them.
        Reg init = newTemporary();
        Reg self = newTemporary();
        unsigned count = node->args.size() + node->keywords.size();
        CallArguments arguments(g, nullptr, count + 1);
        unsigned next = 0;
        for (Expr* argument : node->args)
            expr(argument, arguments.argumentRegister(next++));
        for (Keyword* keyword : node->keywords)
            expr(keyword->value, arguments.argumentRegister(next++));
        load(arguments.thisRegister(), keywordNames(node->keywords));

        Ref<Label> notSuchAClass = g.newLabel();
        Ref<Label> done = g.newLabel();
        loc(node->range);
        g.emitPyPrepareConstruct(init.get(), self.get(), callee.get());
        g.emitPyJumpIfEmpty(self.get(), notSuchAClass.get());
        for (unsigned i = count; i--;)
            g.move(arguments.argumentRegister(i + 1), arguments.argumentRegister(i));
        g.move(arguments.argumentRegister(0), self.get());
        loc(node->range);
        g.emitPyCall(init.get(), init.get(), arguments, count + 1);
        g.emitPyFinishConstruct(result.get(), self.get(), init.get());
        hiddenJump(done.get());
        g.emitLabel(notSuchAClass.get());
        loc(node->range);
        markTemporaryMade(temporary);
        g.emitPyCall(result.get(), callee.get(), arguments, count);
        g.emitLabel(done.get());
        return result.get();
    }

    checkCaller(node->func);
    Reg callee = calleeRegister ? Reg(calleeRegister) : newTemporary();
    expr(node->func, callee.get());

    if (auto* name = node->func->tryAs<NameExpr>(); name && node->args.size() == 1 && node->keywords.isEmpty() && node->args[0]->is<GeneratorExp>() && !c.m_symbols->scopeFor(node->args[0])->isCoroutine) {
        auto& id = name->id->string();
        if (id == "all"_s)
            return callOnGeneratorExpression(node, callee.get(), runtime().slot(Runtime::Slot::BuiltinAll), GoesThrough::All, dst, temporary);
        if (id == "any"_s)
            return callOnGeneratorExpression(node, callee.get(), runtime().slot(Runtime::Slot::BuiltinAny), GoesThrough::Any, dst, temporary);
        if (id == "tuple"_s)
            return callOnGeneratorExpression(node, callee.get(), runtime().tupleType(), GoesThrough::Tuple, dst, temporary);
        if (id == "list"_s)
            return callOnGeneratorExpression(node, callee.get(), runtime().listType(), GoesThrough::List, dst, temporary);
        if (id == "set"_s)
            return callOnGeneratorExpression(node, callee.get(), runtime().setType(), GoesThrough::Set, dst, temporary);
    }
    return emitCall(dst, callee.get(), { }, node->args, node->keywords, nullptr, node->range, temporary);
}

// all(x for x in y): as CPython's compiler has it, if the name turns out to be bound to the builtin, the code goes through the generator
// itself. There is no call for a profiler to see, the loop is one that a trace function sees, and all of it comes from the name: that
// is what a traceback underlines when the generator raises.
RegisterID* Unit::callOnGeneratorExpression(Call* node, RegisterID* callee, JSValue builtin, GoesThrough what, RegisterID* dst, unsigned temporary)
{
    const Range& range = node->func->range;
    bool collects = what != GoesThrough::All && what != GoesThrough::Any;
    Ref<Label> isSomethingElse = g.newLabel();
    Ref<Label> loop = g.newLabel();
    Ref<Label> exhausted = g.newLabel();
    Ref<Label> done = g.newLabel();
    Reg result = g.finalDestination(dst);

    loc(range);
    {
        Reg isBuiltin = newTemporary();
        g.emitPyCompare(isBuiltin.get(), callee, constant(builtin), CompareOperator::Is);
        g.emitPyJumpIfFalse(isBuiltin.get(), isSomethingElse.get());
    }
    {
        // The generator expression may use the variable that the result is for.
        Reg collected = newTemporary();
        if (what == GoesThrough::Set)
            callRuntime(collected.get(), RuntimeFunction::BuildSet);
        else if (collects)
            g.emitPyNewList(collected.get(), nullptr, 0);
        Reg generator = newTemporary();
        expr(node->args[0], generator.get());
        Reg item = newTemporary();
        g.settlePythonLineMark();
        g.emitLabel(loop.get());
        line(range);
        loc(range);
        g.emitPyForIter(item.get(), generator.get(), exhausted.get());
        if (what == GoesThrough::Set)
            callRuntimeForEffect(RuntimeFunction::SetAdd, { collected.get(), item.get() });
        else if (collects)
            g.emitPyListAppend(collected.get(), item.get());
        else {
            Ref<Label> isDecided = g.newLabel();
            if (what == GoesThrough::All)
                g.emitPyJumpIfFalse(item.get(), isDecided.get());
            else
                g.emitPyJumpIfTrue(item.get(), isDecided.get());
            jump(loop.get());
            g.emitLabel(isDecided.get());
            // In CPython nothing refers to the generator any more.
            callRuntimeForEffect(RuntimeFunction::DropGenerator, { generator.get() });
            g.move(result.get(), constant(jsBoolean(what == GoesThrough::Any)));
            jump(done.get());
        }
        if (collects)
            jump(loop.get());
        g.emitLabel(exhausted.get());
        silentLine(range);
        if (what == GoesThrough::Tuple)
            callRuntime(result.get(), RuntimeFunction::ListToTuple, { collected.get() });
        else if (collects)
            g.move(result.get(), collected.get());
        else
            g.move(result.get(), constant(jsBoolean(what == GoesThrough::All)));
        jump(done.get());
    }
    g.emitLabel(isSomethingElse.get());
    emitCall(result.get(), callee, { }, node->args, node->keywords, nullptr, node->range, temporary);
    g.emitLabel(done.get());
    return result.get();
}

// ---- Generators and coroutines ----

RegisterID* Unit::emitYield(RegisterID* dst, RegisterID* value)
{
    Reg held = value;
    g.emitPythonYieldPoint(value);
    // Resumed: by send(), or by throw().
    Ref<Label> normal = g.newLabel();
    g.emitPyJumpIfFalse(g.generatorResumeModeRegister(), normal.get());
    callRuntimeForEffect(RuntimeFunction::ThrowIntoGenerator, { g.generatorValueRegister() });
    g.emitLabel(normal.get());
    return dst ? g.move(dst, g.generatorValueRegister()) : nullptr;
}

RegisterID* Unit::yield(Yield* node, RegisterID* dst, bool isForEffect)
{
    if (!isFunctionLike()) {
        fail(node->range, "'yield' outside function"_s);
        return g.finalDestination(dst);
    }
    Reg value = node->value ? expr(node->value) : constant(jsUndefined());
    loc(node->range);
    if (isAsyncGenerator())
        value = callRuntime(g.finalDestination(nullptr, value.get()), RuntimeFunction::AsyncGenWrap, { value.get() });
    if (isForEffect) {
        emitYield(nullptr, value.get());
        return value.get();
    }
    Reg result = g.finalDestination(dst, value.get());
    return emitYield(result.get(), value.get());
}

RegisterID* Unit::emitDelegate(RegisterID* dst, RegisterID* iterator, int firstMode)
{
    Reg heldResult = dst;
    Reg delegate = newTemporary();
    Reg yielded = newTemporary();
    g.move(delegate.get(), iterator);
    Ref<Label> suspend = g.newLabel();
    Ref<Label> done = g.newLabel();
    {
        Reg value = newTemporary();
        Reg mode = newTemporary();
        load(value.get(), jsUndefined());
        load(mode.get(), jsNumber(firstMode));
        g.emitPySend(yielded.get(), delegate.get(), value.get(), mode.get(), done.get());
    }
    g.emitLabel(suspend.get());
    g.emitPythonYieldPoint(yielded.get());
    // Whatever comes in goes on to the iterator.
    g.emitPySend(yielded.get(), delegate.get(), g.generatorValueRegister(), g.generatorResumeModeRegister(), done.get());
    hiddenJump(suspend.get());
    g.emitLabel(done.get());
    return g.move(dst, yielded.get());
}

RegisterID* Unit::yieldFrom(YieldFrom* node, RegisterID* dst)
{
    if (!isFunctionLike()) {
        fail(node->range, "'yield from' outside function"_s);
        return g.finalDestination(dst);
    }
    if (m_kind == UnitKind::AsyncFunction) {
        fail(node->range, "'yield from' inside async function"_s);
        return g.finalDestination(dst);
    }
    Reg result = g.tempDestination(dst);
    // As CPython's optimizer has it, a list that is only there to be gone through is a tuple, which gi_yieldfrom shows.
    auto* list = node->value->tryAs<List>();
    bool isPlainList = list && list->elts.size() <= 30;
    if (isPlainList) {
        for (Expr* item : list->elts)
            isPlainList &= !item->is<Starred>();
    }
    if (isPlainList)
        sequence(SequenceKind::Tuple, list->elts, result.get(), list->range);
    else
        expr(node->value, result.get());
    loc(node->range);
    callRuntime(result.get(), RuntimeFunction::GetYieldFromIter, { result.get() });
    emitDelegate(result.get(), result.get());
    return dst ? g.move(dst, result.get()) : result.get();
}

RegisterID* Unit::emitAwait(RegisterID* dst, RegisterID* awaitable, int where)
{
    return emitDelegate(dst, awaitable, firstSendOfAwait + where);
}

RegisterID* Unit::await(Await* node, RegisterID* dst)
{
    Reg result = g.tempDestination(dst);
    expr(node->value, result.get());
    loc(node->range);
    emitAwait(result.get(), result.get(), 0);
    return dst ? g.move(dst, result.get()) : result.get();
}

// ---- Comprehensions ----

void Unit::comprehensionElement(ComprehensionKind kind, Expr* element, Expr* value, RegisterID* result)
{
    auto* starred = element->tryAs<Starred>();
    switch (kind) {
    case ComprehensionKind::Generator: {
        if (starred) {
            // (*x for x in y) yields the items of each x.
            Ref<Label> start = g.newLabel();
            Ref<Label> end = g.newLabel();
            Reg iterator = newTemporary();
            expr(starred->value, iterator.get());
            loc(element->range);
            g.emitPyGetIter(iterator.get(), iterator.get());
            Reg item = newTemporary();
            g.emitLabel(start.get());
            g.emitPyForIter(item.get(), iterator.get(), end.get());
            if (isAsyncGenerator())
                callRuntime(item.get(), RuntimeFunction::AsyncGenWrap, { item.get() });
            emitYield(nullptr, item.get());
            jump(start.get());
            g.emitLabel(end.get());
            return;
        }
        Reg item = newTemporary();
        expr(element, item.get());
        loc(element->range);
        if (isAsyncGenerator())
            callRuntime(item.get(), RuntimeFunction::AsyncGenWrap, { item.get() });
        emitYield(nullptr, item.get());
        return;
    }
    case ComprehensionKind::List:
    case ComprehensionKind::Set: {
        bool isList = kind == ComprehensionKind::List;
        if (isList && !starred) {
            Reg item = expr(element);
            loc(element->range);
            g.emitPyListAppend(result, item.get());
            return;
        }
        Arguments arguments(*this, 2);
        g.move(arguments[0], result);
        expr(starred ? starred->value : element, arguments[1]);
        loc(element->range);
        callRuntime(arguments[1], starred ? (isList ? RuntimeFunction::ListExtend : RuntimeFunction::SetUpdate) : RuntimeFunction::SetAdd, arguments);
        return;
    }
    case ComprehensionKind::Dict: {
        if (!value) {
            // {**x for x in y}
            Arguments arguments(*this, 2);
            g.move(arguments[0], result);
            expr(element, arguments[1]);
            loc(element->range);
            callRuntime(arguments[1], RuntimeFunction::DictUpdate, arguments);
            return;
        }
        Arguments arguments(*this, 3);
        g.move(arguments[0], result);
        expr(element, arguments[1]);
        expr(value, arguments[2]);
        loc({ element->range.lineno, element->range.colOffset, value->range.endLineno, value->range.endColOffset });
        callRuntime(arguments[1], RuntimeFunction::MapAdd, arguments);
        return;
    }
    case ComprehensionKind::None:
        break;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// `firstIterator`: for the first clause, the iterator of its iterable.
void Unit::comprehensionGenerator(ComprehensionKind kind, Seq<Comprehension*> generators, unsigned index, Expr* element, Expr* value, RegisterID* result, RegisterID* firstIterator, bool ownsFirstIterator, const Range& range)
{
    if (!isSafeToRecurse())
        return;
    Comprehension* generator = generators[index];
    Ref<Label> start = g.newLabel();
    Ref<Label> next = g.newLabel();
    Ref<Label> end = g.newLabel();
    Label* loop = start.ptr();
    Range placeOfElement = element->range;
    if (value) {
        placeOfElement.endLineno = value->range.endLineno;
        placeOfElement.endColOffset = value->range.endColOffset;
    }

    auto emitInner = [&] {
        for (Expr* condition : generator->ifs) {
            // CPython's compiler has the branches go where the jump at `next` goes, and then they are of the line of that jump.
            SetForScope branchTarget(m_branchTarget, BranchTarget { next.ptr(), placeOfElement, nullptr, { }, loop });
            jumpIf(condition, next.get(), false);
        }
        if (index + 1 < generators.size()) {
            comprehensionGenerator(kind, generators, index + 1, element, value, result, nullptr, false, range);
            // So nothing jumps to where the inner loop goes on when it has ended.
            if (!generator->ifs.isEmpty()) {
                line(placeOfElement);
                jump(start.get());
            }
        } else
            comprehensionElement(kind, element, value, result);
    };

    if (generator->isAsync) {
        Reg iterator = firstIterator;
        if (!iterator) {
            iterator = newTemporary();
            expr(generator->iter, iterator.get());
            loc(generator->iter->range);
            callRuntime(iterator.get(), RuntimeFunction::GetAIterOfLoop, { iterator.get() });
        }
        g.settlePythonLineMark();
        Try whole = beginTry(HandlerType::SynthesizedCatch);
        g.emitLabel(start.get());
        line(range);
        Try guard = beginTry();
        Reg item = newTemporary();
        loc(range);
        callRuntime(item.get(), RuntimeFunction::GetANext, { iterator.get() });
        emitDelegate(item.get(), item.get());
        endTry(guard);
        store(generator->target, item.get());
        item = nullptr;
        emitInner();
        g.emitLabel(next.get());
        line(placeOfElement);
        jump(start.get());

        Reg thrown = beginHandler(guard);
        silentLine(range);
        loc(range);
        callRuntimeForEffect(RuntimeFunction::EndAsyncFor, { thrown.get(), iterator.get() });
        thrown = nullptr;
        endTry(whole);
        hiddenJump(end.get());
        asyncForHandler(whole, iterator.get());
        g.emitLabel(end.get());
        return;
    }

    if (!firstIterator) {
        // `for y in [f(x)]` is an assignment.
        Seq<Expr*> single;
        if (auto* list = generator->iter->tryAs<List>())
            single = list->elts;
        else if (auto* tuple = generator->iter->tryAs<Tuple>())
            single = tuple->elts;
        if (single.size() == 1 && !single[0]->is<Starred>()) {
            Reg item = expr(single[0]);
            store(generator->target, item.get());
            item = nullptr;
            loop = nullptr;
            emitInner();
            g.emitLabel(next.get());
            return;
        }
    }

    Reg iterator = firstIterator;
    bool ownsIterator = ownsFirstIterator;
    if (!iterator) {
        iterator = newTemporary();
        unsigned temporary = beginTemporaryOfLoop(generator->iter);
        Reg iterable = exprToIterateOverOrSearch(generator->iter);
        loc(generator->iter->range);
        markTemporaryUsed(temporary, TemporaryUse::ForHeader, nullptr, true);
        g.emitPyGetIter(iterator.get(), iterable.get());
        ownsIterator = !!temporary;
    }
    g.settlePythonLineMark();
    if (ownsIterator)
        pushBlock({ .kind = BlockKind::ForLoop, .tryDepth = g.tryDepth(), .saved = iterator.get(), .ownsIterator = true });
    g.emitLabel(start.get());
    line(generator->iter->range);
    loc(generator->iter->range);
    bool stored = false;
    if (auto* target = generator->target->tryAs<NameExpr>()) {
        Access access = resolve(mangle(target->id));
        if (access.kind == Access::Kind::Fast) {
            g.emitPyForIter(access.variable->reg, iterator.get(), end.get());
            noteBound(*access.variable);
            stored = true;
        }
    }
    if (!stored) {
        Reg item = newTemporary();
        g.emitPyForIter(item.get(), iterator.get(), end.get());
        store(generator->target, item.get());
    } else
        line(generator->target->range);
    emitInner();
    g.emitLabel(next.get());
    line(placeOfElement);
    jump(start.get());
    if (ownsIterator)
        popBlock();
    g.emitLabel(end.get());
    // A generator expression returns from there.
    if (kind == ComprehensionKind::Generator && !index)
        line(generator->iter->range);
}

RegisterID* Unit::comprehension(Expr* node, ComprehensionKind kind, Seq<Comprehension*> generators, Expr* element, Expr* value, RegisterID* dst)
{
    Scope* scope = c.m_symbols->scopeFor(node);
    Comprehension* outermost = generators[0];
    const Range& range = node->range;
    unsigned temporary = takeTemporary(node);

    auto emitContainer = [&](Unit& unit, RegisterID* result) {
        if (kind != ComprehensionKind::Generator)
            unit.line(range.lineno);
        switch (kind) {
        case ComprehensionKind::List:
            unit.g.emitPyNewList(result, nullptr, 0);
            break;
        case ComprehensionKind::Set:
            unit.callRuntime(result, RuntimeFunction::BuildSet);
            break;
        case ComprehensionKind::Dict:
            unit.callRuntime(result, RuntimeFunction::BuildMap);
            break;
        default:
            break;
        }
    };

    if (scope->isInlinedComprehension) {
        Reg result = g.tempDestination(dst);
        // The first iterable is evaluated where the comprehension is.
        Reg iterator = newTemporary();
        // (CPython has something between this and the call of iter(), so it is a list if it is written as one.)
        // Nothing of the program runs in between: a variable can stay where it is.
        unsigned temporaryOfIterable = outermost->isAsync ? 0 : beginTemporaryOfLoop(outermost->iter);
        Reg iterable = outermost->iter->is<NameExpr>() ? expr(outermost->iter) : expr(outermost->iter, iterator.get());

        VariableMap& variables = *m_variables.get(scope);
        // (In the order of the symbols. That of the table goes by addresses: the same source would not always give the same code.)
        auto forEachVariable = [&](auto&& function) {
            for (auto& symbol : scope->symbols) {
                if (auto it = variables.find(symbol.name->impl()); it != variables.end())
                    function(it->value);
            }
        };
        // Here CPython puts aside what variables of the same names hold.
        forEachVariable([&](Variable& variable) {
            noteUse(variable);
        });
        auto resetVariables = [&] {
            forEachVariable([&](Variable& variable) {
                g.move(variable.reg, emptyValue());
                noteUnbound(variable);
            });
        };
        forEachVariable([&](Variable& variable) {
            if (variable.kind == Variable::Kind::Cell)
                callRuntime(variable.reg, RuntimeFunction::MakeCell, { emptyValue() });
        });

        // Its variables have no value once it is over, however it ends.
        std::optional<Try> guard;
        if (!variables.isEmpty())
            guard = beginTry();
        loc(outermost->iter->range);
        if (outermost->isAsync)
            callRuntime(iterator.get(), RuntimeFunction::GetAIterOfLoop, { iterable.get() });
        else {
            markTemporaryUsed(temporaryOfIterable, TemporaryUse::ForHeader, nullptr, true);
            g.emitPyGetIter(iterator.get(), iterable.get());
        }
        iterable = nullptr;
        m_overlays.append(scope);
        pushBlock({ .kind = BlockKind::InlinedComprehension, .tryDepth = guard ? guard->depthBefore : g.tryDepth(), .comprehension = scope });
        emitContainer(*this, result.get());
        comprehensionGenerator(kind, generators, 0, element, value, result.get(), iterator.get(), !!temporaryOfIterable, range);
        line(range);
        popBlock();
        m_overlays.removeLast();
        if (guard) {
            endTry(*guard);
            resetVariables();
            Ref<Label> end = g.newLabel();
            hiddenJump(end.get());
            // In CPython the handler drops the result so far, which is of no line, and puts back what the variables were before, on the line
            // of the comprehension.
            Reg thrown = beginHandler(*guard);
            resetVariables();
            g.addPythonMark(PythonMark::NoLine, afterNoLineMark);
            line(range);
            reraise(thrown.get());
            g.emitLabel(end.get());
            // No mark: it would also be for what jumps to a label that comes next (`a or [...]`), from a line of its own.
            isStillOnLine(range);
        }
        return dst ? g.move(dst, result.get()) : result.get();
    }

    // A function of its own, called on the spot with the first iterable.
    Name name = kind == ComprehensionKind::Generator ? names().genexpr : kind == ComprehensionKind::List ? names().listcomp : kind == ComprehensionKind::Set ? names().setcomp : names().dictcomp;
    CodeSpec spec { UnitKind::Comprehension, scope, name, range, range.lineno };
    bool isGenerator = kind == ComprehensionKind::Generator;
    // The generator expression has the only reference to what was made for it to go through, in a variable: GeneratorsOwnedByFrame.
    bool ownsFirstIterator = isGenerator && !outermost->isAsync && mayMakeGenerator(outermost->iter);
    if (isGenerator)
        spec.iterateFirstParameter = outermost->isAsync ? CodeSpec::IterateFirstParameter::Async : CodeSpec::IterateFirstParameter::Sync;
    auto* executable = c.compileCode(this, spec, m_source, [&](Unit& unit) {
        unit.m_info.hasLoopThatOwnsItsIterator |= ownsFirstIterator;
        Reg iterator = isGenerator ? unit.m_firstIterator : unit.loadName(names().dotZero, unit.newTemporary().get(), range);
        if (!isGenerator) {
            unit.loc(outermost->iter->range);
            if (outermost->isAsync)
                unit.callRuntime(iterator.get(), RuntimeFunction::GetAIterOfLoop, { iterator.get() });
            else
                unit.g.emitPyGetIter(iterator.get(), iterator.get());
        }
        Reg result = unit.newTemporary();
        emitContainer(unit, result.get());
        unit.comprehensionGenerator(kind, generators, 0, element, value, result.get(), iterator.get(), false, range);
        if (isGenerator)
            unit.emitReturnNone();
        else
            unit.g.emitReturn(result.get());
    });

    Reg result = g.finalDestination(dst);
    Reg function = newTemporary();
    loc(range);
    emitMakeFunction(function.get(), executable, scope, { });
    unsigned temporaryOfIterable = ownsFirstIterator ? beginTemporaryOfLoop(outermost->iter) : 0;
    Reg iterable = outermost->isAsync ? expr(outermost->iter) : exprToIterateOverOrSearch(outermost->iter);
    emitSimpleCall(result.get(), function.get(), { iterable.get() }, range, temporary, temporaryOfIterable);
    if (scope->isCoroutine && !isGenerator)
        emitAwait(result.get(), result.get(), 0);
    return result.get();
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
