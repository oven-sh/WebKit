/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedLoweringInternal.h"

#if ENABLE(AOT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "AOTRuntime.h"
#include "AOTTypedObject.h"
#include "CommonSlowPaths.h"
#include "GlobalObjectMethodTable.h"
#include "JSCInlines.h"
#include "JSDestructibleObject.h"
#include "JSInternalFieldObjectImpl.h"
#include "LinkTimeConstant.h"
#include "ProxyObject.h"

namespace JSC { namespace AOT {

JSC_DEFINE_JIT_OPERATION(operationAOTTypedRequireBuiltinModule, EncodedJSValue, (Instance* instance, uint32_t id))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* requireBuiltinModule = globalObject->globalObjectMethodTable()->requireBuiltinModule;
    if (!requireBuiltinModule) [[unlikely]] {
        throwTypeError(globalObject, scope, withAOTErrorNote("This realm has no builtin modules"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSValue result = requireBuiltinModule(globalObject, id);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedNativeBinding, EncodedJSValue, (Instance* instance, uint32_t index))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    auto* nativeBinding = globalObject->globalObjectMethodTable()->nativeBinding;
    if (!nativeBinding) [[unlikely]] {
        throwTypeError(globalObject, scope, withAOTErrorNote("This realm has no native bindings"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    JSValue result = nativeBinding(globalObject, index);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLinkTimeConstant, JSCell*, (Instance* instance, uint32_t constant))
{
    return instance->globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(constant));
}

// As globalFuncToLength().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedToLength, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    double length = JSValue::decode(value).toLength(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length)));
}

// As operationGetByIdDirectGaveUp().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedGetOwnProperty, EncodedJSValue, (Instance* instance, EncodedJSValue encodedBase, uint32_t name))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& key = instance->typed->name(vm, name);
    PropertySlot slot(base, PropertySlot::InternalMethodType::GetOwnProperty);
    bool found = base.getOwnPropertySlot(globalObject, key, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(found ? slot.getValue(globalObject, key) : jsUndefined()));
}

// As directPutByVal() of the JIT's operations, in strict code: every file of src/js is.
JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutByValDirect, void, (Instance* instance, JSCell* cell, EncodedJSValue encodedKey, EncodedJSValue encodedValue))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = asObject(cell);
    JSValue key = JSValue::decode(encodedKey);
    JSValue value = JSValue::decode(encodedValue);
    if (std::optional<uint32_t> index = key.tryGetAsUint32Index()) {
        object->putDirectIndex(globalObject, *index, value, 0, PutDirectIndexShouldThrow);
        OPERATION_RETURN(scope);
    }
    bool isNameAlready = key.isSymbol() || (key.isString() && asString(key)->tryGetValueImpl() && asString(key)->tryGetValueImpl()->isAtom());
    auto property = key.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (std::optional<uint32_t> index = parseIndex(property)) {
        object->putDirectIndex(globalObject, *index, value, 0, PutDirectIndexShouldThrow);
        OPERATION_RETURN(scope);
    }
    Structure* before = object->structure();
    PutPropertySlot slot(object, true);
    CommonSlowPaths::putDirectWithReify(vm, globalObject, object, property, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (isNameAlready)
        cacheKeyedDefinition(instance, vm, before, object->structure(), key.asCell(), property.impl());
    OPERATION_RETURN(scope);
}

// As operationPutByIdDirectStrictGaveUp().
JSC_DEFINE_JIT_OPERATION(operationAOTTypedPutByIdDirect, void, (Instance* instance, JSCell* cell, uint32_t name, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    JSObject* object = asObject(cell);
    PutPropertySlot slot(object, true);
    CommonSlowPaths::putDirectWithReify(vm, globalObject, object, instance->typed->name(vm, name), JSValue::decode(value), slot);
    OPERATION_RETURN(scope);
}

namespace Typed {

using namespace B3;

// The family aot/builtins/bun.def: what the front end emits for the files of Bun's src/js. In such a file nothing has a type: every
// operand is anything, with a prediction at best. The code in place is the FTL's for the same bytecode.

// By the numbers the front end has for them.
static constexpr JSType jsTypes[] = {
#define AOT_TYPED_JS_TYPE(name) name,
    FOR_EACH_AOT_TYPED_JS_TYPE_NAME(AOT_TYPED_JS_TYPE)
#undef AOT_TYPED_JS_TYPE
};

#define AOT_TYPED_CHECK_CONSTANT(number, expression) static_assert(static_cast<int64_t>(expression) == number);
FOR_EACH_AOT_TYPED_BUN_CONSTANT(AOT_TYPED_CHECK_CONSTANT)
#undef AOT_TYPED_CHECK_CONSTANT

// `$isTypedArrayView`: isTypedArrayType().
static_assert(Int8ArrayType == FirstTypedArrayType && BigUint64ArrayType == LastTypedArrayTypeExcludingDataView);
// `$isJSArray`: where an image is loaded, an array of a subclass or with another prototype is of the second.
static_assert(ArrayType + 1 == DerivedArrayType);

static constexpr LinkTimeConstant linkTimeConstants[] = {
#define AOT_TYPED_LINK_TIME_CONSTANT(name) LinkTimeConstant::name,
    FOR_EACH_AOT_TYPED_LINK_TIME_CONSTANT_NAME(AOT_TYPED_LINK_TIME_CONSTANT)
#undef AOT_TYPED_LINK_TIME_CONSTANT
};

std::optional<Writes> writesOfInlineBun(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    // The type of a cell never changes.
    case InlineBuiltin::IsObject:
    case InlineBuiltin::HasJSType:
        return Writes::Pure;
    case InlineBuiltin::IsCallable:
    case InlineBuiltin::GetInternalField:
    case InlineBuiltin::HasCachedValueOfHostObject:
    case InlineBuiltin::CachedValueOfHostObject:
        return Writes::Nothing;
    // It is made the first time it is asked for.
    case InlineBuiltin::LinkTimeConstant:
        return Writes::RuntimeState;
    case InlineBuiltin::PutInternalField:
    case InlineBuiltin::PutByValDirect:
    // Code of the program runs: the top level of a module, `valueOf`, a getter.
    case InlineBuiltin::RequireBuiltinModule:
    case InlineBuiltin::NativeBinding:
    case InlineBuiltin::ToLength:
    case InlineBuiltin::GetOwnProperty:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::isTestOfCachedValueOfHostObject(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::HasCachedValueOfHostObject;
}

void Lowering::branchOnCachedValueOfHostObject(const Value& value, LBasicBlock isCached, LBasicBlock isNot)
{
    auto integer32 = [&](unsigned index) {
        ValueID id = value.arguments[index];
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        RELEASE_ASSERT(m_function.at(id).opcode == Opcode::Integer);
        return static_cast<uint32_t>(m_function.at(id).integer());
    };
    if (integer32(1) >= Instance::numberOfHostClasses) {
        m_out.jump(isNot);
        return;
    }
    Boxed receiver = in<Boxed>(value.arguments[0]);
    orElse(isCell(receiver), isNot);
    CellPointer cell = asCell(receiver);
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(changing32(Instance::offsetOfStructureIDOfHostClass(integer32(1))))), isNot);
    Boxed cached(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, cell.value(), sizeof(JSDestructibleObject) + integer32(2))));
    m_out.branch(isNotEmpty(cached).value(), unsure(isCached), unsure(isNot));
}

std::optional<Operand> Lowering::lowerInlineBun(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
    // The front end writes these operands as constants.
    auto constant = [&](unsigned index) -> const Value& {
        ValueID id = arguments[index];
        while (m_function.at(id).opcode == Opcode::Cast)
            id = m_function.at(id).child(0);
        return m_function.at(id);
    };
    auto integer32 = [&](unsigned index) {
        RELEASE_ASSERT(constant(index).opcode == Opcode::Integer);
        return static_cast<uint32_t>(constant(index).integer());
    };
    auto name = [&](unsigned index) {
        RELEASE_ASSERT(constant(index).opcode == Opcode::String);
        return word32(constant(index).string().index());
    };
    auto ofCell = [&](const auto& test) {
        Boxed operand = in<Boxed>(arguments[0]);
        return yieldBoolean(either<Word32>(isCell(operand), [&] {
            return test(asCell(operand));
        }, [&] {
            return boolean(false);
        }), type);
    };
    auto internalField = [&] {
        return m_out.address(asCell(in<Boxed>(arguments[0])).value(), m_heaps.JSInternalFieldObjectImpl_internalFields[integer32(1)]);
    };

    switch (which) {
    case InlineBuiltin::IsObject:
        return ofCell([&](CellPointer cell) {
            return aboveOrEqual(typeOfCell(cell), word32(ObjectType));
        });
    case InlineBuiltin::HasJSType: {
        RELEASE_ASSERT(integer32(1) < std::size(jsTypes) && integer32(2) < std::size(jsTypes));
        JSType first = jsTypes[integer32(1)];
        JSType last = jsTypes[integer32(2)];
        RELEASE_ASSERT(first <= last);
        return ofCell([&](CellPointer cell) {
            if (first == last)
                return equal(typeOfCell(cell), word32(first));
            return below(binary<Word32>(&Output::sub, typeOfCell(cell), word32(first)), word32(last - first + 1));
        });
    }
    // What is no function of the engine and does not say for itself whether it can be called cannot. A function of the program says so.
    case InlineBuiltin::IsCallable:
        return ofCell([&](CellPointer cell) {
            return either<Word32>(hasTypeOfFunctionFlag(cell), [&] {
                return boolean(true);
            }, [&] {
                Word32 flags = loadByte(cell, m_heaps.JSCell_typeInfoFlags, IsFixed | safeIfReaches(cell, sizeof(JSCell)));
                return either<Word32>(hasBits(flags, MasqueradesAsUndefined | OverridesGetCallData), [&] {
                    return isCallable(cell);
                }, [&] {
                    return boolean(false);
                });
            });
        });
    // The cell is of a class with that many internal fields: nothing looks, here or in the engine.
    case InlineBuiltin::GetInternalField:
        return ensure(Boxed(m_out.load64(internalField())), type);
    case InlineBuiltin::PutInternalField: {
        Boxed stored = in<Boxed>(arguments[2]);
        didEmitWrite(writes);
        m_out.store64(stored.value(), internalField());
        barrier(asCell(in<Boxed>(arguments[0])));
        return yieldValue(arguments[2], type);
    }
    case InlineBuiltin::ToLength: {
        Boxed operand = in<Boxed>(arguments[0]);
        return ensure(either<Boxed>(isInt32(operand), [&] {
            Word32 length = low32(operand);
            return boxInt32(select(binary<Word32>(&Output::lessThan, length, word32(0)), word32(0), length));
        }, [&] {
            return AOT_TYPED_OPERATION(ToLength, operand);
        }), m_program.at(m_program.types.number), type);
    }
    case InlineBuiltin::PutByValDirect: {
        ValueID object = arguments[0];
        ValueID key = arguments[1];
        ValueID stored = arguments[2];
        if (constant(1).opcode == Opcode::String) {
            AOT_TYPED_OPERATION(PutByIdDirect, asCell(in<Boxed>(object)), name(1), in<Boxed>(stored));
            return yieldValue(stored, type);
        }
        auto generally = [&]() -> Operand {
            callICStub(Stub::PutByValDirectCached, Void, { { in<Boxed>(object).value(), firstStubOperandGPR }, { in<Boxed>(key).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(stored).value(), GPRInfo::argumentGPR2 } }, std::nullopt, effectsOfBuiltinCall(value));
            return { };
        };
        if (!mayBeIndex(key)) {
            generally();
            return yieldValue(stored, type);
        }
        // Where an element of an array is written in place, nothing but the array has a say: a store and a definition are the same.
        Type array = Type::engineClass(m_builtins.arrayClass());
        didEmitWrite(writes);
        ifAsExpected({ { object, &array } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            putArrayElement(get(object).as<CellPointer>(), key, stored, otherwise);
            return { };
        }, generally);
        return yieldValue(stored, type);
    }
    case InlineBuiltin::GetOwnProperty:
        return ensure(AOT_TYPED_OPERATION(GetOwnProperty, in<Boxed>(arguments[0]), name(1)), type);
    case InlineBuiltin::HasCachedValueOfHostObject: {
        if (m_isCarriedOutByBranch.get(result.index()))
            return yieldBoolean(boolean(false), type);
        LBasicBlock isCached = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnCachedValueOfHostObject(value, isCached, isNot);
        m_out.appendTo(isCached);
        join.add(boolean(true));
        m_out.appendTo(isNot);
        join.add(boolean(false));
        return yieldBoolean(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::CachedValueOfHostObject:
        return ensure(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, asCell(in<Boxed>(arguments[0])).value(), sizeof(JSDestructibleObject) + integer32(1)))), type);
    case InlineBuiltin::LinkTimeConstant:
        RELEASE_ASSERT(integer32(0) < std::size(linkTimeConstants));
        return ensure(box(AOT_TYPED_OPERATION(LinkTimeConstant, word32(static_cast<uint32_t>(linkTimeConstants[integer32(0)])))), type);
    case InlineBuiltin::RequireBuiltinModule:
        return ensure(AOT_TYPED_OPERATION(RequireBuiltinModule, word32(integer32(0))), type);
    case InlineBuiltin::NativeBinding:
        return ensure(AOT_TYPED_OPERATION(NativeBinding, word32(integer32(0))), type);
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
