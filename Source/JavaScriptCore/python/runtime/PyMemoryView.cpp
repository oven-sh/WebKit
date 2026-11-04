/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyBuffer.h"

#include "BytecodeLivenessAnalysisInlines.h"
#include "CodeBlock.h"
#include "FrameTracers.h"
#include "HeapAnalyzer.h"
#include "HeapProfiler.h"
#include "HeapUtil.h"
#include "JSArrayBuffer.h"
#include "JSArrayBufferView.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGenericTypedArrayViewInlines.h"
#include "JSTypedArrays.h"
#include "PreventCollectionScope.h"
#include "PyAbstract.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PyNumbers.h"
#include "PyRange.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"
#include "PyTypes.h"
#include "StackVisitor.h"
#include "TopExceptionScope.h"
#include <wtf/SetForScope.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The buffer protocol and memoryview. (Objects/memoryobject.c, Objects/abstract.c and the slots for __buffer__ in
// Objects/typeobject.c of CPython are the reference.) There are no suboffsets: nothing here exports arrays of pointers.

PY_DEFINE_CELL(PyManagedBuffer);
PY_DEFINE_CELL(PyMemoryView);

// ---- Where the memory is ----

static std::span<uint8_t> bufferMemoryOf(JSCell* owner)
{
    if (!owner)
        return { };
    JSType type = owner->type();
    if (type == PyBytesType) {
        auto* bytes = uncheckedDowncast<PyBytes>(owner);
        return { bytes->mutableData(), bytes->length() };
    }
    if (type == PyByteArrayType)
        return uncheckedDowncast<PyByteArray>(owner)->mutableSpan();
    if (type == ArrayBufferType) {
        ArrayBuffer* buffer = uncheckedDowncast<JSArrayBuffer>(owner)->impl();
        return { static_cast<uint8_t*>(buffer->data()), buffer->byteLength() };
    }
    if (isTypedArrayTypeIncludingDataView(type)) {
        auto* view = uncheckedDowncast<JSArrayBufferView>(owner);
        return { static_cast<uint8_t*>(view->vector()), view->byteLength() };
    }
    ASSERT(owner->inherits<PyByteStorage>());
    auto* storage = uncheckedDowncast<PyByteStorage>(owner);
    return { storage->data(), storage->capacity() };
}

static bool bufferLocate(JSGlobalObject* globalObject, JSCell* owner, int64_t offset, int64_t requiredSize, uint8_t*& data)
{
    auto memory = bufferMemoryOf(owner);
    if (static_cast<uint64_t>(requiredSize) > memory.size()) [[unlikely]] {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseBufferError(globalObject, scope, "the ArrayBuffer was detached or shrunk"_s);
        data = nullptr;
        return false;
    }
    data = memory.data() ? memory.data() + offset : nullptr;
    return true;
}

bool refreshBuffer(JSGlobalObject* globalObject, Buffer& buffer)
{
    return bufferLocate(globalObject, buffer.owner, buffer.offset, buffer.requiredSize, buffer.data);
}

// ---- Shapes ----

static bool bufferIsContiguous(int64_t length, int64_t itemSize, unsigned dimensions, const int64_t* shape, const int64_t* strides, char order)
{
    if (!length)
        return true;
    if (order == 'A')
        return bufferIsContiguous(length, itemSize, dimensions, shape, strides, 'C') || bufferIsContiguous(length, itemSize, dimensions, shape, strides, 'F');
    int64_t expected = itemSize;
    for (unsigned i = 0; i < dimensions; ++i) {
        unsigned dimension = order == 'C' ? dimensions - 1 - i : i;
        if (shape[dimension] > 1 && strides[dimension] != expected)
            return false;
        expected *= shape[dimension];
    }
    return true;
}

static void bufferContiguousStrides(int64_t* strides, const int64_t* shape, unsigned dimensions, int64_t itemSize, char order)
{
    int64_t stride = itemSize;
    for (unsigned i = 0; i < dimensions; ++i) {
        unsigned dimension = order == 'F' ? i : dimensions - 1 - i;
        strides[dimension] = stride;
        stride *= shape[dimension];
    }
}

// The two must not overlap, unless both are contiguous.
static void bufferCopyItems(uint8_t* destination, const int64_t* destinationStrides, const uint8_t* source, const int64_t* sourceStrides, const int64_t* shape, unsigned dimensions, int64_t itemSize)
{
    int64_t count = shape[0];
    if (dimensions == 1) {
        if (destinationStrides[0] == itemSize && sourceStrides[0] == itemSize) {
            if (count)
                memmove(destination, source, count * itemSize);
            return;
        }
        for (int64_t i = 0; i < count; ++i, destination += destinationStrides[0], source += sourceStrides[0])
            memcpy(destination, source, itemSize);
        return;
    }
    for (int64_t i = 0; i < count; ++i, destination += destinationStrides[0], source += sourceStrides[0])
        bufferCopyItems(destination, destinationStrides + 1, source, sourceStrides + 1, shape + 1, dimensions - 1, itemSize);
}

static void bufferCopyToContiguous(uint8_t* destination, const uint8_t* source, int64_t length, int64_t itemSize, unsigned dimensions, const int64_t* shape, const int64_t* strides, char order)
{
    if (!length)
        return;
    if (!dimensions || bufferIsContiguous(length, itemSize, dimensions, shape, strides, order)) {
        memcpy(destination, source, length);
        return;
    }
    Vector<int64_t, 8> destinationStrides(dimensions);
    bufferContiguousStrides(destinationStrides.mutableSpan().data(), shape, dimensions, itemSize, order);
    bufferCopyItems(destination, destinationStrides.span().data(), source, strides, shape, dimensions, itemSize);
}

void copyBufferToContiguous(uint8_t* destination, const Buffer& buffer, char order)
{
    if (buffer.dimensions <= 1 && buffer.strides.isEmpty()) {
        if (buffer.length)
            memcpy(destination, buffer.data, buffer.length);
        return;
    }
    ASSERT(buffer.shape.size() == buffer.dimensions);
    Vector<int64_t, 8> strides(buffer.dimensions);
    if (buffer.strides.isEmpty())
        bufferContiguousStrides(strides.mutableSpan().data(), buffer.shape.span().data(), buffer.dimensions, buffer.itemSize, 'C');
    else
        memcpy(strides.mutableSpan().data(), buffer.strides.span().data(), buffer.dimensions * sizeof(int64_t));
    bufferCopyToContiguous(destination, buffer.data, buffer.length, buffer.itemSize, buffer.dimensions, buffer.shape.span().data(), strides.span().data(), order);
}

// ---- Formats ----

// Of an item of a native format: one of the characters the module struct knows, or Zf, or Zd. -1: it is not one.
static int64_t bufferNativeItemSize(char code, char subcode)
{
    if (code == 'Z')
        return subcode == 'f' ? 2 * sizeof(float) : subcode == 'd' ? 2 * sizeof(double) : -1;
    if (subcode)
        return -1;
    switch (code) {
    case 'c':
    case 'b':
    case 'B':
        return 1;
    case '?':
        return sizeof(bool);
    case 'h':
    case 'H':
        return sizeof(short);
    case 'i':
    case 'I':
        return sizeof(int);
    case 'l':
    case 'L':
        return sizeof(long);
    case 'q':
    case 'Q':
        return sizeof(long long);
    case 'n':
    case 'N':
        return sizeof(size_t);
    case 'e':
        return 2;
    case 'f':
        return sizeof(float);
    case 'd':
        return sizeof(double);
    case 'P':
        return sizeof(void*);
    default:
        return -1;
    }
}

static ALWAYS_INLINE bool bufferIsByteFormat(char code, char subcode) { return !subcode && (code == 'b' || code == 'B' || code == 'c'); }

static void bufferParseFormat(StringView format, char& code, char& subcode)
{
    code = 0;
    subcode = 0;
    unsigned start = !format.isEmpty() && format[0] == '@';
    unsigned length = format.length() - start;
    auto at = [&](unsigned i) -> char {
        char16_t character = format[start + i];
        return character < 128 ? static_cast<char>(character) : '\x7f';
    };
    if (length == 1)
        code = at(0);
    else if (length == 2 && at(0) == 'Z') {
        code = 'Z';
        subcode = at(1);
    }
}

// Without the @ it may begin with.
static String bufferAdjustedFormat(const String& format)
{
    return format.startsWith('@') ? format.substring(1) : format;
}

// ---- Telling the exporter ----

enum BufferWrapperField : unsigned {
    BufferWrapperView, // What __buffer__() returned. None once released.
    BufferWrapperExporter,
    BufferWrapperFieldCount,
};

// What stands for the exporter when its __buffer__() is written in Python.
static constexpr TypeDef bufferWrapperTypeDef { "_buffer_wrapper"_s, { TypeFlag::DisallowInstantiation }, { }, { }, { } };

static PyMemoryView* memoryViewFromBuffer(JSGlobalObject*, PyManagedBuffer*, const Buffer&);

// The __release_buffer__() of the type of the object, if it is written in Python.
static JSValue bufferPythonReleaseMethod(JSValue object)
{
    if (!isPyObject(object))
        return { };
    PyType* type = uncheckedDowncast<PyObject>(object.asCell())->type();
    if (!type->has(TypeFlag::Heap))
        return { };
    JSValue method = type->lookup(Special::ReleaseBuffer);
    if (!method || PyNativeFunction::is(method))
        return { };
    return method;
}

static void bufferCallPythonRelease(JSGlobalObject* globalObject, JSValue object, JSValue method, JSValue view)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    callSpecial(globalObject, method, object, view);
    if (scope.exception())
        reportUnraisable(globalObject, makeString("Exception ignored in __release_buffer__ of "_s, typeName(globalObject, object)), { });
}

static ALWAYS_INLINE void memoryViewAddExport(PyMemoryView* view)
{
    ++view->exports;
    ++view->managedBuffer()->bufferCount;
}

static void memoryViewReleaseExport(JSGlobalObject*, JSValue object)
{
    auto* view = PyMemoryView::from(object);
    ASSERT(view->exports && view->managedBuffer());
    --view->exports;
    --view->managedBuffer()->bufferCount;
}

static void bufferReleaseByteArrayExport(JSGlobalObject*, JSValue object)
{
    PyByteArray::from(object)->removeExport();
}

static void bufferWrapperRelease(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    PyRecord* wrapper = PyRecord::from(object);
    PyMemoryView* view = PyMemoryView::tryFrom(wrapper->get(BufferWrapperView));
    JSValue exporter = wrapper->get(BufferWrapperExporter);
    if (!view)
        return;
    memoryViewReleaseExport(globalObject, view);
    // If the view is of the exporter's own memory, as when __buffer__() calls that of bytearray, __release_buffer__() is
    // called when the view is released.
    bool isOwnMemory = view->managedBuffer()->object() == exporter;
    if (!isOwnMemory) {
        if (JSValue method = bufferPythonReleaseMethod(exporter)) {
            SuspendExceptionScope suspended(vm);
            bufferCallPythonRelease(globalObject, exporter, method, view);
        }
    }
    wrapper->set(vm, BufferWrapperView, jsUndefined());
    wrapper->set(vm, BufferWrapperExporter, jsUndefined());
    // In CPython that is now, as a rule: this was the last reference to it. Here nothing tells whether the program kept
    // another. It hardly ever does, and the exporter is waiting.
    if (isOwnMemory && !view->exports && !view->wasReleasedItself())
        view->release(globalObject);
}

void releaseBuffer(JSGlobalObject* globalObject, Buffer& buffer)
{
    JSValue object = std::exchange(buffer.object, JSValue());
    if (!object)
        return;
    if (ArrayBuffer* memory = std::exchange(buffer.pinned, nullptr))
        memory->unpin();
    if (std::exchange(buffer.keepsGIL, false))
        --runtime(globalObject).buffersThatKeepGIL;
    if (JSValue view = std::exchange(buffer.view, JSValue())) {
        memoryViewReleaseExport(globalObject, view);
        PyMemoryView::from(view)->release(globalObject);
        return;
    }
    if (JSValue method = bufferPythonReleaseMethod(object)) {
        // It is lent a view of the memory, which is taken back as soon as it returns.
        VM& vm = getVM(globalObject);
        SuspendExceptionScope suspended(vm);
        Buffer copy = buffer;
        copy.release = nullptr;
        PyMemoryView* view = memoryViewFromBuffer(globalObject, PyManagedBuffer::create(globalObject, copy), copy);
        view->isRestricted = true;
        bufferCallPythonRelease(globalObject, object, method, view);
        view->release(globalObject);
    }
    if (buffer.release)
        buffer.release(globalObject, object);
}

// ---- PyManagedBuffer ----

PyManagedBuffer::PyManagedBuffer(VM& vm, Structure* structure, PyType* type)
    : Base(vm, structure, type)
{
}

PyManagedBuffer* PyManagedBuffer::create(JSGlobalObject* globalObject, Buffer& buffer)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* result = new (NotNull, allocateCell<PyManagedBuffer>(vm)) PyManagedBuffer(vm, rt.structure<PyManagedBuffer>(), rt.managedBufferType());
    result->finishCreation(vm);
    JSValue object = std::exchange(buffer.object, JSValue());
    if (object)
        result->m_object.set(vm, result, object);
    result->m_owner.setMayBeNull(vm, result, buffer.owner);
    result->m_release = std::exchange(buffer.release, nullptr);
    result->requiredSize = buffer.requiredSize;
    // The class of the exporter may be given a __release_buffer__() later.
    if (result->m_release || (isPyObject(object) && uncheckedDowncast<PyObject>(object.asCell())->type()->has(TypeFlag::Heap))) {
        result->m_isRegistered = true;
        rt.registerFinalizable(result);
    }
    return result;
}

void PyManagedBuffer::setMaster(VM& vm, PyMemoryView* master)
{
    m_master.set(vm, this, master);
}

template<typename Visitor>
void PyManagedBuffer::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyManagedBuffer>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_object);
    visitor.append(thisObject->m_owner);
    visitor.append(thisObject->m_master);
}

DEFINE_VISIT_CHILDREN(PyManagedBuffer);

static void memoryViewFillBuffer(const PyMemoryView* view, Buffer& buffer)
{
    buffer.length = view->length;
    buffer.itemSize = view->itemSize;
    buffer.isReadOnly = view->isReadOnly;
    buffer.format = view->format() ? view->formatString() : String();
    buffer.dimensions = view->dimensions();
    buffer.shape.clear();
    buffer.shape.append(std::span { view->shape(), view->dimensions() });
    buffer.strides.clear();
    buffer.strides.append(std::span { view->strides(), view->dimensions() });
    buffer.offset = view->offset;
}

void PyManagedBuffer::release(JSGlobalObject* globalObject)
{
    if (m_isReleased)
        return;
    m_isReleased = true;
    if (std::exchange(m_isRegistered, false))
        runtime(globalObject).unregisterFinalizable(this);
    Buffer buffer;
    if (m_master)
        memoryViewFillBuffer(m_master.get(), buffer);
    buffer.object = m_object.get();
    buffer.owner = m_owner.get();
    buffer.requiredSize = requiredSize;
    buffer.release = m_release;
    m_object.clear();
    m_owner.clear();
    m_master.clear();
    releaseBuffer(globalObject, buffer);
}

bool PyManagedBuffer::releaseRunsCode() const
{
    JSValue object = m_object.get();
    if (object && m_release == bufferWrapperRelease)
        object = PyRecord::from(object)->get(BufferWrapperExporter);
    return !!bufferPythonReleaseMethod(object);
}

// When the collector has found that nothing reaches it.
static JSValue managedBufferFinalize(JSGlobalObject* globalObject, JSValue self)
{
    auto* buffer = PyManagedBuffer::from(self);
    if (buffer->isReleased())
        return jsUndefined();
    // What holds a buffer on one of the views is as unreachable, and has its turn in the same round; this has another later.
    if (buffer->bufferCount)
        runtime(globalObject).registerFinalizable(buffer);
    else
        buffer->release(globalObject);
    return jsUndefined();
}

// ---- What the program has dropped ----

// Calls the function with each cell that a frame refers to: the callee, the arguments, the variables, and those temporaries
// that the code will read again. False: there is a frame of which that cannot be told.
template<typename Function>
static bool bufferForEachCellInFrames(VM& vm, const Function& function)
{
    bool isComplete = true;
    // Where the frame that this one called ends. It lies among the registers of this one.
    Register* endOfCallee = nullptr;
    // A register that the analysis takes to be live need not have been set: what a handler reads counts as live all through the
    // range the handler covers. It then has what a callee left there, which may be no value at all.
    if (!vm.heap.objectSpace().preciseAllocationSet())
        vm.heap.objectSpace().enablePreciseAllocationTracking();
    auto filter = vm.heap.objectSpace().blocks().filter();
    auto consider = [&](Register& slot) {
        JSValue value = slot.jsValue();
        if (value && value.isCell() && HeapUtil::isJSCellStorage(vm.heap, filter, value.asCell()) && !value.asCell()->isZapped())
            function(value.asCell());
    };
    auto visitFrame = [&](StackVisitor& visitor) {
        CodeBlock* codeBlock = visitor->codeBlock();
        if (!visitor->isNativeFrame() && (!codeBlock || JITCode::isOptimizingJIT(codeBlock->jitType()))) {
            isComplete = false;
            return IterationStatus::Done;
        }
        CallFrame* frame = visitor->callFrame();
        size_t argumentCount = frame->argumentCountIncludingThis();
        if (codeBlock)
            argumentCount = std::max<size_t>(argumentCount, codeBlock->numParameters());
        Register* arguments = frame->registers() + CallFrame::argumentOffsetIncludingThis(0);
        function(frame->jsCallee());
        for (size_t i = 0; i < argumentCount; ++i)
            consider(arguments[i]);
        if (codeBlock) {
            function(codeBlock);
            FastBitVector live;
            bool isKnown = liveLocalsOfFrame(frame, codeBlock, endOfCallee, live);
            // The variables come first, after what is saved of the machine's registers. They keep their values for as long
            // as the frame lives, read again or not.
            for (unsigned i = CodeBlock::llintBaselineCalleeSaveSpaceAsVirtualRegisters(); i < codeBlock->numCalleeLocals(); ++i) {
                Register* slot = &frame->uncheckedR(virtualRegisterForLocal(i));
                if (slot < endOfCallee)
                    break;
                if (!isKnown || i < codeBlock->numVars() || live[i])
                    consider(*slot);
            }
        }
        endOfCallee = arguments + argumentCount;
        return IterationStatus::Continue;
    };
    StackVisitor::visit(vm.topCallFrame, vm, visitFrame);

    // The threads that are not running have frames too, and have taken with them what the runtimes hold for the one that is.
    if (vm.apiLock().hasParkedThreads()) {
        vm.apiLock().forEachParkedThread([&](JSLock::ParkedThread& parked) {
            if (!parked.topCallFrame())
                return;
            // A StackVisitor starts from what the VM says is the top.
            SetForScope callFrame(vm.topCallFrame, parked.topCallFrame());
            SetForScope entryFrame(vm.topEntryFrame, parked.topEntryFrame());
            endOfCallee = nullptr;
            StackVisitor::visit(parked.topCallFrame(), vm, visitFrame);
        });
        vm.heap.pythonRuntimes().first()->threads().forEachCellSetAside(function);
    }
    return isComplete;
}

namespace {

// What can be reached from the roots that have a name and from the frames, as opposed to what the collector keeps: it also
// takes for a reference any word of the stack that looks like one, and the frames of C++ are full of what earlier calls left.
//
// It listens to one collection. In one of the young only, an old object is visited if it may refer to a young one; it counts
// as reachable, and nothing is learnt about the old.
class BufferReachability final : public HeapAnalyzer, public Runtime::MarkingObserver {
public:
    explicit BufferReachability(VM& vm)
        : m_vm(vm)
    {
    }

    void analyzeNode(JSCell* cell) final
    {
        Locker locker { m_lock };
        m_young.add(cell);
    }
    void analyzeEdge(JSCell* from, JSCell* to, RootMarkReason) final { add(from, to); }
    void analyzePropertyNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final { add(from, to); }
    void analyzeVariableNameEdge(JSCell* from, JSCell* to, UniquedStringImpl*) final { add(from, to); }
    void analyzeIndexEdge(JSCell* from, JSCell* to, uint32_t) final { add(from, to); }
    void setOpaqueRootReachabilityReasonForCell(JSCell* cell, ASCIILiteral) final { add(nullptr, cell); }
    void setWrappedObjectForCell(JSCell*, void*) final { }
    void setLabelForCell(JSCell*, const String&) final { }

    void addRoot(JSCell* cell) { m_worklist.append(cell); }

    bool didFinishMarking(SlotVisitor&) final
    {
        if (std::exchange(m_isDone, true))
            return false;
        // The properties and the elements of an object are not reported as it is visited. The collector would ask every
        // object of the heap for them at the end, visited or not.
        Vector<JSCell*> visited = std::exchange(m_visited, { });
        for (JSCell* cell : visited) {
            cell->methodTable()->analyzeHeap(cell, *this);
            // The one kind of cell that has no more to say then than before.
            if (auto* butterfly = dynamicDowncast<JSCellButterfly>(cell); butterfly && hasContiguous(butterfly->indexingType())) {
                for (unsigned i = 0; i < butterfly->publicLength(); ++i) {
                    if (JSValue value = butterfly->get(i); value && value.isCell())
                        add(cell, value.asCell());
                }
            }
            if (!m_young.contains(cell))
                m_worklist.append(cell);
        }
        m_vm.heapProfiler()->setActiveHeapAnalyzer(nullptr);
        return false;
    }

    bool isDone() const { return m_isDone; }

    void compute()
    {
        std::ranges::sort(m_edges, { }, &Edge::from);
        while (!m_worklist.isEmpty()) {
            JSCell* cell = m_worklist.takeLast();
            if (!m_reached.add(cell).isNewEntry)
                continue;
            for (const Edge& edge : std::ranges::equal_range(m_edges, cell, { }, &Edge::from))
                m_worklist.append(edge.to);
        }
        m_edges.clear();
    }

    // The collection kept it, but the program cannot get at it.
    bool isKnownToBeUnreachable(JSCell* cell) const { return m_young.contains(cell) && !m_reached.contains(cell); }

private:
    struct Edge {
        JSCell* from;
        JSCell* to;
    };

    void add(JSCell* from, JSCell* to)
    {
        // The only root that refers to a managed buffer is the list of what is to be finalized.
        if (!from && PyManagedBuffer::is(to))
            return;
        Locker locker { m_lock };
        if (!from)
            m_worklist.append(to);
        else if (to->type() == StructureType)
            m_visited.append(from); // The first thing any cell reports.
        else
            m_edges.append(Edge { from, to });
    }

    VM& m_vm;
    Lock m_lock;
    Vector<Edge> m_edges;
    Vector<JSCell*> m_visited;
    Vector<JSCell*> m_worklist;
    UncheckedKeyHashSet<JSCell*> m_young;
    UncheckedKeyHashSet<JSCell*> m_reached;
    bool m_isDone { false };
};

} // anonymous namespace

void Runtime::releaseUnreachableBuffers(bool thorough)
{
    // The VM remembers the last exception thrown, for the debugger, long after it has been handled, and with it the traceback
    // and all that the frames in it refer to.
    m_vm.clearLastException();
    BufferReachability reachability(m_vm);
    bool isPrecise;
    {
        // So that the collection it listens to is the one asked for here, from beginning to end.
        PreventCollectionScope preventCollection(m_vm.heap);
        HeapProfiler& profiler = m_vm.ensureHeapProfiler();
        isPrecise = !profiler.activeHeapAnalyzer() && bufferForEachCellInFrames(m_vm, [&](JSCell* cell) { reachability.addRoot(cell); });
        if (isPrecise) {
            profiler.setActiveHeapAnalyzer(&reachability);
            m_markingObserver = &reachability;
        }
        m_vm.heap.collectNow(Synchronousness::Sync, thorough ? CollectionScope::Full : CollectionScope::Eden);
        if (isPrecise) {
            m_markingObserver = nullptr;
            isPrecise = reachability.isDone();
            if (isPrecise)
                reachability.compute();
            else
                profiler.setActiveHeapAnalyzer(nullptr);
        }
    }

    // Nothing below allocates a cell, so those that nothing visits stay.
    Vector<PyManagedBuffer*> buffers;
    size_t pendingCount;
    {
        Locker locker { m_finalizationLock };
        auto add = [&](JSCell* cell) {
            if (auto* buffer = PyManagedBuffer::tryFrom(cell))
                buffers.append(buffer);
        };
        for (JSCell* cell : m_pendingFinalization)
            add(cell);
        pendingCount = buffers.size();
        if (isPrecise) {
            for (JSCell* cell : m_finalizable)
                add(cell);
        }
    }
    // What holds a buffer on a view has to go before the view can.
    bool didQueue = false;
    for (bool madeProgress = true; madeProgress;) {
        madeProgress = false;
        for (size_t i = buffers.size(); i--;) {
            PyManagedBuffer* buffer = buffers[i];
            if (!buffer || buffer->bufferCount)
                continue;
            bool isPending = i < pendingCount;
            buffers[i] = nullptr;
            if (buffer->isReleased() || (!isPending && !reachability.isKnownToBeUnreachable(buffer)))
                continue;
            bool runsCode = buffer->releaseRunsCode();
            {
                Locker locker { m_finalizationLock };
                if (!runsCode) {
                    if (auto it = m_pendingFinalization.findIf([&](JSCell* cell) { return cell == buffer; }); it != m_pendingFinalization.end())
                        m_pendingFinalization.remove(it);
                }
                else if (!isPending) {
                    // At the next safe point.
                    m_finalizable.remove(buffer);
                    m_pendingFinalization.append(buffer);
                    didQueue = true;
                }
            }
            if (runsCode)
                continue;
            buffer->release(m_globalObject);
            madeProgress = true;
        }
    }
    if (didQueue)
        didQueueCalls();
}

bool isHeldByFrames(JSGlobalObject* globalObject, JSValue exporter)
{
    bool found = false;
    bufferForEachCellInFrames(getVM(globalObject), [&](JSCell* cell) {
        auto* view = PyMemoryView::tryFrom(cell);
        found |= view && !view->isReleased() && view->managedBuffer()->object() == exporter;
    });
    return found;
}

void releaseUnreachableBuffers(JSGlobalObject* globalObject, bool thorough)
{
    runtime(globalObject).releaseUnreachableBuffers(thorough);
}

// ---- PyMemoryView ----

PyMemoryView::PyMemoryView(VM& vm, Structure* structure, PyType* type, unsigned dimensions)
    : Base(vm, structure, type)
    , m_dimensions(dimensions)
{
    for (unsigned i = 0; i < 2 * dimensions; ++i)
        shape()[i] = 0;
}

PyMemoryView* PyMemoryView::createUninitialized(JSGlobalObject* globalObject, PyManagedBuffer* managedBuffer, unsigned dimensions)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto* view = new (NotNull, allocateCell<PyMemoryView>(vm, allocationSize(dimensions))) PyMemoryView(vm, rt.structure<PyMemoryView>(), rt.memoryviewType(), dimensions);
    view->finishCreation(vm);
    view->m_managedBuffer.set(vm, view, managedBuffer);
    ++managedBuffer->viewCount;
    return view;
}

PyMemoryView* PyMemoryView::createForMemory(JSGlobalObject* globalObject, JSCell* owner, size_t offset, size_t length, bool isReadOnly)
{
    Buffer buffer;
    buffer.owner = owner;
    buffer.offset = offset;
    buffer.length = length;
    buffer.requiredSize = offset + length;
    buffer.isReadOnly = isReadOnly;
    return memoryViewFromBuffer(globalObject, PyManagedBuffer::create(globalObject, buffer), buffer);
}

template<typename Visitor>
void PyMemoryView::visitChildrenImpl(JSCell* cell, Visitor& visitor)
{
    auto* thisObject = uncheckedDowncast<PyMemoryView>(cell);
    Base::visitChildren(thisObject, visitor);
    visitor.append(thisObject->m_managedBuffer);
    visitor.append(thisObject->m_format);
}

DEFINE_VISIT_CHILDREN(PyMemoryView);

void PyMemoryView::release(JSGlobalObject* globalObject)
{
    ASSERT(!exports);
    PyManagedBuffer* managedBuffer = m_managedBuffer.get();
    if (!managedBuffer)
        return;
    m_managedBuffer.clear();
    ASSERT(managedBuffer->viewCount);
    if (!--managedBuffer->viewCount)
        managedBuffer->release(globalObject);
}

uint8_t* PyMemoryView::data(JSGlobalObject* globalObject) const
{
    uint8_t* result;
    bufferLocate(globalObject, m_managedBuffer->owner(), offset, m_managedBuffer->requiredSize, result);
    return result;
}

String PyMemoryView::formatString() const
{
    return m_format ? stringOf(m_format.get()) : "B"_str;
}

void PyMemoryView::setFormat(VM& vm, JSString* format)
{
    m_format.setMayBeNull(vm, this, format);
    if (format)
        bufferParseFormat(stringOf(format), m_formatCode, m_formatSubcode);
    else {
        m_formatCode = 'B';
        m_formatSubcode = 0;
    }
}

void PyMemoryView::updateLength()
{
    length = itemSize;
    for (unsigned i = 0; i < m_dimensions; ++i)
        length *= shape()[i];
}

void PyMemoryView::updateContiguity()
{
    if (m_dimensions == 1)
        isCContiguous = isFContiguous = shape()[0] == 1 || strides()[0] == itemSize;
    else {
        isCContiguous = bufferIsContiguous(length, itemSize, m_dimensions, shape(), strides(), 'C');
        isFContiguous = bufferIsContiguous(length, itemSize, m_dimensions, shape(), strides(), 'F');
    }
}

// A view of all of the buffer. What the buffer leaves out is worked out. Null: a ValueError.
static PyMemoryView* memoryViewFromBuffer(JSGlobalObject* globalObject, PyManagedBuffer* managedBuffer, const Buffer& buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned dimensions = buffer.dimensions;
    if (dimensions > maxBufferDimensions) {
        raiseValueError(globalObject, scope, "memoryview: number of dimensions must not exceed 64"_s);
        return nullptr;
    }
    PyMemoryView* view = PyMemoryView::createUninitialized(globalObject, managedBuffer, dimensions);
    view->offset = buffer.offset;
    view->length = buffer.length;
    view->itemSize = buffer.itemSize;
    view->isReadOnly = buffer.isReadOnly;
    if (!buffer.format.isNull() && buffer.format != "B"_s)
        view->setFormat(vm, makeStr(vm, buffer.format));
    if (dimensions == 1) {
        view->shape()[0] = buffer.shape.isEmpty() ? buffer.length / buffer.itemSize : buffer.shape[0];
        view->strides()[0] = buffer.strides.isEmpty() ? buffer.itemSize : buffer.strides[0];
    } else if (dimensions) {
        memcpy(view->shape(), buffer.shape.span().data(), dimensions * sizeof(int64_t));
        if (buffer.strides.isEmpty())
            bufferContiguousStrides(view->strides(), view->shape(), dimensions, view->itemSize, 'C');
        else
            memcpy(view->strides(), buffer.strides.span().data(), dimensions * sizeof(int64_t));
    }
    view->updateContiguity();
    if (!managedBuffer->master())
        managedBuffer->setMaster(vm, view);
    return view;
}

// Another view of the same.
static PyMemoryView* memoryViewCopy(JSGlobalObject* globalObject, PyMemoryView* source)
{
    VM& vm = getVM(globalObject);
    unsigned dimensions = source->dimensions();
    PyMemoryView* view = PyMemoryView::createUninitialized(globalObject, source->managedBuffer(), dimensions);
    view->offset = source->offset;
    view->length = source->length;
    view->itemSize = source->itemSize;
    view->isReadOnly = source->isReadOnly;
    view->isCContiguous = source->isCContiguous;
    view->isFContiguous = source->isFContiguous;
    view->setFormat(vm, source->format());
    memcpy(view->shape(), source->shape(), 2 * dimensions * sizeof(int64_t));
    return view;
}

static ALWAYS_INLINE bool memoryViewCheckReleased(JSGlobalObject* globalObject, ThrowScope& scope, const PyMemoryView* view)
{
    if (!view->isReleased()) [[likely]]
        return true;
    raiseValueError(globalObject, scope, "operation forbidden on released memoryview object"_s);
    return false;
}

static ALWAYS_INLINE bool memoryViewCheckRestricted(JSGlobalObject* globalObject, ThrowScope& scope, const PyMemoryView* view)
{
    if (!view->isRestricted) [[likely]]
        return true;
    raiseValueError(globalObject, scope, "cannot create new view on restricted memoryview"_s);
    return false;
}

// ---- Getting a buffer ----

static constexpr unsigned bufferFlagRead = 0x100;
static constexpr unsigned bufferFlagWrite = 0x200;

static bool bufferCheckFlags(JSGlobalObject* globalObject, unsigned flags)
{
    if (flags != bufferFlagRead && flags != bufferFlagWrite) [[likely]]
        return true;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseSystemError(globalObject, scope, "bad argument to internal function"_s);
    return false;
}

// A one-dimensional array of items that follow one another.
static bool bufferFill(JSGlobalObject* globalObject, Buffer& buffer, JSValue object, const BufferExport& source, unsigned flags)
{
    if ((flags & BufferWritable) && source.isReadOnly) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseBufferError(globalObject, scope, "Object is not writable."_s);
        return false;
    }
    buffer.owner = source.owner;
    buffer.offset = source.offset;
    buffer.requiredSize = source.offset + source.length;
    if (!refreshBuffer(globalObject, buffer))
        return false;
    buffer.object = object;
    buffer.length = source.length;
    buffer.itemSize = source.itemSize;
    buffer.isReadOnly = source.isReadOnly;
    buffer.format = (flags & BufferFormat) && !source.format.isNull() ? String(source.format) : String();
    buffer.dimensions = 1;
    buffer.shape.clear();
    buffer.strides.clear();
    if ((flags & BufferND) == BufferND)
        buffer.shape.append(static_cast<int64_t>(source.length / source.itemSize));
    if ((flags & BufferStrides) == BufferStrides)
        buffer.strides.append(static_cast<int64_t>(source.itemSize));
    buffer.release = source.release;
    return true;
}

static bool bufferGetFromMemoryView(JSGlobalObject* globalObject, PyMemoryView* view, Buffer& buffer, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckRestricted(globalObject, scope, view)))
        return false;
    auto fail = [&](ASCIILiteral message) {
        raiseBufferError(globalObject, scope, message);
        return false;
    };
    auto asksFor = [&](unsigned flag) { return (flags & flag) == flag; };
    if (asksFor(BufferWritable) && view->isReadOnly)
        return fail("memoryview: underlying buffer is not writable"_s);
    if (asksFor(BufferCContiguous) && !view->isCContiguous)
        return fail("memoryview: underlying buffer is not C-contiguous"_s);
    if (asksFor(BufferFContiguous) && !view->isFContiguous)
        return fail("memoryview: underlying buffer is not Fortran contiguous"_s);
    if (asksFor(BufferAnyContiguous) && !view->isCContiguous && !view->isFContiguous)
        return fail("memoryview: underlying buffer is not contiguous"_s);
    if (!asksFor(BufferStrides) && !view->isCContiguous)
        return fail("memoryview: underlying buffer is not C-contiguous"_s);
    if (!asksFor(BufferND) && asksFor(BufferFormat))
        return fail("memoryview: cannot cast to unsigned bytes if the format flag is present"_s);

    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    memoryViewFillBuffer(view, buffer);
    buffer.data = data;
    // Without the format the items are bytes, though the size of an item stays what it was; without the shape, there is one
    // dimension.
    if (!asksFor(BufferFormat))
        buffer.format = String();
    if (!asksFor(BufferStrides))
        buffer.strides.clear();
    if (!asksFor(BufferND)) {
        buffer.dimensions = 1;
        buffer.shape.clear();
    }
    buffer.owner = view->managedBuffer()->owner();
    buffer.requiredSize = view->managedBuffer()->requiredSize;
    buffer.object = view;
    buffer.release = memoryViewReleaseExport;
    memoryViewAddExport(view);
    return true;
}

// bytes, bytearray and memoryview, whatever their class makes of __buffer__.
static ALWAYS_INLINE bool bufferIsCoreExporter(JSCell* cell)
{
    return cell->type() == PyBytesType || cell->type() == PyByteArrayType || PyMemoryView::is(cell);
}

static bool bufferGetFromCoreExporter(JSGlobalObject* globalObject, JSCell* cell, Buffer& buffer, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyMemoryView::is(cell))
        RELEASE_AND_RETURN(scope, bufferGetFromMemoryView(globalObject, uncheckedDowncast<PyMemoryView>(cell), buffer, flags));
    if (failed(scope, bufferCheckFlags(globalObject, flags)))
        return false;
    if (cell->type() == PyBytesType)
        RELEASE_AND_RETURN(scope, bufferFill(globalObject, buffer, cell, { cell, 0, uncheckedDowncast<PyBytes>(cell)->length(), true }, flags));
    if (cell->type() == PyByteArrayType) {
        auto* array = uncheckedDowncast<PyByteArray>(cell);
        if (!bufferFill(globalObject, buffer, cell, { cell, 0, array->length(), false, 1, { }, bufferReleaseByteArrayExport }, flags))
            return false;
        array->addExport();
        return true;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static ALWAYS_INLINE bool bufferIsJSExporter(JSCell* cell)
{
    return cell->type() == ArrayBufferType || isTypedArrayTypeIncludingDataView(cell->type());
}

static bool bufferGetFromJS(JSGlobalObject* globalObject, JSCell* cell, Buffer& buffer, unsigned flags)
{
    ASCIILiteral format;
    size_t itemSize = 1;
    if (cell->type() != ArrayBufferType) {
        TypedArrayType type = typedArrayType(cell->type());
        itemSize = type == TypeDataView ? 1 : elementSize(type);
        switch (type) {
        case TypeInt8:
            format = "b"_s;
            break;
        case TypeInt16:
            format = "h"_s;
            break;
        case TypeUint16:
            format = "H"_s;
            break;
        case TypeInt32:
            format = "i"_s;
            break;
        case TypeUint32:
            format = "I"_s;
            break;
        case TypeFloat16:
            format = "e"_s;
            break;
        case TypeFloat32:
            format = "f"_s;
            break;
        case TypeFloat64:
            format = "d"_s;
            break;
        case TypeBigInt64:
            format = "q"_s;
            break;
        case TypeBigUint64:
            format = "Q"_s;
            break;
        default:
            break;
        }
    }
    return bufferFill(globalObject, buffer, cell, { cell, 0, bufferMemoryOf(cell).size(), false, itemSize, format }, flags);
}

// The type is written in C++, and so is its __buffer__(): the view that returns is of the object's own memory, and nothing
// else has it.
static bool bufferGetFromNativeMethod(JSGlobalObject* globalObject, JSValue object, JSValue method, Buffer& buffer, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = callSpecial(globalObject, method, object, jsNumber(static_cast<int32_t>(flags)));
    RETURN_IF_EXCEPTION(scope, false);
    auto* view = PyMemoryView::tryFrom(result);
    if (!view || view->isReleased()) {
        raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, object), '\'');
        return false;
    }
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, false);
    memoryViewFillBuffer(view, buffer);
    buffer.data = data;
    buffer.owner = view->managedBuffer()->owner();
    buffer.requiredSize = view->managedBuffer()->requiredSize;
    buffer.object = object;
    buffer.view = view;
    // Nothing but the buffer refers to the view.
    memoryViewAddExport(view);
    return true;
}

static bool bufferGetFromPythonMethod(JSGlobalObject* globalObject, JSValue object, JSValue method, Buffer& buffer, unsigned flags)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue result = callSpecial(globalObject, method, object, jsNumber(static_cast<int32_t>(flags)));
    RETURN_IF_EXCEPTION(scope, false);
    auto* view = PyMemoryView::tryFrom(result);
    if (!view) {
        raiseTypeError(globalObject, scope, typeNameForMessage(globalObject, rt.typeOf(object)), ".__buffer__() must return a memoryview, not "_s, typeNameForMessage(globalObject, rt.typeOf(result)));
        return false;
    }
    bool ok = bufferGetFromMemoryView(globalObject, view, buffer, flags);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT_UNUSED(ok, ok);
    PyRecord* wrapper = PyRecord::create(globalObject, rt.typeFor(bufferWrapperTypeDef), BufferWrapperFieldCount);
    RETURN_IF_EXCEPTION(scope, false);
    wrapper->set(vm, BufferWrapperView, view);
    wrapper->set(vm, BufferWrapperExporter, object);
    buffer.object = wrapper;
    buffer.release = bufferWrapperRelease;
    return true;
}

bool hasBuffer(JSValue value)
{
    if (!value || !value.isCell())
        return false;
    JSCell* cell = value.asCell();
    if (!isPyObject(cell))
        return bufferIsJSExporter(cell);
    return cell->type() == PyBytesType || cell->type() == PyByteArrayType || !!uncheckedDowncast<PyObject>(cell)->type()->lookup(Special::Buffer);
}

static bool bufferGet(JSGlobalObject* globalObject, JSValue object, Buffer& buffer, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    ASSERT(!buffer.object);
    if (failed(scope, bufferCheckFlags(globalObject, flags)))
        return false;
    if (object && object.isCell()) {
        JSCell* cell = object.asCell();
        if (isPyObject(cell)) {
            PyType* type = uncheckedDowncast<PyObject>(cell)->type();
            bool isCore = bufferIsCoreExporter(cell);
            if (isCore && !type->has(TypeFlag::Heap))
                RELEASE_AND_RETURN(scope, bufferGetFromCoreExporter(globalObject, cell, buffer, flags));
            if (JSValue method = type->lookup(Special::Buffer)) {
                if (!PyNativeFunction::is(method))
                    RELEASE_AND_RETURN(scope, bufferGetFromPythonMethod(globalObject, object, method, buffer, flags));
                if (isCore)
                    RELEASE_AND_RETURN(scope, bufferGetFromCoreExporter(globalObject, cell, buffer, flags));
                RELEASE_AND_RETURN(scope, bufferGetFromNativeMethod(globalObject, object, method, buffer, flags));
            }
        } else if (bufferIsJSExporter(cell))
            RELEASE_AND_RETURN(scope, bufferGetFromJS(globalObject, cell, buffer, flags));
    }
    raiseTypeError(globalObject, scope, "a bytes-like object is required, not '"_s, typeName(globalObject, object), '\'');
    return false;
}

// See PyBuffer.h. What is not pinned:
// - Memory of WebAssembly, which may move when it grows, pinned or not.
// - Anything, while there is no other thread. To be pinned, a small typed array has to be given an ArrayBuffer, which costs.
static bool bufferHoldMemoryOfJS(JSGlobalObject* globalObject, Buffer& buffer)
{
    Runtime& rt = runtime(globalObject);
    JSCell* owner = buffer.owner;
    ArrayBuffer* memory = nullptr;
    if (rt.threads().runningCount()) {
        memory = owner->type() == ArrayBufferType ? uncheckedDowncast<JSArrayBuffer>(owner)->impl() : uncheckedDowncast<JSArrayBufferView>(owner)->possiblySharedBuffer();
        if (memory && memory->isWasmMemory())
            memory = nullptr;
    }
    if (!memory) {
        buffer.keepsGIL = true;
        ++rt.buffersThatKeepGIL;
        return true;
    }
    memory->pin();
    buffer.pinned = memory;
    // That of a small typed array has just moved.
    return refreshBuffer(globalObject, buffer);
}

bool getBuffer(JSGlobalObject* globalObject, JSValue object, Buffer& buffer, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool ok = bufferGet(globalObject, object, buffer, flags);
    RETURN_IF_EXCEPTION(scope, false);
    ASSERT_UNUSED(ok, ok);
    if (!buffer.owner || !bufferIsJSExporter(buffer.owner)) [[likely]]
        return true;
    ok = bufferHoldMemoryOfJS(globalObject, buffer);
    if (!ok)
        releaseBuffer(globalObject, buffer);
    RETURN_IF_EXCEPTION(scope, false);
    return true;
}

bool getBufferRunsCode(JSValue object)
{
    if (!isPyObject(object))
        return false;
    PyType* type = uncheckedDowncast<PyObject>(object.asCell())->type();
    if (!type->has(TypeFlag::Heap))
        return false;
    JSValue method = type->lookup(Special::Buffer);
    return (method && !PyNativeFunction::is(method)) || bufferPythonReleaseMethod(object);
}

// getBytesLike() has nothing to release, so the memory is not held. It is kept from being freed, though, for a while: what
// __buffer__() returned may be all that refers to it, and the __buffer__() of the next argument may resize the bytearray that
// this one is. A function takes the bytes of a few objects at most before it uses them.
static void bufferKeepMemoryForNow(JSGlobalObject* globalObject, JSCell* owner)
{
    static constexpr unsigned ringSize = 16;
    Runtime& rt = runtime(globalObject);
    if (owner && owner->type() == PyByteArrayType)
        owner = uncheckedDowncast<PyByteArray>(owner)->storage();
    if (!owner)
        return;
    PyRecord* ring = PyRecord::tryFrom(rt.slot(Runtime::Slot::RecentBufferOwners), rt.objectType());
    if (!ring) {
        ring = PyRecord::create(globalObject, rt.objectType(), ringSize);
        rt.setSlot(Runtime::Slot::RecentBufferOwners, ring);
    }
    ring->set(rt.vm(), ring->number++ % ringSize, owner);
}

bool getBytesLikeSlow(JSGlobalObject* globalObject, JSValue value, std::span<const uint8_t>& result)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* array = PyByteArray::tryFrom(value); array && !array->type()->has(TypeFlag::Heap)) {
        result = array->span();
        bufferKeepMemoryForNow(globalObject, array);
        return true;
    }
    result = { };
    Buffer buffer;
    if (failed(scope, bufferGet(globalObject, value, buffer, BufferSimple)))
        return false;
    result = buffer.span();
    bufferKeepMemoryForNow(globalObject, buffer.owner);
    releaseBuffer(globalObject, buffer);
    return true;
}

// A view of all of the buffer, which is released with the last view of it.
static JSValue memoryViewTakingOver(JSGlobalObject* globalObject, Buffer& buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue view = std::exchange(buffer.view, JSValue())) {
        memoryViewReleaseExport(globalObject, view);
        buffer.object = JSValue();
        return view;
    }
    PyManagedBuffer* managedBuffer = PyManagedBuffer::create(globalObject, buffer);
    PyMemoryView* view = memoryViewFromBuffer(globalObject, managedBuffer, buffer);
    if (!view) {
        SuspendExceptionScope suspended(vm);
        managedBuffer->release(globalObject);
    }
    RETURN_IF_EXCEPTION(scope, { });
    return view;
}

JSValue makeMemoryView(JSGlobalObject* globalObject, JSValue object, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (auto* view = PyMemoryView::tryFrom(object)) {
        if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckRestricted(globalObject, scope, view)))
            return { };
        RELEASE_AND_RETURN(scope, memoryViewCopy(globalObject, view));
    }
    if (!hasBuffer(object))
        return raiseTypeError(globalObject, scope, "memoryview: a bytes-like object is required, not '"_s, typeName(globalObject, object), '\'');
    Buffer buffer;
    bool ok = bufferGet(globalObject, object, buffer, flags);
    RETURN_IF_EXCEPTION(scope, { });
    ASSERT_UNUSED(ok, ok);
    RELEASE_AND_RETURN(scope, memoryViewTakingOver(globalObject, buffer));
}

// ---- For the host ----

bool exportsMemoryToHost(JSValue value)
{
    return isPyObject(value) && hasBuffer(value);
}

JSValue memoryForHost(JSGlobalObject* globalObject, JSValue object)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (PyBytes::is(object) && !getBufferRunsCode(object))
        return object;
    if (auto* view = PyMemoryView::tryFrom(object); view && view->isForHost)
        return view;
    JSValue made = makeMemoryView(globalObject, object);
    RETURN_IF_EXCEPTION(scope, { });
    PyMemoryView* view = PyMemoryView::from(made);
    auto fail = [&] {
        SuspendExceptionScope suspended(vm);
        view->release(globalObject);
        return JSValue();
    };
    if (!view->isCContiguous) {
        raiseBufferError(globalObject, scope, "memoryview: underlying buffer is not C-contiguous"_s);
        return fail();
    }
    uint8_t* data = view->data(globalObject);
    if (scope.exception()) [[unlikely]]
        return fail();
    JSValue exporter = view->managedBuffer()->object();
    if (!exporter || isPyObject(exporter)) {
        view->isForHost = true;
        return view;
    }

    JSCell* owner = view->managedBuffer()->owner();
    ASSERT(bufferIsJSExporter(owner));
    JSValue result;
    if (view->isReadOnly) {
        // JS has no view that cannot be written through.
        result = PyBytes::create(globalObject, std::span<const uint8_t> { data, static_cast<size_t>(view->length) });
    } else {
        RefPtr<ArrayBuffer> memory;
        size_t offset = view->offset;
        if (owner->type() == ArrayBufferType)
            memory = uncheckedDowncast<JSArrayBuffer>(owner)->impl();
        else {
            auto* ownerView = uncheckedDowncast<JSArrayBufferView>(owner);
            memory = ownerView->possiblySharedBuffer();
            offset += ownerView->byteOffset();
        }
        if (!memory)
            throwOutOfMemoryError(globalObject, scope);
        else
            result = JSUint8Array::create(globalObject, globalObject->typedArrayStructure(TypeUint8, false), WTF::move(memory), offset, view->length);
    }
    if (scope.exception()) [[unlikely]]
        return fail();
    view->release(globalObject);
    return result;
}

bool locateMemoryForHost(JSCell* holder, HostMemory& result)
{
    result = { };
    if (holder->type() == PyBytesType) {
        auto* bytes = uncheckedDowncast<PyBytes>(holder);
        result = { bytes->mutableData(), bytes->length(), true };
        return true;
    }
    auto* view = PyMemoryView::tryFrom(holder);
    if (!view || !view->isForHost || view->isReleased())
        return false;
    auto memory = bufferMemoryOf(view->managedBuffer()->owner());
    if (static_cast<uint64_t>(view->managedBuffer()->requiredSize) > memory.size())
        return false;
    result = { memory.data() ? memory.data() + view->offset : nullptr, static_cast<size_t>(view->length), view->isReadOnly };
    return true;
}

bool pinMemoryForHost(JSCell* holder)
{
    auto* view = PyMemoryView::tryFrom(holder);
    if (!view || !view->isForHost || view->isReleased())
        return false;
    memoryViewAddExport(view);
    return true;
}

void unpinMemoryForHost(JSCell* holder)
{
    // No asking what it is: a cell has no class to ask while the collector sweeps.
    auto* view = static_cast<PyMemoryView*>(holder);
    ASSERT(view->exports && view->managedBuffer());
    --view->exports;
    --view->managedBuffer()->bufferCount;
}

// ---- __buffer__ and __release_buffer__ ----

static bool bufferFlagsArgument(JSGlobalObject* globalObject, JSValue value, unsigned& flags)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t number = toSsize(globalObject, value, rt.OverflowError());
    RETURN_IF_EXCEPTION(scope, false);
    if (number > std::numeric_limits<int>::max() || number < std::numeric_limits<int>::min()) {
        raiseOverflowError(globalObject, scope, "buffer flags out of range"_s);
        return false;
    }
    flags = static_cast<unsigned>(static_cast<int>(number));
    return true;
}

JSValue bufferMethod(JSGlobalObject* globalObject, JSValue self, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned flags;
    Buffer buffer;
    if (failed(scope, bufferFlagsArgument(globalObject, flagsValue, flags)) || !bufferGetFromCoreExporter(globalObject, self.asCell(), buffer, flags))
        return { };
    RELEASE_AND_RETURN(scope, memoryViewTakingOver(globalObject, buffer));
}

JSValue exportBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flagsValue, const BufferExport& source)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned flags;
    Buffer buffer;
    if (failed(scope, bufferFlagsArgument(globalObject, flagsValue, flags)) || !bufferFill(globalObject, buffer, self, source, flags))
        return { };
    RELEASE_AND_RETURN(scope, memoryViewTakingOver(globalObject, buffer));
}

JSValue exportBuffer(JSGlobalObject* globalObject, JSValue flagsValue, Buffer& buffer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned flags;
    if (failed(scope, bufferFlagsArgument(globalObject, flagsValue, flags)) || failed(scope, refreshBuffer(globalObject, buffer)))
        return { };
    RELEASE_AND_RETURN(scope, memoryViewTakingOver(globalObject, buffer));
}

static JSValue memoryViewRelease(JSGlobalObject*, JSValue self);

JSValue releaseBufferMethod(JSGlobalObject* globalObject, JSValue self, JSValue viewValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::tryFrom(viewValue);
    if (!view)
        return raiseTypeError(globalObject, scope, "expected a memoryview object"_s);
    if (view->wasReleasedItself())
        return raiseValueError(globalObject, scope, "memoryview's buffer has already been released"_s);
    JSValue object = view->managedBuffer()->object();
    // Nothing was told of the view, so there is nothing to tell. Such is the one lent to a __release_buffer__() written in
    // Python, which may well call this.
    if (!object)
        return jsUndefined();
    if (object != self)
        return raiseValueError(globalObject, scope, "memoryview's buffer is not this object"_s);
    RELEASE_AND_RETURN(scope, memoryViewRelease(globalObject, view));
}

// ---- Items ----

template<typename T>
static ALWAYS_INLINE T bufferLoad(const uint8_t* pointer)
{
    T value;
    memcpy(&value, pointer, sizeof(T));
    return value;
}

template<typename T>
static ALWAYS_INLINE void bufferStore(uint8_t* pointer, T value)
{
    memcpy(pointer, &value, sizeof(T));
}

static NEVER_INLINE JSValue memoryViewRaiseFormatNotSupported(JSGlobalObject* globalObject, const PyMemoryView* view)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    return raiseNotImplementedError(globalObject, scope, "memoryview: format "_s, bufferAdjustedFormat(view->formatString()), " not supported"_s);
}

// Items can be told apart only if the format is that of a single one.
static ALWAYS_INLINE bool memoryViewCheckFormat(JSGlobalObject* globalObject, ThrowScope& scope, const PyMemoryView* view)
{
    if (view->formatCode()) [[likely]]
        return true;
    raiseNotImplementedError(globalObject, scope, "memoryview: unsupported format "_s, view->formatString());
    return false;
}

// Empty, with no exception: the format is not a native one.
static JSValue bufferUnpackItem(JSGlobalObject* globalObject, char code, char subcode, const uint8_t* pointer)
{
    switch (code) {
    case 'B':
        return jsNumber(*pointer);
    case 'b':
        return jsNumber(static_cast<int8_t>(*pointer));
    case 'h':
        return jsNumber(bufferLoad<short>(pointer));
    case 'H':
        return jsNumber(bufferLoad<unsigned short>(pointer));
    case 'i':
        return jsNumber(bufferLoad<int>(pointer));
    case 'I':
        return jsNumber(bufferLoad<unsigned>(pointer));
    case 'l':
        return makeInt(globalObject, static_cast<int64_t>(bufferLoad<long>(pointer)));
    case 'L':
        return makeInt(globalObject, static_cast<uint64_t>(bufferLoad<unsigned long>(pointer)));
    case 'q':
        return makeInt(globalObject, static_cast<int64_t>(bufferLoad<long long>(pointer)));
    case 'Q':
        return makeInt(globalObject, static_cast<uint64_t>(bufferLoad<unsigned long long>(pointer)));
    case 'n':
        return makeInt(globalObject, static_cast<int64_t>(bufferLoad<ssize_t>(pointer)));
    case 'N':
        return makeInt(globalObject, static_cast<uint64_t>(bufferLoad<size_t>(pointer)));
    case 'P':
        return makeInt(globalObject, static_cast<uint64_t>(bufferLoad<uintptr_t>(pointer)));
    case '?':
        return jsBoolean(!!*pointer);
    case 'e':
        return makeFloatExactly(globalObject, unpackFloat16(bufferLoad<uint16_t>(pointer)));
    case 'f':
        return makeFloatExactly(globalObject, bufferLoad<float>(pointer));
    case 'd':
        return makeFloatExactly(globalObject, bufferLoad<double>(pointer));
    case 'Z':
        if (subcode == 'f')
            return PyComplex::create(globalObject, bufferLoad<float>(pointer), bufferLoad<float>(pointer + sizeof(float)));
        if (subcode == 'd')
            return PyComplex::create(globalObject, bufferLoad<double>(pointer), bufferLoad<double>(pointer + sizeof(double)));
        return { };
    case 'c':
        return PyBytes::create(globalObject, std::span<const uint8_t> { pointer, 1 });
    default:
        return { };
    }
}

static ALWAYS_INLINE JSValue memoryViewUnpack(JSGlobalObject* globalObject, const PyMemoryView* view, const uint8_t* pointer)
{
    if (JSValue item = bufferUnpackItem(globalObject, view->formatCode(), view->formatSubcode(), pointer)) [[likely]]
        return item;
    return memoryViewRaiseFormatNotSupported(globalObject, view);
}

// The item `offset` bytes from the first.
static JSValue memoryViewUnpackAt(JSGlobalObject* globalObject, const PyMemoryView* view, int64_t offset)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, memoryViewUnpack(globalObject, view, data + offset));
}

// Sets the item `offset` bytes from the first. Converting the value may run code, which may release the view.
static bool memoryViewPackAt(JSGlobalObject* globalObject, PyMemoryView* view, int64_t offset, JSValue item)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    char code = view->formatCode();
    char subcode = view->formatSubcode();

    auto raiseInvalid = [&](PyType* type, ASCIILiteral what) {
        raise(globalObject, scope, type, makeString("memoryview: invalid "_s, what, " for format '"_s, bufferAdjustedFormat(view->formatString()), '\''));
        return false;
    };
    auto raiseInvalidType = [&] { return raiseInvalid(rt.TypeError(), "type"_s); };
    auto raiseInvalidValue = [&] { return raiseInvalid(rt.ValueError(), "value"_s); };
    // What went wrong in the conversion, said in terms of the format.
    auto fixError = [&] {
        if (clearPendingExceptionIfMatches(globalObject, rt.TypeError()))
            return raiseInvalidType();
        if (clearPendingExceptionIfMatches(globalObject, rt.OverflowError()) || clearPendingExceptionIfMatches(globalObject, rt.ValueError()))
            return raiseInvalidValue();
        return false;
    };
    uint8_t* pointer = nullptr;
    auto locate = [&] {
        if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
            return false;
        pointer = view->data(globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        pointer += offset;
        return true;
    };

    switch (code) {
    case 'b':
    case 'h':
    case 'i':
    case 'l':
    case 'q':
    case 'n': {
        JSValue index = toIndex(globalObject, item);
        if (scope.exception()) [[unlikely]]
            return fixError();
        auto value = tryInt64(index);
        if (!value)
            return raiseInvalidValue();
        if (!locate())
            return false;
        int64_t size = bufferNativeItemSize(code, 0);
        if (size < 8 && (*value < -(1ll << (8 * size - 1)) || *value >= 1ll << (8 * size - 1)))
            return raiseInvalidValue();
        memcpy(pointer, &*value, size); // Little endian, as is every machine JSC runs on.
        return true;
    }
    case 'B':
    case 'H':
    case 'I':
    case 'L':
    case 'Q':
    case 'N': {
        JSValue index = toIndex(globalObject, item);
        if (scope.exception()) [[unlikely]]
            return fixError();
        auto value = tryUInt64(index);
        if (!value)
            return raiseInvalidValue();
        if (!locate())
            return false;
        int64_t size = bufferNativeItemSize(code, 0);
        if (size < 8 && *value >> (8 * size))
            return raiseInvalidValue();
        memcpy(pointer, &*value, size);
        return true;
    }
    case 'P': {
        // An int and nothing else; a negative one stands for the address with the same bits.
        if (!isInt(item))
            return raiseInvalidType();
        JSValue number = intValue(item);
        uint64_t bits;
        if (auto value = tryUInt64(number))
            bits = *value;
        else if (auto negative = tryInt64(number))
            bits = static_cast<uint64_t>(*negative);
        else
            return raiseInvalidValue();
        if (!locate())
            return false;
        bufferStore(pointer, static_cast<uintptr_t>(bits));
        return true;
    }
    case 'e':
    case 'f':
    case 'd': {
        double value = toDouble(globalObject, item);
        if (scope.exception()) [[unlikely]]
            return fixError();
        if (!locate())
            return false;
        if (code == 'd')
            bufferStore(pointer, value);
        else if (code == 'f')
            bufferStore(pointer, static_cast<float>(value));
        else {
            auto half = packFloat16(value);
            if (!half)
                return raiseInvalidValue();
            bufferStore(pointer, *half);
        }
        return true;
    }
    case 'Z': {
        if (subcode != 'f' && subcode != 'd')
            break;
        double real;
        double imaginary;
        if (failed(scope, complexArgument(globalObject, item, real, imaginary)))
            return fixError();
        if (!locate())
            return false;
        if (subcode == 'd') {
            bufferStore(pointer, real);
            bufferStore(pointer + sizeof(double), imaginary);
        } else {
            bufferStore(pointer, static_cast<float>(real));
            bufferStore(pointer + sizeof(float), static_cast<float>(imaginary));
        }
        return true;
    }
    case '?': {
        bool value = isTrue(globalObject, item);
        RETURN_IF_EXCEPTION(scope, false);
        if (!locate())
            return false;
        *pointer = value;
        return true;
    }
    case 'c': {
        auto* bytes = PyBytes::tryFrom(item);
        if (!bytes)
            return raiseInvalidType();
        if (bytes->length() != 1)
            return raiseInvalidValue();
        if (!locate())
            return false;
        *pointer = bytes->span()[0];
        return true;
    }
    default:
        break;
    }
    memoryViewRaiseFormatNotSupported(globalObject, view);
    return false;
}

// ---- Construction and release ----

static JSValue memoryViewNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "object"_s };
    JSValue object;
    if (failed(scope, unpackKeywordArguments(globalObject, "memoryview"_s, keywords, 1, 1, false, args, &object)))
        return { };
    scope.release();
    JSValue view = makeMemoryView(globalObject, object);
    Runtime& rt = runtime(globalObject);
    rt.didCallType(rt.memoryviewType(), view, true);
    return view;
}

static JSValue memoryViewFromFlags(JSGlobalObject* globalObject, JSValue, JSValue object, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int flags = toCInt(globalObject, flagsValue);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, makeMemoryView(globalObject, object, static_cast<unsigned>(flags)));
}

static JSValue memoryViewRelease(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (view->exports)
        return raiseBufferError(globalObject, scope, "memoryview has "_s, view->exports, " exported buffer"_s, view->exports == 1 ? ""_s : "s"_s);
    view->release(globalObject);
    return jsUndefined();
}

static JSValue memoryViewEnter(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, PyMemoryView::from(self))))
        return { };
    return self;
}

static JSValue memoryViewExit(JSGlobalObject* globalObject, JSValue self, const CallArgs&)
{
    return memoryViewRelease(globalObject, self);
}

static JSValue memoryViewRepr(JSGlobalObject* globalObject, JSValue self)
{
    return makeStr(globalObject, makeString(PyMemoryView::from(self)->wasReleasedItself() ? "<released memory at "_s : "<memory at "_s, addressForRepr(globalObject, self), '>'));
}

// ---- Casting ----

static JSValue memoryViewCast(JSGlobalObject* globalObject, JSValue self, JSValue formatValue, JSValue shapeValue)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (!isStr(formatValue))
        return raiseTypeError(globalObject, scope, "cast() argument 'format' must be str, not "_s, typeNameOfArgument(globalObject, formatValue));
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckRestricted(globalObject, scope, view)))
        return { };
    if (!view->isCContiguous)
        return raiseTypeError(globalObject, scope, "memoryview: casts are restricted to C-contiguous views"_s);
    if (shapeValue || view->dimensions() != 1) {
        for (unsigned i = 0; i < view->dimensions(); ++i) {
            if (!view->shape()[i])
                return raiseTypeError(globalObject, scope, "memoryview: cannot cast view with zeros in shape or strides"_s);
        }
    }
    MarkedArgumentBuffer shapeItems;
    if (shapeValue) {
        if (auto* tuple = PyTuple::tryFrom(shapeValue)) {
            for (unsigned i = 0; i < tuple->length(); ++i)
                shapeItems.append(tuple->at(i));
        } else if (isList(shapeValue)) {
            JSArray* list = asList(shapeValue);
            for (unsigned i = 0; i < listLength(list); ++i)
                shapeItems.append(listGet(list, i));
        } else
            return raiseTypeError(globalObject, scope, "shape must be a list or a tuple"_s);
        if (shapeItems.size() > maxBufferDimensions)
            return raiseValueError(globalObject, scope, "memoryview: number of dimensions must not exceed 64"_s);
        if (view->dimensions() != 1 && shapeItems.size() != 1)
            return raiseTypeError(globalObject, scope, "memoryview: cast must be 1D -> ND or ND -> 1D"_s);
    }

    // To one dimension, of items of the new format.
    JSString* formatString = strValue(formatValue);
    String format = formatString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!format.containsOnlyASCII()) {
        strEncode(globalObject, formatString, makeStr(vm, "ascii"_s), nullptr);
        RETURN_IF_EXCEPTION(scope, { });
    }
    // As CPython, which reads it as a string of C.
    if (size_t end = format.find('\0'); end != notFound)
        format = format.left(end);
    char code;
    char subcode;
    bufferParseFormat(format, code, subcode);
    int64_t itemSize = bufferNativeItemSize(code, subcode);
    if (itemSize < 0)
        return raiseValueError(globalObject, scope, "memoryview: destination format must be a native format prefixed with an optional '@'"_s);
    bool sourceIsBytes = bufferIsByteFormat(view->formatCode(), view->formatSubcode());
    if (!sourceIsBytes && !bufferIsByteFormat(code, subcode))
        return raiseTypeError(globalObject, scope, "memoryview: cannot cast between two non-byte formats"_s);
    if (view->length % itemSize)
        return raiseTypeError(globalObject, scope, "memoryview: length is not a multiple of itemsize"_s);

    // And from there to the new shape.
    Vector<int64_t, 8> shape;
    if (!shapeValue)
        shape.append(view->length / itemSize);
    else {
        int64_t length = itemSize;
        for (unsigned i = 0; i < shapeItems.size(); ++i) {
            JSValue item = shapeItems.at(i);
            if (!isInt(item))
                return raiseTypeError(globalObject, scope, "memoryview.cast(): elements of shape must be integers"_s);
            auto extent = tryInt64(intValue(item));
            if (!extent)
                return raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
            if (*extent <= 0)
                return raiseValueError(globalObject, scope, "memoryview.cast(): elements of shape must be integers > 0"_s);
            if (*extent > std::numeric_limits<int64_t>::max() / length)
                return raiseValueError(globalObject, scope, "memoryview.cast(): product(shape) > SSIZE_MAX"_s);
            length *= *extent;
            shape.append(*extent);
        }
        if (length != view->length)
            return raiseTypeError(globalObject, scope, "memoryview: product(shape) * itemsize != buffer size"_s);
    }

    PyMemoryView* result = PyMemoryView::createUninitialized(globalObject, view->managedBuffer(), shape.size());
    result->offset = view->offset;
    result->length = view->length;
    result->itemSize = itemSize;
    result->isReadOnly = view->isReadOnly;
    result->setFormat(vm, format == "B"_s ? nullptr : makeStr(vm, format));
    memcpy(result->shape(), shape.span().data(), shape.size() * sizeof(int64_t));
    bufferContiguousStrides(result->strides(), result->shape(), shape.size(), itemSize, 'C');
    result->updateContiguity();
    return result;
}

static JSValue memoryViewToReadOnly(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckRestricted(globalObject, scope, view)))
        return { };
    PyMemoryView* result = memoryViewCopy(globalObject, view);
    result->isReadOnly = true;
    return result;
}

// ---- Conversions ----

static JSValue memoryViewToListFrom(JSGlobalObject* globalObject, const PyMemoryView* view, const uint8_t* pointer, unsigned dimension)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t count = view->shape()[dimension];
    int64_t stride = view->strides()[dimension];
    bool isLast = dimension + 1 == view->dimensions();
    JSArray* list = newList(globalObject, static_cast<unsigned>(std::min<int64_t>(count, 1 << 20)));
    RETURN_IF_EXCEPTION(scope, { });
    for (int64_t i = 0; i < count; ++i, pointer += stride) {
        JSValue item = isLast ? memoryViewUnpack(globalObject, view, pointer) : memoryViewToListFrom(globalObject, view, pointer, dimension + 1);
        RETURN_IF_EXCEPTION(scope, { });
        listAppend(globalObject, list, item);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return list;
}

static JSValue memoryViewToList(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
        return { };
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    if (!view->dimensions())
        RELEASE_AND_RETURN(scope, memoryViewUnpack(globalObject, view, data));
    RELEASE_AND_RETURN(scope, memoryViewToListFrom(globalObject, view, data, 0));
}

// A bytes of the items, in that order. Null: an exception.
static PyBytes* memoryViewContiguousCopy(JSGlobalObject* globalObject, const PyMemoryView* view, char order)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyBytes* result = newBytes(globalObject, view->length);
    RETURN_IF_EXCEPTION(scope, nullptr);
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    bufferCopyToContiguous(result->mutableData(), data, view->length, view->itemSize, view->dimensions(), view->shape(), view->strides(), order);
    return result;
}

static JSValue memoryViewToBytes(JSGlobalObject* globalObject, JSValue self, JSValue orderValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    char order = 'C';
    String orderString;
    if (orderValue && !isNone(orderValue)) {
        if (!isStr(orderValue))
            return raiseTypeError(globalObject, scope, "tobytes() argument 'order' must be str or None, not "_s, typeName(globalObject, orderValue));
        orderString = strValue(orderValue)->value(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (orderString.contains('\0'))
            return raiseValueError(globalObject, scope, "embedded null character"_s);
    }
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    if (!orderString.isNull()) {
        if (orderString == "F"_s)
            order = 'F';
        else if (orderString == "A"_s)
            order = 'A';
        else if (orderString != "C"_s)
            return raiseValueError(globalObject, scope, "order must be 'C', 'F' or 'A'"_s);
    }
    RELEASE_AND_RETURN(scope, memoryViewContiguousCopy(globalObject, view, order));
}

static JSValue memoryViewHex(JSGlobalObject* globalObject, JSValue self, JSValue separator, JSValue bytesPerSeparator)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    int64_t group = 1;
    if (bytesPerSeparator && failed(scope, ssizeArgument(globalObject, bytesPerSeparator, group)))
        return { };
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    PyBytes* bytes = memoryViewContiguousCopy(globalObject, view, 'C');
    RETURN_IF_EXCEPTION(scope, { });
    // len(sep) may run code, which cannot release the view meanwhile.
    memoryViewAddExport(view);
    JSValue result = hexOfBytes(globalObject, bytes, separator, makeInt(globalObject, group));
    memoryViewReleaseExport(globalObject, view);
    RELEASE_AND_RETURN(scope, result);
}

// ---- Indexing ----

static bool memoryViewLookUpDimension(JSGlobalObject* globalObject, const PyMemoryView* view, unsigned dimension, int64_t index, int64_t& offset)
{
    int64_t count = view->shape()[dimension];
    if (index < 0)
        index += count;
    if (index < 0 || index >= count) {
        VM& vm = getVM(globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        raiseIndexError(globalObject, scope, "index out of bounds on dimension "_s, dimension + 1);
        return false;
    }
    offset += view->strides()[dimension] * index;
    return true;
}

static bool memoryViewOffsetFromTuple(JSGlobalObject* globalObject, const PyMemoryView* view, PyTuple* tuple, int64_t& offset)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    offset = 0;
    if (tuple->length() > view->dimensions()) {
        raiseTypeError(globalObject, scope, "cannot index "_s, view->dimensions(), "-dimension view with "_s, tuple->length(), "-element tuple"_s);
        return false;
    }
    for (unsigned i = 0; i < tuple->length(); ++i) {
        int64_t index = toSsize(globalObject, tuple->at(i), rt.IndexError());
        RETURN_IF_EXCEPTION(scope, false);
        if (failed(scope, memoryViewLookUpDimension(globalObject, view, i, index, offset)))
            return false;
    }
    return true;
}

static bool memoryViewIsMultiIndex(JSGlobalObject* globalObject, JSValue key)
{
    auto* tuple = PyTuple::tryFrom(key);
    if (!tuple)
        return false;
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (!hasIndex(globalObject, tuple->at(i)))
            return false;
    }
    return true;
}

static bool memoryViewIsMultiSlice(JSValue key)
{
    auto* tuple = PyTuple::tryFrom(key);
    if (!tuple || !tuple->length())
        return false;
    for (unsigned i = 0; i < tuple->length(); ++i) {
        if (!PySlice::is(tuple->at(i)))
            return false;
    }
    return true;
}

static ALWAYS_INLINE bool memoryViewIsEmptyTuple(JSValue key)
{
    auto* tuple = PyTuple::tryFrom(key);
    return tuple && !tuple->length();
}

static JSValue memoryViewItem(JSGlobalObject* globalObject, const PyMemoryView* view, int64_t index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
        return { };
    if (!view->dimensions())
        return raiseTypeError(globalObject, scope, "invalid indexing of 0-dim memory"_s);
    if (view->dimensions() > 1)
        return raiseNotImplementedError(globalObject, scope, "multi-dimensional sub-views are not implemented"_s);
    int64_t offset = 0;
    if (failed(scope, memoryViewLookUpDimension(globalObject, view, 0, index, offset)))
        return { };
    RELEASE_AND_RETURN(scope, memoryViewUnpackAt(globalObject, view, offset));
}

static JSValue memoryViewSubscript(JSGlobalObject*, JSValue self, JSValue key);

static JSValue memoryViewGetItem(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    JSValue result = memoryViewSubscript(globalObject, self, key);
    runtime(globalObject).didSubscript(self, result);
    return result;
}

static JSValue memoryViewSubscript(JSGlobalObject* globalObject, JSValue self, JSValue key)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };

    if (!view->dimensions()) {
        if (memoryViewIsEmptyTuple(key)) {
            if (failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
                return { };
            RELEASE_AND_RETURN(scope, memoryViewUnpackAt(globalObject, view, 0));
        }
        if (key == rt.ellipsis())
            return self;
        return raiseTypeError(globalObject, scope, "invalid indexing of 0-dim memory"_s);
    }

    if (hasIndex(globalObject, key)) {
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, memoryViewItem(globalObject, view, index));
    }
    if (auto* slice = PySlice::tryFrom(key)) {
        if (failed(scope, memoryViewCheckRestricted(globalObject, scope, view)))
            return { };
        PyMemoryView* result = memoryViewCopy(globalObject, view);
        PySlice::Indices indices;
        if (failed(scope, slice->indices(globalObject, result->shape()[0], indices))) {
            SuspendExceptionScope suspended(vm);
            result->release(globalObject);
            return { };
        }
        result->offset += result->strides()[0] * indices.start;
        result->shape()[0] = indices.length;
        result->strides()[0] *= indices.step;
        result->updateLength();
        result->updateContiguity();
        return result;
    }
    if (memoryViewIsMultiIndex(globalObject, key)) {
        auto* tuple = PyTuple::from(key);
        if (failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
            return { };
        if (tuple->length() < view->dimensions())
            return raiseNotImplementedError(globalObject, scope, "sub-views are not implemented"_s);
        int64_t offset;
        if (failed(scope, memoryViewOffsetFromTuple(globalObject, view, tuple, offset)))
            return { };
        RELEASE_AND_RETURN(scope, memoryViewUnpackAt(globalObject, view, offset));
    }
    if (memoryViewIsMultiSlice(key))
        return raiseNotImplementedError(globalObject, scope, "multi-dimensional slicing is not implemented"_s);
    return raiseTypeError(globalObject, scope, "memoryview: invalid slice key"_s);
}

// An empty value deletes.
static JSValue memoryViewAssign(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)) || failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
        return { };
    if (view->isReadOnly)
        return raiseTypeError(globalObject, scope, "cannot modify read-only memory"_s);
    if (!value)
        return raiseTypeError(globalObject, scope, "cannot delete memory"_s);
    auto packAt = [&](int64_t offset) -> JSValue {
        bool ok = memoryViewPackAt(globalObject, view, offset, value);
        RETURN_IF_EXCEPTION(scope, { });
        ASSERT_UNUSED(ok, ok);
        return jsUndefined();
    };

    if (!view->dimensions()) {
        if (key == rt.ellipsis() || memoryViewIsEmptyTuple(key))
            return packAt(0);
        return raiseTypeError(globalObject, scope, "invalid indexing of 0-dim memory"_s);
    }

    if (hasIndex(globalObject, key)) {
        if (view->dimensions() > 1)
            return raiseNotImplementedError(globalObject, scope, "sub-views are not implemented"_s);
        int64_t index = toSsize(globalObject, key, rt.IndexError());
        RETURN_IF_EXCEPTION(scope, { });
        int64_t offset = 0;
        if (failed(scope, memoryViewLookUpDimension(globalObject, view, 0, index, offset)))
            return { };
        return packAt(offset);
    }

    if (auto* slice = PySlice::tryFrom(key); slice && view->dimensions() == 1) {
        BufferHolder source(globalObject);
        if (failed(scope, source.get(value, BufferFullRO)))
            return { };
        const Buffer& buffer = source.buffer;
        PySlice::Indices indices;
        if (failed(scope, slice->indices(globalObject, view->shape()[0], indices)))
            return { };
        // That may have run code.
        if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
            return { };
        String sourceFormat = buffer.format.isNull() ? "B"_str : buffer.format;
        bool sameShape = buffer.dimensions == 1 && buffer.shape[0] == indices.length;
        if (bufferAdjustedFormat(view->formatString()) != bufferAdjustedFormat(sourceFormat) || view->itemSize != buffer.itemSize || !sameShape)
            return raiseValueError(globalObject, scope, "memoryview assignment: lvalue and rvalue have different structures"_s);
        uint8_t* data = view->data(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        if (!refreshBuffer(globalObject, source.buffer))
            return { };
        if (!indices.length)
            return jsUndefined();
        int64_t itemSize = view->itemSize;
        int64_t stride = view->strides()[0] * indices.step;
        data += view->strides()[0] * indices.start;
        if (stride == itemSize && buffer.strides[0] == itemSize) {
            memmove(data, buffer.data, indices.length * itemSize);
            return jsUndefined();
        }
        // By way of a copy, as the two may overlap.
        Vector<uint8_t, 256> copy;
        if (!copy.tryReserveCapacity(indices.length * itemSize))
            return raiseMemoryError(globalObject, scope, String());
        copy.grow(indices.length * itemSize);
        bufferCopyItems(copy.mutableSpan().data(), &itemSize, buffer.data, buffer.strides.span().data(), &indices.length, 1, itemSize);
        bufferCopyItems(data, &stride, copy.span().data(), &itemSize, &indices.length, 1, itemSize);
        return jsUndefined();
    }

    if (memoryViewIsMultiIndex(globalObject, key)) {
        auto* tuple = PyTuple::from(key);
        if (tuple->length() < view->dimensions())
            return raiseNotImplementedError(globalObject, scope, "sub-views are not implemented"_s);
        int64_t offset;
        if (failed(scope, memoryViewOffsetFromTuple(globalObject, view, tuple, offset)))
            return { };
        return packAt(offset);
    }
    if (PySlice::is(key) || memoryViewIsMultiSlice(key))
        return raiseNotImplementedError(globalObject, scope, "memoryview slice assignments are currently restricted to ndim = 1"_s);
    return raiseTypeError(globalObject, scope, "memoryview: invalid slice key"_s);
}

static JSValue memoryViewSetItem(JSGlobalObject* globalObject, JSValue self, JSValue key, JSValue value) { return memoryViewAssign(globalObject, self, key, value); }
static JSValue memoryViewDelItem(JSGlobalObject* globalObject, JSValue self, JSValue key) { return memoryViewAssign(globalObject, self, key, { }); }

static JSValue memoryViewLen(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    if (!view->dimensions())
        return raiseTypeError(globalObject, scope, "0-dim memory has no length"_s);
    RELEASE_AND_RETURN(scope, makeInt(globalObject, view->shape()[0]));
}

// ---- Iteration and searching ----

enum MemoryViewIteratorField : unsigned {
    MemoryViewIteratorView, // None once exhausted.
    MemoryViewIteratorFieldCount,
};
// PyRecord::number is the index of the next item.

static JSValue memoryViewIter(JSGlobalObject* globalObject, JSValue self)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    if (!view->dimensions())
        return raiseTypeError(globalObject, scope, "invalid indexing of 0-dim memory"_s);
    if (view->dimensions() != 1)
        return raiseNotImplementedError(globalObject, scope, "multi-dimensional sub-views are not implemented"_s);
    if (failed(scope, memoryViewCheckFormat(globalObject, scope, view)))
        return { };
    PyRecord* iterator = PyRecord::create(globalObject, rt.memoryviewIteratorType(), MemoryViewIteratorFieldCount);
    iterator->set(vm, MemoryViewIteratorView, view);
    iterator->number = 0;
    return iterator;
}

static JSValue memoryViewIteratorIter(JSGlobalObject*, JSValue self) { return self; }

static JSValue memoryViewIteratorNext(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyRecord* iterator = PyRecord::from(self);
    auto* view = PyMemoryView::tryFrom(iterator->get(MemoryViewIteratorView));
    if (!view)
        return raiseStopIteration(globalObject, scope);
    // The shape of a view stays what it is, released or not.
    if (iterator->number < view->shape()[0])
        RELEASE_AND_RETURN(scope, memoryViewUnpackAt(globalObject, view, view->strides()[0] * iterator->number++));
    iterator->set(vm, MemoryViewIteratorView, jsUndefined());
    return raiseStopIteration(globalObject, scope);
}

static JSValue memoryViewCount(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator = getIter(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    int64_t count = 0;
    while (true) {
        JSValue item = iterNext(globalObject, iterator);
        RETURN_IF_EXCEPTION(scope, { });
        if (!item)
            break;
        bool equal = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, { });
        count += equal;
    }
    RELEASE_AND_RETURN(scope, makeInt(globalObject, count));
}

static JSValue memoryViewIndex(JSGlobalObject* globalObject, JSValue self, JSValue value, JSValue startValue, JSValue stopValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    int64_t start = 0;
    int64_t stop = std::numeric_limits<int64_t>::max();
    if ((startValue && failed(scope, sliceIndexArgument(globalObject, startValue, start))) || (stopValue && failed(scope, sliceIndexArgument(globalObject, stopValue, stop))))
        return { };
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    if (!view->dimensions())
        return raiseTypeError(globalObject, scope, "invalid lookup on 0-dim memory"_s);
    if (view->dimensions() != 1)
        return raiseNotImplementedError(globalObject, scope, "multi-dimensional lookup is not implemented"_s);
    int64_t count = view->shape()[0];
    if (start < 0)
        start = std::max<int64_t>(start + count, 0);
    if (stop < 0)
        stop = std::max<int64_t>(stop + count, 0);
    stop = std::min(stop, count);
    start = std::min(start, stop);
    for (int64_t index = start; index < stop; ++index) {
        JSValue item = memoryViewItem(globalObject, view, index);
        RETURN_IF_EXCEPTION(scope, { });
        bool equal = isEqual(globalObject, item, value);
        RETURN_IF_EXCEPTION(scope, { });
        if (equal)
            RELEASE_AND_RETURN(scope, makeInt(globalObject, index));
    }
    return raiseValueError(globalObject, scope, "memoryview.index(x): x not found"_s);
}

// ---- Comparison ----

// One of the two arrays compared.
struct MemoryViewOperand {
    const uint8_t* data;
    const int64_t* shape;
    const int64_t* strides;
    unsigned dimensions;
    int64_t itemSize;
    String format;
    char code;
    char subcode;
};

// Items of formats that are not native, or not the same, are compared as what struct.unpack() makes of them.
struct MemoryViewUnpacker {
    JSValue unpackFrom; // struct.Struct(format).unpack_from
    PyByteStorage* item { nullptr };
    PyMemoryView* view { nullptr }; // Of the item.
    int64_t itemSize { 0 };
};

static bool memoryViewMakeUnpacker(JSGlobalObject* globalObject, const MemoryViewOperand& operand, MemoryViewUnpacker& unpacker)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue module = importModule(globalObject, "struct"_s);
    RETURN_IF_EXCEPTION(scope, false);
    JSValue structType = getAttr(globalObject, module, internString(vm, "Struct"_s));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue structObject = call(globalObject, structType, PyBytes::create(globalObject, byteCast<uint8_t>(operand.format.utf8().span())));
    RETURN_IF_EXCEPTION(scope, false);
    unpacker.unpackFrom = getAttr(globalObject, structObject, internString(vm, "unpack_from"_s));
    RETURN_IF_EXCEPTION(scope, false);
    unpacker.itemSize = operand.itemSize;
    unpacker.item = PyByteStorage::create(vm, rt.structure<PyByteStorage>(), operand.itemSize);
    unpacker.view = PyMemoryView::createForMemory(globalObject, unpacker.item, 0, operand.itemSize, false);
    return true;
}

static JSValue memoryViewUnpackWithStruct(JSGlobalObject* globalObject, const MemoryViewUnpacker& unpacker, const uint8_t* pointer)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    memcpy(unpacker.item->data(), pointer, unpacker.itemSize);
    JSValue result = call(globalObject, unpacker.unpackFrom, unpacker.view);
    RETURN_IF_EXCEPTION(scope, { });
    if (auto* tuple = PyTuple::tryFrom(result); tuple && tuple->length() == 1)
        return tuple->at(0);
    return result;
}

static bool memoryViewNativeItemsAreEqual(char code, char subcode, const uint8_t* p, const uint8_t* q)
{
    switch (code) {
    case 'B':
    case 'b':
    case 'c':
        return *p == *q;
    case '?':
        return !*p == !*q;
    case 'h':
    case 'H':
        return bufferLoad<unsigned short>(p) == bufferLoad<unsigned short>(q);
    case 'i':
    case 'I':
        return bufferLoad<unsigned>(p) == bufferLoad<unsigned>(q);
    case 'l':
    case 'L':
        return bufferLoad<unsigned long>(p) == bufferLoad<unsigned long>(q);
    case 'q':
    case 'Q':
        return bufferLoad<unsigned long long>(p) == bufferLoad<unsigned long long>(q);
    case 'n':
    case 'N':
    case 'P':
        return bufferLoad<size_t>(p) == bufferLoad<size_t>(q);
    case 'e':
        return unpackFloat16(bufferLoad<uint16_t>(p)) == unpackFloat16(bufferLoad<uint16_t>(q));
    case 'f':
        return bufferLoad<float>(p) == bufferLoad<float>(q);
    case 'd':
        return bufferLoad<double>(p) == bufferLoad<double>(q);
    case 'Z':
        if (subcode == 'f')
            return bufferLoad<float>(p) == bufferLoad<float>(q) && bufferLoad<float>(p + sizeof(float)) == bufferLoad<float>(q + sizeof(float));
        return bufferLoad<double>(p) == bufferLoad<double>(q) && bufferLoad<double>(p + sizeof(double)) == bufferLoad<double>(q + sizeof(double));
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

// False: they differ, or there is an exception.
template<typename Function>
static bool memoryViewCompareItems(VM& vm, const uint8_t* p, const uint8_t* q, const int64_t* shape, const int64_t* pStrides, const int64_t* qStrides, unsigned dimensions, const Function& areEqual)
{
    if (!dimensions)
        return areEqual(p, q);
    for (int64_t i = 0; i < shape[0]; ++i, p += pStrides[0], q += qStrides[0]) {
        if (!memoryViewCompareItems(vm, p, q, shape + 1, pStrides + 1, qStrides + 1, dimensions - 1, areEqual))
            return false;
    }
    return true;
}

static void memoryViewMakeOperand(const PyMemoryView* view, const uint8_t* data, MemoryViewOperand& operand)
{
    operand = { data, view->shape(), view->strides(), view->dimensions(), view->itemSize, view->formatString(), view->formatCode(), view->formatSubcode() };
}

// Empty with no exception: NotImplemented.
static std::optional<bool> memoryViewEquals(JSGlobalObject* globalObject, PyMemoryView* view, JSValue other)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool isSame = other == JSValue(view);
    if (view->isReleased())
        return isSame;
    // What has no floats in it is equal to itself.
    if (isSame && !view->formatSubcode() && view->formatCode() && strchr("bBchHiIlLnNqQ?", view->formatCode()))
        return true;

    MemoryViewOperand v;
    MemoryViewOperand w;
    BufferHolder holder(globalObject);
    auto* otherView = PyMemoryView::tryFrom(other);
    if (otherView) {
        if (otherView->isReleased())
            return isSame;
        uint8_t* data = otherView->data(globalObject);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        memoryViewMakeOperand(otherView, data, w);
    } else {
        if (failed(scope, holder.get(other, BufferFullRO))) {
            takePendingException(globalObject);
            return std::nullopt;
        }
        // __buffer__() may have done anything.
        if (view->isReleased())
            return false;
        const Buffer& buffer = holder.buffer;
        w = { buffer.data, buffer.shape.span().data(), buffer.strides.span().data(), buffer.dimensions, buffer.itemSize, buffer.format.isNull() ? "B"_str : buffer.format, 0, 0 };
        bufferParseFormat(w.format, w.code, w.subcode);
    }
    uint8_t* data = view->data(globalObject);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    memoryViewMakeOperand(view, data, v);

    // The same shape, as far as the first dimension that has nothing in it.
    if (v.dimensions != w.dimensions)
        return false;
    for (unsigned i = 0; i < v.dimensions; ++i) {
        if (v.shape[i] != w.shape[i])
            return false;
        if (!v.shape[i])
            break;
    }

    bool vIsNative = bufferNativeItemSize(v.code, v.subcode) > 0;
    bool wIsNative = bufferNativeItemSize(w.code, w.subcode) > 0;
    if (vIsNative && wIsNative && v.code == w.code && v.subcode == w.subcode) {
        return memoryViewCompareItems(vm, v.data, w.data, v.shape, v.strides, w.strides, v.dimensions, [&](const uint8_t* p, const uint8_t* q) {
            return memoryViewNativeItemsAreEqual(v.code, v.subcode, p, q);
        });
    }

    // Two NaNs are one value here, where struct.unpack() makes two objects of them.
    auto valuesAreEqual = [&](JSValue a, JSValue b) {
        if (isFloat(a) && isFloat(b))
            return floatValue(a) == floatValue(b);
        return isEqual(globalObject, a, b);
    };
    if (vIsNative && wIsNative) {
        bool equal = memoryViewCompareItems(vm, v.data, w.data, v.shape, v.strides, w.strides, v.dimensions, [&](const uint8_t* p, const uint8_t* q) {
            JSValue a = bufferUnpackItem(globalObject, v.code, v.subcode, p);
            JSValue b = bufferUnpackItem(globalObject, w.code, w.subcode, q);
            return valuesAreEqual(a, b);
        });
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        return equal;
    }

    MemoryViewUnpacker unpackV;
    MemoryViewUnpacker unpackW;
    if (failed(scope, memoryViewMakeUnpacker(globalObject, v, unpackV)) || failed(scope, memoryViewMakeUnpacker(globalObject, w, unpackW))) {
        // A format that struct does not know makes them different.
        if (pendingExceptionMatches(globalObject, rt.ImportError()) || pendingExceptionMatches(globalObject, rt.MemoryError()))
            return std::nullopt;
        takePendingException(globalObject);
        return false;
    }
    // The module struct is code like any other: the memory has to stay while it runs.
    memoryViewAddExport(view);
    if (otherView)
        memoryViewAddExport(otherView);
    bool equal = memoryViewCompareItems(vm, v.data, w.data, v.shape, v.strides, w.strides, v.dimensions, [&](const uint8_t* p, const uint8_t* q) {
        JSValue a = memoryViewUnpackWithStruct(globalObject, unpackV, p);
        RETURN_IF_EXCEPTION(scope, false);
        JSValue b = memoryViewUnpackWithStruct(globalObject, unpackW, q);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, valuesAreEqual(a, b));
    });
    memoryViewReleaseExport(globalObject, view);
    if (otherView)
        memoryViewReleaseExport(globalObject, otherView);
    RETURN_IF_EXCEPTION(scope, std::nullopt);
    return equal;
}

template<bool negate>
static JSValue memoryViewCompare(JSGlobalObject* globalObject, JSValue self, JSValue other)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto equal = memoryViewEquals(globalObject, PyMemoryView::from(self), other);
    RETURN_IF_EXCEPTION(scope, { });
    if (!equal)
        RELEASE_AND_RETURN(scope, runtime(globalObject).notImplemented());
    return jsBoolean(*equal != negate);
}

static JSValue memoryViewNoOrder(JSGlobalObject* globalObject, JSValue, JSValue) { return runtime(globalObject).notImplemented(); }

static JSValue memoryViewHash(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (view->hasHash)
        RELEASE_AND_RETURN(scope, makeInt(globalObject, view->hash));
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    if (!view->isReadOnly)
        return raiseValueError(globalObject, scope, "cannot hash writable memoryview object"_s);
    if (!bufferIsByteFormat(view->formatCode(), view->formatSubcode()))
        return raiseValueError(globalObject, scope, "memoryview: hashing is restricted to formats 'B', 'b' or 'c'"_s);
    if (JSValue object = view->managedBuffer()->object()) {
        // What cannot be hashed may change. Its __hash__() cannot release the view meanwhile.
        memoryViewAddExport(view);
        Python::hash(globalObject, object);
        memoryViewReleaseExport(globalObject, view);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (view->isCContiguous) {
        uint8_t* data = view->data(globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        view->hash = hashOfBytes({ data, static_cast<size_t>(view->length) });
    } else {
        PyBytes* bytes = memoryViewContiguousCopy(globalObject, view, 'C');
        RETURN_IF_EXCEPTION(scope, { });
        view->hash = hashOfBytes(bytes->span());
    }
    view->hasHash = true;
    RELEASE_AND_RETURN(scope, makeInt(globalObject, view->hash));
}

// ---- Attributes ----

static JSValue memoryViewIntTuple(JSGlobalObject* globalObject, const int64_t* values, unsigned count)
{
    MarkedArgumentBuffer items;
    for (unsigned i = 0; i < count; ++i)
        items.append(makeInt(globalObject, values[i]));
    return PyTuple::create(globalObject, spanOf(items));
}

template<typename Function>
static ALWAYS_INLINE JSValue memoryViewAttribute(JSGlobalObject* globalObject, JSValue self, const Function& function)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* view = PyMemoryView::from(self);
    if (failed(scope, memoryViewCheckReleased(globalObject, scope, view)))
        return { };
    return function(view);
}

static JSValue memoryViewGetObj(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) {
        JSValue object = view->managedBuffer()->object();
        return object ? object : jsUndefined();
    });
}

static JSValue memoryViewGetNBytes(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return makeInt(globalObject, view->length); });
}

static JSValue memoryViewGetReadOnly(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return jsBoolean(view->isReadOnly); });
}

static JSValue memoryViewGetItemSize(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return makeInt(globalObject, view->itemSize); });
}

static JSValue memoryViewGetFormat(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) -> JSValue {
        return view->format() ? view->format() : makeStr(globalObject, "B"_s);
    });
}

static JSValue memoryViewGetNDim(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return jsNumber(view->dimensions()); });
}

static JSValue memoryViewGetShape(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return memoryViewIntTuple(globalObject, view->shape(), view->dimensions()); });
}

static JSValue memoryViewGetStrides(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return memoryViewIntTuple(globalObject, view->strides(), view->dimensions()); });
}

static JSValue memoryViewGetSuboffsets(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView*) -> JSValue { return runtime(globalObject).emptyTuple(); });
}

static JSValue memoryViewGetCContiguous(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return jsBoolean(view->isCContiguous); });
}

static JSValue memoryViewGetFContiguous(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return jsBoolean(view->isFContiguous); });
}

static JSValue memoryViewGetContiguous(JSGlobalObject* globalObject, JSValue self)
{
    return memoryViewAttribute(globalObject, self, [&](PyMemoryView* view) { return jsBoolean(view->isCContiguous || view->isFContiguous); });
}

// ---- The types ----

static constexpr NativeDef managedBufferMethods[] = {
    PY_SLOT("__del__", managedBufferFinalize, "($self, /)"),
};

// A method that keeps nothing of the view. See PyTemporaries.cpp.
template<auto function, bool returnsNewView> struct MemoryViewUse;
template<typename... Arguments, JSValue (*function)(JSGlobalObject*, JSValue, Arguments...), bool returnsNewView>
struct MemoryViewUse<function, returnsNewView> {
    static JSValue call(JSGlobalObject* globalObject, JSValue self, Arguments... arguments)
    {
        JSValue result = function(globalObject, self, arguments...);
        runtime(globalObject).didCallMethod(self, result, returnsNewView);
        return result;
    }
};

static constexpr NativeDef memoryViewMethods[] = {
    PY_NEW(memoryViewNew, "($type, *args, **kwargs)"),
    PY_SLOT("__repr__", memoryViewRepr, "($self, /)"),
    PY_SLOT("__hash__", memoryViewHash, "($self, /)"),
    PY_SLOT("__eq__", memoryViewCompare<false>, "($self, value, /)"),
    PY_SLOT("__ne__", memoryViewCompare<true>, "($self, value, /)"),
    PY_SLOT("__lt__", memoryViewNoOrder, "($self, value, /)"),
    PY_SLOT("__le__", memoryViewNoOrder, "($self, value, /)"),
    PY_SLOT("__gt__", memoryViewNoOrder, "($self, value, /)"),
    PY_SLOT("__ge__", memoryViewNoOrder, "($self, value, /)"),
    PY_SLOT("__iter__", memoryViewIter, "($self, /)"),
    PY_SLOT("__len__", memoryViewLen, "($self, /)"),
    PY_SLOT("__getitem__", memoryViewGetItem, "($self, key, /)"),
    PY_SLOT("__setitem__", memoryViewSetItem, "($self, key, value, /)"),
    PY_SLOT("__delitem__", memoryViewDelItem, "($self, key, /)"),
    PY_SLOT("__buffer__", bufferMethod, "($self, flags, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
    PY_METHOD("release", memoryViewRelease, "($self, /)"),
    PY_METHOD_WITH_FLAGS("tobytes", (MemoryViewUse<memoryViewToBytes, false>::call), "($self, /, order='C')", KeepsNothing),
    PY_METHOD_WITH_FLAGS("hex", (MemoryViewUse<memoryViewHex, false>::call), "($self, /, sep=<unrepresentable>, bytes_per_sep=1)", KeepsNothing),
    PY_METHOD_WITH_FLAGS("tolist", (MemoryViewUse<memoryViewToList, false>::call), "($self, /)", KeepsNothing),
    PY_METHOD_WITH_FLAGS("cast", (MemoryViewUse<memoryViewCast, true>::call), "($self, /, format, shape=<unrepresentable>)", KeepsNothing),
    PY_METHOD_WITH_FLAGS("toreadonly", (MemoryViewUse<memoryViewToReadOnly, true>::call), "($self, /)", KeepsNothing),
    PY_CLASSMETHOD("_from_flags", memoryViewFromFlags, "($type, /, object, flags)"),
    PY_METHOD("count", memoryViewCount, "($self, value, /)"),
    PY_METHOD("index", memoryViewIndex, "($self, value, start=0, stop=sys.maxsize, /)"),
    PY_METHOD("__enter__", memoryViewEnter, "($self, /)"),
    PY_METHOD("__exit__", memoryViewExit, "($self, /, *exc_info)"),
    PY_CLASSMETHOD("__class_getitem__", classGetItem, "($type, object, /)"),
};

static constexpr NativeGetSet memoryViewGetSets[] = {
    { "obj"_s, memoryViewGetObj },
    { "nbytes"_s, memoryViewGetNBytes },
    { "readonly"_s, memoryViewGetReadOnly },
    { "itemsize"_s, memoryViewGetItemSize },
    { "format"_s, memoryViewGetFormat },
    { "ndim"_s, memoryViewGetNDim },
    { "shape"_s, memoryViewGetShape },
    { "strides"_s, memoryViewGetStrides },
    { "suboffsets"_s, memoryViewGetSuboffsets },
    { "c_contiguous"_s, memoryViewGetCContiguous },
    { "f_contiguous"_s, memoryViewGetFContiguous },
    { "contiguous"_s, memoryViewGetContiguous },
};

static constexpr NativeDef memoryViewIteratorMethods[] = {
    PY_SLOT("__iter__", memoryViewIteratorIter, "($self, /)"),
    PY_SLOT("__next__", memoryViewIteratorNext, "($self, /)"),
};

void initializeMemoryViewTypes(JSGlobalObject* globalObject)
{
    Runtime& rt = runtime(globalObject);
    rt.managedBufferType()->initializeBuiltin(globalObject, { "managedbuffer"_s, { TypeFlag::DisallowInstantiation }, managedBufferMethods, { }, { } }, rt.typeType(), rt.objectType());
    rt.memoryviewType()->initializeBuiltin(globalObject, { "memoryview"_s, { TypeFlag::Sequence, TypeFlag::HasWeakref }, memoryViewMethods, memoryViewGetSets, "Create a new memoryview object which references the given object."_s }, rt.typeType(), rt.objectType());
    rt.memoryviewIteratorType()->initializeBuiltin(globalObject, { "memory_iterator"_s, { }, memoryViewIteratorMethods, { }, { } }, rt.typeType(), rt.objectType());
}

// ---- pickle.PickleBuffer ----

enum PickleBufferField : unsigned {
    PickleBufferView, // A view of the object, which holds its memory. None once released.
    PickleBufferFieldCount,
};

extern const TypeDef pickleBufferTypeDef;

// Null: a ValueError.
static PyMemoryView* pickleBufferView(JSGlobalObject* globalObject, JSValue self)
{
    auto* view = PyMemoryView::tryFrom(PyRecord::from(self)->get(PickleBufferView));
    if (view && !view->isReleased())
        return view;
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseValueError(globalObject, scope, "operation forbidden on released PickleBuffer object"_s);
    return nullptr;
}

static JSValue memoryViewOfBuffer(JSGlobalObject* globalObject, JSValue object, unsigned flags)
{
    Buffer buffer;
    if (!bufferGet(globalObject, object, buffer, flags))
        return { };
    return memoryViewTakingOver(globalObject, buffer);
}

static JSValue pickleBufferNew(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (args.totalCount() > 1)
        return raiseTypeError(globalObject, scope, "PickleBuffer() takes at most 1 argument ("_s, args.totalCount(), " given)"_s);
    if (args.size() != 1)
        return raiseTypeError(globalObject, scope, "PickleBuffer() takes exactly 1 positional argument (0 given)"_s);
    JSValue view = memoryViewOfBuffer(globalObject, args[0], BufferFullRO);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* result = PyRecord::create(globalObject, rt.typeFor(pickleBufferTypeDef), PickleBufferFieldCount);
    result->set(vm, PickleBufferView, view);
    return result;
}

// The buffer is that of the object itself.
static JSValue pickleBufferBuffer(JSGlobalObject* globalObject, JSValue self, JSValue flagsValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned flags;
    if (failed(scope, bufferFlagsArgument(globalObject, flagsValue, flags)))
        return { };
    PyMemoryView* view = pickleBufferView(globalObject, self);
    if (!view)
        return { };
    RELEASE_AND_RETURN(scope, memoryViewOfBuffer(globalObject, view->managedBuffer()->object(), flags));
}

static JSValue pickleBufferRaw(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyMemoryView* view = pickleBufferView(globalObject, self);
    if (!view)
        return { };
    if (!bufferIsContiguous(view->length, view->itemSize, view->dimensions(), view->shape(), view->strides(), 'A'))
        return raiseBufferError(globalObject, scope, "cannot extract raw buffer from non-contiguous buffer"_s);
    JSValue whole = memoryViewOfBuffer(globalObject, view->managedBuffer()->object(), BufferFullRO);
    RETURN_IF_EXCEPTION(scope, { });
    auto* source = PyMemoryView::from(whole);
    PyMemoryView* result = PyMemoryView::createUninitialized(globalObject, source->managedBuffer(), 1);
    result->offset = source->offset;
    result->length = source->length;
    result->isReadOnly = source->isReadOnly;
    result->shape()[0] = source->length;
    result->strides()[0] = 1;
    source->release(globalObject);
    return result;
}

static JSValue pickleBufferRelease(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* record = PyRecord::from(self);
    auto* view = PyMemoryView::tryFrom(record->get(PickleBufferView));
    record->set(getVM(globalObject), PickleBufferView, jsUndefined());
    if (view && !view->exports)
        view->release(globalObject);
    return jsUndefined();
}

static constexpr NativeDef pickleBufferMethods[] = {
    PY_NEW(pickleBufferNew, "($type, *args, **kwargs)"),
    PY_SLOT("__buffer__", pickleBufferBuffer, "($self, flags, /)"),
    PY_SLOT("__release_buffer__", releaseBufferMethod, "($self, buffer, /)"),
    PY_METHOD("raw", pickleBufferRaw, "($self, /)"),
    PY_METHOD("release", pickleBufferRelease, "($self, /)"),
};

constexpr TypeDef pickleBufferTypeDef { "pickle.PickleBuffer"_s, { TypeFlag::HasWeakref }, pickleBufferMethods, { }, "Wrapper for potentially out-of-band buffers"_s };

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
