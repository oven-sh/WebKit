/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTLowering.h"

#if ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))

#include "B3ValueInlines.h"
#include "BytecodeStructs.h"
#include "DateInstance.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSMap.h"
#include "JSPropertyNameEnumerator.h"
#include "JSSet.h"
#include "MathCommon.h"
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

using namespace B3;

static ASCIILiteral pathOf(Builtin builtin)
{
    switch (builtin) {
#define AOT_BUILTIN_PATH(name, path) \
    case Builtin::name: \
        return path ""_s;
    FOR_EACH_AOT_BUILTIN(AOT_BUILTIN_PATH)
#undef AOT_BUILTIN_PATH
    case Builtin::None:
        break;
    }
    return ""_s;
}

LValue Lowering::isReceiverKind(Node* baseNode, LValue base, Receiver receiver)
{
    switch (receiver) {
    case Receiver::None:
        break;
    case Receiver::Number:
        RELEASE_ASSERT(isSubtype(baseNode->type, TNumber));
        return nullptr;
    case Receiver::String:
        if (isSubtype(baseNode->type, TString))
            return nullptr;
        return isCellAnd(baseNode, base, [&](LValue cell) { return isCellOfType(cell, StringType); });
    case Receiver::Array:
        return isCellAnd(baseNode, base, [&](LValue cell) { return isOriginalArray(cell); });
    case Receiver::Map:
    case Receiver::Set:
    case Receiver::WeakMap:
    case Receiver::WeakSet:
    case Receiver::RegExp:
    case Receiver::Date:
        return isCellAnd(baseNode, base, [&](LValue cell) {
            return m_out.equal(m_out.load32(cell, m_heaps.JSCell_structureID), fixed32(Instance::offsetOfReceiverStructureIDs() + static_cast<unsigned>(receiver) * sizeof(uint32_t)));
        });
    }
    RELEASE_ASSERT_NOT_REACHED();
    return nullptr;
}

bool Lowering::receiverMayHaveChangedSince(Node* read, Node* call) const
{
    auto& nodes = m_block->nodes;
    if (m_nodeIndex >= nodes.size() || nodes[m_nodeIndex] != call)
        return true;
    for (unsigned i = m_nodeIndex; i--;) {
        Node* earlier = nodes[i];
        if (earlier == read)
            return false;
        if (!earlier->isElided && Graph::propertyEffectOf(earlier) != PropertyEffect::None)
            return true;
    }
    return true;
}

void Lowering::lowerBuiltinRead(Node* node, Node* baseNode)
{
    unsigned number = std::exchange(node->builtinCalled, 0);
    LValue known = m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[number]);
    LValue isExpectedReceiver = isReceiverKind(baseNode, lowJSValue(baseNode), static_cast<Receiver>(node->builtinReceiver));
    m_receiverChecks.set(node, isExpectedReceiver);
    if (!isExpectedReceiver)
        setJSValue(node, known);
    else {
        LBasicBlock otherwise = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        ValueFromBlock quick = m_out.anchor(known);
        m_out.branch(isExpectedReceiver, unsure(continuation), unsure(otherwise));
        m_out.appendTo(otherwise);
        {
            SetForScope isOnOnePathOnly(m_isOnOnePathOnly, true);
            lowerGetById(node);
        }
        ValueFromBlock found = m_out.anchor(lowJSValue(node));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        setJSValue(node, m_out.phi(Int64, quick, found));
    }
    node->builtinCalled = number;
}

bool Lowering::lowerSizeOfMapOrSet(Node* node, Node* baseNode)
{
    auto bytecode = node->as<OpGetById>();
    if (!ImmutableIntrinsics::shared() || code().codeBlock()->identifier(bytecode.m_property) != m_graph.vm().propertyNames->size)
        return false;
    Receiver receiver = receiverWithType(baseNode->type);
    if (receiver != Receiver::Map && receiver != Receiver::Set)
        return false;
    m_graph.remark("inline-size-of-collection"_s);
    LValue base = lowJSValue(baseNode);
    auto readSize = [&] {
        LBasicBlock hasStorage = m_out.newBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue storage = m_out.loadPtr(base, receiver == Receiver::Map ? m_heaps.JSMap_storage : m_heaps.JSSet_storage);
        ValueFromBlock withoutStorage = m_out.anchor(m_out.int32Zero);
        m_out.branch(m_out.isNull(storage), unsure(continuation), unsure(hasStorage));
        m_out.appendTo(hasStorage);
        static_assert(JSMap::Helper::aliveEntryCountIndex() == JSSet::Helper::aliveEntryCountIndex());
        ValueFromBlock withStorage = m_out.anchor(m_out.load32(m_out.address(m_heaps.root, storage, JSCellButterfly::offsetOfData() + JSMap::Helper::aliveEntryCountIndex() * sizeof(EncodedJSValue))));
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return m_out.phi(Int32, withoutStorage, withStorage);
    };
    LValue isExpectedReceiver = isReceiverKind(baseNode, base, receiver);
    LBasicBlock isExpected = m_out.newBlock();
    LBasicBlock otherwise = m_out.newBlock();
    LBasicBlock continuation = m_out.newBlock();
    m_out.branch(isExpectedReceiver, usually(isExpected), rarely(otherwise));
    m_out.appendTo(isExpected);
    ValueFromBlock quick = m_out.anchor(boxInt32(readSize()));
    m_out.jump(continuation);
    m_out.appendTo(otherwise);
    ValueFromBlock found = m_out.anchor(getByIdCached(node, base, baseNode->type, Entry::operationAOTGetById, bytecode.m_property));
    m_out.jump(continuation);
    m_out.appendTo(continuation);
    setJSValue(node, m_out.phi(Int64, quick, found));
    return true;
}

bool Lowering::lowerBuiltinCall(Node* node, Node* calleeNode, unsigned argc, unsigned argv, const Arguments& arguments, bool hasResult, LBasicBlock& afterwards, Vector<ValueFromBlock, 2>& results, Rep& repOfResults)
{
    Builtin builtin = builtinAtIndex(node->builtinCalled);
    Receiver receiver = static_cast<Receiver>(node->builtinReceiver);
    unsigned count = argc - 1;
    int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
    auto nodeAt = [&](unsigned i) { return node->use(VirtualRegister(firstArgument + static_cast<int>(i))); };
    auto typeAt = [&](unsigned i) { return nodeAt(i)->type; };
    for (unsigned i = 0; i < argc; ++i) {
        if (!typeAt(i))
            return false;
    }

    LValue fits = nullptr;
    auto also = [&](LValue condition) {
        if (condition)
            fits = fits ? m_out.bitAnd(fits, condition) : condition;
    };
    bool repeatsReceiverCheck = false;
    bool checksAlias = receiver == Receiver::None && calleeNode->kind != NodeKind::Intrinsic && !isSubtype(calleeNode->type, TFunction);
    if (checksAlias)
        also(m_out.equal(lowJSValue(calleeNode), m_out.load64(m_instance, m_heaps.AOTInstance_intrinsics[node->builtinCalled])));
    if (receiver != Receiver::None) {
        auto check = m_receiverChecks.find(calleeNode);
        if (check == m_receiverChecks.end())
            return false;
        also(check->value);
        if (check->value && (receiver == Receiver::Array || receiver == Receiver::RegExp) && receiverMayHaveChangedSince(calleeNode, node)) {
            repeatsReceiverCheck = true;
            also(isReceiverKind(nodeAt(0), arguments[0], receiver));
        }
    } else if (Receiver required = requiredReceiver(node->builtinCalled); required != Receiver::None) {
        if (!mayBe(typeAt(0), typeOf(required)) || (required == Receiver::Number && !isSubtype(typeAt(0), TNumber)))
            return false;
        also(isReceiverKind(nodeAt(0), arguments[0], required));
    }
    auto requireType = [&](unsigned i, Type type, auto&& test) {
        if (i >= argc || !mayBe(typeAt(i), type))
            return false;
        if (!isSubtype(typeAt(i), type))
            also(test(arguments[i]));
        return true;
    };
    auto mustBeNumber = [&](unsigned i) { return requireType(i, TNumber, [&](LValue value) { return isNumber(value); }); };
    Vector<LValue, 8> int32Arguments;
    int32Arguments.fill(nullptr, argc);
    auto mustBeInt32 = [&](unsigned i) {
        if (i >= argc)
            return false;
        Node* argument = nodeAt(i);
        switch (argument->rep()) {
        case Rep::Int32:
            int32Arguments[i] = lowInt32(argument);
            return true;
        case Rep::Int64: {
            LValue wide = lowRaw(argument);
            int32Arguments[i] = m_out.castToInt32(wide);
            also(m_out.equal(m_out.signExt32To64(int32Arguments[i]), wide));
            return true;
        }
        case Rep::Double: {
            if (argument->isConstant())
                return false;
            m_graph.remark("int32-argument-from-integral-double"_s);
            LValue number = lowRaw(argument);
            int32Arguments[i] = m_out.doubleToInt32(number);
            also(m_out.doubleEqual(m_out.intToDouble(int32Arguments[i]), number));
            return true;
        }
        case Rep::Boolean:
        case Rep::JSValue:
            break;
        }
        return requireType(i, TInt32, [&](LValue value) { return isInt32(value); });
    };
    auto mustBeString = [&](unsigned i) {
        return requireType(i, TString, [&](LValue value) { return isCellAnd(nodeAt(i), value, [&](LValue cell) { return isCellOfType(cell, StringType); }); });
    };
    auto mustBeObject = [&](unsigned i) {
        return requireType(i, TAnyObject, [&](LValue value) { return isCellAnd(nodeAt(i), value, [&](LValue cell) { return isObjectCell(cell); }); });
    };
    auto mustBeOriginalRegExp = [&](unsigned i) {
        if (i >= argc || !isSubtype(typeAt(i), TRegExp))
            return false;
        also(isReceiverKind(nodeAt(i), arguments[i], Receiver::RegExp));
        also(isNumber(m_out.load64(arguments[i], m_heaps.RegExpObject_lastIndex)));
        return true;
    };
    auto asDouble = [&](unsigned i) { return isSubtype(typeAt(i), TNumber) ? lowDouble(nodeAt(i)) : numberToDouble(arguments[i]); };
    auto asInt32 = [&](unsigned i) { return int32Arguments[i] ? int32Arguments[i] : unboxInt32(arguments[i]); };
    auto toInt32 = [&](unsigned i) { return isSubtype(typeAt(i), TInt32) ? lowInt32(nodeAt(i)) : doubleToInt32(asDouble(i)); };

    LBasicBlock otherwise = nullptr;
    auto begin = [&](bool mayGiveUp = false) {
        m_graph.remark("lowered-builtin"_s, pathOf(builtin));
        if (repeatsReceiverCheck)
            m_graph.remark("repeats-receiver-check-at-call"_s, pathOf(builtin));
        if (checksAlias)
            m_graph.remark("checks-alias-of-builtin"_s, pathOf(builtin));
        if (!fits && !mayGiveUp)
            return;
        otherwise = m_out.newBlock();
        afterwards = m_out.newBlock();
        if (fits) {
            LBasicBlock direct = m_out.newBlock();
            m_out.branch(fits, usually(direct), rarely(otherwise));
            m_out.appendTo(direct);
        }
    };
    auto finish = [&](LValue value, Rep rep) {
        if (!otherwise) {
            if (hasResult)
                setResult(node, value, rep);
            return true;
        }
        Type type = rep == Rep::Int32 ? TInt32 : rep == Rep::Double ? TNumber : rep == Rep::Boolean ? TBoolean : TTop;
        repOfResults = hasResult && rep == node->rep() && rep != Rep::Int64 ? rep : Rep::JSValue;
        results.append(m_out.anchor(convert(value, rep, type, repOfResults)));
        m_out.jump(afterwards);
        m_out.appendTo(otherwise);
        return true;
    };
    auto finishBoolean = [&](LValue value) { return finish(value, Rep::Boolean); };
    auto finishValue = [&](LValue value) { return finish(value, Rep::JSValue); };
    auto isTrueResult = [&](LValue returned) { return m_out.testNonZero32(m_out.castToInt32(returned), m_out.constInt32(0xff)); };
    LValue undefined = m_out.constInt64(JSValue::encode(jsUndefined()));
    LValue thisValue = arguments[0];

    auto mathFunction = [&](MathFunction which) {
        if (!mustBeNumber(1))
            return false;
        begin();
        return finish(plainCall(Double, Entry::operationAOTMath, asDouble(1), m_out.constInt32(static_cast<uint32_t>(which))), Rep::Double);
    };
    auto dateField = [&](DateField which, bool isUTC) {
        begin();
        return finishValue(plainCall(Int64, Entry::operationAOTDateField, m_vm, thisValue, m_out.constInt32(static_cast<uint32_t>(which) | (isUTC ? dateFieldIsUTC : 0))));
    };
    auto searchInString = [&](Entry plain, std::optional<Entry> withIndex) -> LValue {
        if (count == 1 && mustBeString(1)) {
            begin();
            return vmCall(node, Int64, plain, contextOf(plain), thisValue, arguments[1]);
        }
        if (count == 2 && withIndex && mustBeString(1) && mustBeInt32(2)) {
            begin();
            return vmCall(node, Int64, *withIndex, contextOf(*withIndex), thisValue, arguments[1], asInt32(2));
        }
        return nullptr;
    };
    auto jsValueArrayButterfly = [&](LValue array) {
        LValue shape = m_out.bitAnd(m_out.load8ZeroExt32(array, m_heaps.JSCell_indexingTypeAndMisc), m_out.constInt32(IndexingShapeMask));
        orElse(m_out.bitOr(m_out.equal(shape, m_out.constInt32(Int32Shape)), m_out.equal(shape, m_out.constInt32(ContiguousShape))), otherwise);
        return m_out.loadPtr(array, m_heaps.JSObject_butterfly);
    };

    switch (builtin) {
    case Builtin::None:
        return false;

    case Builtin::MathFloor:
    case Builtin::MathCeil:
    case Builtin::MathTrunc:
    case Builtin::MathRound: {
        if (!mustBeNumber(1))
            return false;
        begin();
        if (isSubtype(typeAt(1), TInt32))
            return finish(lowInt32(nodeAt(1)), Rep::Int32);
        LValue value = asDouble(1);
        switch (builtin) {
        case Builtin::MathFloor:
            return finish(m_out.doubleFloor(value), Rep::Double);
        case Builtin::MathCeil:
            return finish(m_out.doubleCeil(value), Rep::Double);
        case Builtin::MathTrunc:
            return finish(m_out.doubleTrunc(value), Rep::Double);
        default: {
            LValue integerValue = m_out.doubleCeil(value);
            LValue ceilMinusHalf = m_out.doubleSub(integerValue, m_out.constDouble(0.5));
            LValue integerValueRoundedDown = m_out.doubleSub(integerValue, m_out.constDouble(1));
            return finish(m_out.select(m_out.doubleGreaterThanOrUnordered(ceilMinusHalf, value), integerValueRoundedDown, integerValue), Rep::Double);
        }
        }
    }
    case Builtin::MathAbs:
        if (!mustBeNumber(1))
            return false;
        begin();
        return finish(m_out.doubleAbs(asDouble(1)), Rep::Double);
    case Builtin::MathSqrt:
        if (!mustBeNumber(1))
            return false;
        begin();
        return finish(m_out.doubleSqrt(asDouble(1)), Rep::Double);
    case Builtin::MathFround:
        if (!mustBeNumber(1))
            return false;
        begin();
        return finish(m_out.fround(asDouble(1)), Rep::Double);
    case Builtin::MathSign: {
        if (!mustBeNumber(1))
            return false;
        begin();
        LValue value = asDouble(1);
        LValue zero = m_out.constDouble(0);
        return finish(m_out.select(m_out.doubleGreaterThan(value, zero), m_out.constDouble(1), m_out.select(m_out.doubleLessThan(value, zero), m_out.constDouble(-1), value)), Rep::Double);
    }
    case Builtin::MathMin:
    case Builtin::MathMax: {
        if (!count || count > 4)
            return false;
        bool areInt32 = true;
        for (unsigned i = 1; i <= count; ++i) {
            if (!mustBeNumber(i))
                return false;
            areInt32 &= isSubtype(typeAt(i), TInt32);
        }
        begin();
        bool isMin = builtin == Builtin::MathMin;
        if (areInt32) {
            LValue result = lowInt32(nodeAt(1));
            for (unsigned i = 2; i <= count; ++i) {
                LValue next = lowInt32(nodeAt(i));
                result = m_out.select(isMin ? m_out.lessThan(result, next) : m_out.greaterThan(result, next), result, next);
            }
            return finish(result, Rep::Int32);
        }
        LValue result = asDouble(1);
        for (unsigned i = 2; i <= count; ++i)
            result = isMin ? m_out.doubleMin(result, asDouble(i)) : m_out.doubleMax(result, asDouble(i));
        return finish(result, Rep::Double);
    }
    case Builtin::MathClz32:
        if (!mustBeNumber(1))
            return false;
        begin();
        return finish(m_out.ctlz32(toInt32(1)), Rep::Int32);
    case Builtin::MathImul:
        if (!mustBeNumber(1) || !mustBeNumber(2))
            return false;
        begin();
        return finish(m_out.mul(toInt32(1), toInt32(2)), Rep::Int32);
    case Builtin::MathPow:
        if (!mustBeNumber(1) || !mustBeNumber(2))
            return false;
        begin();
        return finish(plainCall(Double, Entry::operationAOTPow, asDouble(1), asDouble(2)), Rep::Double);
    case Builtin::MathAtan2:
        if (!mustBeNumber(1) || !mustBeNumber(2))
            return false;
        begin();
        return finish(plainCall(Double, Entry::operationAOTMathAtan2, asDouble(1), asDouble(2)), Rep::Double);
    case Builtin::MathRandom:
        begin();
        return finish(plainCall(Double, Entry::operationAOTRandom, m_instance), Rep::Double);
#define AOT_MATH_FUNCTION(name) \
    case Builtin::Math##name: \
        return mathFunction(MathFunction::name);
    AOT_MATH_FUNCTION(Sin) AOT_MATH_FUNCTION(Cos) AOT_MATH_FUNCTION(Tan) AOT_MATH_FUNCTION(Asin) AOT_MATH_FUNCTION(Acos) AOT_MATH_FUNCTION(Atan)
    AOT_MATH_FUNCTION(Sinh) AOT_MATH_FUNCTION(Cosh) AOT_MATH_FUNCTION(Tanh) AOT_MATH_FUNCTION(Asinh) AOT_MATH_FUNCTION(Acosh) AOT_MATH_FUNCTION(Atanh)
    AOT_MATH_FUNCTION(Log) AOT_MATH_FUNCTION(Log2) AOT_MATH_FUNCTION(Log10) AOT_MATH_FUNCTION(Log1p) AOT_MATH_FUNCTION(Exp) AOT_MATH_FUNCTION(Expm1) AOT_MATH_FUNCTION(Cbrt)
#undef AOT_MATH_FUNCTION

    case Builtin::NumberIsInteger:
    case Builtin::NumberIsSafeInteger:
    case Builtin::NumberIsFinite:
    case Builtin::NumberIsNaN:
    case Builtin::GlobalIsNaN:
    case Builtin::GlobalIsFinite: {
        if (!count)
            return false;
        bool converts = builtin == Builtin::GlobalIsNaN || builtin == Builtin::GlobalIsFinite;
        if (converts && !mustBeNumber(1))
            return false;
        begin();
        if (!mayBe(typeAt(1), TNumber))
            return finishBoolean(m_out.booleanFalse);
        bool wantsNaN = builtin == Builtin::NumberIsNaN || builtin == Builtin::GlobalIsNaN;
        if (isSubtype(typeAt(1), TInt32))
            return finishBoolean(wantsNaN ? m_out.booleanFalse : m_out.booleanTrue);
        LValue value = asDouble(1);
        LValue isFinite = m_out.doubleEqual(m_out.doubleSub(value, value), m_out.constDouble(0));
        LValue answer;
        if (wantsNaN)
            answer = m_out.doubleNotEqualOrUnordered(value, value);
        else if (builtin == Builtin::NumberIsFinite || builtin == Builtin::GlobalIsFinite)
            answer = isFinite;
        else {
            answer = m_out.bitAnd(isFinite, m_out.doubleEqual(m_out.doubleTrunc(value), value));
            if (builtin == Builtin::NumberIsSafeInteger)
                answer = m_out.bitAnd(answer, m_out.doubleLessThanOrEqual(m_out.doubleAbs(value), m_out.constDouble(maxSafeInteger())));
        }
        if (!isSubtype(typeAt(1), TNumber) && !converts)
            answer = m_out.bitAnd(answer, isNumber(arguments[1]));
        return finishBoolean(answer);
    }
    case Builtin::GlobalParseInt:
    case Builtin::NumberParseInt:
        if (count == 1) {
            if (isSubtype(typeAt(1), TInt32)) {
                begin();
                return finish(lowInt32(nodeAt(1)), Rep::Int32);
            }
            if (isSubtype(typeAt(1), TNumber)) {
                begin();
                return finishValue(vmCall(node, Int64, Entry::operationParseIntDoubleNoRadix, m_globalObject, lowDouble(nodeAt(1))));
            }
            if (!mustBeString(1))
                return false;
            begin();
            return finishValue(vmCall(node, Int64, Entry::operationParseIntStringNoRadix, m_globalObject, arguments[1]));
        }
        if (count != 2 || !mustBeString(1) || !mustBeInt32(2))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationParseIntString, m_globalObject, arguments[1], asInt32(2)));
    case Builtin::NumberConstructor:
        if (count != 1 || !mustBeNumber(1))
            return false;
        begin();
        if (isSubtype(typeAt(1), TNumber))
            return finish(lowRaw(nodeAt(1)), nodeAt(1)->rep());
        return finishValue(arguments[1]);
    case Builtin::BooleanConstructor:
        begin();
        return finishBoolean(count ? toBoolean(nodeAt(1)) : m_out.booleanFalse);
    case Builtin::StringConstructor:
        if (count != 1)
            return false;
        if (isSubtype(typeAt(1), TNumber)) {
            begin();
            return finishValue(numberToString(node, nodeAt(1)));
        }
        if (!isSubtype(typeAt(1), TString) && !mayBe(typeAt(1), TSymbol | TAnyObject)) {
            begin();
            return finishValue(vmCall(node, pointerType(), Entry::operationToString, m_globalObject, arguments[1]));
        }
        if (!mustBeString(1))
            return false;
        begin();
        return finishValue(arguments[1]);
    case Builtin::NumberToString: {
        int32_t radix = 10;
        if (count) {
            Node* radixNode = nodeAt(1);
            if (count != 1 || !radixNode->range.isKnown() || radixNode->range.min != radixNode->range.max || radixNode->range.min < 2 || radixNode->range.min > 36 || !isSubtype(radixNode->type, TInt32))
                return false;
            radix = static_cast<int32_t>(radixNode->range.min);
        }
        begin();
        if (radix == 10)
            return finishValue(numberToString(node, nodeAt(0)));
        if (isSubtype(typeAt(0), TInt32))
            return finishValue(vmCall(node, pointerType(), Entry::operationInt32ToStringWithValidRadix, m_globalObject, lowInt32(nodeAt(0)), m_out.constInt32(radix)));
        return finishValue(vmCall(node, pointerType(), Entry::operationDoubleToStringWithValidRadix, m_globalObject, lowDouble(nodeAt(0)), m_out.constInt32(radix)));
    }

    case Builtin::ArrayIsArray: {
        if (!count)
            return false;
        if (isSubtype(typeAt(1), TArray) || !mayBe(typeAt(1), TArray | TOtherObject)) {
            begin();
            return finishBoolean(isSubtype(typeAt(1), TArray) ? m_out.booleanTrue : m_out.booleanFalse);
        }
        bool mayBeProxy = mayBe(typeAt(1), TOtherObject);
        begin(mayBeProxy);
        LBasicBlock notArray = m_out.newBlock();
        LBasicBlock settled = m_out.newBlock();
        Vector<ValueFromBlock, 3> answers;
        if (!isSubtype(typeAt(1), TCell)) {
            LBasicBlock cellCase = m_out.newBlock();
            answers.append(m_out.anchor(m_out.booleanFalse));
            m_out.branch(isCell(arguments[1]), unsure(cellCase), unsure(settled));
            m_out.appendTo(cellCase);
        }
        LValue type = cellType(arguments[1]);
        static_assert(DerivedArrayType == ArrayType + 1);
        answers.append(m_out.anchor(m_out.booleanTrue));
        m_out.branch(m_out.below(m_out.sub(type, m_out.constInt32(ArrayType)), m_out.constInt32(2)), unsure(settled), unsure(notArray));
        m_out.appendTo(notArray);
        if (mayBeProxy)
            orElse(m_out.notEqual(type, m_out.constInt32(ProxyObjectType)), otherwise);
        answers.append(m_out.anchor(m_out.booleanFalse));
        m_out.jump(settled);
        m_out.appendTo(settled);
        return finishBoolean(m_out.phi(Int32, answers));
    }
    case Builtin::ObjectIs: {
        if (count != 2)
            return false;
        begin();
        LValue a = arguments[1];
        LValue b = arguments[2];
        LBasicBlock differ = m_out.newBlock();
        LBasicBlock numbers = m_out.newBlock();
        LBasicBlock notNumbers = m_out.newBlock();
        LBasicBlock cells = newColdBlock();
        LBasicBlock settled = m_out.newBlock();
        Vector<ValueFromBlock, 4> answers;
        answers.append(m_out.anchor(m_out.booleanTrue));
        m_out.branch(m_out.equal(a, b), unsure(settled), unsure(differ));
        m_out.appendTo(differ);
        m_out.branch(m_out.bitAnd(isNumber(a), isNumber(b)), unsure(numbers), unsure(notNumbers));
        m_out.appendTo(numbers);
        LValue first = numberToDouble(a);
        LValue second = numberToDouble(b);
        LValue bothAreNaN = m_out.bitAnd(m_out.doubleNotEqualOrUnordered(first, first), m_out.doubleNotEqualOrUnordered(second, second));
        answers.append(m_out.anchor(m_out.bitOr(m_out.equal(m_out.bitCast(first, Int64), m_out.bitCast(second, Int64)), bothAreNaN)));
        m_out.jump(settled);
        m_out.appendTo(notNumbers);
        answers.append(m_out.anchor(m_out.booleanFalse));
        m_out.branch(m_out.bitAnd(isCell(a), isCell(b)), unsure(cells), unsure(settled));
        m_out.appendTo(cells);
        answers.append(m_out.anchor(isTrueResult(vmCall(node, Int64, Entry::operationSameValue, m_globalObject, a, b))));
        m_out.jump(settled);
        m_out.appendTo(settled);
        return finishBoolean(m_out.phi(Int32, answers));
    }
    case Builtin::ObjectKeys: {
        if (!mustBeObject(1))
            return false;
        begin();
        LValue object = arguments[1];
        return finishValue(withHelper(Stub::HelperObjectKeys, { object }, [&] { return vmCall(node, pointerType(), Entry::operationObjectKeysObject, m_globalObject, object); }));
    }
    case Builtin::ObjectGetOwnPropertyNames:
        if (!mustBeObject(1))
            return false;
        begin();
        return finishValue(vmCall(node, pointerType(), Entry::operationObjectGetOwnPropertyNamesObject, m_globalObject, arguments[1]));
    case Builtin::ObjectGetOwnPropertySymbols:
        if (!mustBeObject(1))
            return false;
        begin();
        return finishValue(vmCall(node, pointerType(), Entry::operationObjectGetOwnPropertySymbolsObject, m_globalObject, arguments[1]));
    case Builtin::ObjectGetPrototypeOf:
    case Builtin::ReflectGetPrototypeOf:
        if (!mustBeObject(1))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationGetPrototypeOfObject, m_globalObject, arguments[1]));
    case Builtin::ObjectCreate:
        if (count != 1)
            return false;
        begin();
        return finishValue(vmCall(node, pointerType(), Entry::operationObjectCreate, m_globalObject, arguments[1]));
    case Builtin::ObjectAssign:
        if (count != 2 || !mustBeObject(1))
            return false;
        begin();
        vmCall(node, Void, Entry::operationObjectAssignUntyped, m_globalObject, arguments[1], arguments[2]);
        return finishValue(arguments[1]);
    case Builtin::ObjectHasOwn:
        if (count != 2 || !mustBeObject(1))
            return false;
        begin();
        return finishBoolean(isTrueResult(vmCall(node, Int64, Entry::operationAOTHasOwnProperty, m_instance, arguments[1], arguments[2])));
    case Builtin::ObjectPrototypeHasOwnProperty: {
        Node* name = count == 1 ? nodeAt(1) : nullptr;
        Node* next = name && name->kind == NodeKind::Proj ? name->uses[0].node : nullptr;
        if (!next || !next->isBytecode(op_enumerator_next) || mayBe(typeAt(0), TEmpty))
            return false;
        auto bytecode = next->as<OpEnumeratorNext>();
        if (name->reg != bytecode.m_propertyName || next->use(bytecode.m_base) != nodeAt(0))
            return false;
        Node* mode = nullptr;
        for (Node* candidate : next->block->nodes) {
            if (candidate->kind == NodeKind::Proj && candidate->uses[0].node == next && candidate->reg == bytecode.m_mode && !candidate->isElided)
                mode = candidate;
        }
        if (!mode)
            return false;
        LValue enumerator = lowJSValue(next->use(bytecode.m_enumerator));
        also(m_out.equal(unboxInt32(lowJSValue(mode)), m_out.constInt32(JSPropertyNameEnumerator::OwnStructureMode)));
        also(isCellAnd(nodeAt(0), thisValue, [&](LValue cell) {
            return m_out.equal(m_out.load32(cell, m_heaps.JSCell_structureID), m_out.load32(enumerator, m_heaps.JSPropertyNameEnumerator_cachedStructureID));
        }));
        begin();
        return finishBoolean(m_out.booleanTrue);
    }
    case Builtin::DateNow:
        begin();
        return finish(plainCall(Double, Entry::operationDateNow, m_globalObject), Rep::Double);

    case Builtin::StringFromCharCode: {
        if (count != 1 || !mustBeInt32(1))
            return false;
        begin();
        LValue code = m_out.bitAnd(asInt32(1), m_out.constInt32(0xffff));
        LBasicBlock small = m_out.newBlock();
        LBasicBlock big = newColdBlock();
        LBasicBlock made = m_out.newBlock();
        m_out.branch(m_out.belowOrEqual(code, m_out.constInt32(maxSingleCharacterString)), usually(small), rarely(big));
        m_out.appendTo(small);
        ValueFromBlock smallCodeResult = m_out.anchor(singleCharacterString(code));
        m_out.jump(made);
        m_out.appendTo(big);
        ValueFromBlock largeCodeResult = m_out.anchor(vmCall(node, pointerType(), Entry::operationStringFromCharCode, m_globalObject, code));
        m_out.jump(made);
        m_out.appendTo(made);
        return finishValue(m_out.phi(pointerType(), smallCodeResult, largeCodeResult));
    }
    case Builtin::StringValueOf:
    case Builtin::StringToString:
        begin();
        return finishValue(thisValue);
    case Builtin::StringStartsWith:
    case Builtin::StringEndsWith: {
        bool atStart = builtin == Builtin::StringStartsWith;
        if (auto written = count == 1 ? constantStringOf(nodeAt(1)) : std::nullopt; written && written->length() && written->length() <= 24) {
            begin();
            LBasicBlock needsSlowPath = newColdBlock();
            LBasicBlock isLongEnough = m_out.newBlock();
            LBasicBlock settled = m_out.newBlock();
            Vector<ValueFromBlock, 2> lengthsOtherwise;
            Vector<ValueFromBlock, 3> answers;
            auto [characters, length] = latin1CharactersOf(thisValue, needsSlowPath, lengthsOtherwise);
            LValue needed = m_out.constInt32(written->length());
            answers.append(m_out.anchor(m_out.booleanFalse));
            m_out.branch(m_out.aboveOrEqual(length, needed), unsure(isLongEnough), unsure(settled));
            m_out.appendTo(isLongEnough);
            if (!atStart && isCompact()) {
                LBasicBlock lengthIsExact = m_out.newBlock();
                lengthsOtherwise.append(m_out.anchor(length));
                m_out.branch(m_out.below(length, m_out.constInt32(std::numeric_limits<uint16_t>::max())), usually(lengthIsExact), rarely(needsSlowPath));
                m_out.appendTo(lengthIsExact);
            }
            LValue where = atStart ? characters : m_out.add(characters, m_out.zeroExtPtr(m_out.sub(length, needed)));
            answers.append(m_out.anchor(m_out.isZero64(compareWithLiteral(where, written->span8()))));
            m_out.jump(settled);
            m_out.appendTo(needsSlowPath);
            m_out.phi(Int32, lengthsOtherwise);
            answers.append(m_out.anchor(isTrueResult(vmCall(node, Int64, atStart ? Entry::operationStringStartsWith : Entry::operationStringEndsWith, m_globalObject, thisValue, arguments[1]))));
            m_out.jump(settled);
            m_out.appendTo(settled);
            return finishBoolean(m_out.phi(Int32, answers));
        }
        LValue returned = atStart ? searchInString(Entry::operationStringStartsWith, Entry::operationStringStartsWithWithIndex) : searchInString(Entry::operationStringEndsWith, Entry::operationStringEndsWithWithEndPosition);
        return returned && finishBoolean(isTrueResult(returned));
    }
    case Builtin::StringIndexOf: {
        LValue returned = searchInString(Entry::operationStringIndexOf, Entry::operationStringIndexOfWithIndex);
        return returned && finish(m_out.castToInt32(returned), Rep::Int32);
    }
    case Builtin::StringIncludes: {
        LValue returned = searchInString(Entry::operationStringIndexOf, Entry::operationStringIndexOfWithIndex);
        return returned && finishBoolean(m_out.greaterThanOrEqual(m_out.castToInt32(returned), m_out.int32Zero));
    }
    case Builtin::StringLastIndexOf: {
        LValue returned = searchInString(Entry::operationStringLastIndexOf, std::nullopt);
        return returned && finish(m_out.castToInt32(returned), Rep::Int32);
    }
    case Builtin::StringLocaleCompare: {
        LValue returned = searchInString(Entry::operationStringLocaleCompare, std::nullopt);
        return returned && finish(m_out.castToInt32(returned), Rep::Int32);
    }
    case Builtin::StringSlice:
    case Builtin::StringSubstring: {
        if (!count || count > 2 || !mustBeInt32(1) || (count == 2 && !mustBeInt32(2)))
            return false;
        begin();
        bool isSlice = builtin == Builtin::StringSlice;
        LValue start = asInt32(1);
        LValue end = count == 2 ? asInt32(2) : m_out.constInt32(std::numeric_limits<int32_t>::max());
        return finishValue(withHelper(isSlice ? Stub::HelperStringSlice : Stub::HelperStringSubstring, { thisValue, start, end }, [&] {
            return vmCall(node, pointerType(), isSlice ? Entry::operationStringSliceWithEnd : Entry::operationStringSubstringWithEnd, m_globalObject, thisValue, start, end);
        }));
    }
    case Builtin::StringTrim:
    case Builtin::StringTrimStart:
    case Builtin::StringTrimEnd:
        begin();
        return finishValue(vmCall(node, pointerType(), builtin == Builtin::StringTrim ? Entry::operationStringTrim : builtin == Builtin::StringTrimStart ? Entry::operationStringTrimStart : Entry::operationStringTrimEnd, m_globalObject, thisValue));
    case Builtin::StringToLowerCase:
    case Builtin::StringToUpperCase: {
        bool isToLowerCase = builtin == Builtin::StringToLowerCase;
        begin();
        if (usesDataStubs())
            return finishValue(callStub(isToLowerCase ? Stub::ToLowerCase : Stub::ToUpperCase, pointerType(), { { thisValue, GPRInfo::argumentGPR1 } }, { }, StubClobbers::CallerSavedRegisters, node));
        return finishValue(vmCall(node, pointerType(), isToLowerCase ? Entry::operationToLowerCase : Entry::operationToUpperCase, m_globalObject, thisValue, m_out.int32Zero));
    }
    case Builtin::StringConcat: {
        if (count != 1 || !mustBeString(1))
            return false;
        begin();
        LValue other = arguments[1];
        return finishValue(withHelper(Stub::HelperMakeRope2, { thisValue, other }, [&] { return vmCall(node, pointerType(), Entry::operationMakeRope2, m_globalObject, thisValue, other); }));
    }
    case Builtin::StringReplace:
    case Builtin::StringReplaceAll: {
        if (count != 2 || !mustBeString(2))
            return false;
        bool all = builtin == Builtin::StringReplaceAll;
        if (isSubtype(typeAt(1), TString) && !all) {
            begin();
            return finishValue(vmCall(node, pointerType(), Entry::operationStringReplaceStringString, m_globalObject, thisValue, arguments[1], arguments[2]));
        }
        if (!mustBeOriginalRegExp(1))
            return false;
        begin();
        return finishValue(vmCall(node, pointerType(), all ? Entry::operationStringProtoFuncReplaceAllRegExpString : Entry::operationStringProtoFuncReplaceRegExpString, m_globalObject, thisValue, arguments[1], arguments[2]));
    }
    case Builtin::StringSplit: {
        if (!count || count > 2)
            return false;
        LValue limit = count == 2 ? arguments[2] : undefined;
        if (count == 2 && !isSubtype(typeAt(2), TNumber | TUndefined))
            return false;
        if (isSubtype(typeAt(1), TString)) {
            begin();
            return finishValue(vmCall(node, pointerType(), Entry::operationStringSplit, m_globalObject, thisValue, arguments[1], limit));
        }
        if (!mustBeOriginalRegExp(1))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationStringSplitRegExp, m_globalObject, thisValue, arguments[1], limit));
    }
    case Builtin::StringMatch:
    case Builtin::StringSearch:
        if (count != 1 || !mustBeOriginalRegExp(1))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, builtin == Builtin::StringMatch ? Entry::operationStringMatchRegExp : Entry::operationStringSearchRegExp, m_globalObject, thisValue, arguments[1]));

    case Builtin::ArrayPush:
        if (count < 2)
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationAOTArrayPushMultiple, m_instance, thisValue, storeArgumentsToScratch(arguments), m_out.constInt32(count)));
    case Builtin::ArrayShift:
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationArrayShift, m_globalObject, thisValue));
    case Builtin::ArrayUnshift:
        if (count != 1)
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationArrayUnshift, m_globalObject, thisValue, arguments[1]));
    case Builtin::ArrayJoin:
        if (!count) {
            begin();
            return finishValue(vmCall(node, pointerType(), Entry::operationArrayJoinGeneric, m_globalObject, thisValue, undefined));
        }
        if (count != 1 || !mustBeString(1))
            return false;
        begin();
        return finishValue(vmCall(node, pointerType(), Entry::operationArrayJoin, m_globalObject, thisValue, arguments[1]));
    case Builtin::ArrayIndexOf:
    case Builtin::ArrayIncludes: {
        if (count != 1)
            return false;
        bool isIndexOf = builtin == Builtin::ArrayIndexOf;
        also(m_out.notZero32(changing32(Instance::offsetOfArraysLackInheritedElements())));
        begin(true);
        LValue butterfly = jsValueArrayButterfly(thisValue);
        LValue found;
        if (usesDataStubs() && !isSubtype(typeAt(1), TBigInt | (isIndexOf ? TNone : TUndefined)))
            found = callStub(isIndexOf ? Stub::ArrayIndexOf : Stub::ArrayIncludes, Int64, { { butterfly, GPRInfo::argumentGPR1 }, { lowJSValuePreferringInt32(nodeAt(1)), GPRInfo::argumentGPR2 } }, { }, StubClobbers::CallerSavedRegisters, node);
        else
            found = vmCall(node, Int64, isIndexOf ? Entry::operationArrayIndexOfValueInt32OrContiguous : Entry::operationArrayIncludesValueInt32OrContiguous, m_globalObject, butterfly, arguments[1], m_out.int32Zero);
        if (isIndexOf)
            return finish(m_out.castToInt32(found), Rep::Int32);
        return finishBoolean(m_out.notZero32(m_out.castToInt32(found)));
    }
    case Builtin::ArrayAt: {
        if (count != 1 || !mustBeInt32(1))
            return false;
        begin(true);
        LValue butterfly = jsValueArrayButterfly(thisValue);
        LValue length = m_out.load32NonNegative(butterfly, m_heaps.Butterfly_publicLength);
        LValue index = asInt32(1);
        LValue place = m_out.select(m_out.lessThan(index, m_out.int32Zero), m_out.add(index, length), index);
        LBasicBlock isWithin = m_out.newBlock();
        LBasicBlock settled = m_out.newBlock();
        ValueFromBlock outside = m_out.anchor(undefined);
        m_out.branch(m_out.below(place, length), usually(isWithin), rarely(settled));
        m_out.appendTo(isWithin);
        LValue element = m_out.load64(m_out.baseIndex(m_heaps.indexedContiguousProperties, butterfly, m_out.zeroExtPtr(place)));
        orElse(m_out.notZero64(element), otherwise);
        ValueFromBlock inside = m_out.anchor(element);
        m_out.jump(settled);
        m_out.appendTo(settled);
        return finishValue(m_out.phi(Int64, outside, inside));
    }
    case Builtin::ArraySlice: {
        if (count > 2 || (count >= 1 && !mustBeInt32(1)) || (count == 2 && !mustBeInt32(2)))
            return false;
        begin(true);
        LValue made = vmCall(node, pointerType(), Entry::operationAOTArraySlice, m_instance, thisValue, count >= 1 ? asInt32(1) : m_out.int32Zero, count == 2 ? asInt32(2) : m_out.constInt32(std::numeric_limits<int32_t>::max()));
        orElse(m_out.notNull(made), otherwise);
        return finishValue(made);
    }
    case Builtin::ArraySplice:
        if (count != 2 || !mustBeInt32(1) || !mustBeInt32(2))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, hasResult ? Entry::operationArraySplice : Entry::operationArraySpliceIgnoreResult, m_globalObject, thisValue, asInt32(1), asInt32(2), m_out.intPtrZero, m_out.int32Zero));
    case Builtin::ArrayConcat: {
        if (count != 1)
            return false;
        also(m_out.notZero32(changing32(Instance::offsetOfNewArrayWithContiguousStructureID())));
        also(m_out.notZero32(changing32(Instance::offsetOfArraysLackIsConcatSpreadable())));
        begin(true);
        LValue made = vmCall(node, pointerType(), Entry::operationArrayConcatAppendOne, m_globalObject, thisValue, arguments[1]);
        orElse(m_out.notNull(made), otherwise);
        return finishValue(made);
    }

    case Builtin::MapGet:
    case Builtin::MapHas:
    case Builtin::MapSet:
    case Builtin::SetHas:
    case Builtin::SetAdd: {
        if (count != (builtin == Builtin::MapSet ? 2 : 1) || !usesDataStubs())
            return false;
        begin();
        Stub stub = builtin == Builtin::MapGet ? Stub::MapGet : builtin == Builtin::MapHas ? Stub::MapHas : builtin == Builtin::MapSet ? Stub::MapSet : builtin == Builtin::SetHas ? Stub::SetHas : Stub::SetAdd;
        Vector<StubArgument, 8> operands { { thisValue, thisGPR }, { lowJSValuePreferringInt32(nodeAt(1)), argumentGPR(0) } };
        if (builtin == Builtin::MapSet)
            operands.append({ arguments[2], argumentGPR(1) });
        LValue result = callStub(stub, Int64, operands, { }, StubClobbers::CallerSavedRegisters, node);
        m_nodeKeepsReads = true;
        return finishValue(result);
    }
    case Builtin::MapDelete:
    case Builtin::SetDelete:
        if (count != 1)
            return false;
        begin();
        return finishBoolean(isTrueResult(vmCall(node, Int64, builtin == Builtin::MapDelete ? Entry::operationAOTMapDelete : Entry::operationAOTSetDelete, m_instance, thisValue, arguments[1])));
    case Builtin::WeakMapGet:
    case Builtin::WeakMapHas:
    case Builtin::WeakSetHas: {
        if (count != 1)
            return false;
        bool isGet = builtin == Builtin::WeakMapGet;
        begin();
        if (usesDataStubs()) {
            PatchpointValue* found = callStub(isGet ? Stub::WeakMapGet : builtin == Builtin::WeakMapHas ? Stub::WeakMapHas : Stub::WeakSetHas, isGet ? Int64 : Int32,
                { { thisValue, firstStubOperandGPR }, { arguments[1], GPRInfo::argumentGPR1 } }, { }, StubClobbers::Temporaries);
            found->effects = Effects::none();
            found->effects.reads = HeapRange::top();
            found->effects.controlDependent = true;
            return isGet ? finishValue(found) : finishBoolean(found);
        }
        if (isGet)
            return finishValue(plainCall(Int64, Entry::operationAOTWeakMapGet, thisValue, arguments[1]));
        return finishBoolean(isTrueResult(plainCall(Int64, builtin == Builtin::WeakMapHas ? Entry::operationAOTWeakMapHas : Entry::operationAOTWeakSetHas, thisValue, arguments[1])));
    }

    case Builtin::RegExpTest:
        if (count != 1 || !mustBeString(1))
            return false;
        begin();
        return finishBoolean(isTrueResult(vmCall(node, Int64, Entry::operationRegExpTestString, m_globalObject, thisValue, arguments[1])));
    case Builtin::RegExpExec:
        if (count != 1 || !mustBeString(1))
            return false;
        begin();
        return finishValue(vmCall(node, Int64, Entry::operationRegExpExecString, m_globalObject, thisValue, arguments[1]));

    case Builtin::DateGetTime:
    case Builtin::DateValueOf:
        begin();
        return finish(m_out.loadDouble(thisValue, m_heaps.DateInstance_internalNumber), Rep::Double);
    case Builtin::DateGetMilliseconds:
    case Builtin::DateGetUTCMilliseconds: {
        begin();
        LValue time = m_out.loadDouble(thisValue, m_heaps.DateInstance_internalNumber);
        LValue perSecond = m_out.constDouble(msPerSecond);
        return finish(m_out.doubleSub(time, m_out.doubleMul(m_out.doubleFloor(m_out.doubleDiv(time, perSecond)), perSecond)), Rep::Double);
    }
    case Builtin::DateGetTimezoneOffset:
        return dateField(DateField::TimezoneOffset, false);
#define AOT_DATE_FIELD(name) \
    case Builtin::DateGet##name: \
        return dateField(DateField::name, false); \
    case Builtin::DateGetUTC##name: \
        return dateField(DateField::name, true);
    AOT_DATE_FIELD(FullYear) AOT_DATE_FIELD(Month) AOT_DATE_FIELD(Date) AOT_DATE_FIELD(Day) AOT_DATE_FIELD(Hours) AOT_DATE_FIELD(Minutes) AOT_DATE_FIELD(Seconds)
#undef AOT_DATE_FIELD

    case Builtin::StringCharCodeAt:
    case Builtin::StringCharAt:
    case Builtin::StringAt:
    case Builtin::StringCodePointAt:
    case Builtin::ArrayPop:
    case Builtin::WeakMapSet:
    case Builtin::WeakMapDelete:
    case Builtin::WeakSetAdd:
    case Builtin::WeakSetDelete:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
    return false;
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT) && (CPU(ARM64) || CPU(X86_64))
