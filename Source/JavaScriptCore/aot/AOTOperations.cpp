/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTOperations.h"

#include "AOTImage.h"

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
#include "MegamorphicCache.h"
#include "PutByIdFlags.h"
#include "StructureChain.h"
#include "VMTrapsInlines.h"

namespace JSC { namespace AOT {

#define AOT_OPERATION_PROLOGUE(globalObject) \
    VM& vm = (globalObject)->vm(); \
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm); \
    JITOperationPrologueCallFrameTracer tracer(vm, callFrame); \
    countOperationOnBehalfOf(globalObject, callFrame); \
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
        if (FunctionRef function = vm.topCallFrame ? functionThatCalled(vm.topCallFrame) : FunctionRef { }) {
            if (auto* executable = function.executable()) {
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
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    Instance& instance = *globalObject->aotInstance();
    if (base.isCell()) {
        auto& known = instance.customGetterFor(base.asCell()->structureID().bits(), ident.impl());
        if (known.uid == ident.impl() && known.structureID == base.asCell()->structureID().bits() && vm.megamorphicCache() && known.epoch == vm.megamorphicCache()->epoch()) {
            auto getter = GetValueFunc(std::bit_cast<GetValueFunc::Ptr>(known.getter));
            OPERATION_RETURN(scope, getter(known.holder->globalObject(), known.isGivenHolder ? JSValue::encode(known.holder) : encodedBase, ident));
        }
    }
    PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
    Structure* structureBefore = base.isCell() ? base.asCell()->structure() : nullptr;
    JSValue result = getByIdAndFillMegamorphicCache(globalObject, base, ident, slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (slot.isCacheableCustom() && base.isObject() && base.asCell()->structure() == structureBefore)
        noteCustomGetter(globalObject, instance, asObject(base), ident, slot);
    if (slot.isUnset() && structureBefore && structureBefore->knownShape())
        caller(globalObject, callFrame).instance->lookAtObjectPrototype();
    ASCIILiteral whyNotCached = cacheGetById(globalObject, callerData(globalObject, callFrame), base, structureBefore, ident, slot, cache, true);
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
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
    bool isDirect = flagBits & 1;
    bool isStrict = flagBits & 2;

    PutPropertySlot slot(base, isStrict, putByIdContextOf(globalObject, callFrame));
    Structure* oldStructure = base.isCell() ? base.asCell()->structure() : nullptr;
    if (isDirect && oldStructure->bornAs() && SlotsOfBornObjects::areStructs()) [[unlikely]] {
        // (A field that a class declares and gives nothing to start with is undefined until the constructor gets to it. There is nothing in its slot until then.)
        if (!asObject(base)->putDirect(vm, ident, value, slot) && !value.isUndefined()) [[unlikely]]
            throwTypeError(globalObject, scope, TypedFieldError);
    } else if (isDirect)
        CommonSlowPaths::putDirectWithReify(vm, globalObject, asObject(base), ident, value, slot, &oldStructure);
    else
        base.putInline(globalObject, ident, value, slot);
    if (scope.exception()) [[unlikely]] {
        // BUN_AOT_LOG_REFUSED=1: which store it was that a struct would not have. (The error says neither, and whoever catches it may say still less.)
        static const bool logs = !!getenv("BUN_AOT_LOG_REFUSED");
        if (logs && oldStructure && oldStructure->bornAs()) {
            dataLog("AOT: REFUSED: .", ident.impl(), isDirect ? " (direct)" : "", " of what was born as ", oldStructure->bornAs(), " is given ");
            dumpType(WTF::dataFile(), typeOfValue(value));
            dataLogLn();
        }
    }
    OPERATION_RETURN_IF_EXCEPTION(scope);
    if (Options::aotReportSlowPaths()) [[unlikely]] { // TEMPORARY-SLOT-STATS
        bool tookRoom = base.isCell() && oldStructure && base.asCell()->structure()->outOfLineCapacity() != oldStructure->outOfLineCapacity();
        bool isBorn = oldStructure && oldStructure->bornAs();
        ASCIILiteral what = "?"_s;
        switch (slot.type()) {
        case PutPropertySlot::Uncachable:
            what = !base.isObject() ? "uncacheable: no object"_s : oldStructure->isDictionary() ? "uncacheable: a dictionary"_s : isBorn ? "uncacheable, of a struct"_s : "uncacheable"_s;
            break;
        case PutPropertySlot::ExistingProperty:
            what = isBorn ? "there already, of a struct but no field"_s : "there already"_s;
            break;
        case PutPropertySlot::NewProperty:
            what = tookRoom ? "new, and took room"_s : isBorn ? "new, of a struct but no field"_s : "new"_s;
            break;
        case PutPropertySlot::ExistingFieldOfStruct:
            what = "a field that is there already"_s;
            break;
        case PutPropertySlot::NewFieldOfStruct:
            what = "a new field"_s;
            break;
        default:
            what = "a setter or the like"_s;
            break;
        }
        noteSlowPath("put_by_id"_s, base, ident.impl(), what);
        noteSlowPath(isDirect ? "put_by_id (direct), how"_s : "put_by_id, how"_s, JSValue(), nullptr, what);
        noteSlowPath("put_by_id, the site"_s, JSValue(), nullptr, SharedData::contains(cache) ? "has no slot of its own"_s : "has a slot"_s);
    }
    // (What makes a property come what may has done what any store would have, if nothing that the object inherits from has a say in the matter.)
    if (!isDirect || (slot.type() == PutPropertySlot::NewProperty && base.isObject() && asObject(base)->canPerformFastPutInline(vm, ident)))
        fillMegamorphicCacheAfterPut(globalObject, base, oldStructure, ident, slot);
    cachePutById(globalObject, callerData(globalObject, callFrame), base, oldStructure, ident, slot, isDirect, cache);
    OPERATION_RETURN(scope);
}

// Graph::readsElementsOrEmpty. index: a number that is not negative, and whole.
JSC_DEFINE_JIT_OPERATION(operationAOTGetElementOrEmpty, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedArray, EncodedJSValue encodedIndex))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSObject* array = JSValue::decode(encodedArray).toObject(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    PropertySlot slot(array, PropertySlot::InternalMethodType::Get);
    double index = JSValue::decode(encodedIndex).asNumber();
    bool has = index <= MAX_ARRAY_INDEX ? array->getPropertySlot(globalObject, static_cast<unsigned>(index), slot) : array->getPropertySlot(globalObject, Identifier::from(vm, index), slot);
    OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    if (!has)
        OPERATION_RETURN(scope, encodedJSValue());
    OPERATION_RETURN(scope, JSValue::encode(index <= MAX_ARRAY_INDEX ? slot.getValue(globalObject, static_cast<unsigned>(index)) : slot.getValue(globalObject, Identifier::from(vm, index))));
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
    if ((Options::aotDisableFastPaths() & 32) || SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
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
    if ((Options::aotDisableFastPaths() & 32) || SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
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
        didFillSlot(vm, callerData(globalObject, callFrame));
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
            cacheVariableOfEnvironment(vm, callerData(globalObject, callFrame), cache, environment, entry.scopeOffset());
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
                didFillSlot(vm, callerData(globalObject, callFrame));
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
    if ((Options::aotDisableFastPaths() & 64) || SharedData::contains(cache)) [[unlikely]]
        cache = &unusedSlot;
    const Identifier& ident = identifierAt(globalObject, callFrame, identifierIndex);
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
            cacheVariableOfEnvironment(vm, callerData(globalObject, callFrame), cache, environment, offset);
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

JSC_DEFINE_JIT_OPERATION(operationAOTGetLengthTheLongWay, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    OPERATION_RETURN(scope, JSValue::encode(JSValue::decode(encodedBase).get(globalObject, vm.propertyNames->length)));
}

// Comes back if the value is of the family by then.
JSC_DEFINE_JIT_OPERATION(operationAOTAssertBornAs, void, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t family))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isUndefinedOrNull()) {
        // What getting at a property of it says.
        if (!SlotsOfBornObjects::audits())
            value.toObject(globalObject);
        OPERATION_RETURN(scope);
    }
    if (value.isObject() && (asObject(value)->structure()->bornAs() == family || Instance::adopt(vm, asObject(value), safeCast<uint16_t>(family))))
        OPERATION_RETURN(scope);
    if (!value.isObject())
        SlotsOfBornObjects::s_whyNotAdopted = "it is no object"_s;
    if (SlotsOfBornObjects::audits()) {
        SlotsOfBornObjects::audit(SlotsOfBornObjects::s_whyNotAdopted, safeCast<uint16_t>(family), value);
        OPERATION_RETURN(scope);
    }
    throwTypeError(globalObject, scope, makeString("Type check failed: this is not an object of the type it is used as, and cannot be made one: "_s, SlotsOfBornObjects::s_whyNotAdopted));
    OPERATION_RETURN(scope);
}

// What stands for whatever is of a type of a family and is not one of the family. Nobody writes to it.
alignas(16) static const EncodedJSValue s_structWithNothingInIt[2 + 256] = { };

// Where typed code is to look for what the value has: in the value, if it is of the family by now.
JSC_DEFINE_JIT_OPERATION(operationAOTViewAs, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t family))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (value.isObject() && (asObject(value)->structure()->bornAs() == family || Instance::adopt(vm, asObject(value), safeCast<uint16_t>(family)))) {
        Instance::noteView("made one of the family"_s);
        OPERATION_RETURN(scope, encodedValue);
    }
    Instance::noteView(value.isObject() ? SlotsOfBornObjects::s_whyNotAdopted : value.isUndefinedOrNull() ? "it is undefined or null"_s : "it is no object"_s);
    if (SlotsOfBornObjects::audits() && !value.isUndefinedOrNull()) [[unlikely]]
        SlotsOfBornObjects::audit(value.isObject() ? SlotsOfBornObjects::s_whyNotAdopted : "it is no object"_s, safeCast<uint16_t>(family), value);
    OPERATION_RETURN(scope, static_cast<EncodedJSValue>(std::bit_cast<uintptr_t>(&s_structWithNothingInIt[0])));
}

JSC_DEFINE_JIT_OPERATION(operationAOTGetFieldTheLongWay, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint64_t which))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    uint16_t family = static_cast<uint16_t>(which >> 32);
    unsigned slot = which >> 48 & 0xff;
    bool undefinedWillDo = which >> 56 & 1;
    JSValue base = JSValue::decode(encodedBase);
    UniquedStringImpl* uid = StaticHeap::identifiersOfProgram()[static_cast<uint32_t>(which)];
    JSValue value;
    MegamorphicCache* cache = vm.megamorphicCache();
    if (auto* known = cache && base.isObject() ? cache->findLoad(asObject(base)->structureID(), uid) : nullptr) {
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
        PropertySlot slot(base, PropertySlot::InternalMethodType::Get);
        value = getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, uid), slot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
    }
    if (value.isUndefined() && undefinedWillDo)
        OPERATION_RETURN(scope, JSValue::encode(value));
    if (SlotsOfBornObjects::says(family, slot, value) == SlotsOfBornObjects::Says::Refuses) {
        throwTypeError(globalObject, scope, "Type check failed: a property is not what the type of the object says it is"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(SlotsOfBornObjects::asHeld(family, slot, value)));
}

// The Structure of the base does not say that the field is in its slot.
JSC_DEFINE_JIT_OPERATION(operationAOTReadField, EncodedJSValue, (JSGlobalObject* globalObject, EncodedJSValue encodedBase, uint32_t which))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    unsigned slot = which >> 16 & 0xff;
    bool undefinedWillDo = which >> 24 & 1;
    const SlotsOfBornObjects::Named& field = SlotsOfBornObjects::fieldWithId(slot, static_cast<uint16_t>(which));
    uint16_t family = SlotsOfBornObjects::familyOf(field);
    JSValue base = JSValue::decode(encodedBase);
    ASCIILiteral why = "it is no object"_s;
    if (base.isObject()) {
        JSObject* object = asObject(base);
        // What code that knows nothing of the types has made is made one of the family, if it can be.
        if (object->structure()->isNeverAdopted())
            why = "its like is never adopted"_s;
        else if (!object->structure()->bornAs())
            why = Instance::adopt(vm, object, family) ? "made one of the family"_s : SlotsOfBornObjects::s_whyNotAdopted;
        else
            why = object->structure()->bornAs() != family ? "it is of another type's family"_s : !object->structure()->fieldInSlot(slot) ? "it has no such property"_s : "the property is somewhere else, or there is no telling"_s;
        Instance::noteView(why);
        if (Options::aotReportSlowPaths()) [[unlikely]] { // TEMPORARY
            UniquedStringImpl* name = StaticHeap::identifiersOfProgram()[field.identifier];
            unsigned attributes = 0;
            PropertyOffset offset = object->structure()->get(vm, name, attributes);
            noteSlowPath(offset == invalidOffset ? "read_field (not its own)"_s : (attributes & PropertyAttribute::Accessor) ? "read_field (a getter)"_s : (attributes & PropertyAttribute::CustomAccessorOrValue) ? "read_field (custom)"_s : attributes ? "read_field (plain but for its attributes)"_s : "read_field (plain)"_s, base, name, why);
        }
        Structure* structure = object->structure();
        if (structure->bornAs() == family) {
            uint16_t there = structure->fieldInSlot(slot);
            if (there == field.id) {
                if (JSValue value = object->getDirect(static_cast<PropertyOffset>(slot)))
                    OPERATION_RETURN(scope, JSValue::encode(value));
            } else if (!there && undefinedWillDo)
                OPERATION_RETURN(scope, JSValue::encode(jsUndefined()));
        }
    } else
        Instance::noteView(base.isUndefinedOrNull() ? "it is undefined or null"_s : "it is no object"_s);
    UniquedStringImpl* uid = StaticHeap::identifiersOfProgram()[field.identifier];
    JSValue value;
    MegamorphicCache* cache = vm.megamorphicCache();
    if (auto* known = cache && base.isObject() ? cache->findLoad(asObject(base)->structureID(), uid) : nullptr) {
        JSCell* holder = known->m_holder == JSCell::seenMultipleCalleeObjects() ? base.asCell() : known->m_holder;
        value = holder ? asObject(holder)->getDirect(known->m_offset) : jsUndefined();
    } else {
        PropertySlot propertySlot(base, PropertySlot::InternalMethodType::Get);
        value = getByIdAndFillMegamorphicCache(globalObject, base, Identifier::fromUid(vm, uid), propertySlot);
        OPERATION_RETURN_IF_EXCEPTION(scope, encodedJSValue());
        // (What the megamorphic cache has is a plain property, or the lack of one.)
        if (!base.isObject() || (propertySlot.isUnset() ? propertySlot.isTaintedByOpaqueObject() : !propertySlot.isCacheableValue()))
            globalObject->aotInstance()->noteNotJustRead(slot, field.id);
    }
    if (value.isUndefined() && undefinedWillDo)
        OPERATION_RETURN(scope, JSValue::encode(value));
    // (What comes next takes it for what the field holds. So it is that, or this does not come back.)
    if (SlotsOfBornObjects::says(field, value) == SlotsOfBornObjects::Says::Refuses) {
        throwTypeError(globalObject, scope, "Type check failed: a property is not what the type of the object says it is"_s);
        OPERATION_RETURN(scope, encodedJSValue());
    }
    OPERATION_RETURN(scope, JSValue::encode(SlotsOfBornObjects::asHeld(field, value)));
}

JSC_DEFINE_JIT_OPERATION(operationAOTSettleStruct, void, (JSGlobalObject* globalObject, JSObject* object))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    uint16_t family = object->structure()->bornAs();
    if (SlotsOfBornObjects::isVerified(family)) {
        bool isRefused = false;
        object->structure()->forEachProperty(vm, [&](const PropertyTableEntry& entry) {
            if (auto* named = SlotsOfBornObjects::named(family, entry.key()))
                isRefused = SlotsOfBornObjects::says(*named, object->getDirect(entry.offset())) == SlotsOfBornObjects::Says::Refuses;
            return !isRefused;
        });
        if (isRefused)
            throwTypeError(globalObject, scope, TypedFieldError);
        OPERATION_RETURN(scope);
    }
    for (unsigned slot = 0; slot < SlotsOfBornObjects::numberOfSlots(family); ++slot) {
        JSValue value = object->getDirect(SlotsOfBornObjects::offsetInFamily(family, slot));
        if (value && SlotsOfBornObjects::says(family, slot, value) == SlotsOfBornObjects::Says::Refuses) {
            throwTypeError(globalObject, scope, TypedFieldError);
            OPERATION_RETURN(scope);
        }
    }
    OPERATION_RETURN(scope);
}

// Options::aotVerifiesFacts()
// That it is here is not to change what the program does: it may be called with an exception on its way to whoever catches it.
JSC_DEFINE_NOEXCEPT_JIT_OPERATION(operationAOTVerifyFact, size_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint64_t lowHalfOfType, uint64_t highHalfOfType, uint32_t which, uint32_t identifierIndexPlusOne, uint64_t scopeWhenCompiled, uint32_t scopeOffset))
{
    Type type = static_cast<Type>(highHalfOfType) << 64 | lowHalfOfType;
    Type actual = typeOfValue(JSValue::decode(encodedValue));
    // What it was born as.
    if (actual & TFinalObjectTag)
        actual = (actual & ~TFinalObject) | typeOfObjectBornAs(JSValue::decode(encodedValue).asCell()->structure()->bornAs());
    // Which function it is, if it is one of the program's.
    if (actual & TFunctionTag) {
        if (auto* function = dynamicDowncast<JSFunction>(JSValue::decode(encodedValue).asCell()); function && !function->isHostFunction()) {
            Image* image = Image::withCode();
            uint32_t index = function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall);
            if (image && function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall) && index < image->header().numberOfFunctions) {
                if (uint32_t number = image->at<uint32_t>(image->header().numbersOfFunctionsOffset)[index])
                    actual = (actual & ~TFunction) | typeOfFunction(number);
            } else if (!function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall)) {
                // One that there is no code for: every call of it was made part of whoever made it. There is no telling which it is.
                actual = (actual & ~TWhicheverFunction) | (type & TWhicheverFunction);
            }
        }
    }
    // (Nothing: what a stub takes for there being no exception.)
    if (isSubtype(actual, type)) [[likely]]
        return 0;
    VM& vm = globalObject->vm();
    CallFrame* callFrame = DECLARE_CALL_FRAME(vm);
    NativeCallFrameTracer tracer(vm, callFrame);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    scope.clearException();
    dataLog("AOT: A FACT IS NOT ONE: what ");
    if (which >= 1000000)
        dataLog(opcodeNames[which / 1000000], " at bc#", which % 1000000);
    else if (which % 100 == static_cast<unsigned>(NodeKind::Argument))
        dataLog("argument ", which / 100);
    else
        dataLog("a node of kind ", which);
    if (identifierIndexPlusOne)
        dataLog(" of `", identifierAt(globalObject, callFrame, identifierIndexPlusOne - 1).impl(), "`");
    if (scopeWhenCompiled)
        dataLog(" (scope ", RawPointer(std::bit_cast<void*>(static_cast<uintptr_t>(scopeWhenCompiled))), " offset ", scopeOffset, ")");
    dataLog(" gives was found to be ");
    if (!type)
        dataLog("never reached");
    else
        dumpType(WTF::dataFile(), type);
    dataLog(" and is ");
    dumpType(WTF::dataFile(), actual);
    if (auto* function = (actual & TFunctionTag) ? dynamicDowncast<JSFunction>(JSValue::decode(encodedValue).asCell()) : nullptr; function && !function->isHostOrBuiltinFunction())
        dataLog(" (index ", function->jsExecutable()->aotIndexFor(CodeSpecializationKind::CodeForCall), ", entry ", RawHex(function->jsExecutable()->aotEntryFor(CodeSpecializationKind::CodeForCall)), ")");
    dataLog(" (bits ", RawHex(static_cast<uint64_t>(encodedValue)));
    if (static_cast<uint64_t>(encodedValue) == std::bit_cast<uintptr_t>(&s_structWithNothingInIt[0]))
        dataLog(": the struct with nothing in it");
    else if (JSValue value = JSValue::decode(encodedValue); value && value.isCell())
        dataLog(", a cell of type ", static_cast<unsigned>(value.asCell()->type()), value.isObject() ? " " : "", value.isObject() ? asObject(value)->classInfo()->className : ""_s);
    dataLog(")");
    dataLogLn();
    if (JSValue value = JSValue::decode(encodedValue); value && value.isCell() && !value.asCell()->type() && static_cast<uint64_t>(encodedValue) != std::bit_cast<uintptr_t>(&s_structWithNothingInIt[0])) {
        // What is left of a cell, or what is not one yet.
        JSCell* cell = value.asCell();
        auto* words = std::bit_cast<const uint64_t*>(cell);
        dataLog("    words:");
        for (unsigned i = 0; i < 12; ++i)
            dataLog(" ", RawHex(words[i]));
        dataLogLn();
        dataLog("    before it:");
        for (int i = -8; i < 0; ++i)
            dataLog(" ", RawHex(words[i]));
        dataLogLn();
        if (StructureID id = cell->structureID()) {
            Structure* structure = id.decode();
            auto* ofStructure = std::bit_cast<const uint64_t*>(structure);
            dataLog("    its structure ", RawPointer(structure), ":");
            for (unsigned i = 0; i < 6; ++i)
                dataLog(" ", RawHex(ofStructure[i]));
            dataLogLn("; blob ", RawHex(structure->typeInfoBlob()), ", type ", static_cast<unsigned>(structure->typeInfo().type()), ", born as ", structure->bornAs(), ", inline capacity ", structure->inlineCapacity(), StaticHeap::contains(structure) ? " (static)" : "",
                !StaticHeap::contains(structure) ? (structure->markedBlock().handle().isLive(structure) ? ", live" : ", NOT LIVE") : "");
        }
        if (!cell->isPreciseAllocation() && !StaticHeap::contains(cell)) {
            MarkedBlock& block = cell->markedBlock();
            dataLogLn("    in a block of cells of ", block.handle().cellSize(), " bytes; marked: ", block.isMarked(cell), ", newly allocated: ", block.isNewlyAllocated(cell), ", live: ", block.handle().isLive(cell), ", free-listed: ", block.handle().isFreeListed());
        }
    }
    // Where that is, the way anything that goes wrong in a program says where it went wrong.
    JSObject* error = createError(globalObject, "the stack:"_s);
    JSValue stack = error->get(globalObject, vm.propertyNames->stack);
    if (!scope.exception() && stack.isString())
        dataLogLn(asString(stack)->value(globalObject).data);
    // (Not a crash: whatever is there to report those may take its time, and whoever is waiting for this is a test.)
    _exit(70);
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

JSC_DEFINE_JIT_OPERATION(operationAOTNarrowAtomThatSaysTheSame, StringImpl*, (JSGlobalObject* globalObject, JSString* string))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    auto atom = string->toExistingAtomString(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, nullptr); // Out of memory resolving a rope.
    StringImpl* impl = atom.data;
    OPERATION_RETURN(scope, impl && impl->is8Bit() ? impl : nullptr);
}

// The jump offset, relative to the switch; 0 for the default.
JSC_DEFINE_JIT_OPERATION(operationAOTSwitchString, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue, uint32_t tableIndex, uint32_t whose))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        OPERATION_RETURN(scope, 0);
    auto string = asString(value)->value(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, 0); // Out of memory resolving a rope.
    const UnlinkedStringJumpTable& table = functionOfBytecodeOfCaller(globalObject, callFrame, whose).stringSwitchJumpTable(tableIndex);
    OPERATION_RETURN(scope, table.offsetForValue(string.data.impl()));
}

// The character of a one character string, or -1.
JSC_DEFINE_JIT_OPERATION(operationAOTSwitchChar, int32_t, (JSGlobalObject* globalObject, EncodedJSValue encodedValue))
{
    AOT_OPERATION_PROLOGUE(globalObject);
    JSValue value = JSValue::decode(encodedValue);
    if (!value.isString())
        OPERATION_RETURN(scope, -1);
    JSString* string = asString(value);
    if (string->length() != 1)
        OPERATION_RETURN(scope, -1);
    auto view = string->view(globalObject);
    OPERATION_RETURN_IF_EXCEPTION(scope, -1);
    OPERATION_RETURN(scope, view[0]);
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
