/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRuntime.h"

#include "AbstractSlotVisitorInlines.h"
#include "BlockDirectoryInlines.h"
#include "HeapAnalyzer.h"
#include "HeapIterationScope.h"
#include "HeapProfiler.h"
#include "JSCInlines.h"
#include "MarkedSpaceInlines.h"
#include "PreventCollectionScope.h"
#include "PyAbstract.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyWeakRef.h"
#include "SlotVisitorInlines.h"
#include "TopExceptionScope.h"
#include <wtf/HexNumber.h>
#include <wtf/SetForScope.h>

namespace JSC {
namespace Python {

// The module gc. (How objects are finalized is in PyFinalization.cpp.)
//
// The collector is the engine's, which knows nothing of generations of Python objects, thresholds or reference counts. What the
// module says is what can be said truthfully: see "The collector, finalizers and weak references" in README.md.

// ---- What a program may be given ----
//
// get_objects(), get_referrers() and get_referents() find objects by looking at the heap, and the heap is full of what is not for
// a program to see. The rule is the same for the three. A cell is handed out if
//  - it is an object of Python, of this global object, of a kind that programs get to see (isObjectOfPython(), TypeFlag::Internal);
//  - it is whole: a tuple has all its items (isWhole());
//  - it is not a part of something else: a tuple, list, dict or set that the runtime made for its own use, which code in C++
//    trusts to hold what it put there. Those are the namespace of a class and the list of its subclasses, the variables of a frame
//    that has ended, what is kept under a hidden key (hiddenField()) or in a slot of the runtime (Runtime::Slot), and, where that
//    is a tuple that holds the state of a module, the containers in it.
// A reference from such a part, or from the cell in which a dict or a set keeps its entries, counts as one from the object it is a
// part of.

static bool isObjectOfPython(JSCell* cell, JSGlobalObject* globalObject)
{
    JSType type = cell->type();
    bool isOfKind;
    if (isPyObjectType(type))
        isOfKind = type != PyKwNamesType;
    else if (type == ArrayType || type == DerivedArrayType)
        isOfKind = true;
    else if (type == JSFunctionType)
        isOfKind = PyFunction::is(cell) && !wholeThatPartRefersTo(cell);
    else
        isOfKind = type == InternalFunctionType && (PyType::is(cell) || PyNativeFunction::is(cell));
    if (!isOfKind || cell->structure()->realm() != globalObject)
        return false;
    if (type == PyObjectType) {
        if (isInternalObject(cell))
            return false;
        if (auto* weakRef = PyWeakRef::tryFrom(cell); weakRef && weakRef->isInternal())
            return false;
    } else if (type == InternalFunctionType) {
        if (auto* pythonType = PyType::tryFrom(cell); pythonType && pythonType->has(TypeFlag::Internal))
            return false;
    }
    return true;
}

static bool isWhole(JSCell* cell)
{
    if (cell->type() != PyTupleType)
        return true;
    for (JSValue item : uncheckedDowncast<PyTuple>(cell)->span()) {
        if (!item)
            return false;
    }
    return true;
}

static bool isContainer(JSCell* cell)
{
    JSType type = cell->type();
    return type == PyTupleType || type == PyKwNamesType || type == ArrayType || type == DerivedArrayType || type == PyDictType || type == PySetType;
}

static bool isSlotOfPublicObject(Runtime::Slot slot)
{
    return slot == Runtime::Slot::GCGarbage || slot == Runtime::Slot::GCCallbacks || slot == Runtime::Slot::LazyModules;
}

// function(part, hasPartsInIt) for what the object keeps in fields of its own.
template<typename Function>
static void forEachPartInFields(JSCell* cell, const Function& function)
{
    if (auto* dict = PyDict::tryFrom(cell))
        function(static_cast<JSCell*>(const_cast<void*>(dict->storageIdentity())), false);
    else if (auto* set = PySet::tryFrom(cell))
        function(static_cast<JSCell*>(const_cast<void*>(set->storageIdentity())), false);
    else if (auto* type = PyType::tryFrom(cell)) {
        function(type->dict(), false);
        if (JSValue subclasses = type->subclasses())
            function(subclasses.asCell(), false);
    } else if (auto* frame = PyFrame::tryFrom(cell)) {
        function(frame->values(), false);
        function(frame->extraLocals(), false);
    } else if (auto* pythonFunction = PyFunction::tryFrom(cell))
        function(pythonFunction->generatorBodyIfMade(), false);
    else if (cell->type() == PyObjectType && uncheckedDowncast<PyObject>(cell)->type()->has(TypeFlag::OwnsContainers)) {
        auto* record = uncheckedDowncast<PyRecord>(cell);
        for (unsigned i = 0; i < record->fieldCount(); ++i) {
            if (JSValue field = record->get(i); field && field.isCell() && isContainer(field.asCell()))
                function(field.asCell(), false);
        }
    }
}

// The heap as it is now: which cells are objects, and which are parts of what. No cell may be allocated while there is one.
class HeapView {
    WTF_MAKE_NONCOPYABLE(HeapView);
public:
    explicit HeapView(JSGlobalObject* globalObject)
        : m_globalObject(globalObject)
        , m_runtime(runtime(globalObject))
    {
        VM& vm = m_runtime.vm();
        for (unsigned i = 0; i < static_cast<unsigned>(Runtime::Slot::Count); ++i) {
            if (!isSlotOfPublicObject(static_cast<Runtime::Slot>(i)))
                addPart(m_runtime.slot(static_cast<Runtime::Slot>(i)), nullptr, true);
        }
        HeapIterationScope iterationScope(vm.heap);
        vm.heap.objectSpace().forEachLiveCell(iterationScope, [&](HeapCell* heapCell, HeapCell::Kind kind) {
            if (!isJSCellKind(kind))
                return IterationStatus::Continue;
            auto* cell = static_cast<JSCell*>(heapCell);
            if (!cell->structureID() || !cell->isObject() || cell->structure()->realm() != globalObject)
                return IterationStatus::Continue;
            forEachPartInFields(cell, [&](JSCell* part, bool hasPartsInIt) {
                addPart(part, cell, hasPartsInIt);
            });
            addHiddenFields(asObject(cell));
            if (isObjectOfPython(cell, globalObject))
                m_objects.append(cell);
            return IterationStatus::Continue;
        });
    }

    // All the objects of Python there are, parts included.
    const Vector<JSCell*>& objects() const { return m_objects; }

    bool mayBeGiven(JSCell* cell) const { return !m_owners.contains(cell) && isObjectOfPython(cell, m_globalObject) && isWhole(cell); }

    // The cell, or what it is a part of. Null: nothing that may be given.
    JSCell* objectOf(JSCell* cell) const
    {
        for (unsigned depth = 0; cell && depth < 8; ++depth) {
            auto it = m_owners.find(cell);
            if (it == m_owners.end())
                return mayBeGiven(cell) ? cell : nullptr;
            cell = it->value;
        }
        return nullptr;
    }

private:
    void addPart(JSValue part, JSCell* owner, bool hasPartsInIt)
    {
        if (!part || !part.isCell())
            return;
        addPart(part.asCell(), owner, hasPartsInIt);
    }

    void addPart(JSCell* part, JSCell* owner, bool hasPartsInIt)
    {
        // What is kept under a hidden key may also be an object like any other.
        if (!part || (part->isObject() && !isContainer(part) && isObjectOfPython(part, m_globalObject)))
            return;
        m_owners.add(part, owner);
        if (!hasPartsInIt || part->type() != PyTupleType)
            return;
        for (JSValue item : uncheckedDowncast<PyTuple>(part)->span())
            addPart(item, owner, false);
    }

    void addHiddenFields(JSObject* object)
    {
        Structure* structure = object->structure();
        auto result = m_hiddenOffsets.add(structure, HiddenOffsets { });
        HiddenOffsets& offsets = result.iterator->value;
        if (result.isNewEntry) {
            for (unsigned i = 0; i < Runtime::numberOfHiddenKeys; ++i) {
                offsets.offset[i] = structure->getConcurrently(&m_runtime.hiddenKey(i).uid());
                offsets.hasAny |= offsets.offset[i] != invalidOffset;
            }
        }
        if (!offsets.hasAny) [[likely]]
            return;
        // What a type keeps there is the state of a module.
        bool hasPartsInIt = PyType::is(object);
        for (PropertyOffset offset : offsets.offset) {
            if (offset != invalidOffset)
                addPart(object->getDirect(offset), object, hasPartsInIt);
        }
    }

    struct HiddenOffsets {
        std::array<PropertyOffset, Runtime::numberOfHiddenKeys> offset;
        bool hasAny { false };
    };

    JSGlobalObject* m_globalObject;
    Runtime& m_runtime;
    Vector<JSCell*> m_objects;
    UncheckedKeyHashMap<JSCell*, JSCell*> m_owners;
    UncheckedKeyHashMap<Structure*, HiddenOffsets> m_hiddenOffsets;
};

// ---- What CPython's collector keeps track of ----

// The types written in C whose instances cannot refer to other objects, or that leave it to reference counts.
static constexpr ASCIILiteral untrackedTypes[] = {
    "EncodingMap"_s, "NoDefaultType"_s, "NoneType"_s, "NotImplementedType"_s, "Token.MISSING"_s, "_hashlib.HASH"_s, "_hashlib.HASHXOF"_s,
    "_hashlib.HMAC"_s, "_hmac.HMAC"_s, "_random.Random"_s, "_thread._localdummy"_s, "_tokenize.TokenizerIter"_s, "bool"_s, "bytearray"_s,
    "bytes"_s, "code"_s, "complex"_s, "datetime.date"_s, "datetime.datetime"_s, "datetime.time"_s, "datetime.timedelta"_s,
    "datetime.timezone"_s, "datetime.tzinfo"_s, "ellipsis"_s, "fieldnameiterator"_s, "float"_s, "formatteriterator"_s,
    "functools._lru_list_elem"_s, "int"_s, "line_iterator"_s, "longrange_iterator"_s, "object"_s, "positions_iterator"_s,
    "posix.DirEntry"_s, "posix.ScandirIterator"_s, "range"_s, "range_iterator"_s, "select.epoll"_s, "select.poll"_s, "str"_s,
    "symtable entry"_s,
};

// The types written in C that are not in builtins are nearly all allocated when their module is made, like classes. But for these.
static constexpr ASCIILiteral staticTypesOutsideBuiltins[] = {
    "Token.MISSING"_s, "_contextvars.Context"_s, "_contextvars.ContextVar"_s, "_contextvars.Token"_s, "collections.OrderedDict"_s,
    "datetime.date"_s, "datetime.datetime"_s, "datetime.time"_s, "datetime.timedelta"_s, "datetime.timezone"_s, "datetime.tzinfo"_s,
    "pickle.PickleBuffer"_s, "string.templatelib.Interpolation"_s, "string.templatelib.Template"_s, "string.templatelib.TemplateIter"_s,
    "sys.flags"_s, "sys.float_info"_s, "sys.hash_info"_s, "sys.int_info"_s, "sys.legacy_event_handler"_s, "sys.thread_info"_s,
    "sys.version_info"_s, "types.GenericAlias"_s, "types.SimpleNamespace"_s, "typing.TypeAliasType"_s, "typing.Union"_s,
    "weakref.CallableProxyType"_s, "weakref.ProxyType"_s, "weakref.ReferenceType"_s,
};

static bool contains(std::span<const ASCIILiteral> sortedNames, const String& name)
{
    return std::ranges::binary_search(sortedNames, StringView(name), [](StringView a, StringView b) {
        return codePointCompareLessThan(a, b);
    });
}

static bool isAllocatedInCPython(PyType* type)
{
    if (type->isHeapType())
        return true;
    String name = type->nameInMessages();
    return name.contains('.') ? !contains(staticTypesOutsideBuiltins, name) : name == "ExceptionGroup"_s;
}

static bool isOfUntrackedType(PyType* type)
{
    if (type->isHeapType())
        return false;
    return contains(untrackedTypes, type->nameInMessages());
}

// gc.is_tracked(). It allocates no cell, and the object may be one that the collector has just found dead.
// `isYoung`: it has not come through a collection of CPython's.
static bool isTracked(Runtime& rt, JSValue value, bool isYoung = false)
{
    if (!value.isCell() || !value.asCell()->isObject())
        return false;
    JSCell* cell = value.asCell();
    // What is not allocated is not kept track of.
    if (auto* type = PyType::tryFrom(cell))
        return isAllocatedInCPython(type);
    PyType* type = rt.typeOf(cell);
    if (type->isHeapType())
        return true;
    // A tuple is kept track of if any of its items is of a type whose instances may be, until a collection finds that none is.
    if (cell->type() == PyTupleType && type == rt.tupleType()) {
        for (JSValue item : uncheckedDowncast<PyTuple>(cell)->span()) {
            if (!item || !item.isCell() || !item.asCell()->isObject())
                continue;
            if (isYoung ? !isOfUntrackedType(rt.typeOf(item)) : isTracked(rt, item))
                return true;
        }
        return false;
    }
    return !isOfUntrackedType(type);
}

// ---- gc.collect(): how much was found ----
//
// CPython's collector deals with what reference counts do not free: the objects that are in a cycle, and what only such
// objects refer to. gc.collect() returns how many it found. The engine's collector finds all that is unreachable and does not
// look at it, which is why it is fast. When a program asks for a collection, the cells that have died in it are looked at, once
// all is marked and before anything is swept (PyFinalizationOrder.cpp). Of what reference counts would have left, those
// cells are counted that are objects which CPython's collector keeps track of.
class Census final : public Runtime::MarkingObserver {
public:
    Census(Runtime& rt, bool keepsWhatItFinds, bool describesWhatItFinds)
        : m_runtime(rt)
        , m_keepsWhatItFinds(keepsWhatItFinds)
        , m_describesWhatItFinds(describesWhatItFinds)
    {
    }

    size_t found { 0 }; // In all the collections it has seen.
    size_t candidates { 0 }; // How many cells there were to look at, dead or alive, in the last.
    MarkedArgumentBuffer kept; // What was found, if it is to be kept.
    Vector<String> descriptions;

    void willCollect() final { m_isDone = false; }

    void didCollect() final
    {
        // Nothing else refers to them, and nothing has been allocated since.
        for (JSCell* cell : m_keptByLastCollection)
            kept.append(cell);
        m_keptByLastCollection.clear();
    }

    bool wantsGarbage() final { return !m_isDone; }

    void didFindGarbage(std::span<JSCell* const> garbage, size_t cellsLookedAt, Vector<JSCell*>& toKeep) final
    {
        m_isDone = true;
        candidates = cellsLookedAt;
        JSGlobalObject* globalObject = m_runtime.globalObject();
        for (JSCell* cell : garbage) {
            if (!cell->isObject() || !isObjectOfPython(cell, globalObject) || !isWhole(cell) || !isTracked(m_runtime, cell, true))
                continue;
            ++found;
            if (m_keepsWhatItFinds)
                m_keptByLastCollection.append(cell);
            if (m_describesWhatItFinds)
                descriptions.append(makeString('<', m_runtime.typeOf(cell)->nameInMessages(), " 0x"_s, hex(std::bit_cast<uintptr_t>(cell), WTF::Lowercase), '>'));
        }
        toKeep.appendVector(m_keptByLastCollection);
    }

private:
    Runtime& m_runtime;
    bool m_keepsWhatItFinds;
    bool m_describesWhatItFinds;
    bool m_isDone { false };
    Vector<JSCell*> m_keptByLastCollection;
};

// ---- Collections ----

static constexpr int debugStats = 1;
static constexpr int debugCollectable = 2;
static constexpr int debugUncollectable = 4;
static constexpr int debugSaveAll = 32;

static JSArray* gcList(Runtime& rt, Runtime::Slot slot)
{
    JSValue list = rt.slot(slot);
    return list ? asList(list) : nullptr;
}

// gc.callbacks. Raises nothing.
static void callCollectionCallbacks(JSGlobalObject* globalObject, ASCIILiteral phase, int generation, const Runtime::CollectionStats& stats)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSArray* callbacks = gcList(rt, Runtime::Slot::GCCallbacks);
    if (!callbacks || !listLength(callbacks))
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    PyDict* info = PyDict::create(globalObject);
    info->set(vm, internString(vm, "generation"_s), jsNumber(generation));
    info->set(vm, internString(vm, "collected"_s), makeInt(globalObject, stats.collected));
    info->set(vm, internString(vm, "uncollectable"_s), makeInt(globalObject, stats.uncollectable));
    info->set(vm, internString(vm, "candidates"_s), makeInt(globalObject, stats.candidates));
    info->set(vm, internString(vm, "duration"_s), makeFloat(globalObject, stats.duration));
    JSString* phaseString = internString(vm, phase);
    for (unsigned i = 0; i < listLength(callbacks); ++i) {
        JSValue callback = listGet(callbacks, i);
        call(globalObject, callback, phaseString, info);
        if (!scope.exception())
            continue;
        if (vm.hasPendingTerminationException()) [[unlikely]]
            return;
        reportUnraisable(globalObject, makeString("Exception ignored while calling GC callback "_s, reprForMessage(globalObject, callback)), { });
    }
}

void Runtime::tellOfCollection(int generation, double duration, bool isOver)
{
    CollectionStats stats;
    if (isOver)
        stats.duration = duration;
    callCollectionCallbacks(m_globalObject, isOver ? "stop"_s : "start"_s, generation, stats);
}

bool Runtime::hasCollectionCallbacks() const
{
    JSValue callbacks = slot(Slot::GCCallbacks);
    return callbacks && listLength(asList(callbacks));
}

static JSValue gcCollect(JSGlobalObject* globalObject, JSValue generationValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int generation = 2;
    if (generationValue) {
        generation = toCInt(globalObject, generationValue);
        RETURN_IF_EXCEPTION(scope, { });
        if (generation < 0 || generation > 2)
            return raiseValueError(globalObject, scope, "invalid generation"_s);
    }
    // From a finalizer, or the callback of a weak reference. Nothing that a collection found now could be finalized before this
    // call returns, and a hundred finalizers that do this would make for a hundred collections, or, if it is a call of this
    // function that runs them, as many times the rounds below. (In CPython a collection within a collection does nothing.)
    if (rt.isRunningPendingCalls()) {
        rt.collectAfterPendingCalls();
        return jsNumber(0);
    }
    // From one of gc.callbacks, or from another thread that got its turn meanwhile.
    if (rt.gc.isCollecting)
        return jsNumber(0);
    SetForScope isCollecting(rt.gc.isCollecting, true);

    Runtime::CollectionStats stats;
    callCollectionCallbacks(globalObject, "start"_s, generation, stats);
    RETURN_IF_EXCEPTION(scope, { });
    int debug = rt.gc.debugFlags;
    if (debug & debugStats)
        writeToStderr(globalObject, makeString("gc: collecting generation "_s, generation, "...\ngc: objects in each generation: 0 0 0\ngc: objects in permanent generation: 0\n"_s));
    MonotonicTime start = MonotonicTime::now();

    Census census(rt, debug & debugSaveAll, debug & debugCollectable);
    collectAndFinalize(globalObject, &census);
    RETURN_IF_EXCEPTION(scope, { });

    stats.collected = census.found;
    stats.candidates = census.candidates;
    stats.duration = (MonotonicTime::now() - start).seconds();
    for (const String& description : census.descriptions)
        writeToStderr(globalObject, makeString("gc: collectable "_s, description, '\n'));
    if (JSArray* garbage = gcList(rt, Runtime::Slot::GCGarbage)) {
        for (unsigned i = 0; i < census.kept.size(); ++i) {
            listAppend(globalObject, garbage, census.kept.at(i));
            RETURN_IF_EXCEPTION(scope, { });
        }
    }
    if (debug & debugStats)
        writeToStderr(globalObject, makeString("gc: done, "_s, stats.collected, " unreachable, 0 uncollectable, "_s, FormattedNumber::fixedWidth(stats.duration, 4), "s elapsed\n"_s));

    auto& total = rt.gc.stats[generation];
    ++total.collections;
    total.collected += stats.collected;
    total.candidates += stats.candidates;
    total.duration += stats.duration;
    callCollectionCallbacks(globalObject, "stop"_s, generation, stats);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeInt(globalObject, static_cast<uint64_t>(stats.collected)));
}

static JSValue gcEnable(JSGlobalObject* globalObject)
{
    runtime(globalObject).gc.isEnabled = true;
    return jsUndefined();
}

// The collector cannot be turned off: it is the only thing that frees memory. While it is "off" the program is not told of
// collections (gc.callbacks, gc.get_stats()).
static JSValue gcDisable(JSGlobalObject* globalObject)
{
    runtime(globalObject).gc.isEnabled = false;
    return jsUndefined();
}

static JSValue gcIsEnabled(JSGlobalObject* globalObject) { return jsBoolean(runtime(globalObject).gc.isEnabled); }

static JSValue gcGetThreshold(JSGlobalObject* globalObject)
{
    auto& thresholds = runtime(globalObject).gc.thresholds;
    return PyTuple::create(globalObject, { jsNumber(thresholds[0]), jsNumber(thresholds[1]), jsNumber(thresholds[2]) });
}

// When the engine collects is up to the engine. What was asked for is remembered.
static JSValue gcSetThreshold(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, rejectKeywords(globalObject, "set_threshold"_s, args)))
        return { };
    if (!args.size() || args.size() > 3)
        return raiseTypeError(globalObject, scope, "gc.set_threshold requires 1 to 3 arguments"_s);
    auto thresholds = runtime(globalObject).gc.thresholds;
    for (unsigned i = 0; i < args.size(); ++i) {
        thresholds[i] = toCInt(globalObject, args[i]);
        RETURN_IF_EXCEPTION(scope, { });
    }
    runtime(globalObject).gc.thresholds = thresholds;
    return jsUndefined();
}

static JSValue gcGetCount(JSGlobalObject* globalObject) { return PyTuple::create(globalObject, { jsNumber(0), jsNumber(0), jsNumber(0) }); }
static JSValue gcGetDebug(JSGlobalObject* globalObject) { return jsNumber(runtime(globalObject).gc.debugFlags); }

static JSValue gcSetDebug(JSGlobalObject* globalObject, JSValue flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int value = toCInt(globalObject, flags);
    RETURN_IF_EXCEPTION(scope, { });
    runtime(globalObject).gc.debugFlags = value;
    return jsUndefined();
}

static JSValue gcGetStats(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    MarkedArgumentBuffer result;
    for (auto stats : rt.gc.stats) {
        PyDict* dict = PyDict::create(globalObject);
        dict->set(vm, internString(vm, "collections"_s), makeInt(globalObject, stats.collections));
        dict->set(vm, internString(vm, "collected"_s), makeInt(globalObject, stats.collected));
        dict->set(vm, internString(vm, "uncollectable"_s), makeInt(globalObject, stats.uncollectable));
        dict->set(vm, internString(vm, "candidates"_s), makeInt(globalObject, stats.candidates));
        dict->set(vm, internString(vm, "duration"_s), makeFloat(globalObject, stats.duration));
        result.append(dict);
    }
    return newList(globalObject, spanOf(result));
}

// ---- Looking at the heap ----

// What can refer to other objects, as in CPython: not numbers, not strs.
static JSValue gcGetObjects(JSGlobalObject* globalObject, JSValue generation)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!audit(globalObject, "gc.get_objects"_s, generation && !isNone(generation) ? generation : jsNumber(-1)))
        return { };
    if (generation && !isNone(generation)) {
        if (!hasIndex(globalObject, generation))
            return raiseTypeError(globalObject, scope, "argument should be integer or None, not '"_s, typeName(globalObject, generation), '\'');
        int number = toCInt(globalObject, generation);
        RETURN_IF_EXCEPTION(scope, { });
        if (number < -1)
            return raiseValueError(globalObject, scope, "generation parameter cannot be negative"_s);
        if (number > 2)
            return raiseValueError(globalObject, scope, "generation parameter must be less than the number of available generations ("_s, 3, ')');
        // There are no such generations here. Everything is as old as can be.
        if (number >= 0 && number < 2)
            RELEASE_AND_RETURN(scope, newList(globalObject));
    }
    // What is dead but has not been swept yet must not come back.
    vm.clearLastException();
    vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
    MarkedArgumentBuffer objects;
    {
        HeapView view(globalObject);
        for (JSCell* cell : view.objects()) {
            if (view.mayBeGiven(cell) && isTracked(rt, cell))
                objects.append(cell);
        }
    }
    RELEASE_AND_RETURN(scope, newList(globalObject, spanOf(objects)));
}

// Who refers to whom is what the collector finds out as it marks. This listens to a collection, as a heap snapshot does, for the
// references to some objects. What only a running frame refers to has no referrer: the stack is not made of objects.
class ReferrerListener final : public HeapAnalyzer {
public:
    explicit ReferrerListener(std::span<const JSValue> objects)
    {
        for (JSValue object : objects) {
            if (object.isCell())
                m_objects.add(object.asCell());
        }
    }

    void listen(VM& vm)
    {
        if (m_objects.isEmpty())
            return;
        HeapProfiler& profiler = vm.ensureHeapProfiler();
        if (profiler.activeHeapAnalyzer())
            return;
        PreventCollectionScope preventCollectionScope(vm.heap);
        profiler.setActiveHeapAnalyzer(this);
        vm.heap.collectNow(Synchronousness::Sync, CollectionScope::Full);
        profiler.setActiveHeapAnalyzer(nullptr);
    }

    const Vector<JSCell*>& found() const { return m_found; }

private:
    void analyzeNode(JSCell*) final { }
    void analyzeEdge(JSCell* from, JSCell* to, RootMarkReason) final
    {
        if (!from || !m_objects.contains(to)) [[likely]]
            return;
        Locker locker { m_lock };
        m_found.append(from);
    }
    void analyzePropertyNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final { analyzeEdge(from, to, RootMarkReason::None); }
    void analyzeVariableNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final { analyzeEdge(from, to, RootMarkReason::None); }
    void analyzeIndexEdge(JSCell* from, JSCell* to, uint32_t) final { analyzeEdge(from, to, RootMarkReason::None); }
    void setOpaqueRootReachabilityReasonForCell(JSCell*, ASCIILiteral) final { }
    void setWrappedObjectForCell(JSCell*, void*) final { }
    void setLabelForCell(JSCell*, const String&) final { }

    UncheckedKeyHashSet<JSCell*> m_objects;
    Lock m_lock;
    Vector<JSCell*> m_found;
};

static JSValue gcGetReferrers(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    // The hooks are given the objects in a tuple, which refers to them for as long as it is there.
    PyTuple* forHooks = isAudited(globalObject) ? PyTuple::create(globalObject, args.positional()) : nullptr;
    if (forHooks && !audit(globalObject, "gc.get_referrers"_s, forHooks))
        return { };
    MarkedArgumentBuffer result;
    {
        vm.clearLastException();
        ReferrerListener listener(args.positional());
        listener.listen(vm);
        if (!listener.found().isEmpty()) {
            HeapView view(globalObject);
            UncheckedKeyHashSet<JSCell*> seen;
            if (forHooks)
                seen.add(forHooks);
            for (JSCell* cell : listener.found()) {
                cell = view.objectOf(cell);
                if (cell && seen.add(cell).isNewEntry)
                    result.append(cell);
            }
        }
    }
    RELEASE_AND_RETURN(scope, newList(globalObject, spanOf(result)));
}

// What a cell says it refers to.
class ReferentCollector final : public AbstractSlotVisitor {
public:
    explicit ReferentCollector(VM& vm)
        : AbstractSlotVisitor(vm.heap.collector(), "PythonReferents"_s, m_opaqueRootStorage)
    {
    }

    Vector<JSCell*> cells;

    void visit(JSCell* cell) { cell->methodTable()->visitChildren(cell, *this); }

    void append(const ConservativeRoots&) final { }
    void appendUnbarriered(JSCell* cell) final { reach(cell); }
    void appendHiddenUnbarriered(JSCell* cell) final { reach(cell); }
    bool isFirstVisit() const final { return false; }
    bool isMarked(const void*) const final { return true; }
    bool isMarked(MarkedBlock&, HeapCell*) const final { return true; }
    bool isMarked(PreciseAllocation&, HeapCell*) const final { return true; }
    void markAuxiliary(const void*) final { }
    void reportExtraMemoryVisited(size_t) final { }
#if ENABLE(RESOURCE_USAGE)
    void reportExternalMemoryVisited(size_t) final { }
#endif
    bool mutatorsAreStopped() const final { return true; }
    void didRace(const VisitRaceKey&) final { }
    void visitAsConstraint(const JSCell* cell) final { visit(const_cast<JSCell*>(cell)); }
    void addParallelConstraintTask(RefPtr<SharedTask<void(AbstractSlotVisitor&)>>) final { }
    void addParallelConstraintTask(RefPtr<SharedTask<void(SlotVisitor&)>>) final { }

private:
    void reach(JSCell* cell)
    {
        if (cell)
            cells.append(cell);
    }

    ConcurrentPtrHashSet m_opaqueRootStorage;
};

// Allocates no cell.
static void appendReferents(JSGlobalObject* globalObject, JSCell* object, MarkedArgumentBuffer& result, unsigned depth = 0)
{
    Runtime& rt = runtime(globalObject);
    // Numbers are not cells, so no cell says that it refers to one. The items of these are asked for.
    if (JSArray* list = tryAsList(object)) {
        for (unsigned i = 0; i < listLength(list); ++i)
            result.append(listGet(list, i));
    } else if (auto* tuple = PyTuple::tryFrom(object)) {
        for (JSValue item : tuple->span()) {
            if (item)
                result.append(item);
        }
    } else if (auto* dict = PyDict::tryFrom(object)) {
        // CPython's does not mention its keys if they are all strings.
        bool mentionsKeys = false;
        unsigned position = 0;
        JSValue key;
        JSValue value;
        while (!mentionsKeys && dict->next(position, key, value))
            mentionsKeys = !key.isString();
        position = 0;
        while (dict->next(position, key, value)) {
            result.append(value);
            if (mentionsKeys)
                result.append(key);
        }
    } else if (auto* set = PySet::tryFrom(object)) {
        size_t position = 0;
        JSValue key;
        while (set->next(position, key))
            result.append(key);
    } else {
        UncheckedKeyHashSet<JSCell*> parts;
        forEachPartInFields(object, [&](JSCell* part, bool) {
            if (part)
                parts.add(part);
        });
        UncheckedKeyHashSet<JSCell*> hidden;
        for (unsigned i = 0; i < Runtime::numberOfHiddenKeys; ++i) {
            PropertyOffset offset = object->structure()->getConcurrently(&rt.hiddenKey(i).uid());
            if (offset == invalidOffset)
                continue;
            if (JSValue value = asObject(object)->getDirect(offset); value && value.isCell() && (isContainer(value.asCell()) || !isObjectOfPython(value.asCell(), globalObject)))
                hidden.add(value.asCell());
        }
        ReferentCollector collector(rt.vm());
        collector.visit(object);
        PyType* type = rt.typeOf(object);
        for (JSCell* cell : collector.cells) {
            if (hidden.contains(cell) || cell == type)
                continue;
            if (parts.contains(cell)) {
                if (depth < 2)
                    appendReferents(globalObject, cell, result, depth + 1);
                continue;
            }
            if (cell->isString() || cell->isHeapBigInt() || (cell->isObject() && isObjectOfPython(cell, globalObject) && isWhole(cell)))
                result.append(cell);
        }
    }
    // In CPython an object holds on to its type only if that is a class.
    if (PyType* type = rt.typeOf(object); !depth && type->isHeapType())
        result.append(type);
}

static JSValue gcGetReferents(JSGlobalObject* globalObject, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (isAudited(globalObject) && !audit(globalObject, "gc.get_referents"_s, PyTuple::create(globalObject, args.positional())))
        return { };
    MarkedArgumentBuffer result;
    for (JSValue object : args.positional()) {
        if (object.isCell() && object.asCell()->isObject() && isObjectOfPython(object.asCell(), globalObject) && !isOfUntrackedType(typeOf(globalObject, object)))
            appendReferents(globalObject, object.asCell(), result);
    }
    RELEASE_AND_RETURN(scope, newList(globalObject, spanOf(result)));
}

static JSValue gcNothing(JSGlobalObject*) { return jsUndefined(); }
static JSValue gcZero(JSGlobalObject*) { return jsNumber(0); }
static JSValue gcIsTracked(JSGlobalObject* globalObject, JSValue object) { return jsBoolean(isTracked(runtime(globalObject), object)); }
static JSValue gcIsFinalized(JSGlobalObject* globalObject, JSValue object) { return jsBoolean(object.isCell() && runtime(globalObject).hasBeenFinalized(object.asCell())); }

static constexpr NativeDef gcFunctions[] = {
    NativeDef(NativeKind::Function, "collect"_s, gcCollect, "(generation=2)"_s, OnWipedStack),
    PY_FUNCTION("enable", gcEnable, "()"),
    PY_FUNCTION("disable", gcDisable, "()"),
    PY_FUNCTION("isenabled", gcIsEnabled, "()"),
    PY_FUNCTION("get_threshold", gcGetThreshold, "()"),
    PY_FUNCTION("set_threshold", gcSetThreshold, "(*args)"),
    PY_FUNCTION("get_count", gcGetCount, "()"),
    PY_FUNCTION("get_debug", gcGetDebug, "()"),
    PY_FUNCTION("set_debug", gcSetDebug, "(flags, /)"),
    PY_FUNCTION("get_stats", gcGetStats, "()"),
    // What the program has dropped is not to be found.
    NativeDef(NativeKind::Function, "get_objects"_s, gcGetObjects, "(generation=None)"_s, OnWipedStack),
    NativeDef(NativeKind::Function, "get_referrers"_s, gcGetReferrers, "(*objs)"_s, OnWipedStack),
    PY_FUNCTION("get_referents", gcGetReferents, "(*objs)"),
    PY_FUNCTION("is_tracked", gcIsTracked, "(obj, /)"),
    PY_FUNCTION("is_finalized", gcIsFinalized, "(obj, /)"),
    PY_FUNCTION("freeze", gcNothing, "()"),
    PY_FUNCTION("unfreeze", gcNothing, "()"),
    PY_FUNCTION("get_freeze_count", gcZero, "()"),
};

void initializeGCModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    constexpr auto comesBefore = [](ASCIILiteral a, ASCIILiteral b) {
        return std::string_view(a.characters(), a.length()) < std::string_view(b.characters(), b.length());
    };
    static_assert(std::ranges::is_sorted(untrackedTypes, comesBefore));
    static_assert(std::ranges::is_sorted(staticTypesOutsideBuiltins, comesBefore));
    if (!rt.slot(Runtime::Slot::GCGarbage)) {
        rt.setSlot(Runtime::Slot::GCGarbage, newList(globalObject));
        rt.setSlot(Runtime::Slot::GCCallbacks, newList(globalObject));
    }
    module->addFunctions(globalObject, gcFunctions);
    module->add(globalObject, "garbage"_s, rt.slot(Runtime::Slot::GCGarbage));
    module->add(globalObject, "callbacks"_s, rt.slot(Runtime::Slot::GCCallbacks));
    module->add(globalObject, "DEBUG_STATS"_s, jsNumber(debugStats));
    module->add(globalObject, "DEBUG_COLLECTABLE"_s, jsNumber(debugCollectable));
    module->add(globalObject, "DEBUG_UNCOLLECTABLE"_s, jsNumber(debugUncollectable));
    module->add(globalObject, "DEBUG_SAVEALL"_s, jsNumber(debugSaveAll));
    module->add(globalObject, "DEBUG_LEAK"_s, jsNumber(debugCollectable | debugUncollectable | debugSaveAll));
}

} // namespace Python
} // namespace JSC
