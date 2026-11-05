/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PySlowPaths.h"

#include "BytecodeStructs.h"
#include "CodeBlockInlines.h"
#include "FrameTracers.h"
#include "JSCInlines.h"
#include "LLIntData.h"
#include "LLIntExceptions.h"
#include "MaxFrameExtentForSlowPathCall.h"
#include "PyAbstractInlines.h"
#include "PyBytecode.h"
#include "PyDescriptors.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyHooks.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyTuple.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {

using namespace Python;

// What the op_py_* instructions do. The interpreter does the commonest cases of some of them itself (LowLevelInterpreter64.asm).

#define BEGIN() \
    CodeBlock* codeBlock = callFrame->codeBlock(); \
    JSGlobalObject* globalObject = codeBlock->globalObject(); \
    VM& vm = codeBlock->vm(); \
    SlowPathFrameTracer tracer(vm, callFrame); \
    auto throwScope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_PARAM(throwScope); \
    UNUSED_PARAM(globalObject); \
    setCurrentVPC(callFrame, codeBlock, pc)

#define GET(operand) (callFrame->uncheckedR(operand))
#define GET_C(operand) (callFrame->r(operand))

#define END_IMPL() return encodeResult(pc, callFrame)

#define CHECK_EXCEPTION() do { \
        if (throwScope.exception()) [[unlikely]] { \
            pc = LLInt::returnToThrow(vm); \
            return encodeResult(pc, LLInt::exceptionSignal()); \
        } \
    } while (false)

#define END() do { \
        CHECK_EXCEPTION(); \
        END_IMPL(); \
    } while (false)

#define RETURN(value) do { \
        JSValue returnValue__ = (value); \
        CHECK_EXCEPTION(); \
        GET(bytecode.m_dst) = returnValue__; \
        END_IMPL(); \
    } while (false)

#define JUMP_IF(condition) do { \
        bool condition__ = (condition); \
        CHECK_EXCEPTION(); \
        if (condition__) \
            pc = bytecode.m_targetLabel \
                ? reinterpret_cast<const JSInstruction*>(reinterpret_cast<const uint8_t*>(pc) + bytecode.m_targetLabel) \
                : codeBlock->outOfLineJumpTarget(pc); \
        else \
            pc = reinterpret_cast<const JSInstruction*>(reinterpret_cast<const uint8_t*>(pc) + sizeOfInstruction<decltype(bytecode)>(pc)); \
        END_IMPL(); \
    } while (false)

// pc->size(), without looking at the opcode, which a tool may have replaced.
template<typename Op>
static ALWAYS_INLINE size_t sizeOfInstruction(const JSInstruction* pc)
{
    int shift = pc->sizeShiftAmount();
    return 1 + Op::prefixBytes + (static_cast<size_t>(opcodeLengths[Op::opcodeID]) << shift) + !!shift;
}

static ALWAYS_INLINE PyFunction* calleeOf(CallFrame* callFrame)
{
    return uncheckedDowncast<PyFunction>(callFrame->jsCallee());
}

// ---- Names ----

static NEVER_INLINE JSValue raiseNameNotDefined(JSGlobalObject* globalObject, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The name as "%.200s" shows its UTF-8: up to a NUL, and as many whole characters as take no more than 200 bytes.
    String text = stringOf(name);
    if (!lengthInUTF8(text)) [[unlikely]] {
        strEncode(globalObject, name, nullptr, nullptr);
        RETURN_IF_EXCEPTION(scope, { });
    }
    StringView shown = StringView(text).left(text.find(static_cast<char16_t>(0)));
    unsigned units = 0;
    for (size_t bytes = 0; units < shown.length();) {
        bool isPair = U16_IS_LEAD(shown[units]);
        bytes += isPair ? 4 : U8_LENGTH(shown[units]);
        if (bytes > 200)
            break;
        units += isPair ? 2 : 1;
    }
    PyException* exception = newException(globalObject, rt.NameError(), makeString("name '"_s, shown.left(units), "' is not defined"_s));
    RETURN_IF_EXCEPTION(scope, { });
    exception->setField(vm, ExceptionField::nameErrorName, name);
    return raiseNewException(globalObject, scope, exception);
}

static NEVER_INLINE JSValue raiseUnbound(JSGlobalObject* globalObject, const String& name, UnboundKind kind)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyException* exception = kind == UnboundKind::Local
        ? newException(globalObject, rt.UnboundLocalError(), makeString("cannot access local variable '"_s, name, "' where it is not associated with a value"_s))
        : newException(globalObject, rt.NameError(), makeString("cannot access free variable '"_s, name, "' where it is not associated with a value in enclosing scope"_s));
    RETURN_IF_EXCEPTION(scope, { });
    // The name is there for suggestions, of which an UnboundLocalError gets none.
    if (kind != UnboundKind::Local)
        exception->setField(vm, ExceptionField::nameErrorName, makeStr(vm, name));
    return raiseNewException(globalObject, scope, exception);
}

// mapping[name], or an empty value and no exception if there is no such key.
static JSValue getOptionalItem(JSGlobalObject* globalObject, JSValue mapping, JSString* name)
{
    Runtime& rt = runtime(globalObject);
    auto scope = DECLARE_THROW_SCOPE(rt.vm());
    if (auto* dict = PyDict::tryFrom(mapping); dict && isExactDict(rt, dict)) [[likely]] {
        if (dict->hasOnlyStringKeys()) [[likely]]
            return dict->get(name);
        // A key that is no str may say that it is equal to the name.
        RELEASE_AND_RETURN(scope, dict->get(globalObject, JSValue(name)));
    }
    JSValue value = getItem(globalObject, mapping, name);
    if (failed(scope, value))
        clearPendingExceptionIfMatches(globalObject, rt.KeyError());
    return value;
}

enum class GlobalLookup : uint8_t {
    Global, // A name that the compiler knows to be global.
    Name, // A name that was not found in the locals of a module or a class body.
};

// For PyFunction::hasUnusualNamespaces(), and for a name that is the key of neither dict. The second kind of lookup takes the
// globals for a dict whatever they are.
static NEVER_INLINE JSValue loadGlobalFromMappings(JSGlobalObject* globalObject, PyFunction* function, JSString* name, GlobalLookup lookup)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto find = [&](JSValue mapping) {
        return getOptionalItem(globalObject, mapping, name);
    };
    JSValue names = function->globals();
    JSValue value = lookup == GlobalLookup::Name ? function->globals()->get(globalObject, JSValue(name)) : find(names);
    RETURN_IF_EXCEPTION(scope, { });
    if (!value) {
        names = function->builtinsObject();
        value = find(names);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            RELEASE_AND_RETURN(scope, raiseNameNotDefined(globalObject, name));
    }
    if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]]
        RELEASE_AND_RETURN(scope, reifyLazyImport(globalObject, lazy, name, names));
    return value;
}

static ALWAYS_INLINE JSValue loadGlobal(JSGlobalObject* globalObject, PyFunction* function, JSString* name, GlobalLookup lookup = GlobalLookup::Global)
{
    // (In a dict with keys that are no strs, one of them may say that it is equal to the name.)
    if (function->hasUnusualNamespaces() || !function->globals()->hasOnlyStringKeys() || !function->builtins()->hasOnlyStringKeys()) [[unlikely]]
        return loadGlobalFromMappings(globalObject, function, name, lookup);
    PyDict* names = function->globals();
    JSValue value = names->get(name);
    if (!value) {
        names = function->builtins();
        value = names->get(name);
        if (!value) [[unlikely]]
            return loadGlobalFromMappings(globalObject, function, name, lookup);
    }
    if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]]
        return reifyLazyImport(globalObject, lazy, name, names);
    return value;
}

// ---- Hooks ----

namespace Python {

void throwNotCallable(JSGlobalObject* globalObject, JSValue callee)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // import pprint; pprint(x)
    if (auto* module = PyModule::tryFrom(callee); module && module->type() == rt.moduleType()) {
        JSValue name = module->dict()->get(rt.names.dName());
        if (name && name.isString()) {
            JSValue attribute = getAttrIfPresent(globalObject, callee, asString(name));
            if (scope.exception())
                takePendingException(globalObject);
            else if (attribute && isCallable(globalObject, attribute)) {
                String text = stringOf(asString(name));
                raiseTypeError(globalObject, scope, "'module' object is not callable. Did you mean: '"_s, text, '.', text, "(...)'?"_s);
                return;
            }
        }
    }
    raiseTypeError(globalObject, scope, '\'', typeName(globalObject, callee), "' object is not callable"_s);
}

void throwUnboundLocal(JSGlobalObject* globalObject, CodeBlock* codeBlock, VirtualRegister reg)
{
    CodeInfo* info = uncheckedDowncast<FunctionExecutable>(codeBlock->ownerExecutable())->unlinkedExecutable()->pythonCodeInfo();
    for (auto& variable : info->variables) {
        if (variable.reg == reg) {
            raiseUnbound(globalObject, variable.name.string(), UnboundKind::Local);
            return;
        }
    }
    raiseUnbound(globalObject, "?"_s, UnboundKind::Local);
}

}

// ---- The instructions ----

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_bind_args)
{
    BEGIN();
    // Whatever is thrown here leaves the frame, which counts it out.
    Runtime& rt = runtime(globalObject);
    if (++rt.frameDepth > rt.recursionLimit) [[unlikely]] {
        raiseRecursionError(globalObject, throwScope, "maximum recursion depth exceeded"_s);
        CHECK_EXCEPTION();
    }
    if (auto* call = rt.runtimeCall; call && !call->entryFrame)
        call->entryFrame = vm.topEntryFrame;
    PyFunction* callee = calleeOf(callFrame);
    // (Nothing has run since the call began: the function still has the code.)
    if (!callee->codeInfo().takesNamespace) {
        bindArguments(globalObject, callFrame, callee);
        CHECK_EXCEPTION();
    } else if (!callFrame->argumentCount()) [[unlikely]] {
        // types.FunctionType(code of a module, globals)()
        callFrame->setArgument(0, callee->globals());
    }
    // Loops poll with op_py_jmp_back; this is the poll of recursion.
    if (vm.traps().needHandling(VMTraps::NeedPythonCalls)) [[unlikely]]
        makeCallsOwed(vm);
    END();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_load_cell)
{
    BEGIN();
    auto bytecode = pc->as<OpPyLoadCell>();
    JSValue value = PyCell::from(GET(bytecode.m_cell).jsValue())->value();
    if (!value) [[unlikely]]
        raiseUnbound(globalObject, stringOf(asString(GET_C(bytecode.m_name).jsValue())), static_cast<UnboundKind>(bytecode.m_kind));
    RETURN(value);
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_store_cell)
{
    BEGIN();
    auto bytecode = pc->as<OpPyStoreCell>();
    PyCell::from(GET(bytecode.m_cell).jsValue())->setValue(vm, GET_C(bytecode.m_value).jsValue());
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_list_append)
{
    BEGIN();
    auto bytecode = pc->as<OpPyListAppend>();
    listAppend(globalObject, asList(GET(bytecode.m_list).jsValue()), GET_C(bytecode.m_value).jsValue());
    END();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_load_global)
{
    BEGIN();
    auto bytecode = pc->as<OpPyLoadGlobal>();
    PyFunction* function = calleeOf(callFrame);
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    if (function->hasUnusualNamespaces() || !function->globals()->hasOnlyStringKeys() || !function->builtins()->hasOnlyStringKeys()) [[unlikely]]
        RETURN(loadGlobal(globalObject, function, name));

    // Where the name is now is where the interpreter looks next time, while the dicts have the versions they have now.
    auto& metadata = bytecode.metadata(codeBlock);
    PyDict* globals = function->globals();
    int32_t index = globals->indexOf(name);
    uint64_t builtinsVersion = 0;
    if (index < 0) {
        PyDict* builtins = function->builtins();
        index = builtins->indexOf(name);
        if (index >= 0)
            builtinsVersion = builtins->watchedVersion();
    }
    if (index >= 0) {
        metadata.m_globalsVersion = globals->watchedVersion();
        metadata.m_builtinsVersion = builtinsVersion;
        metadata.m_index = index;
    }

    RETURN(loadGlobal(globalObject, function, name));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_store_global)
{
    BEGIN();
    auto bytecode = pc->as<OpPyStoreGlobal>();
    PyDict* globals = calleeOf(callFrame)->globals();
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    if (isFrozenDict(globals)) [[unlikely]] {
        raiseTypeError(globalObject, throwScope, "'frozendict' object does not support item assignment"_s);
        CHECK_EXCEPTION();
    }
    if (!globals->hasOnlyStringKeys()) [[unlikely]] {
        // A key that is no str may say that it is equal to the name: the value is then stored with that.
        globals->set(globalObject, name, GET_C(bytecode.m_value).jsValue());
        END();
    }
    globals->set(vm, name, GET_C(bytecode.m_value).jsValue());
    auto& metadata = bytecode.metadata(codeBlock);
    metadata.m_globalsVersion = globals->watchedVersion();
    metadata.m_index = globals->indexOf(name);
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_load_name)
{
    BEGIN();
    auto bytecode = pc->as<OpPyLoadName>();
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    JSValue locals = GET(bytecode.m_locals).jsValue();
    PyFunction* function = calleeOf(callFrame);
    // In the body of a module the locals are the globals, which are asked once. (Twice, as locals and as globals, if a key could tell.)
    if (locals == JSValue(function->globals()) && !function->hasUnusualNamespaces() && function->globals()->hasOnlyStringKeys())
        RETURN(loadGlobal(globalObject, function, name, GlobalLookup::Name));
    JSValue value = getOptionalItem(globalObject, locals, name);
    CHECK_EXCEPTION();
    if (!value)
        value = loadGlobal(globalObject, function, name, GlobalLookup::Name);
    else if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]]
        value = reifyLazyImport(globalObject, lazy, name, locals);
    RETURN(value);
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_store_name)
{
    BEGIN();
    auto bytecode = pc->as<OpPyStoreName>();
    JSValue locals = GET(bytecode.m_locals).jsValue();
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    JSValue value = GET_C(bytecode.m_value).jsValue();
    if (auto* dict = PyDict::tryFrom(locals); dict && dict->type() == runtime(globalObject).dictType() && dict->hasOnlyStringKeys()) [[likely]] {
        dict->set(vm, name, value);
        END_IMPL();
    }
    setItem(globalObject, locals, name, value);
    END();
}

template<typename Metadata>
static ALWAYS_INLINE void remember(Metadata& metadata, const AttributeCache& cache)
{
    metadata.m_payload = cache.payload;
    metadata.m_structureID = cache.structureID;
    metadata.m_typeVersion = cache.typeVersion;
    metadata.m_offset = cache.offset;
    metadata.m_jsType = cache.jsType;
    metadata.m_metatypeVersion = cache.metatypeVersion;
}

// The lookup that op_py_get_attr and op_py_get_method make when they remember nothing of an object like this one. One walk finds the
// attribute and what there is to remember. An object of which nothing can be remembered leaves what is remembered as it is.
// Empty, and no exception: the attribute is not of a kind that is read in a line. That is left to getAttr() or getMethod(), and to the
// caller if it is a property, which is in `property` then.
// `self`: as for getMethod().
template<typename Metadata>
static ALWAYS_INLINE JSValue findAttribute(JSGlobalObject* globalObject, Metadata& metadata, JSValue base, JSString* name, JSValue* self, PyProperty** property = nullptr)
{
    // As JS's own instruction does (llint_slow_path_get_by_id): an object that has lost attributes, or has very many, is given a Structure
    // that can be gone by again, once.
    if (base.isCell() && base.asCell()->type() == PyInstanceType) {
        if (Structure* structure = base.asCell()->structure(); structure->isDictionary() && !structure->hasBeenFlattenedBefore()) [[unlikely]]
            structure->flattenDictionaryStructure(getVM(globalObject), asObject(base));
    }
    AttributeCache cache;
    switch (describeAttribute(globalObject, base, name, !!self, cache)) {
    case AttributeDescription::None:
        return { };
    case AttributeDescription::Absent:
        return getAttrOfAbsent(globalObject, base, name);
    case AttributeDescription::OfAllAlike:
        remember(metadata, cache);
        break;
    case AttributeDescription::OfThisObjectNow:
        break;
    }
    if (cache.jsType) {
        *self = base;
        return JSValue::decode(cache.payload);
    }
    if (cache.offset >= 0)
        return asObject(base)->getDirect(cache.offset);
    switch (cache.offset) {
    case AttributeCache::classValue:
        if (self)
            *self = base;
        [[fallthrough]];
    case AttributeCache::unboundClassValue:
    case AttributeCache::valueOfClass:
        return JSValue::decode(cache.payload);
    case AttributeCache::classMethod:
    case AttributeCache::classMethodOfClass: {
        // To anything but a function of Python's, the class may be something else than the first argument.
        JSValue function = uncheckedDowncast<PyWrapperObject>(JSValue::decode(cache.payload).asCell())->wrapped();
        if (!PyFunction::is(function))
            return { };
        *self = cache.offset == AttributeCache::classMethod ? JSValue(typeOf(globalObject, base)) : base;
        return function;
    }
    case AttributeCache::staticMethod:
    case AttributeCache::staticMethodOfClass:
        return uncheckedDowncast<PyWrapperObject>(JSValue::decode(cache.payload).asCell())->wrapped();
    case AttributeCache::moduleEntry: {
        JSValue value = uncheckedDowncast<PyModule>(base.asCell())->dict()->valueAt(cache.typeVersion);
        return asLazyImport(globalObject, value) ? JSValue() : value;
    }
    case AttributeCache::tupleItem:
        if (auto* tuple = uncheckedDowncast<PyTuple>(base.asCell()); cache.payload < tuple->length())
            return tuple->at(cache.payload);
        return { };
    case AttributeCache::propertyGetter:
        *property = uncheckedDowncast<PyProperty>(JSValue::decode(cache.payload).asCell());
        return { };
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_get_attr)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGetAttr>();
    JSValue base = GET_C(bytecode.m_base).jsValue();
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    PyProperty* property = nullptr;
    JSValue result = findAttribute(globalObject, bytecode.metadata(codeBlock), base, name, nullptr, &property);
    CHECK_EXCEPTION();
    if (result)
        RETURN(result);
    // See "Calls that instructions make" below. Nothing of the program's has run: the interpreter finds the object where it was.
    PyFunction* getter = property ? PyFunction::tryFrom(property->getter.get()) : nullptr;
    if (getter && canBeCalledByInstruction(getter, 1)) {
        ASSERT(GET_C(bytecode.m_base).jsValue() == base);
        return encodeResult(getter, nullptr);
    }
    RETURN(getAttr(globalObject, base, name));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_set_attr)
{
    BEGIN();
    auto bytecode = pc->as<OpPySetAttr>();
    JSValue base = GET_C(bytecode.m_base).jsValue();
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    JSValue value = GET_C(bytecode.m_value).jsValue();
    // One walk, as in findAttribute().
    using Kind = SetAttributeDescription::Kind;
    SetAttributeDescription description = describeSetAttribute(globalObject, base, name);
    StructureID newStructureID;
    switch (description.kind) {
    case Kind::None:
        setAttr(globalObject, base, name, value);
        END();
    case Kind::InDict:
        description.dict->set(vm, name, value);
        END_IMPL();
    case Kind::Replace:
        asObject(base)->putDirectOffset(vm, description.offset, value);
        break;
    case Kind::Add: {
        JSObject* object = asObject(base);
        // opaque(): so that what is kept over the call is the pointer, which the collector knows, not the 32 bits it can be made of.
        Structure* oldStructure = WTF::opaque(object->structure());
        PutPropertySlot slot(object);
        object->putDirect(vm, PropertyName(description.key), value, static_cast<unsigned>(PropertyAttribute::DontEnum), slot);
        // What llint_slow_path_put_by_id asks of a transition before it remembers it. (The instruction does not allocate.)
        Structure* newStructure = object->structure();
        if (slot.type() != PutPropertySlot::NewProperty || newStructure->isDictionary() || (vm.mayHaveUncacheableTransitions() && !newStructure->isCacheableTransitionFrom(oldStructure))
            || newStructure->previousID() != oldStructure || oldStructure->outOfLineCapacity() != newStructure->outOfLineCapacity())
            END_IMPL();
        newStructureID = newStructure->id();
        description.offset = slot.cachedOffset();
        break;
    }
    }
    // A type is asked for its version last: it has only so many to give.
    uint32_t typeVersion = description.holdsForAllAlike ? description.type->version() : 0;
    if (!typeVersion)
        END_IMPL();
    auto& metadata = bytecode.metadata(codeBlock);
    {
        // The collector reads these: CodeBlock::propagateTransitions().
        ConcurrentJSLocker locker(codeBlock->m_lock);
        metadata.m_oldStructureID = description.structureID;
        metadata.m_newStructureID = newStructureID;
        metadata.m_typeVersion = typeVersion;
        metadata.m_offset = description.offset;
    }
    vm.writeBarrier(codeBlock);
    END_IMPL();
}

static JSValue loadSpecialForWith(JSGlobalObject*, JSValue object, SpecialMethodForWith, JSValue& self, AttributeCache&);

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_get_method)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGetMethod>();
    auto& metadata = bytecode.metadata(codeBlock);
    JSValue self;
    JSValue base = GET_C(bytecode.m_base).jsValue();
    if (JSValue which = GET_C(bytecode.m_name).jsValue(); which.isInt32()) [[unlikely]] {
        AttributeCache cache;
        JSValue method = loadSpecialForWith(globalObject, base, static_cast<SpecialMethodForWith>(which.asInt32()), self, cache);
        CHECK_EXCEPTION();
        if (cache.structureID)
            remember(metadata, cache);
        GET(bytecode.m_self) = self;
        GET(bytecode.m_dst) = method;
        END_IMPL();
    }
    JSString* name = asString(GET_C(bytecode.m_name).jsValue());
    JSValue method = findAttribute(globalObject, metadata, base, name, &self);
    CHECK_EXCEPTION();
    if (!method) {
        method = getMethod(globalObject, base, name, self);
        CHECK_EXCEPTION();
    }
    GET(bytecode.m_self) = self;
    GET(bytecode.m_dst) = method;
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_prepare_construct)
{
    BEGIN();
    auto bytecode = pc->as<OpPyPrepareConstruct>();
    JSValue self;
    JSValue init = prepareConstruct(globalObject, GET_C(bytecode.m_callee).jsValue(), self);
    CHECK_EXCEPTION();
    GET(bytecode.m_self) = self;
    GET(bytecode.m_dst) = init;
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_finish_construct)
{
    BEGIN();
    auto bytecode = pc->as<OpPyFinishConstruct>();
    JSValue result = GET(bytecode.m_result).jsValue();
    if (!isNone(result)) [[unlikely]] {
        raiseTypeError(globalObject, throwScope, "__init__() should return None, not '"_s, typeName(globalObject, result), '\'');
        CHECK_EXCEPTION();
    }
    RETURN(GET(bytecode.m_self).jsValue());
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_get_item)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGetItem>();
    RETURN(getItemInline(globalObject, GET_C(bytecode.m_base).jsValue(), GET_C(bytecode.m_key).jsValue()));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_set_item)
{
    BEGIN();
    auto bytecode = pc->as<OpPySetItem>();
    setItemInline(globalObject, GET_C(bytecode.m_base).jsValue(), GET_C(bytecode.m_key).jsValue(), GET_C(bytecode.m_value).jsValue());
    END();
}

// ---- Calls that instructions make ----

// An operator may come to a call of a function written in Python. If C++ made that call, its frames would be on the stack for as long
// as the function runs: that costs an entry into the interpreter, and what those frames do not write is taken for references by the
// collector. So the slow path only finds out what is to be called, and returns that in place of where to go on, with how far the
// operator will have got in place of the frame. The interpreter makes the call, looks at what comes back, and if that does not settle
// it asks a second slow path, which may come up with another function (pyOperatorSlowPath in LowLevelInterpreter64.asm).

bool Python::canBeCalledByInstruction(PyFunction* function, unsigned maxParameterCount)
{
#if !ENABLE(C_LOOP) && !CPU(ARM64E)
    CodeBlock* codeBlock = function->functionExecutable()->codeBlockForCall();
    return codeBlock && codeBlock->numParameters() <= 1 + maxParameterCount;
#else
    UNUSED_PARAM(function);
    UNUSED_PARAM(maxParameterCount);
    return false;
#endif
}

bool Python::canBeCalledByOperator(PyFunction* function)
{
    return canBeCalledByInstruction(function, 2 + parametersOperatorMethodMayHaveBeyondOperands);
}

// The frame of the method that has returned, from the first argument on. It is still there for the second slow path, and for any other
// that the interpreter calls before it has given the frame up: right below the registers of the caller, where the stack of C++ begins
// otherwise.
static ALWAYS_INLINE Register* registersOfOperator(CallFrame* callFrame, CodeBlock* codeBlock)
{
    return endOfFrameMadeByInstruction(callFrame, codeBlock) - sizeOfFrameMadeByInstruction(RegisterOfOperator::End) + static_cast<int>(CallFrameSlot::firstArgument);
}

// As pyGiveUpFrameOfOperator: all that is left for the interpreter to do is to move the stack pointer.
static_assert(sizeOfFrameMadeByInstruction(RegisterOfOperator::End) - CallerFrameAndPC::sizeInRegisters == 12 && sizeOfFrameMadeByInstruction(2) - CallerFrameAndPC::sizeInRegisters == 6, "pyGiveUpFrame...");
static_assert(sizeOfFrameMadeByInstruction(RegisterOfGetter::End) == sizeOfFrameMadeByInstruction(2), "pyGiveUpFrameWithTwoRegisters");
static ALWAYS_INLINE void clearFrameOfOperator(CallFrame* callFrame, CodeBlock* codeBlock)
{
    Register* end = endOfFrameMadeByInstruction(callFrame, codeBlock);
    for (Register* slot = end - sizeOfFrameMadeByInstruction(RegisterOfOperator::End) + CallerFrameAndPC::sizeInRegisters; slot < end; ++slot)
        *slot = JSValue();
}

enum class ResultOfOperator { IsStored, JumpsIfTrue, JumpsIfFalse };

#define SETTLE_OPERATOR(result) do { \
        if constexpr (isGoingOn) \
            clearFrameOfOperator(callFrame, codeBlock); \
        if constexpr (whatBecomesOfResult == ResultOfOperator::IsStored) { \
            GET(bytecode.m_dst) = result; \
            END_IMPL(); \
        } else \
            JUMP_IF(isTrue(globalObject, result) == (whatBecomesOfResult == ResultOfOperator::JumpsIfTrue)); \
    } while (false)

// begin...() or continue...() has returned nothing. On its own, so that what returns a result need not keep anything for this.
template<typename Op, bool isGoingOn, ResultOfOperator whatBecomesOfResult, typename GoOn>
static NEVER_INLINE UGPRPair operatorSlowPathWithoutResult(CallFrame* callFrame, const JSInstruction* pc, const GoOn& goOn)
{
    BEGIN();
    auto bytecode = pc->as<Op>();
    if (isGoingOn && throwScope.exception())
        clearFrameOfOperator(callFrame, codeBlock);
    CHECK_EXCEPTION();
    auto method = std::exchange(runtime(globalObject).methodToCall, { });
    RELEASE_ASSERT(method.function);
    ASSERT(canBeCalledByOperator(method.function));
    JSValue lhs = JSValue::decode(method.left);
    JSValue rhs = JSValue::decode(method.right);
    // The interpreter takes the operands from where the slow path has.
    if (isGoingOn || (GET_C(bytecode.m_lhs).jsValue() == lhs && GET_C(bytecode.m_rhs).jsValue() == rhs)) [[likely]]
        return encodeResult(method.function, std::bit_cast<void*>(static_cast<uintptr_t>(method.progress)));
    // Those are variables, and a method that C++ has called in the meantime has given them other values (frame.f_locals).
    bool isReflected = method.progress & OperatorProgress::IsReflected;
    JSValue result = call(globalObject, method.function, isReflected ? rhs : lhs, isReflected ? lhs : rhs);
    CHECK_EXCEPTION();
    if (result == runtime(globalObject).notImplemented()) {
        result = goOn(globalObject, bytecode, lhs, rhs, method.progress, MethodsWrittenInPython::AreCalled);
        CHECK_EXCEPTION();
    }
    SETTLE_OPERATOR(result);
}

// `isGoingOn`: a method has returned NotImplemented. `begin(bytecode, lhs, rhs)`, `goOn(bytecode, lhs, rhs, progress, methods)`: begin...()
// and continue...().
template<typename Op, bool isGoingOn, ResultOfOperator whatBecomesOfResult, typename Begin, typename GoOn>
static ALWAYS_INLINE UGPRPair operatorSlowPath(CallFrame* callFrame, const JSInstruction* pc, const Begin& begin, const GoOn& goOn)
{
    BEGIN();
    auto bytecode = pc->as<Op>();
    Register* registers = isGoingOn ? registersOfOperator(callFrame, codeBlock) : nullptr;
    JSValue lhs = (isGoingOn ? registers[RegisterOfOperator::Left] : GET_C(bytecode.m_lhs)).jsValue();
    JSValue rhs = (isGoingOn ? registers[RegisterOfOperator::Right] : GET_C(bytecode.m_rhs)).jsValue();
    JSValue result = isGoingOn ? goOn(globalObject, bytecode, lhs, rhs, registers[RegisterOfOperator::Progress].jsValue().asInt32(), MethodsWrittenInPython::AreLeftToCaller) : begin(globalObject, bytecode, lhs, rhs);
    EXCEPTION_ASSERT(!throwScope.exception() || !result);
    if (!result) [[unlikely]]
        RELEASE_AND_RETURN(throwScope, (operatorSlowPathWithoutResult<Op, isGoingOn, whatBecomesOfResult>(callFrame, pc, goOn)));
    SETTLE_OPERATOR(result);
}

template<typename Op, bool isGoingOn>
static ALWAYS_INLINE UGPRPair binaryOperatorSlowPath(CallFrame* callFrame, const JSInstruction* pc)
{
    return operatorSlowPath<Op, isGoingOn, ResultOfOperator::IsStored>(callFrame, pc, [](JSGlobalObject* globalObject, const Op& bytecode, JSValue lhs, JSValue rhs) ALWAYS_INLINE_LAMBDA {
        return beginBinaryOperation(globalObject, static_cast<BinaryOperator>(bytecode.m_operation), lhs, rhs);
    }, [](JSGlobalObject* globalObject, const Op& bytecode, JSValue lhs, JSValue rhs, unsigned progress, MethodsWrittenInPython methods) ALWAYS_INLINE_LAMBDA {
        return continueBinaryOperation(globalObject, static_cast<BinaryOperator>(bytecode.m_operation), lhs, rhs, progress, methods);
    });
}

template<typename Op, CompareOperator compareOperator, bool isGoingOn, ResultOfOperator whatBecomesOfResult>
static ALWAYS_INLINE UGPRPair comparisonSlowPath(CallFrame* callFrame, const JSInstruction* pc)
{
    return operatorSlowPath<Op, isGoingOn, whatBecomesOfResult>(callFrame, pc, [](JSGlobalObject* globalObject, const Op&, JSValue lhs, JSValue rhs) ALWAYS_INLINE_LAMBDA {
        return beginComparison(globalObject, lhs, rhs, compareOperator);
    }, [](JSGlobalObject* globalObject, const Op&, JSValue lhs, JSValue rhs, unsigned progress, MethodsWrittenInPython methods) ALWAYS_INLINE_LAMBDA {
        return continueComparison(globalObject, lhs, rhs, compareOperator, progress, methods);
    });
}

#undef SETTLE_OPERATOR

// The second of each pair: a method has returned NotImplemented.
#define DEFINE_ARITHMETIC_SLOW_PATH(name, Op) \
    JSC_DEFINE_COMMON_SLOW_PATH(name) \
    { \
        return binaryOperatorSlowPath<Op, false>(callFrame, pc); \
    } \
    JSC_DEFINE_COMMON_SLOW_PATH(name##_again) \
    { \
        return binaryOperatorSlowPath<Op, true>(callFrame, pc); \
    }

DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_binary, OpPyBinary)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_add, OpPyAdd)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_sub, OpPySub)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_mul, OpPyMul)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_div, OpPyDiv)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_floor_div, OpPyFloorDiv)
DEFINE_ARITHMETIC_SLOW_PATH(slow_path_py_mod, OpPyMod)

#undef DEFINE_ARITHMETIC_SLOW_PATH

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_unary)
{
    BEGIN();
    auto bytecode = pc->as<OpPyUnary>();
    RETURN(unaryOperation(globalObject, static_cast<UnaryOperator>(bytecode.m_operation), GET_C(bytecode.m_operand).jsValue()));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_compare)
{
    BEGIN();
    auto bytecode = pc->as<OpPyCompare>();
    JSValue lhs = GET_C(bytecode.m_lhs).jsValue();
    JSValue rhs = GET_C(bytecode.m_rhs).jsValue();
    auto operation = static_cast<CompareOperator>(bytecode.m_operation);
    if (operation == CompareOperator::In || operation == CompareOperator::NotIn) {
        // x in g(): nothing but the search has the generator.
        Runtime& rt = runtime(globalObject);
        bool isNew = rt.isNewGenerator(rhs, callFrame);
        if (isNew) [[unlikely]]
            rt.claimIteratorOfLoop(codeBlock->bytecodeOffset(pc));
        bool found = containsInline(globalObject, rhs, lhs);
        if (isNew) [[unlikely]]
            rt.releaseGeneratorGoneThrough(uncheckedDowncast<PyGenerator>(rhs.asCell()));
        CHECK_EXCEPTION();
        RETURN(jsBoolean(found == (operation == CompareOperator::In)));
    }
    RETURN(compare(globalObject, lhs, rhs, operation));
}

// The interpreter has put the result of the comparison where the instruction keeps how far it has got.
template<typename Op, bool jumpsIfTrue>
static ALWAYS_INLINE UGPRPair jumpOnResultOfComparisonSlowPath(CallFrame* callFrame, const JSInstruction* pc)
{
    BEGIN();
    auto bytecode = pc->as<Op>();
    JSValue result = registersOfOperator(callFrame, codeBlock)[RegisterOfOperator::Progress].jsValue();
    clearFrameOfOperator(callFrame, codeBlock);
    JUMP_IF(isTrue(globalObject, result) == jumpsIfTrue);
}

#define DEFINE_COMPARISON_AND_JUMP_SLOW_PATHS(name, Op, compareOperator, jumpsIfTrue) \
    JSC_DEFINE_COMMON_SLOW_PATH(name##_begin) \
    { \
        return comparisonSlowPath<Op, CompareOperator::compareOperator, false, jumpsIfTrue ? ResultOfOperator::JumpsIfTrue : ResultOfOperator::JumpsIfFalse>(callFrame, pc); \
    } \
    JSC_DEFINE_COMMON_SLOW_PATH(name##_again) \
    { \
        return comparisonSlowPath<Op, CompareOperator::compareOperator, true, jumpsIfTrue ? ResultOfOperator::JumpsIfTrue : ResultOfOperator::JumpsIfFalse>(callFrame, pc); \
    } \
    JSC_DEFINE_COMMON_SLOW_PATH(name##_finish) \
    { \
        return jumpOnResultOfComparisonSlowPath<Op, jumpsIfTrue>(callFrame, pc); \
    }

// The comparison; the comparison and a jump if what it gives is true; and a jump if that is false.
// The jumps come in two ways. The interpreter goes by _begin, _again and _finish. The one without a suffix does it all and calls
// whatever there is to call: that is for C++ (slowPathOfBranch() in PyMonitoring.cpp).
#define DEFINE_COMPARISON_SLOW_PATHS(name, Name, compareOperator) \
    JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_##name) \
    { \
        return comparisonSlowPath<OpPy##Name, CompareOperator::compareOperator, false, ResultOfOperator::IsStored>(callFrame, pc); \
    } \
    JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_##name##_again) \
    { \
        return comparisonSlowPath<OpPy##Name, CompareOperator::compareOperator, true, ResultOfOperator::IsStored>(callFrame, pc); \
    } \
    DEFINE_COMPARISON_AND_JUMP_SLOW_PATHS(slow_path_py_j##name, OpPyJ##name, compareOperator, true) \
    DEFINE_COMPARISON_AND_JUMP_SLOW_PATHS(slow_path_py_jn##name, OpPyJn##name, compareOperator, false) \
    JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_j##name) \
    { \
        BEGIN(); \
        auto bytecode = pc->as<OpPyJ##name>(); \
        JSValue result = compare(globalObject, GET_C(bytecode.m_lhs).jsValue(), GET_C(bytecode.m_rhs).jsValue(), CompareOperator::compareOperator); \
        CHECK_EXCEPTION(); \
        JUMP_IF(isTrue(globalObject, result)); \
    } \
    JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_jn##name) \
    { \
        BEGIN(); \
        auto bytecode = pc->as<OpPyJn##name>(); \
        JSValue result = compare(globalObject, GET_C(bytecode.m_lhs).jsValue(), GET_C(bytecode.m_rhs).jsValue(), CompareOperator::compareOperator); \
        CHECK_EXCEPTION(); \
        JUMP_IF(!isTrue(globalObject, result)); \
    }

DEFINE_COMPARISON_SLOW_PATHS(eq, Eq, Eq)
DEFINE_COMPARISON_SLOW_PATHS(ne, Ne, NotEq)
DEFINE_COMPARISON_SLOW_PATHS(lt, Lt, Lt)
DEFINE_COMPARISON_SLOW_PATHS(le, Le, LtE)
DEFINE_COMPARISON_SLOW_PATHS(gt, Gt, Gt)
DEFINE_COMPARISON_SLOW_PATHS(ge, Ge, GtE)

#undef DEFINE_COMPARISON_SLOW_PATHS
#undef DEFINE_COMPARISON_AND_JUMP_SLOW_PATHS

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_jtrue)
{
    BEGIN();
    auto bytecode = pc->as<OpPyJtrue>();
    JUMP_IF(isTrue(globalObject, GET_C(bytecode.m_condition).jsValue()));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_jfalse)
{
    BEGIN();
    auto bytecode = pc->as<OpPyJfalse>();
    JUMP_IF(!isTrue(globalObject, GET_C(bytecode.m_condition).jsValue()));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_get_iter)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGetIter>();
    JSValue iterable = GET_C(bytecode.m_iterable).jsValue();
    if (Runtime& rt = runtime(globalObject); rt.isNewGenerator(iterable, callFrame))
        rt.claimIteratorOfLoop(codeBlock->bytecodeOffset(pc));
    RETURN(getIter(globalObject, iterable));
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_catch)
{
    BEGIN();
    auto bytecode = pc->as<OpPyCatch>();
    JSValue exception = throwScope.exception()->value();
    // What ends the thread goes on.
    if (!throwScope.tryClearException()) [[unlikely]]
        CHECK_EXCEPTION();
    GET(bytecode.m_exception) = exception;
    auto mode = static_cast<CatchMode>(bytecode.m_mode);
    if (mode == CatchMode::Thrown)
        END_IMPL();
    if (auto* ours = PyException::tryFrom(exception)) [[likely]] {
        if (mode != CatchMode::HandledUnseen)
            ours->willBeSeen();
    } else {
        // What goes wrong with this goes wrong in the handler, which the blocks around it cover.
        GET(bytecode.m_previous) = JSValue();
        setCurrentVPC(callFrame, codeBlock, codeBlock->instructions().at(whereHandlerBegins(codeBlock, codeBlock->bytecodeOffset(pc))).ptr());
        exception = exceptionFromThrownValue(globalObject, exception);
        CHECK_EXCEPTION();
        GET(bytecode.m_exception) = exception;
    }
    GET(bytecode.m_previous) = runtime(globalObject).setHandledExceptionInTopSlot(exception);
    END_IMPL();
}

// In builds with assertions: op_py_ret, which otherwise does without C++.
JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_assert_frame_may_end)
{
#if ASSERT_ENABLED
    runtime(callFrame->codeBlock()->globalObject()).assertNoNotedEntryOf(callFrame);
#endif
    return encodeResult(pc, callFrame);
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_for_iter)
{
    BEGIN();
    auto bytecode = pc->as<OpPyForIter>();
    JSValue item = iterNext(globalObject, GET(bytecode.m_iterator).jsValue());
    CHECK_EXCEPTION();
    if (item)
        GET(bytecode.m_dst) = item;
    JUMP_IF(!item);
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_send)
{
    BEGIN();
    auto bytecode = pc->as<OpPySend>();
    int mode = GET(bytecode.m_mode).jsValue().asInt32();
    if (mode >= firstSendOfAwait) {
        JSValue iterator = getAwaitable(globalObject, GET(bytecode.m_iterator).jsValue(), mode - firstSendOfAwait);
        CHECK_EXCEPTION();
        GET(bytecode.m_iterator) = iterator;
        mode = static_cast<int>(JSGenerator::ResumeMode::NormalMode);
    }
    JSValue yielded = sendToDelegate(globalObject, GET(bytecode.m_iterator).jsValue(), GET(bytecode.m_value).jsValue(), static_cast<JSGenerator::ResumeMode>(mode));
    if (!yielded)
        uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell())->throwIsWithDelegate = false;
    CHECK_EXCEPTION();
    JSValue result = yielded ? yielded : runtime(globalObject).takeSendResult();
    if (!yielded && mode != static_cast<int>(JSGenerator::ResumeMode::NormalMode)) [[unlikely]] {
        runtime(globalObject).monitoring().delegateReturnedWhenThrownInto(globalObject, callFrame, codeBlock->bytecodeOffset(pc), result);
        CHECK_EXCEPTION();
    }
    GET(bytecode.m_dst) = result;
    JUMP_IF(!yielded);
}

// ---- The body of a generator: see PyGenerator::resume() ----

static_assert(static_cast<int>(JSGenerator::Argument::NumberOfArguments) + 1 == 6); // pyResumeGenerator in LowLevelInterpreter64.asm

static ALWAYS_INLINE PyGenerator* generatorOf(CallFrame* callFrame)
{
    return uncheckedDowncast<PyGenerator>(callFrame->uncheckedArgument(0).asCell());
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_generator_enter)
{
    BEGIN();
    PyGenerator* generator = generatorOf(callFrame);
    int32_t state = generator->state();
    generator->enterBody(globalObject);
    CHECK_EXCEPTION();
    pc = state ? codeBlock->instructions().at(state).ptr() : reinterpret_cast<const JSInstruction*>(reinterpret_cast<const uint8_t*>(pc) + pc->size());
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_generator_suspend)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGeneratorSuspend>();
#if ASSERT_ENABLED
    runtime(globalObject).assertNoNotedEntryOf(callFrame);
#endif
    PyGenerator* generator = generatorOf(callFrame);
    for (unsigned i = 0; i < bytecode.m_count; ++i)
        generator->setSlot(vm, bytecode.m_offset + i, GET(VirtualRegister(bytecode.m_first.offset() - static_cast<int>(i))).jsValue());
    // For what the op_py_generator_registers before this one have stored, which the interpreter does without.
    vm.writeBarrier(generator);
    generator->suspendBody(globalObject, codeBlock->bytecodeOffset(pc) + sizeOfInstruction<OpPyGeneratorSuspend>(pc));
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_generator_return)
{
    BEGIN();
    runtime(globalObject).frameIsEnding(callFrame);
    generatorOf(callFrame)->returnFromBody(globalObject);
    END_IMPL();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_generator_registers)
{
    BEGIN();
    auto bytecode = pc->as<OpPyGeneratorRegisters>();
    PyGenerator* generator = generatorOf(callFrame);
    for (unsigned i = 0; i < bytecode.m_count; ++i) {
        Register& reg = GET(VirtualRegister(bytecode.m_first.offset() - static_cast<int>(i)));
        if (!bytecode.m_isSave)
            reg = generator->slot(bytecode.m_offset + i);
        generator->setSlot(vm, bytecode.m_offset + i, bytecode.m_isSave ? reg.jsValue() : JSValue());
    }
    END_IMPL();
}

static void unpackIterable(JSGlobalObject* globalObject, CallFrame* callFrame, JSValue iterable, VirtualRegister first, unsigned before, int after)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto target = [&](unsigned index) -> Register& { return callFrame->uncheckedR(VirtualRegister(first.offset() - static_cast<int>(index))); };

    // The common cases, in which nothing else runs.
    if (after < 0) {
        if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == rt.tupleType() && tuple->length() == before) {
            for (unsigned i = 0; i < before; ++i)
                target(i) = tuple->at(i);
            return;
        }
        if (isExactList(iterable) && listLength(asList(iterable)) == before) {
            for (unsigned i = 0; i < before; ++i)
                target(i) = listGet(asList(iterable), i);
            return;
        }
    }

    JSValue iterator = getIter(globalObject, iterable);
    if (failed(scope, iterator)) {
        PyType* type = rt.typeOf(iterable);
        if (pendingExceptionMatches(globalObject, rt.TypeError()) && !type->lookup(Special::Iter) && !type->lookup(Special::GetItem)) {
            takePendingException(globalObject);
            raiseTypeError(globalObject, scope, "cannot unpack non-iterable "_s, type->name(), " object"_s);
        }
        return;
    }

    // The registers may be variables, which keep what they have if this fails, and while the iterator runs.
    MarkedArgumentBuffer items;
    auto store = [&] {
        if (items.hasOverflowed()) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return;
        }
        for (unsigned i = 0; i < items.size(); ++i)
            target(i) = items.at(i);
    };

    for (unsigned i = 0; i < before; ++i) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!item) {
            if (after < 0)
                raiseValueError(globalObject, scope, "not enough values to unpack (expected "_s, before, ", got "_s, i, ')');
            else
                raiseValueError(globalObject, scope, "not enough values to unpack (expected at least "_s, before + after, ", got "_s, i, ')');
            return;
        }
        items.append(item);
    }

    if (after < 0) {
        JSValue extra = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, void());
        if (!extra)
            return store();
        std::optional<size_t> size;
        if (isExactList(iterable))
            size = listLength(asList(iterable));
        else if (auto* tuple = PyTuple::tryFrom(iterable); tuple && tuple->type() == rt.tupleType())
            size = tuple->length();
        else if (auto* dict = PyDict::tryFrom(iterable); dict && dict->type() == rt.dictType())
            size = dict->size();
        if (size && *size > before)
            raiseValueError(globalObject, scope, "too many values to unpack (expected "_s, before, ", got "_s, *size, ')');
        else
            raiseValueError(globalObject, scope, "too many values to unpack (expected "_s, before, ')');
        return;
    }

    JSArray* rest = listFromIterable(globalObject, iterator);
    RETURN_IF_EXCEPTION(scope, void());
    unsigned length = listLength(rest);
    unsigned trailing = static_cast<unsigned>(after);
    if (length < trailing) {
        raiseValueError(globalObject, scope, "not enough values to unpack (expected at least "_s, before + trailing, ", got "_s, before + length, ')');
        return;
    }
    items.append(rest);
    for (unsigned i = 0; i < trailing; ++i)
        items.append(listGet(rest, length - trailing + i));
    listTruncate(globalObject, rest, length - trailing);
    RETURN_IF_EXCEPTION(scope, void());
    store();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_unpack)
{
    BEGIN();
    auto bytecode = pc->as<OpPyUnpack>();
    unpackIterable(globalObject, callFrame, GET_C(bytecode.m_iterable).jsValue(), bytecode.m_argv, bytecode.m_before, static_cast<int>(bytecode.m_afterPlusOne) - 1);
    END();
}

// ---- op_py_runtime ----

namespace {

// The arguments of a runtime function: registers, from the first down.
class RuntimeArguments {
public:
    RuntimeArguments(CallFrame* callFrame, VirtualRegister first, unsigned count)
        : m_first(count ? &callFrame->r(first) : nullptr) // A single one may be a constant.
        , m_count(count)
    {
    }

    unsigned size() const { return m_count; }
    JSValue operator[](unsigned index) const
    {
        ASSERT(index < m_count);
        return (m_first - index)->jsValue();
    }
    const JSValue* first() const { return reinterpret_cast<const JSValue*>(m_first); }
    // For a function that has more to say than its result.
    void set(unsigned index, JSValue value) const
    {
        ASSERT(index < m_count);
        *(m_first - index) = value;
    }

private:
    Register* m_first;
    unsigned m_count;
};

}

static JSValue buildString(JSGlobalObject* globalObject, const RuntimeArguments& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // In a tree that a program has made, the parts of an f-string may be anything.
    uint64_t length = 0;
    bool isFlatLatin1 = true;
    for (unsigned i = 0; i < args.size(); ++i) {
        if (!isStr(args[i])) [[unlikely]]
            return raiseTypeError(globalObject, scope, "sequence item "_s, i, ": expected str instance, "_s, typeName(globalObject, args[i]), " found"_s);
        JSString* part = strValue(args[i]);
        length += part->length();
        isFlatLatin1 &= !part->isRope() && part->is8Bit();
    }

    // What an f-string makes is nearly always looked at, and at once: printed, compared, hashed, joined. A rope is written out
    // then. Short text is written out here, which takes no longer than to make the rope (up to this length, measured) and saves the rope.
    constexpr unsigned longestWrittenOutAtOnce = 256;
    if (isFlatLatin1 && length <= longestWrittenOutAtOnce && args.size() > 1 && length) {
        std::span<Latin1Character> buffer;
        auto text = StringImpl::createUninitialized(length, buffer);
        for (unsigned i = 0; i < args.size(); ++i) {
            auto part = strValue(args[i])->tryGetValueImpl()->span8();
            memcpySpan(buffer, part);
            skip(buffer, part.size());
        }
        return jsString(vm, String(WTF::move(text)));
    }

    // As a template literal of JS is put together: jsStringFromRegisterArray().
    JSRopeString::RopeBuilder<RecordOverflow> builder(vm);
    for (unsigned i = 0; i < args.size(); ++i) {
        if (!builder.append(strValue(args[i]))) [[unlikely]] {
            throwOutOfMemoryError(globalObject, scope);
            return { };
        }
    }
    return builder.release();
}

static JSValue convertValue(JSGlobalObject* globalObject, JSValue value, JSValue conversion)
{
    // (What comes of it may be an instance of a class derived from str, with a __format__ of its own.)
    switch (conversion.asInt32()) {
    case 's':
        return strObject(globalObject, value);
    case 'r':
        return reprObject(globalObject, value);
    case 'a':
        return asciiObject(globalObject, value);
    default:
        return value;
    }
}

static JSValue formatValue(JSGlobalObject* globalObject, JSValue value, JSValue conversion, JSValue spec)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    value = convertValue(globalObject, value, conversion);
    RETURN_IF_EXCEPTION(scope, { });
    if (!spec && value.isString())
        return value;
    if (spec && !isStr(spec)) [[unlikely]]
        return raiseSystemError(globalObject, scope, "Format specifier must be a string, not "_s, typeName(globalObject, spec));
    RELEASE_AND_RETURN(scope, formatObject(globalObject, value, spec ? strValue(spec) : nullptr));
}

static JSValue makeFunction(JSGlobalObject* globalObject, CallFrame* callFrame, const RuntimeArguments& args)
{
    VM& vm = getVM(globalObject);
    PyFunction* enclosing = calleeOf(callFrame);
    unsigned indexAndParts = args[0].asInt32();
    PyFunction* function = PyFunction::create(globalObject, callFrame->codeBlock()->functionExpr(indexAndParts >> makeFunctionIndexShift), enclosing->globals());
    unsigned next = 1;
    if (indexAndParts & HasDefaults)
        function->setDefaults(vm, PyTuple::from(args[next++]));
    if (indexAndParts & HasKwDefaults)
        function->setKwDefaults(vm, PyDict::from(args[next++]));
    if (indexAndParts & HasAnnotate)
        function->setAnnotate(vm, args[next++]);
    if (args.size() > next)
        function->setClosure(vm, PyTuple::createFromNegativeIndexed(globalObject, args.first() - next, args.size() - next));
    return function;
}

// What the body starts with: the values of the parameters, and then `extra` if there is one.
static JSValue makeGenerator(JSGlobalObject* globalObject, CallFrame* callFrame, unsigned slotCount, JSValue extra)
{
    VM& vm = getVM(globalObject);
    PyFunction* function = calleeOf(callFrame);
    CodeInfo& info = codeInfoOf(callFrame);
    unsigned count = info.takesNamespace ? 1 : info.parameterCount();
    ASSERT(count + !!extra <= slotCount);
    PyGenerator* generator = PyGenerator::create(globalObject, function, function->generatorBody(globalObject, callFrame->codeBlock()), slotCount);
    for (unsigned i = 0; i < count; ++i)
        generator->setSlot(vm, i, callFrame->r(virtualRegisterForArgumentIncludingThis(i + 1)).jsValue()); // There may be fewer arguments than parameters.
    if (extra)
        generator->setSlot(vm, count, extra);
    runtime(globalObject).didMakeGenerator(generator, callFrame);
    return generator;
}

// Whether a hole of the array is undefined to JS as well, which asks the prototypes: none of them has anything to say about an index.
static ALWAYS_INLINE bool holesAreUndefinedToJS(JSGlobalObject* globalObject, JSArray* list)
{
    Structure* structure = list->structure();
    if (globalObject->isOriginalArrayStructure(structure) && globalObject->arrayPrototypeChainIsSane()) [[likely]]
        return true;
    return !structure->holesMustForwardToPrototype(list);
}

// callee(*positional, **keywords): the arguments in an array, as op_call_varargs takes them. The names of the keywords are set aside.
// `keywords`: a dict that the compiled code has made. `mapping`: what was after the only **. If both are empty, nobody asks for the names.
static JSValue callArguments(JSGlobalObject* globalObject, JSValue callee, JSValue positional, JSValue keywords, JSValue mapping)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    auto* tuple = PyTuple::tryFrom(positional);
    if (tuple && tuple->type() != rt.tupleType())
        tuple = nullptr;
    JSArray* list = isExactList(positional) ? asList(positional) : nullptr;

    // In CPython's order: what is after the ** is read first. To go through what is after the * may run code, which may change it.
    PyDict* dict = keywords ? PyDict::from(keywords) : nullptr;
    if (mapping) {
        dict = PyDict::tryFrom(mapping);
        // (Keys that are not exactly strs may be asked when they are put into another dict.)
        if (!dict || !isExactDict(rt, dict) || !dict->hasOnlyStringKeys() || (!tuple && !list)) {
            dict = PyDict::create(globalObject);
            dictMergeForCall(globalObject, dict, mapping, callee);
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (dict && dict->isEmpty())
        dict = nullptr;

    JSArray* array;
    if (list && !dict && holesAreUndefinedToJS(globalObject, list)) [[likely]] {
        // Made by the compiled code, or about to be copied into the frame anyway: by the rules of JS, which ask the prototypes about a hole.
        array = list;
    } else if (list && listLength(list) > maxArguments) {
        // It is refused for its length, before anything of it is read.
        if (keywords || mapping)
            rt.setPendingKeywordNames(jsUndefined());
        return list;
    } else if (tuple && !dict) {
        array = newList(globalObject, tuple->span());
        RETURN_IF_EXCEPTION(scope, { });
    } else {
        array = newList(globalObject, dict ? (tuple ? tuple->length() : list ? list->length() : 0) + dict->size() : 0);
        RETURN_IF_EXCEPTION(scope, { });
        listExtend(globalObject, array, positional, LengthHint::NotAsked);
        if (scope.exception()) [[unlikely]] {
            PyType* type = rt.typeOf(positional);
            if (pendingExceptionMatches(globalObject, rt.TypeError()) && !type->lookup(Special::Iter) && !type->lookup(Special::GetItem)) {
                takePendingException(globalObject);
                return raiseTypeError(globalObject, scope, "Value after * must be an iterable, not "_s, type->name());
            }
            return { };
        }
        // What was copied in one piece has the holes it had.
        if (!holesAreUndefinedToJS(globalObject, array)) [[unlikely]] {
            for (unsigned i = 0; i < listLength(array); ++i) {
                if (array->tryGetIndexQuickly(i))
                    continue;
                listSet(globalObject, array, i, jsUndefined());
                RETURN_IF_EXCEPTION(scope, { });
            }
        }
    }
    if (!keywords && !mapping)
        return array;

    rt.setPendingKeywordNames(jsUndefined());
    if (!dict)
        return array;
    PyTuple* names = keywordNamesOf(globalObject, dict);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue key;
    JSValue value;
    for (unsigned position = 0; dict->next(position, key, value);) {
        listAppend(globalObject, array, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    rt.setPendingKeywordNames(names);
    return array;
}

static JSValue importNameForInstruction(JSGlobalObject* globalObject, CallFrame* callFrame, JSString* name, JSValue fromList, JSValue level, ImportMode mode)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyFunction* function = calleeOf(callFrame);
    CodeInfo& info = codeInfoOf(callFrame);
    JSValue locals = info.takesNamespace ? callFrame->r(info.namespaceRegister).jsValue() : jsUndefined();

    bool isLazy = mode == ImportMode::Lazy;
    if (mode == ImportMode::Default && locals == JSValue(function->globals())) {
        // The program may have asked for imports at module level to be lazy: all of them, or those of some modules.
        JSValue all = rt.slot(Runtime::Slot::LazyImportsMode);
        isLazy = all && stringOf(asString(all)) == "all"_s;
        if (!isLazy) {
            isLazy = isInLazyModules(globalObject, name, function->globals(), level.asInt32());
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (isLazy) {
        JSValue lazyImportFunction = function->lookupBuiltin(globalObject, internString(vm, "__lazy_import__"_s));
        RETURN_IF_EXCEPTION(scope, { });
        if (!lazyImportFunction)
            return raiseImportError(globalObject, scope, "__lazy_import__ not found"_s);
        if (lazyImportFunction == rt.builtins()->get(internString(vm, "__lazy_import__"_s)) && PyNativeFunction::tryFrom(lazyImportFunction))
            RELEASE_AND_RETURN(scope, lazyImportName(globalObject, name, function->builtins(), function->globals(), locals, fromList, level.asInt32()));
        RELEASE_AND_RETURN(scope, call(globalObject, lazyImportFunction, name, function->globals(), locals, fromList, level));
    }

    JSValue importFunction = function->lookupBuiltin(globalObject, rt.names.dImport());
    RETURN_IF_EXCEPTION(scope, { });
    if (!importFunction)
        return raiseImportError(globalObject, scope, "__import__ not found"_s);
    if (importFunction == rt.defaultImportFunction())
        RELEASE_AND_RETURN(scope, importName(globalObject, name, function->globals(), jsUndefined(), fromList, level.asInt32()));
    RELEASE_AND_RETURN(scope, call(globalObject, importFunction, name, function->globals(), locals, fromList, level));
}

static JSValue loadSpecialForWith(JSGlobalObject* globalObject, JSValue object, SpecialMethodForWith which, JSValue& self, AttributeCache& cache)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr Special specials[] = { Special::Enter, Special::Exit, Special::AEnter, Special::AExit };
    Special special = specials[static_cast<unsigned>(which)];
    JSValue method = getSpecialMethod(globalObject, object, special, self);
    RETURN_IF_EXCEPTION(scope, { });
    if (method) {
        if (!describeSpecialMethod(globalObject, object, special, cache))
            cache = { };
        return method;
    }
    method = contextManagerMethodOfJS(globalObject, object, which);
    RETURN_IF_EXCEPTION(scope, { });
    if (method)
        return method;

    PyType* type = rt.typeOf(object);
    bool isAsync = which == SpecialMethodForWith::AEnter || which == SpecialMethodForWith::AExit;
    bool hasTheOther = isAsync ? type->lookup(Special::Enter) && type->lookup(Special::Exit) : type->lookup(Special::AEnter) && type->lookup(Special::AExit);
    StrBuilder message;
    message.append('\'', typeNameForMessage(globalObject, type), "' object does not support the "_s, isAsync ? "asynchronous "_s : ""_s, "context manager protocol (missed "_s, stringOf(rt.name(special)), " method)"_s);
    if (hasTheOther)
        message.append(" but it supports the "_s, isAsync ? ""_s : "asynchronous "_s, "context manager protocol. Did you mean to use '"_s, isAsync ? "with"_s : "async with"_s, "'?"_s);
    return raiseTypeError(globalObject, scope, message.toString());
}

static JSValue matchKeys(JSGlobalObject* globalObject, JSValue subject, PyTuple* keys)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned count = keys->length();
    if (!count)
        return rt.emptyTuple();
    // subject.get(key, dummy): one call tells whether the key is there, and a defaultdict is not made to grow.
    JSValue self;
    JSValue get = getMethod(globalObject, subject, rt.names.get(), self);
    RETURN_IF_EXCEPTION(scope, { });
    PySet* seen = PySet::create(globalObject);
    JSValue dummy = PyObject::create(vm, rt.structure<PyObject>(), rt.objectType());
    PyTuple* values = PyTuple::create(globalObject, count);
    for (unsigned i = 0; i < count; ++i) {
        JSValue key = keys->at(i);
        bool isNew = seen->add(globalObject, key);
        RETURN_IF_EXCEPTION(scope, { });
        if (!isNew)
            return raiseValueError(globalObject, scope, "mapping pattern checks duplicate key ("_s, reprForMessage(globalObject, key), ')');
        JSValue value = self ? call(globalObject, get, self, key, dummy) : call(globalObject, get, key, dummy);
        RETURN_IF_EXCEPTION(scope, { });
        if (value == dummy)
            return jsUndefined();
        values->initializeAt(vm, i, value);
    }
    return values;
}

static JSValue matchClass(JSGlobalObject* globalObject, JSValue subject, JSValue typeValue, unsigned positionalCount, PyTuple* names)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* type = PyType::tryFrom(typeValue);
    if (!type)
        return raiseTypeError(globalObject, scope, "class pattern must refer to a class"_s);
    bool matches = isInstance(globalObject, subject, type);
    RETURN_IF_EXCEPTION(scope, { });
    if (!matches)
        return jsUndefined();
    unsigned total = positionalCount + names->length();
    if (!total)
        return rt.emptyTuple();

    PySet* seen = positionalCount && total > 1 ? PySet::create(globalObject) : nullptr;
    PyTuple* attributes = PyTuple::create(globalObject, total);
    // False: no match, or an exception.
    auto extract = [&](unsigned index, JSString* name) {
        if (seen) {
            bool isNew = seen->add(globalObject, name);
            RETURN_IF_EXCEPTION(scope, false);
            if (!isNew) {
                raiseTypeError(globalObject, scope, type->name(), "() got multiple sub-patterns for attribute "_s, strRepr(stringOf(name)));
                return false;
            }
        }
        JSValue value = getAttrIfPresent(globalObject, subject, name);
        RETURN_IF_EXCEPTION(scope, false);
        if (!value)
            return false;
        attributes->initializeAt(vm, index, value);
        return true;
    };

    if (positionalCount) {
        JSValue matchArgs = getAttrIfPresent(globalObject, type, rt.names.dMatchArgs());
        RETURN_IF_EXCEPTION(scope, { });
        bool matchSelf = false;
        PyTuple* tuple = rt.emptyTuple();
        if (matchArgs) {
            tuple = PyTuple::tryFrom(matchArgs);
            if (!tuple || tuple->type() != rt.tupleType())
                return raiseTypeError(globalObject, scope, type->name(), ".__match_args__ must be a tuple (got "_s, typeName(globalObject, matchArgs), ')');
        } else
            matchSelf = type->has(TypeFlag::MatchSelf);
        unsigned allowed = matchSelf ? 1 : tuple->length();
        if (allowed < positionalCount)
            return raiseTypeError(globalObject, scope, type->name(), "() accepts "_s, allowed, " positional sub-pattern"_s, allowed == 1 ? ""_s : "s"_s, " ("_s, positionalCount, " given)"_s);
        if (matchSelf)
            attributes->initializeAt(vm, 0, subject);
        else {
            for (unsigned i = 0; i < positionalCount; ++i) {
                JSValue name = tuple->at(i);
                if (!name.isString())
                    return raiseTypeError(globalObject, scope, "__match_args__ elements must be strings (got "_s, typeName(globalObject, name), ')');
                JSString* interned = internString(globalObject, asString(name));
                RETURN_IF_EXCEPTION(scope, { });
                bool found = extract(i, interned);
                RETURN_IF_EXCEPTION(scope, { });
                if (!found)
                    return jsUndefined();
            }
        }
    }
    for (unsigned i = 0; i < names->length(); ++i) {
        bool found = extract(positionalCount + i, asString(names->at(i)));
        RETURN_IF_EXCEPTION(scope, { });
        if (!found)
            return jsUndefined();
    }
    return attributes;
}

static JSValue copyDictWithoutKeys(JSGlobalObject* globalObject, JSValue subject, PyTuple* keys)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyDict* copy = PyDict::create(globalObject);
    dictUpdate(globalObject, copy, subject);
    RETURN_IF_EXCEPTION(scope, { });
    for (unsigned i = 0; i < keys->length(); ++i) {
        bool removed = copy->remove(globalObject, keys->at(i));
        RETURN_IF_EXCEPTION(scope, { });
        if (!removed)
            return raiseKeyError(globalObject, scope, keys->at(i));
    }
    return copy;
}

static JSValue runRuntimeFunction(JSGlobalObject* globalObject, CallFrame* callFrame, RuntimeFunction function, const RuntimeArguments& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    switch (function) {
    // ---- Building ----
    case RuntimeFunction::BuildTuple:
        RELEASE_AND_RETURN(scope, PyTuple::createFromNegativeIndexed(globalObject, args.first(), args.size()));
    case RuntimeFunction::BuildSet: {
        PySet* set = PySet::create(globalObject);
        for (unsigned i = 0; i < args.size(); ++i) {
            set->add(globalObject, args[i]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return set;
    }
    case RuntimeFunction::BuildMap: {
        bool onlyStringKeys = true;
        for (unsigned i = 0; i + 1 < args.size(); i += 2)
            onlyStringKeys &= args[i].isString();
        PyDict* dict = PyDict::createWithCapacity(globalObject, args.size() / 2, onlyStringKeys);
        for (unsigned i = 0; i + 1 < args.size(); i += 2) {
            dict->set(globalObject, args[i], args[i + 1]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        return dict;
    }
    case RuntimeFunction::BuildString:
        RELEASE_AND_RETURN(scope, buildString(globalObject, args));
    case RuntimeFunction::BuildSlice:
        RELEASE_AND_RETURN(scope, PySlice::create(globalObject, args[0], args[1], args[2]));
    case RuntimeFunction::ListExtend: {
        listExtend(globalObject, asList(args[0]), args[1]);
        if (scope.exception()) [[unlikely]] {
            PyType* type = rt.typeOf(args[1]);
            if (pendingExceptionMatches(globalObject, rt.TypeError()) && !type->lookup(Special::Iter) && !type->lookup(Special::GetItem)) {
                takePendingException(globalObject);
                return raiseTypeError(globalObject, scope, "Value after * must be an iterable, not "_s, type->name());
            }
        }
        return jsUndefined();
    }
    case RuntimeFunction::ListToTuple:
        RELEASE_AND_RETURN(scope, tupleFromList(globalObject, asList(args[0])));
    case RuntimeFunction::SetAdd: {
        // A program can bind __conditional_annotations__ to anything.
        auto* set = PySet::tryFrom(args[0]);
        if (!set || set->type() != rt.setType()) [[unlikely]]
            return raiseTypeError(globalObject, scope, '\'', typeNameForMessage(globalObject, rt.typeOf(args[0])), "' object is not a set"_s);
        scope.release();
        set->add(globalObject, args[1]);
        return jsUndefined();
    }
    case RuntimeFunction::SetUpdate:
        scope.release();
        setUpdate(globalObject, args[0], args[1]);
        return jsUndefined();
    case RuntimeFunction::MapAdd:
        scope.release();
        PyDict::from(args[0])->set(globalObject, args[1], args[2]);
        return jsUndefined();
    case RuntimeFunction::DictUpdate: {
        dictUpdateFromMapping(globalObject, PyDict::from(args[0]), args[1]);
        if (scope.exception() && pendingExceptionMatches(globalObject, rt.AttributeError())) [[unlikely]] {
            JSValue original = takePendingException(globalObject);
            bool hasKeys = hasAttr(globalObject, args[1], rt.names.keys());
            RETURN_IF_EXCEPTION(scope, { });
            if (hasKeys) {
                throwPythonException(globalObject, scope, original);
                return { };
            }
            return raiseTypeError(globalObject, scope, '\'', typeNameForMessage(globalObject, rt.typeOf(args[1])), "' object is not a mapping"_s);
        }
        return jsUndefined();
    }
    case RuntimeFunction::DictMerge:
        scope.release();
        dictMergeForCall(globalObject, PyDict::from(args[0]), args[1], args[2]);
        return jsUndefined();
    case RuntimeFunction::FormatValue:
        RELEASE_AND_RETURN(scope, formatValue(globalObject, args[0], args[1], args[2]));
    case RuntimeFunction::FormatSimple:
        RELEASE_AND_RETURN(scope, formatValue(globalObject, args[0], jsNumber(0), JSValue()));
    case RuntimeFunction::ConvertValue:
        RELEASE_AND_RETURN(scope, convertValue(globalObject, args[0], args[1]));
    case RuntimeFunction::BuildInterpolation: {
        JSValue conversion = jsUndefined();
        if (int32_t character = args[2].asInt32())
            conversion = jsSingleCharacterString(vm, static_cast<Latin1Character>(character));
        RELEASE_AND_RETURN(scope, buildInterpolation(globalObject, args[0], args[1], conversion, args[3] ? args[3] : JSValue(jsEmptyString(vm))));
    }
    case RuntimeFunction::BuildTemplate:
        RELEASE_AND_RETURN(scope, buildTemplate(globalObject, args[0], args[1]));

    // ---- Names ----
    case RuntimeFunction::DeleteName: {
        JSString* name = asString(args[1]);
        if (auto* dict = PyDict::tryFrom(args[0]); dict && dict->type() == rt.dictType() && dict->hasOnlyStringKeys()) {
            if (!dict->remove(vm, name))
                RELEASE_AND_RETURN(scope, raiseNameNotDefined(globalObject, name));
            return jsUndefined();
        }
        delItem(globalObject, args[0], name);
        if (scope.exception() && clearPendingExceptionIfMatches(globalObject, rt.KeyError()))
            RELEASE_AND_RETURN(scope, raiseNameNotDefined(globalObject, name));
        return jsUndefined();
    }
    case RuntimeFunction::DeleteGlobal: {
        if (isFrozenDict(calleeOf(callFrame)->globals())) [[unlikely]]
            return raiseTypeError(globalObject, scope, "'frozendict' object does not support item deletion"_s);
        bool removed = calleeOf(callFrame)->globals()->remove(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (!removed)
            RELEASE_AND_RETURN(scope, raiseNameNotDefined(globalObject, asString(args[0])));
        return jsUndefined();
    }
    case RuntimeFunction::DeleteCell: {
        auto* cell = PyCell::from(args[0]);
        if (!cell->value())
            RELEASE_AND_RETURN(scope, raiseUnbound(globalObject, stringOf(asString(args[1])), static_cast<UnboundKind>(args[2].asInt32())));
        cell->setValue(vm, { });
        return jsUndefined();
    }
    case RuntimeFunction::DeleteAttr:
        scope.release();
        delAttr(globalObject, args[0], asString(args[1]));
        return jsUndefined();
    case RuntimeFunction::DeleteItem:
        scope.release();
        delItem(globalObject, args[0], args[1]);
        return jsUndefined();
    case RuntimeFunction::LoadFromDictOrGlobals: {
        JSValue value = getOptionalItem(globalObject, args[0], asString(args[1]));
        RETURN_IF_EXCEPTION(scope, { });
        if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]]
            RELEASE_AND_RETURN(scope, reifyLazyImport(globalObject, lazy, asString(args[1]), args[0]));
        if (value)
            return value;
        RELEASE_AND_RETURN(scope, loadGlobal(globalObject, calleeOf(callFrame), asString(args[1])));
    }
    case RuntimeFunction::LoadFromDictOrCell: {
        JSValue value = getOptionalItem(globalObject, args[0], asString(args[1]));
        RETURN_IF_EXCEPTION(scope, { });
        if (PyRecord* lazy = asLazyImport(globalObject, value)) [[unlikely]]
            RELEASE_AND_RETURN(scope, reifyLazyImport(globalObject, lazy, asString(args[1]), args[0]));
        if (value)
            return value;
        value = PyCell::from(args[2])->value();
        if (!value)
            RELEASE_AND_RETURN(scope, raiseUnbound(globalObject, stringOf(asString(args[1])), UnboundKind::Free));
        return value;
    }
    case RuntimeFunction::MakeCell:
        RELEASE_AND_RETURN(scope, PyCell::create(globalObject, args[0]));
    case RuntimeFunction::LoadClosure:
        return calleeOf(callFrame)->closure()->at(args[0].asInt32());
    case RuntimeFunction::LoadSuperAttr:
    case RuntimeFunction::LoadZeroSuperAttr:
        RELEASE_AND_RETURN(scope, loadSuperAttribute(globalObject, args[0], args[1], args[2], asString(args[3]), function == RuntimeFunction::LoadZeroSuperAttr, nullptr));
    case RuntimeFunction::LoadSuperMethod:
    case RuntimeFunction::LoadZeroSuperMethod: {
        JSValue self;
        JSValue method = loadSuperAttribute(globalObject, args[0], args[1], args[2], asString(args[3]), function == RuntimeFunction::LoadZeroSuperMethod, &self);
        RETURN_IF_EXCEPTION(scope, { });
        args.set(2, self);
        return method;
    }
    case RuntimeFunction::LoadBuildClass: {
        JSValue builder = calleeOf(callFrame)->lookupBuiltin(globalObject, rt.names.dBuildClass());
        RETURN_IF_EXCEPTION(scope, { });
        if (!builder)
            return raiseNameError(globalObject, scope, "__build_class__ not found"_s);
        return builder;
    }

    // ---- Functions ----
    case RuntimeFunction::MakeFunction:
        RELEASE_AND_RETURN(scope, makeFunction(globalObject, callFrame, args));
    case RuntimeFunction::SetFunctionTypeParams:
        PyFunction::from(args[0])->setTypeParams(vm, args[1]);
        return args[0];
    case RuntimeFunction::MakeGenerator:
        RELEASE_AND_RETURN(scope, makeGenerator(globalObject, callFrame, args[0].asInt32(), args.size() > 1 ? args[1] : JSValue()));
    case RuntimeFunction::ReleaseIterator:
        scope.release();
        rt.releaseIteratorOfLoop(args[0]);
        return jsUndefined();
    case RuntimeFunction::CallArguments:
        RELEASE_AND_RETURN(scope, callArguments(globalObject, args[0], args[1], args[2], args.size() > 3 ? args[3] : JSValue()));
    case RuntimeFunction::TakeKeywordNames:
        return rt.takePendingKeywordNames();

    // ---- Imports ----
    case RuntimeFunction::ImportName:
        RELEASE_AND_RETURN(scope, importNameForInstruction(globalObject, callFrame, asString(args[0]), args[1], args[2], static_cast<ImportMode>(args[3].asInt32())));
    case RuntimeFunction::ImportFrom:
        RELEASE_AND_RETURN(scope, importFrom(globalObject, args[0], asString(args[1])));
    case RuntimeFunction::ImportStar:
        if (!args[1])
            return raiseSystemError(globalObject, scope, "no locals found during 'import *'"_s);
        scope.release();
        importStar(globalObject, args[0], args[1]);
        return jsUndefined();

    // ---- Exceptions ----
    case RuntimeFunction::Raise:
        // `raise` alone raises again what has been through this frame already.
        if (!args[0] && rt.isHandlingException())
            rt.reraisingFrame = callFrame;
        scope.release();
        raiseStatement(globalObject, args[0], args[1], args[2]);
        return { };
    case RuntimeFunction::Reraise:
    case RuntimeFunction::ReraiseGroup:
        rt.reraisingFrame = callFrame;
        throwPythonException(globalObject, scope, args[0]);
        return { };
    case RuntimeFunction::ReraiseUnseen:
    case RuntimeFunction::ReraiseOutOfHandler: {
        JSValue exception = args[0];
        if (function == RuntimeFunction::ReraiseOutOfHandler && rt.monitoring().wantsExceptions()) [[unlikely]]
            exception = rt.monitoring().exceptionLeavesHandler(globalObject, callFrame, exception);
        rt.setHandledExceptionInTopSlot(args[1]);
        RETURN_IF_EXCEPTION(scope, { });
        rt.reraisingFrame = callFrame;
        throwPythonException(globalObject, scope, exception);
        return { };
    }
    case RuntimeFunction::CatchException:
        RELEASE_AND_RETURN(scope, exceptionFromThrownValue(globalObject, args[0]));
    case RuntimeFunction::SetHandledException:
        return rt.setHandledExceptionInTopSlot(args[0]);
    case RuntimeFunction::LeaveHandler:
        rt.handlerIsLeft(callFrame, args[0]);
        return jsUndefined();
    case RuntimeFunction::MatchExceptionGroup: {
        JSValue rest;
        JSValue match = matchExceptionGroup(globalObject, args[0], args[1], rest);
        RETURN_IF_EXCEPTION(scope, { });
        rt.setExceptionGroupRest(rest);
        return match;
    }
    case RuntimeFunction::TakeExceptionGroupRest:
        return rt.takeExceptionGroupRest();
    case RuntimeFunction::PrepareReraiseStar:
        RELEASE_AND_RETURN(scope, prepareReraiseStar(globalObject, args[0], args[1]));

    // ---- with ----
    case RuntimeFunction::WithExit: {
        JSValue exception = args[2];
        JSValue traceback = PyException::from(exception)->traceback();
        if (!traceback)
            traceback = jsUndefined();
        if (args[1])
            RELEASE_AND_RETURN(scope, call(globalObject, args[0], args[1], rt.typeOf(exception), exception, traceback));
        RELEASE_AND_RETURN(scope, call(globalObject, args[0], rt.typeOf(exception), exception, traceback));
    }

    // ---- Generators and coroutines ----
    case RuntimeFunction::GetYieldFromIter:
        // (The body of a generator is called with the generator.)
        RELEASE_AND_RETURN(scope, getYieldFromIter(globalObject, args[0], PyGenerator::from(callFrame->uncheckedArgument(0))->isIterableCoroutine || codeInfoOf(callFrame).flags.contains(CodeFlag::Coroutine)));
    case RuntimeFunction::GetAIterOfLoop:
        RELEASE_AND_RETURN(scope, getAIterOfLoop(globalObject, args[0]));
    case RuntimeFunction::LeaveAsyncFor:
        RELEASE_AND_RETURN(scope, leaveAsyncFor(globalObject, args[0], args[1], true));
    case RuntimeFunction::DropGenerator:
        generatorIsDropped(globalObject, PyGenerator::from(args[0]));
        return jsUndefined();
    case RuntimeFunction::GetANext:
        RELEASE_AND_RETURN(scope, getANext(globalObject, args[0]));
    case RuntimeFunction::EndAsyncFor: {
        JSValue exception = exceptionFromThrownValue(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        if (rt.typeOf(exception)->isSubtypeOf(rt.StopAsyncIteration()))
            return jsUndefined();
        // The wait for the next item has been cut short (the task is cancelled), or has failed. An iterator of JS's is told that nobody
        // will ask again, but what it answers may well come after what it still owes, which is never: nobody waits for that.
        leaveAsyncFor(globalObject, args[1], { }, false);
        if (scope.exception() && !scope.tryClearException()) [[unlikely]]
            return { };
        rt.reraisingFrame = callFrame;
        throwPythonException(globalObject, scope, exception);
        return { };
    }
    case RuntimeFunction::AsyncGenWrap:
        RELEASE_AND_RETURN(scope, wrapAsyncGeneratorValue(globalObject, args[0]));
    case RuntimeFunction::ThrowIntoGenerator:
        return raiseExceptionThrownIntoGenerator(globalObject, scope, args[0]);

    // ---- match ----
    case RuntimeFunction::MatchSequence:
        return jsBoolean(rt.typeOf(args[0])->has(TypeFlag::Sequence));
    case RuntimeFunction::MatchMapping:
        return jsBoolean(rt.typeOf(args[0])->has(TypeFlag::Mapping));
    case RuntimeFunction::MatchKeys:
        RELEASE_AND_RETURN(scope, matchKeys(globalObject, args[0], PyTuple::from(args[1])));
    case RuntimeFunction::MatchClass:
        RELEASE_AND_RETURN(scope, matchClass(globalObject, args[0], args[1], args[2].asInt32(), PyTuple::from(args[3])));
    case RuntimeFunction::GetLength: {
        int64_t size = length(globalObject, args[0]);
        RETURN_IF_EXCEPTION(scope, { });
        return jsNumber(static_cast<double>(size));
    }
    case RuntimeFunction::CopyDictWithoutKeys:
        RELEASE_AND_RETURN(scope, copyDictWithoutKeys(globalObject, args[0], PyTuple::from(args[1])));

    // ---- Typing ----
    case RuntimeFunction::TypeVar:
        RELEASE_AND_RETURN(scope, makeTypeVar(globalObject, args[0], { }, { }));
    case RuntimeFunction::TypeVarWithBound:
        RELEASE_AND_RETURN(scope, makeTypeVar(globalObject, args[0], args[1], { }));
    case RuntimeFunction::TypeVarWithConstraints:
        RELEASE_AND_RETURN(scope, makeTypeVar(globalObject, args[0], { }, args[1]));
    case RuntimeFunction::ParamSpec:
        RELEASE_AND_RETURN(scope, makeParamSpec(globalObject, args[0]));
    case RuntimeFunction::TypeVarTuple:
        RELEASE_AND_RETURN(scope, makeTypeVarTuple(globalObject, args[0]));
    case RuntimeFunction::SetTypeParamDefault:
        RELEASE_AND_RETURN(scope, setTypeParamDefault(globalObject, args[0], args[1]));
    case RuntimeFunction::SubscriptGeneric:
        RELEASE_AND_RETURN(scope, subscriptGeneric(globalObject, args[0]));
    case RuntimeFunction::TypeAlias:
        RELEASE_AND_RETURN(scope, makeTypeAlias(globalObject, args[0], args[1], args[2]));

    // ---- Other ----
    case RuntimeFunction::PrintExpression: {
        JSValue hook = rt.sysDict()->get(internString(vm, "displayhook"_s));
        if (!hook)
            return raiseRuntimeError(globalObject, scope, "lost sys.displayhook"_s);
        RELEASE_AND_RETURN(scope, call(globalObject, hook, args[0]));
    }
    case RuntimeFunction::SetupAnnotations: {
        JSValue annotations = getOptionalItem(globalObject, args[0], rt.names.dAnnotations());
        RETURN_IF_EXCEPTION(scope, { });
        if (!annotations) {
            setItem(globalObject, args[0], rt.names.dAnnotations(), PyDict::create(globalObject));
            RETURN_IF_EXCEPTION(scope, { });
        }
        return jsUndefined();
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

JSC_DEFINE_COMMON_SLOW_PATH(slow_path_py_runtime)
{
    BEGIN();
    auto bytecode = pc->as<OpPyRuntime>();
    RuntimeArguments arguments(callFrame, bytecode.m_argv, bytecode.m_argc);
    RETURN(runRuntimeFunction(globalObject, callFrame, static_cast<RuntimeFunction>(bytecode.m_function), arguments));
}

} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
