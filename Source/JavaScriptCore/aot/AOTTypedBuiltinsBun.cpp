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
    JSGlobalObject* globalObject = instance->globalObject;
    VM& vm = *instance->vm;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    AOTOperationPrologueCallFrameTracer tracer(vm, callFrame);
    countOperationNamed(instance, __func__);
    auto* nativeBinding = globalObject->globalObjectMethodTable()->nativeBinding;
    JSValue result = nativeBinding ? nativeBinding(globalObject, index) : JSValue();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!nativeBinding) [[unlikely]] {
        throwTypeError(globalObject, scope, withAOTErrorNote("This realm has no native bindings"_s));
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedLinkTimeConstant, JSCell*, (Instance* instance, uint32_t constant))
{
    JSCell* cell = instance->globalObject->linkTimeConstant(static_cast<LinkTimeConstant>(constant));
    instance->linkTimeConstants[constant] = JSValue::encode(cell);
    return cell;
}

static constexpr int64_t offsetBase = 2097152;

static std::span<const Latin1Character> flatLatin1Span(JSValue value)
{
    if (!value.isString())
        return { };
    const StringImpl* impl = asString(value)->tryGetValueImpl();
    if (!impl || !impl->is8Bit())
        return { };
    return impl->span8();
}

static bool isCanonicalIPv4(std::span<const Latin1Character> host)
{
    unsigned parts = 0;
    size_t at = 0;
    while (at < host.size()) {
        unsigned value = 0;
        unsigned digits = 0;
        for (; at < host.size() && host[at] != '.'; ++at) {
            if (!isASCIIDigit(host[at]) || (digits == 1 && !value))
                return false;
            value = value * 10 + (host[at] - '0');
            if (++digits > 3)
                return false;
        }
        if (!digits || value > 255)
            return false;
        ++parts;
        if (at < host.size() && ++at == host.size())
            return false;
    }
    return parts == 4;
}

static std::optional<size_t> findPathStart(std::span<const Latin1Character> text, size_t start)
{
    size_t at = start;
    size_t label = start;
    for (; at < text.size(); ++at) {
        Latin1Character character = text[at];
        if (isASCIILower(character) || isASCIIDigit(character))
            continue;
        if (character == '-') {
            if (at == label || (at - label == 3 && text[at - 1] == '-'))
                return std::nullopt;
            continue;
        }
        if (character != '.')
            break;
        if (at == label || text[at - 1] == '-')
            return std::nullopt;
        label = at + 1;
    }
    if (at == label || text[at - 1] == '-')
        return std::nullopt;
    if (isASCIIDigit(text[label]) && !isCanonicalIPv4(text.subspan(start, at - start)))
        return std::nullopt;
    if (at < text.size() && text[at] == ':') {
        unsigned port = 0;
        size_t digits = ++at;
        for (; at < text.size() && isASCIIDigit(text[at]); ++at) {
            port = port * 10 + (text[at] - '0');
            if (port > 65535)
                return std::nullopt;
        }
        if (at == digits)
            return std::nullopt;
    }
    if (at >= text.size() || text[at] != '/')
        return std::nullopt;
    return at;
}

enum URLCharacterClass : uint8_t {
    AllowedInPath = 1,
    AllowedInQuery = 2,
    IsDotOrPercent = 4,
    IsQueryDelimiterOrEscape = 8,
};

static constexpr std::array<uint8_t, 256> urlCharacterClasses = [] {
    std::array<uint8_t, 256> classes { };
    auto isIn = [](unsigned character, uint32_t below64) {
        uint32_t mask = character < 64 ? (character < 32 ? 0 : below64) : character < 96 ? 0x87ffffffu : character < 128 ? 0x47fffffeu : 0;
        return mask >> (character & 31) & 1;
    };
    for (unsigned character = 0; character < 256; ++character) {
        if (isIn(character, 0x2fffff72u))
            classes[character] |= AllowedInPath;
        if (isIn(character, 0xafffff72u))
            classes[character] |= AllowedInQuery;
    }
    classes['.'] |= IsDotOrPercent;
    classes['%'] |= IsDotOrPercent | IsQueryDelimiterOrEscape;
    classes['+'] |= IsQueryDelimiterOrEscape;
    classes['&'] |= IsQueryDelimiterOrEscape;
    classes['='] |= IsQueryDelimiterOrEscape;
    return classes;
}();

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedScanPlainURL, EncodedJSValue, (Instance*, EncodedJSValue encodedInput))
{
    auto text = flatLatin1Span(JSValue::decode(encodedInput));
    if (text.size() < 8 || text.size() >= static_cast<size_t>(offsetBase) || text[0] != 'h' || text[1] != 't' || text[2] != 't' || text[3] != 'p')
        return JSValue::encode(jsUndefined());
    size_t colon = text[4] == 's' ? 5 : 4;
    if (text[colon] != ':' || text[colon + 1] != '/' || text[colon + 2] != '/')
        return JSValue::encode(jsUndefined());
    auto path = findPathStart(text, colon + 3);
    if (!path)
        return JSValue::encode(jsUndefined());
    size_t at = *path + 1;
    for (; at < text.size(); ++at) {
        uint8_t classes = urlCharacterClasses[text[at]];
        if (!(classes & AllowedInPath))
            break;
        if (classes & IsDotOrPercent && text[at - 1] == '/')
            return JSValue::encode(jsUndefined());
    }
    size_t query = at;
    if (at < text.size() && text[at] == '?') {
        for (++at; at < text.size() && urlCharacterClasses[text[at]] & AllowedInQuery; ++at) { }
    }
    if (at < text.size() && text[at] != '#')
        return JSValue::encode(jsUndefined());
    return JSValue::encode(jsNumber(static_cast<double>((static_cast<int64_t>(*path) * offsetBase + static_cast<int64_t>(query)) * offsetBase + static_cast<int64_t>(at))));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTTypedFindPlainQueryValue, EncodedJSValue, (Instance*, EncodedJSValue encodedInput, EncodedJSValue encodedOffsets, EncodedJSValue encodedName))
{
    constexpr int notFound = -1;
    constexpr int needsRealObject = -2;
    JSValue nameValue = JSValue::decode(encodedName);
    auto text = flatLatin1Span(JSValue::decode(encodedInput));
    auto name = flatLatin1Span(nameValue);
    JSValue offsetsValue = JSValue::decode(encodedOffsets);
    if (!offsetsValue.isNumber() || !nameValue.isString() || (name.empty() && asString(nameValue)->length()))
        return JSValue::encode(jsNumber(needsRealObject));
    int64_t offsets = static_cast<int64_t>(offsetsValue.asNumber());
    size_t end = static_cast<size_t>(offsets % offsetBase);
    size_t at = static_cast<size_t>(offsets / offsetBase % offsetBase) + 1;
    if (end > text.size())
        return JSValue::encode(jsNumber(needsRealObject));
    while (at < end) {
        size_t stop = at;
        for (; stop < end && !(urlCharacterClasses[text[stop]] & IsQueryDelimiterOrEscape); ++stop) { }
        if (stop < end && text[stop] != '&' && text[stop] != '=')
            return JSValue::encode(jsNumber(needsRealObject));
        bool hasValue = stop < end && text[stop] == '=';
        bool matchesName = (hasValue || stop != at) && stop - at == name.size() && equalSpans(text.subspan(at, stop - at), name);
        size_t value = stop;
        if (hasValue) {
            for (value = ++stop; stop < end && text[stop] != '&'; ++stop) {
                if (matchesName && (text[stop] == '%' || text[stop] == '+'))
                    return JSValue::encode(jsNumber(needsRealObject));
            }
        }
        if (matchesName)
            return JSValue::encode(jsNumber(static_cast<double>(static_cast<int64_t>(value) * offsetBase + static_cast<int64_t>(stop))));
        at = stop + 1;
    }
    return JSValue::encode(jsNumber(notFound));
}

JSC_DEFINE_JIT_OPERATION(operationAOTTypedToLength, EncodedJSValue, (Instance* instance, EncodedJSValue value))
{
    AOT_OPERATION_BEGIN_WITHOUT_CALLER(instance);
    double length = JSValue::decode(value).toLength(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(length)));
}

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
    bool isAlreadyPropertyName = key.isSymbol() || (key.isString() && asString(key)->tryGetValueImpl() && asString(key)->tryGetValueImpl()->isAtom());
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
    if (isAlreadyPropertyName)
        cacheKeyedDefinition(instance, vm, before, object->structure(), key.asCell(), property.impl());
    OPERATION_RETURN(scope);
}

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

static constexpr JSType jsTypes[] = {
#define AOT_TYPED_JS_TYPE(name) name,
    FOR_EACH_AOT_TYPED_JS_TYPE_NAME(AOT_TYPED_JS_TYPE)
#undef AOT_TYPED_JS_TYPE
};

#define AOT_TYPED_CHECK_CONSTANT(number, expression) static_assert(static_cast<int64_t>(expression) == number);
FOR_EACH_AOT_TYPED_BUN_CONSTANT(AOT_TYPED_CHECK_CONSTANT)
#undef AOT_TYPED_CHECK_CONSTANT

static_assert(Int8ArrayType == FirstTypedArrayType && BigUint64ArrayType == LastTypedArrayTypeExcludingDataView);
static_assert(ArrayType + 1 == DerivedArrayType);

static constexpr LinkTimeConstant linkTimeConstants[] = {
#define AOT_TYPED_LINK_TIME_CONSTANT(name) LinkTimeConstant::name,
    FOR_EACH_AOT_TYPED_LINK_TIME_CONSTANT_NAME(AOT_TYPED_LINK_TIME_CONSTANT)
#undef AOT_TYPED_LINK_TIME_CONSTANT
};

std::optional<Writes> writesOfInlineBun(InlineBuiltin which, std::span<const Type>)
{
    switch (which) {
    case InlineBuiltin::IsObject:
    case InlineBuiltin::HasJSType:
    case InlineBuiltin::ScanPlainURL:
    case InlineBuiltin::FindPlainQueryValue:
        return Writes::Pure;
    case InlineBuiltin::IsCallable:
    case InlineBuiltin::GetInternalField:
    case InlineBuiltin::HasHostClassStructure:
    case InlineBuiltin::IsGlobalCachedAsBuiltin:
    case InlineBuiltin::HasHostObjectCachedValue:
    case InlineBuiltin::HostObjectCachedValue:
        return Writes::Nothing;
    case InlineBuiltin::LinkTimeConstant:
        return Writes::RuntimeState;
    case InlineBuiltin::PutInternalField:
    case InlineBuiltin::PutByValDirect:
    case InlineBuiltin::RequireBuiltinModule:
    case InlineBuiltin::NativeBinding:
    case InlineBuiltin::ToLength:
    case InlineBuiltin::GetOwnProperty:
        return Writes::Heap;
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

bool Lowering::isHostObjectCacheTest(const Value& value) const
{
    if (value.opcode != Opcode::CallBuiltin)
        return false;
    auto& implementation = m_builtins.at(value.builtin()).implementation;
    return implementation.kind == BuiltinImplementation::Inline && (implementation.inlined == InlineBuiltin::HasHostObjectCachedValue || implementation.inlined == InlineBuiltin::HasHostClassStructure);
}

void Lowering::branchOnHostObjectCache(const Value& value, LBasicBlock isCached, LBasicBlock isNot)
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
    orElse(equal(loadField<Word32>(cell, m_heaps.JSCell_structureID), Word32(changing32(Instance::offsetOfHostClassStructureID(integer32(1))))), isNot);
    if (value.arguments.size() == 2) {
        m_out.jump(isCached);
        return;
    }
    Boxed cached(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, cell.value(), sizeof(JSDestructibleObject) + integer32(2))));
    m_out.branch(isNotEmpty(cached).value(), unsure(isCached), unsure(isNot));
}

std::optional<Operand> Lowering::lowerInlineBun(InlineBuiltin which, ValueID result, const Value& value, Writes writes)
{
    auto& arguments = value.arguments;
    auto& type = typeOf(result);
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
        return booleanResult(either<Word32>(isCell(operand), [&] {
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
            return aboveOrEqual(loadCellType(cell), word32(ObjectType));
        });
    case InlineBuiltin::HasJSType: {
        RELEASE_ASSERT(integer32(1) < std::size(jsTypes) && integer32(2) < std::size(jsTypes));
        JSType first = jsTypes[integer32(1)];
        JSType last = jsTypes[integer32(2)];
        RELEASE_ASSERT(first <= last);
        return ofCell([&](CellPointer cell) {
            if (first == last)
                return equal(loadCellType(cell), word32(first));
            return below(binary<Word32>(&Output::sub, loadCellType(cell), word32(first)), word32(last - first + 1));
        });
    }
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
    case InlineBuiltin::GetInternalField:
        return ensure(Boxed(m_out.load64(internalField())), type);
    case InlineBuiltin::PutInternalField: {
        Boxed stored = in<Boxed>(arguments[2]);
        didEmitWrite(writes);
        m_out.store64(stored.value(), internalField());
        barrier(asCell(in<Boxed>(arguments[0])));
        return valueResult(arguments[2], type);
    }
    case InlineBuiltin::ScanPlainURL:
        return ensure(AOT_TYPED_OPERATION(ScanPlainURL, in<Boxed>(arguments[0])), type);
    case InlineBuiltin::FindPlainQueryValue:
        return ensure(AOT_TYPED_OPERATION(FindPlainQueryValue, in<Boxed>(arguments[0]), in<Boxed>(arguments[1]), in<Boxed>(arguments[2])), m_program.at(m_program.types.number), type);
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
            return valueResult(stored, type);
        }
        auto genericPath = [&]() -> Operand {
            callICStub(Stub::PutByValDirectCached, Void, { { in<Boxed>(object).value(), firstStubOperandGPR }, { in<Boxed>(key).value(), GPRInfo::argumentGPR1 }, { in<Boxed>(stored).value(), GPRInfo::argumentGPR2 } }, std::nullopt, builtinCallEffects(value));
            return { };
        };
        if (!mayBeIndex(key)) {
            genericPath();
            return valueResult(stored, type);
        }
        Type array = Type::engineClass(m_builtins.arrayClass());
        didEmitWrite(writes);
        ifPredictionHolds({ { object, &array } }, Rep::Void, [&](LBasicBlock otherwise) -> Operand {
            putArrayElement(get(object).as<CellPointer>(), key, stored, otherwise);
            return { };
        }, genericPath);
        return valueResult(stored, type);
    }
    case InlineBuiltin::GetOwnProperty:
        return ensure(AOT_TYPED_OPERATION(GetOwnProperty, in<Boxed>(arguments[0]), name(1)), type);
    case InlineBuiltin::HasHostClassStructure:
    case InlineBuiltin::HasHostObjectCachedValue: {
        if (m_isFusedWithBranch.get(result.index()))
            return booleanResult(boolean(false), type);
        LBasicBlock isCached = m_out.newBlock();
        LBasicBlock isNot = m_out.newBlock();
        Join join(m_out, Rep::Int32, Int32);
        branchOnHostObjectCache(value, isCached, isNot);
        m_out.appendTo(isCached);
        join.add(boolean(true));
        m_out.appendTo(isNot);
        join.add(boolean(false));
        return booleanResult(join.finish().as<Word32>(), type);
    }
    case InlineBuiltin::IsGlobalCachedAsBuiltin: {
        BuiltinName global(integer32(0));
        uint32_t cacheNumber = m_program.globalPropertyCacheNumber(m_program.stringOf(global), false);
        if (!cacheNumber)
            return booleanResult(boolean(false), type);
        return booleanResult(equal(Boxed(globalPropertyCacheWord(cacheNumber, OBJECT_OFFSETOF(GlobalPropertyCache, value))), box(builtinCell(m_builtins.cellOf(global)))), type);
    }
    case InlineBuiltin::HostObjectCachedValue:
        return ensure(Boxed(m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, asCell(in<Boxed>(arguments[0])).value(), sizeof(JSDestructibleObject) + integer32(1)))), type);
    case InlineBuiltin::LinkTimeConstant: {
        RELEASE_ASSERT(integer32(0) < std::size(linkTimeConstants));
        auto constant = static_cast<uint32_t>(linkTimeConstants[integer32(0)]);
        if (!m_out.m_block->frequency())
            return ensure(box(AOT_TYPED_OPERATION(LinkTimeConstant, word32(constant))), type);
        LBasicBlock isNotFetched = newColdBlock();
        LBasicBlock continuation = m_out.newBlock();
        LValue fetched = m_out.load64(m_out.address(m_heaps.AOTTyped_runtimeState, m_instance, Instance::offsetOfLinkTimeConstant(constant)));
        ValueFromBlock fast = m_out.anchor(fetched);
        m_out.branch(m_out.notZero64(fetched), usually(continuation), rarely(isNotFetched));
        m_out.appendTo(isNotFetched);
        ValueFromBlock slow = m_out.anchor(AOT_TYPED_OPERATION(LinkTimeConstant, word32(constant)).value());
        m_out.jump(continuation);
        m_out.appendTo(continuation);
        return ensure(box(CellPointer(m_out.phi(Int64, fast, slow))), type);
    }
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
