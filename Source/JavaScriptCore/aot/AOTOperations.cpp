/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#if ENABLE(FTL_JIT)

#include "AOTInlineCaches.h"
#include "AOTOperationHelpers.h"
#include "CodeBlock.h"
#include "CommonSlowPaths.h"
#include "ExceptionHelpers.h"
#include "FrameTracers.h"
#include "GetPutInfo.h"
#include "JSCInlines.h"
#include "JSGlobalLexicalEnvironment.h"
#include "JSLexicalEnvironment.h"
#include "JSModuleEnvironment.h"
#include "JSModuleRecord.h"
#include "MathCommon.h"
#include "PutByIdFlags.h"
#include "StructureChain.h"
#include "VMTrapsInlines.h"

namespace JSC { namespace AOT {

#define AOT_OPERATION_PROLOGUE(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    auto scope = DECLARE_THROW_SCOPE(vm); \
    UNUSED_VARIABLE(scope)

void noteSlowPathSlow(ASCIILiteral operation, JSValue base, UniquedStringImpl* name, ASCIILiteral detail)
{
    static Lock lock;
    static NeverDestroyed<UncheckedKeyHashMap<String, unsigned>> counts;
    static unsigned total;
    Locker locker { lock };
    StringPrintStream key;
    key.print(operation);
    if (base)
        key.print(" on ", base.isCell() ? base.asCell()->type() : CellType, base.isCell() ? "" : " (not a cell)");
    if (name)
        key.print(" .", String(name));
    key.print(" ", detail);
    // TEMPORARY-SLOT-STATS: what kind of structure, and who asks.
    static NeverDestroyed<UncheckedKeyHashMap<String, unsigned>> callers;
    if (base && base.isCell()) {
        Structure* structure = base.asCell()->structure();
        key.print(structure->isUncacheableDictionary() ? " [uncacheable dictionary]" : structure->isDictionary() ? " [dictionary]" : "", structure->propertyAccessesAreCacheable() ? "" : " [not cacheable]", " ", structure->classInfoForCells()->className);
        VM& vm = base.asCell()->vm();
        if (CallFrame* frame = vm.topCallFrame; frame && frame->isAOTFrame()) {
            if (auto* executable = dataOf(frame)->executable) {
                StringPrintStream caller;
                caller.print(operation, " ", detail, " IN ");
                if (auto* function = dynamicDowncast<FunctionExecutable>(executable))
                    caller.print(function->ecmaNameWithoutGC());
                caller.print(" ", executable->sourceURL(), ":", executable->firstLine());
                callers.get().add(caller.toString(), 0).iterator->value++;
            }
        }
    }
    counts.get().add(key.toString(), 0).iterator->value++;
    if (++total % Options::aotReportSlowPaths())
        return;
    {
        Vector<std::pair<String, unsigned>> sorted;
        for (auto& entry : callers.get())
            sorted.append({ entry.key, entry.value });
        std::ranges::sort(sorted, [](auto& a, auto& b) { return a.second > b.second; });
        dataLogLn("AOT slow path callers, of ", total, ":");
        for (unsigned i = 0; i < std::min<size_t>(sorted.size(), 60); ++i)
            dataLogLn("    ", sorted[i].second, " ", sorted[i].first);
    }
    Vector<std::pair<String, unsigned>> sorted;
    for (auto& entry : counts.get())
        sorted.append({ entry.key, entry.value });
    std::ranges::sort(sorted, [](auto& a, auto& b) { return a.second > b.second; });
    dataLogLn("AOT slow paths, of ", total, ":");
    for (unsigned i = 0; i < std::min<size_t>(sorted.size(), 90); ++i)
        dataLogLn("    ", sorted[i].second, " ", sorted[i].first);
}

#define AOT_BINARY_OPERATION(name, function) \
    JSC_DEFINE_JIT_OPERATION(name, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight)) \
    { \
        AOT_OPERATION_PROLOGUE(globalObject); \
        OPERATION_RETURN(scope, JSValue::encode(function(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)))); \
    }

AOT_BINARY_OPERATION(operationAOTValueAdd, jsAdd)
AOT_BINARY_OPERATION(operationAOTValueSub, jsSub)
AOT_BINARY_OPERATION(operationAOTValueMul, jsMul)
AOT_BINARY_OPERATION(operationAOTValueDiv, jsDiv)
AOT_BINARY_OPERATION(operationAOTValueMod, jsRemainder)
AOT_BINARY_OPERATION(operationAOTValuePow, jsPow)
AOT_BINARY_OPERATION(operationAOTValueBitAnd, jsBitwiseAnd)
AOT_BINARY_OPERATION(operationAOTValueBitOr, jsBitwiseOr)
AOT_BINARY_OPERATION(operationAOTValueBitXor, jsBitwiseXor)
AOT_BINARY_OPERATION(operationAOTValueLShift, jsLShift)
AOT_BINARY_OPERATION(operationAOTValueRShift, jsRShift)
AOT_BINARY_OPERATION(operationAOTValueURShift, jsURShift)

JSC_DEFINE_JIT_OPERATION(operationAOTValueNegate, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue primitive = JSValue::decode(encodedOperand).toPrimitive(globalObject, PreferNumber);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (primitive.isHeapBigInt())
        OPERATION_RETURN(scope, JSValue::encode(JSBigInt::unaryMinus(globalObject, primitive.asHeapBigInt())));
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(-primitive.toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueBitNot, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsBitwiseNot(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueInc, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsInc(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTValueDec, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsDec(globalObject, JSValue::decode(encodedOperand))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumber, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(jsNumber(JSValue::decode(encodedOperand).toNumber(globalObject))));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToNumeric, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toNumeric(globalObject)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTToString, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedOperand).toString(globalObject)));
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTToBoolean, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedOperand))
{
    return JSValue::decode(encodedOperand).toBoolean(globalObject);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLess, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLess<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareLessEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLessEq<true>(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreater, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLess<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareGreaterEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, jsLessEq<false>(globalObject, JSValue::decode(encodedRight), JSValue::decode(encodedLeft)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::equal(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTCompareStrictEq, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedLeft, EncodedJSValue encodedRight))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::strictEqual(globalObject, JSValue::decode(encodedLeft), JSValue::decode(encodedRight)));
}

// TEMPORARY-SHAPE-STATS: with the caches off (aotDisableFastPaths=1025) every read comes by here.
static void noteShapeOfRead(JSValue base, const PropertySlot& slot)
{
    ASCIILiteral kind = "SHAPE not a cell"_s;
    if (base.isCell()) {
        JSCell* cell = base.asCell();
        Structure* structure = cell->structure();
        switch (cell->type()) {
        case StringType:
            kind = "SHAPE string"_s;
            break;
        case ArrayType:
        case DerivedArrayType:
            kind = "SHAPE array"_s;
            break;
        case JSFunctionType:
            kind = "SHAPE function"_s;
            break;
        case FinalObjectType: {
            kind = structure->isDictionary() ? "SHAPE plain object, dictionary"_s : "SHAPE plain object, other"_s;
            unsigned steps = 0;
            for (Structure* current = structure; current && steps < 200; current = current->previousID(), ++steps) {
                if (uint8_t known = kindOfKnownShape(current)) {
                    if (known == 1)
                        kind = !steps ? "SHAPE literal"_s : "SHAPE literal, changed since"_s;
                    else
                        kind = !steps ? "SHAPE constructed"_s : "SHAPE constructed, changed since"_s;
                    break;
                }
            }
            break;
        }
        default:
            kind = cell->isObject() ? "SHAPE other object"_s : "SHAPE other cell"_s;
            break;
        }
    }
    ASCIILiteral result = slot.isUnset() ? "absent"_s
        : slot.slotBase() == base ? (slot.isCacheableValue() ? "own value"_s : "own, not a plain value"_s)
        : slot.isCacheableValue() ? "inherited value"_s : slot.isCacheableGetter() ? "inherited getter"_s : "inherited, something else"_s;
    noteSlowPath(kind, JSValue(), nullptr, result);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetById, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t identifierIndex, Slot* cache))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    const Identifier& ident = identifierAt(callFrame, identifierIndex);
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (slot.isUnset() && structureBefore && structureBefore->knownShape())
        callerData(callFrame)->instance->lookAtObjectPrototype();
    ASCIILiteral whyNotCached = cacheGetById(globalObject, callerData(callFrame), base, structureBefore, ident, slot, cache);
    noteSlowPath("get_by_id"_s, base, ident.impl(), whyNotCached.isEmpty() ? "cached"_s : whyNotCached);
    if (Options::aotReportSlowPaths()) [[unlikely]]
        noteShapeOfRead(base, slot);
    OPERATION_RETURN(scope, JSValue::encode(result));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutById, void, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t flagBits))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    JSValue value = JSValue::decode(encodedValue);
    const Identifier& ident = identifierAt(callFrame, identifierIndex);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;

    PutPropertySlot slot(base, isStrict, putByIdContextOf(callFrame));
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (!isDirect)
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    cachePutById(globalObject, callerData(callFrame), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetByVal, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedProperty))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    noteSlowPath("get_by_val"_s, base, property.isString() && !asString(property)->isRope() && asString(property)->tryGetValueImpl() && asString(property)->tryGetValueImpl()->isAtom() ? static_cast<UniquedStringImpl*>(const_cast<StringImpl*>(asString(property)->tryGetValueImpl())) : nullptr, property.isInt32() ? "int32"_s : property.isNumber() ? "double"_s : property.isString() ? "string"_s : property.isSymbol() ? "symbol"_s : "other"_s);

    if (base.isObject() && property.isString()) [[likely]] {
        // A name nobody has made an atom of is not the name of any ordinary property. One that is gets to be found faster
        // next time (Lowering::lowerGetByVal()).
        auto existingAtomString = asString(property)->toExistingAtomString(globalObject);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (existingAtomString) {
            PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
            OPERATION_RETURN(scope, JSValue::encode(getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, existingAtomString.data), slot)));
        }
    }

    if (base.isObject() && property.isSymbol()) {
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        OPERATION_RETURN(scope, JSValue::encode(getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(asSymbol(property)->privateName()), slot)));
    }

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        uint32_t i = *index;
        if (isJSString(base)) {
            if (asString(base)->canGetIndex(i))
                OPERATION_RETURN(scope, JSValue::encode(asString(base)->getIndex(globalObject, i)));
        } else if (base.isObject()) {
            JSObject* object = asObject(base);
            if (JSValue result = object->tryGetIndexQuickly(i))
                OPERATION_RETURN(scope, JSValue::encode(result));
            // Past the end of an ordinary array, or in a hole in it, with nothing of the kind in what it inherits from.
            Structure* structure = object->structure();
            if (structure->realm() == globalObject && globalObject->isOriginalArrayStructure(structure) && !hasAnyArrayStorage(structure->indexingType()) && globalObject->arrayPrototypeChainIsSane())
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
        OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, i)));
    }

    base.requireObjectCoercible(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(base.get(globalObject, key)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTPutByVal, void, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, EncodedJSValue encodedProperty, EncodedJSValue encodedValue, uint32_t isStrict))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue base = JSValue::decode(encodedBase);
    JSValue property = JSValue::decode(encodedProperty);
    JSValue value = JSValue::decode(encodedValue);

    if (std::optional<uint32_t> index = property.tryGetAsUint32Index()) {
        uint32_t i = *index;
        if (base.isObject()) {
            JSObject* object = asObject(base);
            if (object->trySetIndexQuickly(vm, i, value))
                OPERATION_RETURN(scope);
            scope.release();
            object->methodTable()->putByIndex(object, globalObject, i, value, isStrict);
            OPERATION_RETURN(scope);
        }
        scope.release();
        base.putByIndex(globalObject, i, value, isStrict);
        OPERATION_RETURN(scope);
    }

    auto key = property.toPropertyKey(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    PutPropertySlot slot(base, isStrict);
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    base.put(globalObject, key, value, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, key, slot);
    OPERATION_RETURN(scope);
}

// What a name that no enclosing function or module declares refers to. The answer is a scope object; where it cannot change
// (an import, or a global while no global lexical binding has come to shadow it) it is cached.
//     cache->pointer: the scope. cache->offset: the global lexical binding epoch it was found in, plus one.
JSC_DEFINE_JIT_OPERATION(operationAOTResolveScope, JSObject*, (JSGlobalObject* globalObject, JSScope* startScope, uint32_t identifierIndex, Slot* cache, uint32_t localScopeDepth))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    Slot unusedSlot { };
    if (Options::aotDisableFastPaths() & 32) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    UNUSED_VARIABLE(uid);
    // The compiler has seen what the code around the function declares, and this is not among it: asking each of those scopes by
    // name would only be the reason for them to know the names of what is in them.
    if (localScopeDepth == Site::resolvesInGlobalScopes)
        startScope = globalObject->globalLexicalEnvironment();
    JSObject* resolved = JSScope::resolve(globalObject, startScope, ident);
    OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));

    bool cacheable = false;
    if (resolved->isGlobalObject()) {
        bool hasProperty = resolved->hasProperty(globalObject, ident);
        OPERATION_RETURN_IF_EXCEPTION(scope, static_cast<JSObject*>(nullptr));
        cacheable = hasProperty && resolved == globalObject;
    } else if (resolved->isGlobalLexicalEnvironment())
        cacheable = resolved == globalObject->globalLexicalEnvironment();
    else if (resolved->type() == ModuleEnvironmentType)
        cacheable = true;
    else if (resolved->type() == LexicalEnvironmentType) {
        // Declared there, that is. What sloppy eval adds to a scope is a property of it, and is there when eval has put it there.
        cacheable = !uncheckedDowncast<JSLexicalEnvironment>(resolved)->symbolTable()->get(uid).isNull();
    }

    // A with scope or a scope that sloppy eval can add to, between here and there, makes the answer good for this time only.
    unsigned depth = 0;
    if (cacheable) {
        for (JSScope* current = startScope; current && current != resolved; current = current->next()) {
            if (current->isWithScope() || (current->isJSLexicalEnvironment() && uncheckedDowncast<JSLexicalEnvironment>(current)->symbolTable()->usesSloppyEval())) {
                cacheable = false;
                break;
            }
            depth++;
        }
    }
    if (cacheable && resolved->type() == LexicalEnvironmentType) {
        // A scope of a function that encloses this one: another object each time, but how the scopes nest is how the code does.
        cache->pointer = nullptr;
        cache->offset = Slot::resolvesByDepth | depth;
    } else if (uint32_t epochPlusOne = globalObject->globalLexicalBindingEpoch() + 1; cacheable && !(epochPlusOne & Slot::resolvesByDepth)) {
        cache->offset = 0;
        cache->pointer = resolved;
        cache->offset = epochPlusOne;
    }
    OPERATION_RETURN(scope, resolved);
}

// For op_get_from_scope and op_put_to_scope: the variable is at `offset` in any environment that has this one's symbol table.
static void cacheVariableOfEnvironment(VM& vm, Data* codeBlock, Slot* cache, JSLexicalEnvironment* environment, ScopeOffset offset)
{
    if (offset.offset() > Slot::offsetMask)
        return;
    cache->structureID = StructureID();
    WTF::storeStoreFence();
    cache->offset = Slot::pointerIsCell | offset.offset();
    cache->pointer = environment->symbolTable();
    WTF::storeStoreFence();
    cache->structureID = environment->structureID();
    didFillSlot(vm, codeBlock);
}

//     cache->structureID: what the structure of the scope has to be for any of this.
//     cache->pointer: the address of the variable, if it has one that lasts. Or see cacheVariableOfEnvironment().
//     cache->offset: with no pointer, where the property is in the global object.
JSC_DEFINE_JIT_OPERATION(operationAOTGetFromScope, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* scopeObject, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    bool throwIfNotFound = how & Site::throwsIfNotFound;
    Slot unusedSlot { };
    if (Options::aotDisableFastPaths() & 32) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    noteSlowPath("get_from_scope"_s, scopeObject, uid);

    // For a scope of which the code can only ever see the one at this place: the global object, its lexical environment, the
    // module's environment. What the name resolves to may be a scope of another kind the next time.
    auto cacheAddress = [&](void* address) {
        cache->structureID = StructureID();
        WTF::storeStoreFence();
        cache->offset = Slot::pointerIsNotCell;
        cache->pointer = address;
        WTF::storeStoreFence();
        cache->structureID = scopeObject->structureID();
        didFillSlot(vm, callerData(callFrame));
    };

    if (scopeObject->type() == ModuleEnvironmentType) {
        auto* environment = uncheckedDowncast<JSModuleEnvironment>(scopeObject);
        SymbolTable* symbolTable = environment->symbolTable();
        ScopeOffset offset;
        if (!(how & Site::isImport)) {
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto iter = symbolTable->find(locker, uid);
            if (iter != symbolTable->end(locker))
                offset = iter->value.scopeOffset();
        }
        if (!!offset) {
            // Empty is either a binding in its dead zone, which the check_tdz that follows reports, or a function
            // declaration nobody has asked for yet.
            JSValue result = JSModuleEnvironment::readLazyClosureVar(vm, environment, offset);
            if (result)
                cacheAddress(environment->variableAt(offset).slot());
            OPERATION_RETURN(scope, JSValue::encode(result));
        }

        // An import: the variable is the exporter's.
        auto resolution = environment->moduleRecord()->resolveImport(globalObject, ident);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        if (resolution.type == AbstractModuleRecord::Resolution::Type::Resolved) {
            JSModuleEnvironment* exporter = resolution.moduleRecord->moduleEnvironment();
            auto entry = exporter->symbolTable()->get(resolution.localName.impl());
            if (!entry.isNull()) {
                JSValue result = JSModuleEnvironment::readLazyClosureVar(vm, exporter, entry.scopeOffset());
                if (result)
                    cacheAddress(exporter->variableAt(entry.scopeOffset()).slot());
                OPERATION_RETURN(scope, JSValue::encode(result));
            }
        }
    }

    if (scopeObject->type() == LexicalEnvironmentType) {
        // Every scope made from the same place in the code has the same table.
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scopeObject);
        auto entry = environment->symbolTable()->get(uid);
        if (!entry.isNull()) {
            cacheVariableOfEnvironment(vm, callerData(callFrame), cache, environment, entry.scopeOffset());
            OPERATION_RETURN(scope, JSValue::encode(environment->variableAt(entry.scopeOffset()).get()));
        }
    }

    OPERATION_RETURN(scope, JSValue::encode(scopeObject->getPropertySlot(globalObject, ident, [&](bool found, PropertySlot& slot) -> JSValue {
        if (!found) {
            if (throwIfNotFound)
                throwException(globalObject, scope, createUndefinedVariableError(globalObject, ident));
            return jsUndefined();
        }

        if (scopeObject->isGlobalLexicalEnvironment()) {
            JSValue result = slot.getValue(globalObject, ident);
            if (result == jsTDZValue()) {
                throwException(globalObject, scope, createTDZError(globalObject, ident.string()));
                return jsUndefined();
            }
            auto* environment = uncheckedDowncast<JSGlobalLexicalEnvironment>(scopeObject);
            auto entry = environment->symbolTable()->get(uid);
            if (!entry.isNull())
                cacheAddress(environment->variableAt(entry.scopeOffset()).slot());
            return result;
        }

        if (scopeObject->isGlobalObject()) {
            // A var or a function declared by a program is a variable of the global object's, not a property in its storage.
            auto* variables = uncheckedDowncast<JSGlobalObject>(scopeObject);
            auto entry = variables->symbolTable()->get(uid);
            if (!entry.isNull())
                cacheAddress(variables->variableAt(entry.scopeOffset()).slot());
            else if (slot.isCacheableValue() && slot.slotBase() == scopeObject && scopeObject->structure()->propertyAccessesAreCacheable()) {
                cache->structureID = StructureID();
                WTF::storeStoreFence();
                cache->offset = slot.cachedOffset();
                cache->pointer = nullptr;
                WTF::storeStoreFence();
                cache->structureID = scopeObject->structureID();
                didFillSlot(vm, callerData(callFrame));
            }
        }
        return slot.getValue(globalObject, ident);
    })));
}

// The environment that an import the compiler knows the place of is in.
JSC_DEFINE_JIT_OPERATION(operationAOTFillImportSlot, JSObject*, (JSGlobalObject* globalObject, JSObject* importer, uint32_t slot))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    auto* environment = uncheckedDowncast<JSModuleEnvironment>(importer);
    OPERATION_RETURN(scope, uncheckedDowncast<JSModuleRecord>(environment->moduleRecord())->fillImportSlot(globalObject, slot));
}

JSC_DEFINE_JIT_OPERATION(operationAOTReadLazyClosureVar, EncodedJSValue, (JSGlobalObject* globalObject, JSObject* scopeObject, uint32_t offset))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSModuleEnvironment::readLazyClosureVar(vm, scopeObject, ScopeOffset(offset))));
}

// closureOffsetPlusOne: nonzero if the compiler knows the variable is at that offset (less one) of the environment.
//     cache->offset: 1 once the variable's watchpoint set is known. cache->pointer: the set, if it has one.
// how: the resolve mode, the initialization mode above it (two bits), whether the code is strict above that.
JSC_DEFINE_JIT_OPERATION(operationAOTPutToScope, void, (JSGlobalObject* globalObject, JSObject* scopeObject, EncodedJSValue encodedValue, uint32_t identifierIndex, Slot* cache, uint32_t how))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    Slot unusedSlot { };
    if (Options::aotDisableFastPaths() & 64) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(callFrame, identifierIndex);
    UniquedStringImpl* uid = ident.impl();
    GetPutInfo getPutInfo(static_cast<ResolveMode>(how & 1), GlobalProperty, static_cast<InitializationMode>((how >> 1) & 3), how & 8 ? ECMAMode::strict() : ECMAMode::sloppy());
    JSValue value = JSValue::decode(encodedValue);

    if (scopeObject->isJSLexicalEnvironment()) {
        auto* environment = uncheckedDowncast<JSLexicalEnvironment>(scopeObject);
        SymbolTable* symbolTable = environment->symbolTable();
        ScopeOffset offset;
        InlineWatchpointSet* set = nullptr;
        {
            ConcurrentJSLocker locker(symbolTable->m_lock);
            auto iter = symbolTable->find(locker, uid);
            if (iter != symbolTable->end(locker) && (!iter->value.isReadOnly() || isInitialization(getPutInfo.initializationMode()))) {
                offset = iter->value.scopeOffset();
                set = iter->value.watchpointSet();
            }
        }
        if (!!offset) {
            environment->variableAt(offset).set(vm, environment, value);
            // From now on it is written to without a word to anybody.
            if (set)
                set->invalidate(vm, StringFireDetail("Executed op_put_to_scope in code from the static compiler"));
            cacheVariableOfEnvironment(vm, callerData(callFrame), cache, environment, offset);
            OPERATION_RETURN(scope);
        }
    }

    bool hasProperty = scopeObject->hasProperty(globalObject, ident);
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (hasProperty && scopeObject->isGlobalLexicalEnvironment() && !isInitialization(getPutInfo.initializationMode())) {
        PropertySlot slot(scopeObject, PropertySlot::InternalMethodType::Get);
        JSGlobalLexicalEnvironment::getOwnPropertySlot(scopeObject, globalObject, ident, slot);
        if (slot.getValue(globalObject, ident) == jsTDZValue()) {
            throwException(globalObject, scope, createTDZError(globalObject, ident.string()));
            OPERATION_RETURN(scope);
        }
    }
    if (getPutInfo.resolveMode() == ThrowIfNotFound && !hasProperty) {
        throwException(globalObject, scope, createUndefinedVariableError(globalObject, ident));
        OPERATION_RETURN(scope);
    }

    PutPropertySlot slot(scopeObject, getPutInfo.ecmaMode().isStrict(), PutPropertySlot::UnknownContext, isInitialization(getPutInfo.initializationMode()));
    scope.release();
    scopeObject->methodTable()->put(scopeObject, globalObject, ident, value, slot);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTThrow, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    throwException(globalObject, scope, JSValue::decode(encodedValue));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTCheckType, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t mask))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    unsigned tag = soundTypeTag(JSValue::decode(encodedValue));
    if (soundTypeMaskAdmits(mask, JSValue::decode(encodedValue)))
        OPERATION_RETURN(scope);
    throwTypeError(globalObject, scope, makeString("Type check failed: expected "_s, toCString(SoundTypeMaskDump(mask)).span(), ", got "_s, toCString(SoundTypeMaskDump(tag)).span()));
    OPERATION_RETURN(scope);
}

JSC_DEFINE_JIT_OPERATION(operationAOTHandleTraps, void, (JSGlobalObject* globalObject))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    ASSERT(vm.traps().needHandling(VMTraps::AsyncEvents));
    vm.traps().handleTraps(VMTraps::AsyncEvents);
    OPERATION_RETURN(scope);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTWriteBarrier, void, (VM* vmPointer, JSCell* cell))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    vm.writeBarrierSlowPath(cell);
}

// Null if the exception is not one that code gets to catch.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTCatch, Exception*, (VM* vmPointer))
{
    VM& vm = *vmPointer;
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    RELEASE_ASSERT(!!scope.exception());
    Exception* exception = scope.exception();
    if (!scope.tryClearException())
        return nullptr;
    return exception;
}

// The jump offset, relative to the switch; 0 for the default.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSwitchString, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t tableIndex))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        return 0;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto string = asString(value)->value(globalObject);
    if (scope.exception()) [[unlikely]]
        return INT32_MIN; // Out of memory resolving a rope: the caller checks.
    const UnlinkedStringJumpTable& table = callerCode(callFrame)->unlinkedStringSwitchJumpTable(tableIndex);
    return table.offsetForValue(string.data.impl());
}

// The character of a one character string, or -1.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTSwitchChar, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue))
{
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        return -1;
    JSString* string = asString(value);
    if (string->length() != 1)
        return -1;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto view = string->view(globalObject);
    if (scope.exception()) [[unlikely]]
        return INT32_MIN;
    return view[0];
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTFMod, double, (double a, double b))
{
    return Math::fmodDouble(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTPow, double, (double a, double b))
{
    return operationMathPow(a, b);
}

JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTDoubleToInt32, int32_t, (double value))
{
    return JSC::toInt32(value);
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
