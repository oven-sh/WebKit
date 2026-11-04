/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyRuntime.h"

#include "ArrayPrototype.h"
#include "FunctionPrototype.h"
#include "JSAsyncFunctionGenerator.h"
#include "JSAsyncGenerator.h"
#include "JSCInlines.h"
#include "JSGenerator.h"
#include "JSCellButterfly.h"
#include "JSWeakMapInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCommandLine.h"
#include "PyDescriptors.h"
#include "PyDict.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyGenerator.h"
#include "PyInternals.h"
#include "PyJSClass.h"
#include "PyIterators.h"
#include "PyJSView.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyMonitoring.h"
#include "PyNative.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PySet.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include "PyType.h"
#include "PyTypes.h"
#include "PyWeakRef.h"
#include <wtf/TZoneMallocInlines.h>

namespace JSC {
namespace Python {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Runtime);

Runtime& Runtime::ensure(JSGlobalObject* globalObject)
{
    if (auto* existing = globalObject->pythonRuntime())
        return *existing;
    auto created = makeUnique<Runtime>(globalObject);
    Runtime& runtime = *created;
    globalObject->setPythonRuntime(WTF::move(created));
    runtime.initialize();
    return runtime;
}

bool Runtime::tryEnterFrame()
{
    if (++frameDepth <= recursionLimit) [[likely]]
        return true;
    --frameDepth;
    auto scope = DECLARE_THROW_SCOPE(m_vm);
    raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded"_s);
    return false;
}

Runtime::Runtime(JSGlobalObject* globalObject)
    : m_vm(globalObject->vm())
    , m_globalObject(globalObject)
    , m_dictKey(PrivateName::PrivateSymbol, "__dict__"_s)
    , m_keywordsKey(PrivateName::PrivateSymbol, "keywords"_s)
    , m_hiddenKeys { PrivateName(PrivateName::PrivateSymbol, "hidden0"_s), PrivateName(PrivateName::PrivateSymbol, "hidden1"_s), PrivateName(PrivateName::PrivateSymbol, "hidden2"_s), PrivateName(PrivateName::PrivateSymbol, "hidden3"_s) }
    , m_instanceKeys { PrivateName(PrivateName::PrivateSymbol, "type"_s), PrivateName(PrivateName::PrivateSymbol, "dict"_s), PrivateName(PrivateName::PrivateSymbol, "hasDict"_s) }
{
    m_threads = Threads::sharedFor(m_vm);
    m_monitoring = makeUnique<Monitoring>(*this);
    m_vm.heap.addPythonRuntime(this);
}

Runtime::~Runtime()
{
    detachSignals(*this);
    m_vm.heap.removePythonRuntime(this);
}

template<typename Visitor>
void Runtime::visit(Visitor& visitor)
{
    for (auto& structure : m_structures)
        visitor.append(structure);
    visitor.append(m_keywordNamesStructure);
    visitor.append(m_typeStructure);
    visitor.append(m_nativeFunctionStructure);
    visitor.append(m_functionStructure);
    visitor.append(m_listSubclassStructure);
    visitor.append(m_weakRefStructure);
    for (auto& object : m_jsViewObjects)
        visitor.append(object);
    for (auto& type : m_types)
        visitor.append(type);
    for (auto& name : m_names)
        visitor.append(name);
    visitor.append(m_notImplemented);
    visitor.append(m_ellipsis);
    visitor.append(m_emptyTuple);
    visitor.append(m_emptyBytes);
    visitor.append(m_modules);
    visitor.append(m_builtinsModule);
    visitor.append(m_builtins);
    visitor.append(m_sysModule);
    visitor.append(m_sysDict);
    visitor.append(m_defaultImportFunction);
    for (auto& slot : m_slots)
        visitor.append(slot);
    visitor.append(m_handledException);
    {
        Locker locker { m_catchesInJSLock };
        for (auto& entry : m_catchesInJS) {
            visitor.appendUnbarriered(entry.thrown);
            visitor.appendUnbarriered(entry.previous);
            visitor.appendUnbarriered(entry.slotOwner);
            // So that nothing else comes to lie at their addresses.
            visitor.appendUnbarriered(entry.code);
            visitor.appendUnbarriered(entry.generator);
        }
    }
    visitor.append(m_catchesOfSuspendedGeneratorsOfJS);
    visitor.append(m_asyncGenFirstIter);
    visitor.append(m_asyncGenFinalizer);
    visitor.append(m_threadLocals);
    visitor.append(m_pendingKeywordNames);
    visitor.append(m_sendResult);
    visitor.append(m_exceptionGroupRest);
    visitPendingCalls(visitor);
    {
        Locker locker { m_moduleTypesLock };
        for (auto& entry : m_moduleTypes)
            visitor.append(entry.value);
    }
}

template void Runtime::visit(AbstractSlotVisitor&);
template void Runtime::visit(SlotVisitor&);

void Runtime::initialize()
{
    VM& vm = getVM(m_globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    optimizationLevel = configuration().optimizationLevel;
    maxStrDigits = configuration().intMaxStrDigits;
    initializeStructures();
    initializeNames();
    initializeTypes();
    initializeModules();
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, startImportSystem(m_globalObject));
}

void Runtime::initializeStructures()
{
    VM& vm = m_vm;
    JSGlobalObject* globalObject = m_globalObject;
    initializeJSView(globalObject); // The prototypes.
#define PY_CREATE_STRUCTURE(Class) m_structures[static_cast<unsigned>(CellClass::Class)].set(vm, globalObject, Class::createStructure(vm, globalObject));
    FOR_EACH_PY_CELL_CLASS(PY_CREATE_STRUCTURE)
#undef PY_CREATE_STRUCTURE
    iteratorStructureID = structure<PyIterator>()->id();
    m_keywordNamesStructure.set(vm, globalObject, PyTuple::createKeywordNamesStructure(vm, globalObject));
    m_structures[static_cast<unsigned>(CellClass::PyException)].set(vm, globalObject, Structure::create(vm, globalObject, jsViewObject(JSViewObject::ExceptionPrototype), TypeInfo(PyExceptionType, PyException::StructureFlags), PyException::info()));
    JSObject* functionPrototype = jsViewObject(JSViewObject::FunctionPrototype);
    m_typeStructure.set(vm, globalObject, PyType::createStructure(vm, globalObject, functionPrototype));
    m_nativeFunctionStructure.set(vm, globalObject, PyNativeFunction::createStructure(vm, globalObject, functionPrototype));
    m_functionStructure.set(vm, globalObject, PyFunction::createStructure(vm, globalObject, functionPrototype));
    functionStructureID = m_functionStructure->id();
    m_listSubclassStructure.set(vm, globalObject, PyListObject::createStructure(vm, globalObject, globalObject->arrayPrototype()));
    m_weakRefStructure.set(vm, globalObject, PyWeakRef::createStructure(vm, globalObject));
}

void Runtime::initializeNames()
{
    static constexpr ASCIILiteral texts[] = {
#define PY_NAME_TEXT(id, text) text ""_s,
        FOR_EACH_PY_SPECIAL_METHOD(PY_NAME_TEXT)
        FOR_EACH_PY_NAME(PY_NAME_TEXT)
#undef PY_NAME_TEXT
    };
    static_assert(std::size(texts) == numberOfNames);
    for (unsigned i = 0; i < numberOfNames; ++i)
        m_names[i].set(m_vm, m_globalObject, internString(m_vm, texts[i]));
}

void Runtime::initializeTypes()
{
    VM& vm = m_vm;
    JSGlobalObject* globalObject = m_globalObject;

    // Filling one type takes objects of many others, so they all exist first.
    for (auto& type : m_types)
        type.set(vm, globalObject, PyType::createBare(vm, m_typeStructure.get()));
    m_emptyTuple.set(vm, globalObject, PyTuple::createUninitialized(vm, structure<PyTuple>(), tupleType(), 0));
    m_emptyBytes.set(vm, globalObject, PyBytes::createUninitialized(vm, structure<PyBytes>(), bytesType(), 0));

    initializeObjectTypes(globalObject);
    initializeSingletonTypes(globalObject);
    initializeIntTypes(globalObject);
    initializeFloatTypes(globalObject);
    initializeStrTypes(globalObject);
    initializeBytesTypes(globalObject);
    initializeListTypes(globalObject);
    initializeTupleTypes(globalObject);
    initializeDictTypes(globalObject);
    initializeSetTypes(globalObject);
    initializeRangeTypes(globalObject);
    initializeFunctionTypes(globalObject);
    initializeDescriptorTypes(globalObject);
    initializeIteratorTypes(globalObject);
    initializeGeneratorTypes(globalObject);
    initializeModuleTypes(globalObject);
    initializeFrameTypes(globalObject);
    initializeExceptionTypes(globalObject);
    initializeGenericAliasTypes(globalObject);
    initializeNamespaceTypes(globalObject);
    initializeTypingTypes(globalObject);
    initializeTemplateTypes(globalObject);
    initializeMemoryViewTypes(globalObject);
    initializeWeakRefTypes(globalObject);
    initializeInteropTypes(globalObject);

    m_notImplemented.set(vm, globalObject, PyObject::create(vm, structure<PyObject>(), notImplementedType()));
    m_ellipsis.set(vm, globalObject, PyObject::create(vm, structure<PyObject>(), ellipsisType()));

#if ASSERT_ENABLED
    for (auto& type : m_types)
        ASSERT(type->mro());
#endif
}

void Runtime::initializeModules()
{
    VM& vm = m_vm;
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSGlobalObject* globalObject = m_globalObject;

    m_modules.set(vm, globalObject, PyDict::create(globalObject));

    PyModule* builtins = PyModule::create(globalObject, names.builtins());
    m_builtinsModule.set(vm, globalObject, builtins);
    m_builtins.set(vm, globalObject, builtins->dict());
    m_modules->set(vm, names.builtins(), builtins);

    PyModule* sys = PyModule::create(globalObject, names.sys());
    m_sysModule.set(vm, globalObject, sys);
    m_sysDict.set(vm, globalObject, sys->dict());
    m_modules->set(vm, names.sys(), sys);

    initializeBuiltinsModule(globalObject, builtins);
    RETURN_IF_EXCEPTION(scope, void());
    RELEASE_AND_RETURN(scope, initializeSysModule(globalObject, sys));
}

// Without the lock, which is there for the collector: it reads the table from a thread of its own. Only a thread that is running
// Python adds to it, and there is one of those at a time.
PyType* Runtime::typeIfMadeFor(const TypeDef& def) const
{
    auto it = m_moduleTypes.find(&def);
    return it != m_moduleTypes.end() ? it->value.get() : nullptr;
}

PyType* Runtime::typeFor(const TypeDef& def, PyType* base)
{
    if (PyType* type = typeIfMadeFor(def)) [[likely]]
        return type;
    PyType* type = PyType::createBare(m_vm, m_typeStructure.get());
    {
        // In the table first: filling the type may ask for it.
        Locker locker { m_moduleTypesLock };
        m_moduleTypes.add(&def, WriteBarrier<PyType>(m_vm, m_globalObject, type));
    }
    type->initializeBuiltin(m_globalObject, def, typeType(), base ? base : objectType());
    return type;
}

PyType* jsSymbolType(Runtime&);
PyType* jsIteratorObjectType(Runtime&);
PyType* jsTypedArrayType(Runtime&);
PyType* jsGeneratorType(Runtime&);
PyType* jsMapType(Runtime&);

PyType* Runtime::typeOfSlow(JSValue value) const
{
    if (value.isCell()) {
        JSCell* cell = value.asCell();
        JSType type = cell->type();
        if (isPyObjectType(type)) [[likely]]
            return uncheckedDowncast<PyObject>(cell)->type();
        switch (type) {
        case StringType:
            return strType();
        case ArrayType:
            return listType();
        case HeapBigIntType:
            return intType();
        case JSFunctionType:
            return PyFunction::is(cell) ? functionType() : jsFunctionType();
        case InternalFunctionType: {
            auto function = uncheckedDowncast<InternalFunction>(cell)->nativeFunctionFor(CodeSpecializationKind::CodeForCall);
            if (function == TaggedNativeFunction(callType))
                return uncheckedDowncast<PyType>(cell)->type();
            if (function != TaggedNativeFunction(callNativeFunction))
                return jsFunctionType();
            auto* native = uncheckedDowncast<PyNativeFunction>(cell);
            switch (native->kind()) {
            case NativeKind::Method:
                return methodDescriptorType();
            case NativeKind::SlotWrapper:
                return wrapperDescriptorType();
            case NativeKind::ClassMethod:
                return classMethodDescriptorType();
            default:
                return native->def().kind == NativeKind::SlotWrapper ? methodWrapperType() : nativeFunctionType();
            }
        }
        case JSPromiseType:
            if (PyType* derived = typeByPrototype(asObject(cell))) [[unlikely]]
                return derived;
            return jsPromiseType();
        case JSMapType:
            if (PyType* derived = typeByPrototype(asObject(cell))) [[unlikely]]
                return derived;
            return jsMapType(const_cast<Runtime&>(*this));
        case SymbolType:
            return jsSymbolType(const_cast<Runtime&>(*this));
        case JSGeneratorType:
            return jsGeneratorType(const_cast<Runtime&>(*this));
        case JSArrayIteratorType:
        case JSMapIteratorType:
        case JSSetIteratorType:
        case JSStringIteratorType:
        case JSRegExpStringIteratorType:
        case JSIteratorHelperType:
        case JSWrapForValidIteratorType:
            return jsIteratorObjectType(const_cast<Runtime&>(*this));
        case DerivedArrayType:
            if (PyListObject::is(cell))
                return uncheckedDowncast<PyListObject>(cell)->type();
            return listType();
        case FinalObjectType: {
            // What { } makes, and Object.create(null).
            JSValue prototype = asObject(cell)->getPrototypeDirect();
            if (prototype.isNull() || prototype == JSValue(cell->structure()->realm()->objectPrototype()))
                return jsPlainObjectType();
            if (PyType* derived = typeByPrototype(asObject(cell))) [[unlikely]]
                return derived;
            return jsObjectType();
        }
        default:
            if (!cell->isObject())
                return jsObjectType();
            // An instance of a class of Python's that derives from a class of JS.
            if (PyType* derived = typeByPrototype(asObject(cell))) [[unlikely]]
                return derived;
            if (isTypedArrayType(cell->type()))
                return jsTypedArrayType(const_cast<Runtime&>(*this));
            return asObject(cell)->isCallable() ? jsFunctionType() : jsObjectType();
        }
    }
    if (value.isInt32())
        return intType();
    if (value.isDouble())
        return isSmallIntValue(value.asDouble()) ? intType() : floatType();
    if (value.isBoolean())
        return boolType();
    ASSERT(value.isUndefinedOrNull());
    return noneType();
}

JSValue Runtime::handledExceptionInSlotOf(PyGenerator* generator) const
{
    return generator ? generator->handledException() : m_handledException.get();
}

void Runtime::setHandledExceptionInSlotOf(PyGenerator* generator, JSValue value)
{
    if (generator)
        generator->setHandledException(m_vm, value);
    else
        m_handledException.set(m_vm, m_globalObject, value);
}

JSValue Runtime::handledExceptionInTopSlot() const
{
    lookAtCatchesInJS();
    return handledExceptionInSlotOf(m_runningGenerator);
}

JSValue Runtime::setHandledExceptionInTopSlot(JSValue value)
{
    lookAtCatchesInJS();
    JSValue previous = handledExceptionInSlotOf(m_runningGenerator);
    setHandledExceptionInSlotOf(m_runningGenerator, value);
    return previous;
}

// Where the code of the frame itself has got to, whatever an optimizing compiler has made part of it. Nowhere: it has made way for what
// it called.
static std::optional<unsigned> offsetInCodeOfFrame(CallFrame* callFrame, CodeBlock* codeBlock)
{
#if ENABLE(DFG_JIT)
    if (codeBlock->hasCodeOrigins()) {
        CallSiteIndex index = callFrame->callSiteIndex();
        if (!codeBlock->canGetCodeOrigin(index))
            return std::nullopt;
        CodeOrigin origin = codeBlock->codeOrigin(index);
        while (InlineCallFrame* inlineCallFrame = origin.inlineCallFrame()) {
            if (inlineCallFrame->isTail())
                return std::nullopt;
            origin = inlineCallFrame->directCaller;
        }
        return origin.bytecodeIndex().offset();
    }
#else
    UNUSED_PARAM(codeBlock);
#endif
    return callFrame->bytecodeIndex().offset();
}

// What keeps the state of the async function or async generator of JS whose body the frame runs. Null: the code is not the body of one.
// (These are resumed by C++, so each time they run they have a frame of their own. next() of a generator is written in JS, and an
// optimizing compiler may make the body part of it, with its arguments nowhere to be found.)
static JSObject* asyncGeneratorOfJSOfFrame(CallFrame* callFrame, UnlinkedCodeBlock* code)
{
    if (!isAsyncFunctionBodyParseMode(code->parseMode()) || !callFrame->argumentCount())
        return nullptr;
    static_assert(static_cast<int>(JSGenerator::Argument::Generator) == 1);
    JSValue generator = callFrame->uncheckedArgument(0);
    if (!generator.isCell())
        return nullptr;
    JSType type = generator.asCell()->type();
    return type == JSAsyncGeneratorType || type == JSAsyncFunctionGeneratorType ? asObject(generator) : nullptr;
}

// Whether it will go on from one of these yield points, for all that can be told without its frame.
static bool asyncGeneratorOfJSMayGoOnFrom(JSObject* generator, unsigned yieldPoints)
{
    int32_t state;
    if (generator->type() == JSAsyncGeneratorType) {
        state = uncheckedDowncast<JSAsyncGenerator>(generator)->state();
        if (state > 0)
            state >>= JSAsyncGenerator::reasonShift;
    } else
        state = uncheckedDowncast<JSAsyncFunctionGenerator>(generator)->state();
    // It has been resumed already.
    if (state == static_cast<int32_t>(JSGenerator::State::Executing))
        return true;
    return state > 0 && PythonMark::isAmongYieldPoints(yieldPoints, state - 1);
}

// Whether the frame, which runs the body of a generator of JS, began at one of these yield points.
static bool generatorOfJSWasResumedAt(CallFrame* callFrame, unsigned yieldPoints)
{
    static_assert(static_cast<int>(JSGenerator::Argument::State) == 2);
    if (callFrame->argumentCount() < 2)
        return false;
    JSValue state = callFrame->uncheckedArgument(1);
    return state.isInt32() && state.asInt32() > 0 && PythonMark::isAmongYieldPoints(yieldPoints, state.asInt32() - 1);
}

// `catching`: the topmost frame, which is at an op_catch, on its way to the clause that begins there.
void Runtime::dropCatchesInJSThatAreOver(CallFrame* catching, unsigned clauseStart)
{
    ASSERT(!catching || catching == m_vm.topCallFrame);
    CallFrame* callFrame = m_vm.topCallFrame;
    EntryFrame* entryFrame = m_vm.topEntryFrame;
    // Of generators that have been suspended, innermost first.
    Vector<CatchInJS, 4> toPutBy;
    // While a catch is not over, neither are those of the callers.
    while (!m_catchesInJS.isEmpty()) {
        CatchInJS& entry = m_catchesInJS.last();
        // The stack grows downwards.
        while (callFrame && callFrame < entry.callFrame)
            callFrame = callFrame->callerFrame(entryFrame);
        bool isOver = true;
        bool frameIsGone = true;
        if (callFrame == entry.callFrame && !callFrame->isNativeCalleeFrame()) {
            CodeBlock* codeBlock = callFrame->codeBlock();
            // Where the frame of an async function was, there may now be that of another call of it.
            if (codeBlock && codeBlock->unlinkedCodeBlock() == entry.code && (!entry.generator || asyncGeneratorOfJSOfFrame(callFrame, entry.code) == entry.generator)) {
                frameIsGone = false;
                // A clause that is entered again has been left.
                if (callFrame == catching)
                    isOver = clauseStart <= entry.start || clauseStart >= entry.end;
                else if (auto offset = offsetInCodeOfFrame(callFrame, codeBlock))
                    isOver = *offset < entry.start || *offset >= entry.end;
                // So with a generator, which may be back in the clause: it caught in a frame that is gone.
                if (!isOver && entry.yieldPoints && !entry.generator)
                    isOver = generatorOfJSWasResumedAt(callFrame, entry.yieldPoints);
            }
        }
        if (!isOver)
            break;
        if (entry.state == CatchInJS::InSlot)
            setHandledExceptionInSlotOf(entry.slotOwner, entry.previous);
        if (entry.generator && frameIsGone && asyncGeneratorOfJSMayGoOnFrom(entry.generator, entry.yieldPoints))
            toPutBy.append(entry);
        Locker locker { m_catchesInJSLock };
        m_catchesInJS.removeLast();
    }

    if (!toPutBy.isEmpty()) {
        // The collector does not see what is in the vector.
        DeferGC deferGC(m_vm);
        while (!toPutBy.isEmpty()) {
            JSObject* generator = toPutBy.last().generator;
            unsigned count = 0;
            while (count < toPutBy.size() && toPutBy[toPutBy.size() - 1 - count].generator == generator)
                ++count;
            auto* catches = JSCellButterfly::tryCreate(m_vm, CopyOnWriteArrayWithContiguous, 4 * count);
            for (unsigned i = 0; i < count; ++i) {
                CatchInJS entry = toPutBy.takeLast();
                if (!catches)
                    continue;
                catches->setIndex(m_vm, 4 * i, jsNumber(entry.start));
                catches->setIndex(m_vm, 4 * i + 1, jsNumber(entry.end));
                catches->setIndex(m_vm, 4 * i + 2, jsNumber(entry.yieldPoints));
                catches->setIndex(m_vm, 4 * i + 3, entry.thrown);
            }
            if (!catches)
                continue;
            if (!m_catchesOfSuspendedGeneratorsOfJS)
                m_catchesOfSuspendedGeneratorsOfJS.set(m_vm, m_globalObject, JSWeakMap::create(m_vm, m_globalObject->weakMapStructure()));
            m_catchesOfSuspendedGeneratorsOfJS->set(m_vm, generator, catches);
        }
    }

    takeUpCatchesOfResumedGeneratorsOfJS(catching, clauseStart);
}

void Runtime::takeUpCatchesOfResumedGeneratorsOfJS(CallFrame* catching, unsigned clauseStart)
{
    JSWeakMap* suspended = m_catchesOfSuspendedGeneratorsOfJS.get();
    if (!suspended)
        return;
    if (!suspended->size()) {
        m_catchesOfSuspendedGeneratorsOfJS.clear();
        return;
    }

    struct Resumed {
        CallFrame* callFrame;
        UnlinkedCodeBlock* code;
        JSObject* generator;
        JSCellButterfly* catches;
        PyGenerator* slotOwner;
        unsigned offset;
    };
    Vector<Resumed, 4> resumed;
    // The frames under that of a catch were there when it was noted, and were looked at then.
    CallFrame* end = m_catchesInJS.isEmpty() ? nullptr : m_catchesInJS.last().callFrame;
    EntryFrame* entryFrame = m_vm.topEntryFrame;
    PyGenerator* running = m_runningGenerator;
    for (CallFrame* callFrame = m_vm.topCallFrame; callFrame && (!end || callFrame < end); callFrame = callFrame->callerFrame(entryFrame)) {
        if (callFrame->isNativeCalleeFrame())
            continue;
        CodeBlock* codeBlock = callFrame->codeBlock();
        if (!codeBlock)
            continue;
        UnlinkedCodeBlock* code = codeBlock->unlinkedCodeBlock();
        if (code->isPython()) {
            // What is under the body of a generator of Python's has the slot of whoever resumed that.
            if (running && code->parseMode() == SourceParseMode::GeneratorBodyMode && callFrame->uncheckedArgument(0) == JSValue(running))
                running = running->resumedBy();
            continue;
        }
        JSObject* generator = asyncGeneratorOfJSOfFrame(callFrame, code);
        if (!generator)
            continue;
        JSValue catches = suspended->get(generator);
        if (!catches.isCell())
            continue;
        suspended->remove(generator);
        // Nowhere: it has made way for what it called, and all its catches are over.
        if (auto offset = callFrame == catching ? std::optional(clauseStart) : offsetInCodeOfFrame(callFrame, codeBlock))
            resumed.append(Resumed { callFrame, code, generator, uncheckedDowncast<JSCellButterfly>(catches.asCell()), running, *offset });
    }
    if (resumed.isEmpty())
        return;
    {
        Locker locker { m_catchesInJSLock };
        for (auto& frame : resumed | std::views::reverse) {
            for (unsigned i = 0; i + 3 < frame.catches->length(); i += 4) {
                unsigned start = frame.catches->get(i).asUInt32();
                unsigned end = frame.catches->get(i + 1).asUInt32();
                // As above.
                bool isOver = frame.callFrame == catching ? frame.offset <= start || frame.offset >= end : frame.offset < start || frame.offset >= end;
                // Then so are those in it.
                if (isOver)
                    break;
                m_catchesInJS.append(CatchInJS { frame.callFrame, frame.code, start, end, frame.catches->get(i + 3), frame.slotOwner, frame.generator, frame.catches->get(i + 2).asUInt32() });
            }
        }
    }
    m_vm.writeBarrier(m_globalObject);
}

void Runtime::forgetCatchesInJSThatAreOver()
{
    if ((m_catchesInJS.isEmpty() && !m_catchesOfSuspendedGeneratorsOfJS) || !threads().isMainThread())
        return;
    dropCatchesInJSThatAreOver(nullptr, 0);
    // A generator that has gone on since and has left the clause, or has come to its end, with nobody looking at the stack meanwhile.
    JSWeakMap* suspended = m_catchesOfSuspendedGeneratorsOfJS.get();
    if (!suspended)
        return;
    MarkedArgumentBuffer generatorsAndCatches;
    suspended->takeSnapshot(generatorsAndCatches);
    for (unsigned i = 0; i + 1 < generatorsAndCatches.size(); i += 2) {
        JSObject* generator = asObject(generatorsAndCatches.at(i));
        if (!asyncGeneratorOfJSMayGoOnFrom(generator, uncheckedDowncast<JSCellButterfly>(generatorsAndCatches.at(i + 1).asCell())->get(2).asUInt32()))
            suspended->remove(generator);
    }
}

void Runtime::putCatchesInJSInSlots()
{
    if (!threads().isMainThread())
        return;
    dropCatchesInJSThatAreOver(nullptr, 0);
    for (size_t i = 0; i < m_catchesInJS.size(); ++i) {
        if (m_catchesInJS[i].state != CatchInJS::Noted)
            continue;
        m_catchesInJS[i].state = CatchInJS::BeingPutInSlot;
        CallFrame* callFrame = m_catchesInJS[i].callFrame;
        JSValue thrown = m_catchesInJS[i].thrown;
        // Of what is no exception of Python's this makes one, and asks it for its message: that may run code of the program, which
        // may catch, and ask for what is being handled.
        JSValue exception = exceptionFromThrownValue(m_globalObject, thrown);
        dropCatchesInJSThatAreOver(nullptr, 0);
        if (i >= m_catchesInJS.size() || m_catchesInJS[i].callFrame != callFrame || m_catchesInJS[i].thrown != thrown)
            return;
        if (!exception)
            continue;
        CatchInJS& entry = m_catchesInJS[i];
        {
            Locker locker { m_catchesInJSLock };
            entry.previous = handledExceptionInSlotOf(entry.slotOwner);
            entry.thrown = exception;
            entry.state = CatchInJS::InSlot;
        }
        m_vm.writeBarrier(m_globalObject);
        setHandledExceptionInSlotOf(entry.slotOwner, exception);
    }
}

void Runtime::didCatchInJS(CallFrame* callFrame, CodeBlock* codeBlock, BytecodeIndex index, JSValue thrown)
{
    UnlinkedCodeBlock* code = codeBlock->unlinkedCodeBlock();
    if (code->isPython())
        return;
    for (auto& mark : code->pythonMarks()) {
        if (mark.offset != index.offset() || mark.kind() != PythonMark::CatchOfJS)
            continue;
        unsigned start = static_cast<unsigned>(mark.bits >> 32);
        dropCatchesInJSThatAreOver(callFrame, start);
        {
            Locker locker { m_catchesInJSLock };
            m_catchesInJS.append(CatchInJS { callFrame, code, start, static_cast<unsigned>(mark.bits), thrown, m_runningGenerator, mark.value() ? asyncGeneratorOfJSOfFrame(callFrame, code) : nullptr, mark.value() });
        }
        m_vm.writeBarrier(m_globalObject);
        return;
    }
}

void didCatchInJS(JSGlobalObject* globalObject, CallFrame* callFrame, CodeBlock* codeBlock, BytecodeIndex index, JSValue thrown)
{
    runtime(globalObject).didCatchInJS(callFrame, codeBlock, index, thrown);
}

JSValue Runtime::handledException() const
{
    lookAtCatchesInJS();
    for (PyGenerator* generator = m_runningGenerator; generator; generator = generator->resumedBy()) {
        if (JSValue value = generator->handledException())
            return value;
    }
    return m_handledException.get();
}

void Runtime::takeThreadLocalState(ThreadLocalState& state)
{
    auto& thrownException = m_slots[static_cast<unsigned>(Slot::ThrownException)];
    auto& throwArguments = m_slots[static_cast<unsigned>(Slot::ThrowArguments)];
    state.currentContext = take(m_slots[static_cast<unsigned>(Slot::CurrentContext)]);
    didChangeContextVariables();
    state.handledException = take(m_handledException);
    state.runningGenerator = std::exchange(m_runningGenerator, nullptr);
    state.asyncGenFirstIter = take(m_asyncGenFirstIter);
    state.asyncGenFinalizer = take(m_asyncGenFinalizer);
    state.pendingKeywordNames = take(m_pendingKeywordNames);
    state.sendResult = take(m_sendResult);
    state.exceptionGroupRest = take(m_exceptionGroupRest);
    state.thrownException = take(thrownException);
    state.throwArguments = take(throwArguments);
    state.threadLocals = take(m_threadLocals);
    state.traceFunction = take(m_slots[static_cast<unsigned>(Slot::TraceFunction)]);
    state.profileFunction = take(m_slots[static_cast<unsigned>(Slot::ProfileFunction)]);
    state.tracingDepth = std::exchange(tracingDepth, 0);
    m_monitoring->toolIsInterrupted();
    state.tracedEvent = std::exchange(tracedEvent, -1);
    state.tracedCallFrame = std::exchange(tracedCallFrame, nullptr);
    state.tracedLine = std::exchange(tracedLine, -1);
    state.tracedOffset = std::exchange(tracedOffset, -1);
    state.reprStack = std::exchange(reprStack, { });
    state.reraisingFrame = std::exchange(reraisingFrame, nullptr);
    state.runtimeCall = std::exchange(runtimeCall, nullptr);
    state.frameDepth = std::exchange(frameDepth, 0);
    state.buffersThatKeepGIL = std::exchange(buffersThatKeepGIL, 0);
    state.isRunningPendingCalls = std::exchange(m_isRunningPendingCalls, false);
}

void Runtime::putThreadLocalState(ThreadLocalState& state)
{
    auto put = [&](WriteBarrier<Unknown>& slot, JSValue value) {
        if (value)
            set(slot, value);
        else
            slot.clear();
    };
    put(m_slots[static_cast<unsigned>(Slot::CurrentContext)], state.currentContext);
    didChangeContextVariables();
    put(m_handledException, state.handledException);
    m_runningGenerator = state.runningGenerator;
    put(m_asyncGenFirstIter, state.asyncGenFirstIter);
    put(m_asyncGenFinalizer, state.asyncGenFinalizer);
    put(m_pendingKeywordNames, state.pendingKeywordNames);
    put(m_sendResult, state.sendResult);
    put(m_exceptionGroupRest, state.exceptionGroupRest);
    put(m_slots[static_cast<unsigned>(Slot::ThrownException)], state.thrownException);
    put(m_slots[static_cast<unsigned>(Slot::ThrowArguments)], state.throwArguments);
    put(m_threadLocals, state.threadLocals);
    put(m_slots[static_cast<unsigned>(Slot::TraceFunction)], state.traceFunction);
    put(m_slots[static_cast<unsigned>(Slot::ProfileFunction)], state.profileFunction);
    tracingDepth = state.tracingDepth;
    tracedEvent = state.tracedEvent;
    tracedCallFrame = state.tracedCallFrame;
    tracedLine = state.tracedLine;
    tracedOffset = state.tracedOffset;
    reprStack = WTF::move(state.reprStack);
    reraisingFrame = state.reraisingFrame;
    runtimeCall = state.runtimeCall;
    frameDepth = state.frameDepth;
    buffersThatKeepGIL = state.buffersThatKeepGIL;
    m_isRunningPendingCalls = state.isRunningPendingCalls;
}

} // namespace Python
} // namespace JSC
