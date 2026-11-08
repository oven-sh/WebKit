/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"

#include "JSCInlines.h"
#include "JSWeakMap.h"
#include "JSWeakMapInlines.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyCodecs.h"
#include "PyException.h"
#include "PyFunction.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyModuleHelpers.h"
#include "PySet.h"
#include "WeakMapImplInlines.h"
#include <wtf/HexNumber.h>
#include <wtf/text/StringBuilder.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _pickle: what makes pickle fast. (Modules/_pickle.c in CPython is the reference; PickleBuffer is in PyMemoryView.cpp.)

extern const TypeDef pickleBufferTypeDef;

enum PickleOp : uint8_t {
    PickleOpMark = '(',
    PickleOpStop = '.',
    PickleOpPop = '0',
    PickleOpPopMark = '1',
    PickleOpDup = '2',
    PickleOpFloat = 'F',
    PickleOpInt = 'I',
    PickleOpBinInt = 'J',
    PickleOpBinInt1 = 'K',
    PickleOpLong = 'L',
    PickleOpBinInt2 = 'M',
    PickleOpNone = 'N',
    PickleOpPersId = 'P',
    PickleOpBinPersId = 'Q',
    PickleOpReduce = 'R',
    PickleOpString = 'S',
    PickleOpBinString = 'T',
    PickleOpShortBinString = 'U',
    PickleOpUnicode = 'V',
    PickleOpBinUnicode = 'X',
    PickleOpAppend = 'a',
    PickleOpBuild = 'b',
    PickleOpGlobal = 'c',
    PickleOpDict = 'd',
    PickleOpEmptyDict = '}',
    PickleOpAppends = 'e',
    PickleOpGet = 'g',
    PickleOpBinGet = 'h',
    PickleOpInst = 'i',
    PickleOpLongBinGet = 'j',
    PickleOpList = 'l',
    PickleOpEmptyList = ']',
    PickleOpObj = 'o',
    PickleOpPut = 'p',
    PickleOpBinPut = 'q',
    PickleOpLongBinPut = 'r',
    PickleOpSetItem = 's',
    PickleOpTuple = 't',
    PickleOpEmptyTuple = ')',
    PickleOpSetItems = 'u',
    PickleOpBinFloat = 'G',
    // Protocol 2
    PickleOpProto = 0x80,
    PickleOpNewObj = 0x81,
    PickleOpExt1 = 0x82,
    PickleOpExt2 = 0x83,
    PickleOpExt4 = 0x84,
    PickleOpTuple1 = 0x85,
    PickleOpTuple2 = 0x86,
    PickleOpTuple3 = 0x87,
    PickleOpNewTrue = 0x88,
    PickleOpNewFalse = 0x89,
    PickleOpLong1 = 0x8a,
    PickleOpLong4 = 0x8b,
    // Protocol 3
    PickleOpBinBytes = 'B',
    PickleOpShortBinBytes = 'C',
    // Protocol 4
    PickleOpShortBinUnicode = 0x8c,
    PickleOpBinUnicode8 = 0x8d,
    PickleOpBinBytes8 = 0x8e,
    PickleOpEmptySet = 0x8f,
    PickleOpAddItems = 0x90,
    PickleOpFrozenSet = 0x91,
    PickleOpNewObjEx = 0x92,
    PickleOpStackGlobal = 0x93,
    PickleOpMemoize = 0x94,
    PickleOpFrame = 0x95,
    // Protocol 5
    PickleOpByteArray8 = 0x96,
    PickleOpNextBuffer = 0x97,
    PickleOpReadonlyBuffer = 0x98,
};

static constexpr int pickleHighestProtocol = 5;
static constexpr int pickleDefaultProtocol = 5;
static constexpr unsigned pickleBatchSize = 1000;
static constexpr int pickleFastNestingLimit = 50;
static constexpr size_t pickleFrameSizeMin = 4;
static constexpr size_t pickleFrameSizeTarget = 64 * 1024;
static constexpr size_t pickleFrameHeaderSize = 9;
static constexpr int64_t pickleMinReadBufferSize = 1 << 20;

// ---- What the module keeps ----

enum PickleStateField : unsigned {
    PickleStatePickleError,
    PickleStatePicklingError,
    PickleStateUnpicklingError,
    PickleStateDispatchTable, // copyreg.dispatch_table,
    PickleStateExtensionRegistry, // _extension_registry,
    PickleStateInvertedRegistry, // _inverted_registry,
    PickleStateExtensionCache, // and _extension_cache
    PickleStateNameMapping2to3, // _compat_pickle.NAME_MAPPING,
    PickleStateImportMapping2to3, // IMPORT_MAPPING,
    PickleStateNameMapping3to2, // REVERSE_NAME_MAPPING,
    PickleStateImportMapping3to2, // and REVERSE_IMPORT_MAPPING
    PickleStateCodecsEncode,
    PickleStatePartial,
    PickleStateGetAttr,
    PickleStatePicklerType,
    PickleStateUnpicklerType,
    PickleStatePicklerMemoProxyType,
    PickleStateUnpicklerMemoProxyType,
    PickleStateFieldCount
};

static PyRecord* pickleState(JSGlobalObject* globalObject)
{
    return uncheckedDowncast<PyRecord>(runtime(globalObject).slot(Runtime::Slot::Pickle).asCell());
}

static PyType* pickleStateType(JSGlobalObject* globalObject, PickleStateField field) { return PyType::from(pickleState(globalObject)->get(field)); }
static PyDict* pickleStateDict(JSGlobalObject* globalObject, PickleStateField field) { return PyDict::from(pickleState(globalObject)->get(field)); }

// Raises one of the exceptions of the module. Returns false, for the caller to return.
template<typename... Parts>
static NEVER_INLINE bool pickleRaise(JSGlobalObject* globalObject, PickleStateField type, Parts... parts)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raise(globalObject, scope, pickleStateType(globalObject, type), makeString(parts...));
    return false;
}

// repr(value), str(value). Null: an exception.
static String pickleRepr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = repr(globalObject, value);
    RETURN_IF_EXCEPTION(scope, String());
    RELEASE_AND_RETURN(scope, text->value(globalObject));
}

static String pickleStr(JSGlobalObject* globalObject, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* text = str(globalObject, value);
    RETURN_IF_EXCEPTION(scope, String());
    RELEASE_AND_RETURN(scope, text->value(globalObject));
}

// %T in a message of CPython.
static String pickleTypeName(JSGlobalObject* globalObject, JSValue value) { return typeNameForMessage(globalObject, typeOf(globalObject, value)); }

// Adds a note to the exception that is propagating. `makeNote` may raise, and then returns a null String.
template<typename Function>
static NEVER_INLINE void pickleAddNote(JSGlobalObject* globalObject, const Function& makeNote)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto add = [&] {
        String note = makeNote();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue notes = getAttrIfPresent(globalObject, exception, rt.names.dNotes());
        RETURN_IF_EXCEPTION(scope, void());
        if (!notes) {
            notes = newList(globalObject);
            setAttr(globalObject, exception, rt.names.dNotes(), notes);
            RETURN_IF_EXCEPTION(scope, void());
        } else if (!isList(notes)) {
            raiseTypeError(globalObject, scope, "Cannot add note: __notes__ is not a list"_s);
            return;
        }
        listAppend(globalObject, asList(notes), makeStr(vm, note));
    };
    add();
    if (scope.exception()) {
        if (JSValue failure = pendingException(globalObject); failure && PyException::is(failure))
            PyException::from(failure)->setContext(vm, exception);
        return;
    }
    throwPythonException(globalObject, scope, exception);
}

// Raises type(makeMessage(exception)) in place of the exception that is propagating, which becomes its context.
template<typename Function>
static NEVER_INLINE void pickleReplaceException(JSGlobalObject* globalObject, PyType* type, const Function& makeMessage)
{
    VM& vm = getVM(globalObject);
    JSValue exception = takePendingException(globalObject);
    if (!exception)
        return;
    auto scope = DECLARE_THROW_SCOPE(vm);
    String message = makeMessage(exception);
    if (!scope.exception())
        raise(globalObject, scope, type, message);
    if (JSValue raised = pendingException(globalObject); raised && raised != exception && PyException::is(raised))
        PyException::from(raised)->setContext(vm, exception);
}

// name.split('.')
static Vector<String> pickleDottedPath(const String& name)
{
    Vector<String> path;
    for (auto part : StringView(name).splitAllowingEmptyEntries('.'))
        path.append(part.toString());
    return path;
}

// The attribute at the end of the path. Empty: an exception, or, if not `raises`, there may be none.
static JSValue pickleGetAttribute(JSGlobalObject* globalObject, JSValue object, const Vector<String>& path, bool raises)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    for (const String& name : path) {
        JSString* nameString = internString(vm, name);
        object = raises ? getAttr(globalObject, object, nameString) : getAttrIfPresent(globalObject, object, nameString);
        RETURN_IF_EXCEPTION(scope, { });
        if (!object)
            return { };
    }
    return object;
}

// What PyImport_Import() does with what is to be the name of a module.
static JSValue pickleImport(JSGlobalObject* globalObject, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!isStr(name))
        return raiseTypeError(globalObject, scope, "module name must be a string"_s);
    if (!strValue(name)->length())
        return raiseValueError(globalObject, scope, "Empty module name"_s);
    RELEASE_AND_RETURN(scope, importModule(globalObject, strValue(name)));
}

// The arguments of a method that takes `count` of them, by position only, and complains as those of _pickle do.
static bool picklePositionalOnly(JSGlobalObject* globalObject, ASCIILiteral name, const CallArgs& args, unsigned count)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    unsigned total = args.totalCount();
    if (total > count) {
        if (!count)
            raiseTypeError(globalObject, scope, name, "() takes no arguments"_s);
        else
            raiseTypeError(globalObject, scope, name, "() takes at most "_s, count, count == 1 ? " argument ("_s : " arguments ("_s, total, " given)"_s);
        return false;
    }
    if (args.size() < count) {
        raiseTypeError(globalObject, scope, name, "() takes exactly "_s, count, count == 1 ? " positional argument ("_s : " positional arguments ("_s, args.size(), " given)"_s);
        return false;
    }
    return true;
}

// ---- Pickler ----

enum PicklerField : unsigned {
    PicklerWrite, // Empty: __init__ has not run, or the pickle is wanted as bytes.
    PicklerPersistentIdAttr,
    PicklerDispatchTable,
    PicklerBufferCallback,
    // The memo: from an object to its number, by identity. A WeakMap of JS does that; the list is what keeps the objects. The list is an object like any other, which gc.get_referents() hands out: nothing is believed of
    // it. How many objects have a number, which is the number of the next, is counted apart.
    PicklerMemoMap,
    PicklerMemoKeys,
    PicklerMemoCount,
    PicklerBin,
    PicklerFast,
    PicklerFieldCount
};
// In `number`: the protocol, and above it
enum PicklerBit : int64_t { PicklerFixImports = 1 << 8, PicklerIsRunning = 1 << 9 };

static void picklerClearMemo(JSGlobalObject* globalObject, PyRecord* pickler)
{
    VM& vm = getVM(globalObject);
    pickler->set(vm, PicklerMemoMap, JSWeakMap::create(vm, globalObject->weakMapStructure()));
    pickler->set(vm, PicklerMemoKeys, newList(globalObject));
    pickler->set(vm, PicklerMemoCount, jsNumber(0));
}

static void picklerMemoSet(JSGlobalObject* globalObject, PyRecord* pickler, JSCell* object, double index)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* map = uncheckedDowncast<JSWeakMap>(pickler->get(PicklerMemoMap).asCell());
    if (!map->has(object)) {
        listAppend(globalObject, asList(pickler->get(PicklerMemoKeys)), object);
        RETURN_IF_EXCEPTION(scope, void());
        pickler->set(vm, PicklerMemoCount, jsNumber(pickler->get(PicklerMemoCount).asNumber() + 1));
    }
    map->set(vm, object, jsNumber(index));
}

static NEVER_INLINE JSValue picklerRaiseMemoKeysChanged(JSGlobalObject* globalObject)
{
    auto scope = DECLARE_THROW_SCOPE(getVM(globalObject));
    return raiseSystemError(globalObject, scope, "the list that keeps what a Pickler has memoized has been changed from outside"_s);
}

// The list of the objects that have a number, for whoever goes through it. Null: an exception: it is not as long as the Pickler has left it.
static JSArray* picklerMemoKeysToGoThrough(JSGlobalObject* globalObject, PyRecord* pickler)
{
    JSArray* keys = asList(pickler->get(PicklerMemoKeys));
    if (listLength(keys) == pickler->get(PicklerMemoCount).asNumber()) [[likely]]
        return keys;
    picklerRaiseMemoKeysChanged(globalObject);
    return nullptr;
}

// The number of what has been read out of that list. Empty: an exception: the Pickler has not put it there.
static JSValue picklerMemoNumberOf(JSGlobalObject* globalObject, JSWeakMap* map, JSValue object)
{
    if (object.isCell()) [[likely]] {
        if (JSValue index = map->get(object.asCell()); index.isNumber()) [[likely]]
            return index;
    }
    return picklerRaiseMemoKeysChanged(globalObject);
}

static PyRecord* picklerCreate(JSGlobalObject* globalObject, PyType* type)
{
    VM& vm = getVM(globalObject);
    PyRecord* pickler = PyRecord::create(globalObject, type, PicklerFieldCount);
    pickler->set(vm, PicklerBin, jsNumber(0));
    pickler->set(vm, PicklerFast, jsNumber(0));
    picklerClearMemo(globalObject, pickler);
    return pickler;
}

static bool picklerSetProtocol(JSGlobalObject* globalObject, PyRecord* pickler, JSValue protocol, bool fixImports)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    int64_t proto = pickleDefaultProtocol;
    if (protocol && !isNone(protocol)) {
        JSValue index = toIndex(globalObject, protocol);
        RETURN_IF_EXCEPTION(scope, false);
        auto value = tryInt64(index);
        if (!value) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
            return false;
        }
        proto = *value;
        if (proto < 0)
            proto = pickleHighestProtocol;
        else if (proto > pickleHighestProtocol) {
            raiseValueError(globalObject, scope, "pickle protocol must be <= "_s, pickleHighestProtocol);
            return false;
        }
    }
    pickler->number = (pickler->number & PicklerIsRunning) | proto | (fixImports && proto < 3 ? PicklerFixImports : 0);
    pickler->set(vm, PicklerBin, jsNumber(proto > 0));
    return true;
}

static bool picklerSetOutputStream(JSGlobalObject* globalObject, PyRecord* pickler, JSValue file)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue write = getAttrIfPresent(globalObject, file, rt.names.write());
    RETURN_IF_EXCEPTION(scope, false);
    if (!write) {
        raiseTypeError(globalObject, scope, "file must have a 'write' attribute"_s);
        return false;
    }
    pickler->set(vm, PicklerWrite, write);
    return true;
}

static bool picklerSetBufferCallback(JSGlobalObject* globalObject, PyRecord* pickler, JSValue callback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (callback && isNone(callback))
        callback = { };
    if (callback && (pickler->number & 0xff) < 5) {
        raiseValueError(globalObject, scope, "buffer_callback needs protocol >= 5"_s);
        return false;
    }
    pickler->set(vm, PicklerBufferCallback, callback);
    return true;
}

static JSValue picklerPersistentId(JSGlobalObject*, JSValue, JSValue) { return jsUndefined(); }
static bool picklerIsDefaultPersistentId(JSValue function, JSValue self);

// One call of dump().
class PicklerRun {
public:
    PicklerRun(JSGlobalObject* globalObject, PyRecord* pickler)
        : m_globalObject(globalObject)
        , m_rt(runtime(globalObject))
        , m_vm(m_rt.vm())
        , m_pickler(pickler)
        , m_state(pickleState(globalObject))
        , m_proto(static_cast<int>(pickler->number & 0xff))
        , m_fixImports(pickler->number & PicklerFixImports)
        , m_write(pickler->get(PicklerWrite))
        , m_dispatchTable(pickler->get(PicklerDispatchTable))
        , m_bufferCallback(pickler->get(PicklerBufferCallback))
    {
        m_bin = pickler->get(PicklerBin).asNumber();
        m_fast = pickler->get(PicklerFast).asNumber();
    }

    // False: an exception.
    bool dump(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool isPlain = JSValue(m_pickler->type()) == m_state->get(PickleStatePicklerType);
        if (!isPlain || m_pickler->get(PicklerPersistentIdAttr)) {
            m_persistentId = getAttr(m_globalObject, m_pickler, internString(m_vm, "persistent_id"_s));
            RETURN_IF_EXCEPTION(scope, false);
            if (picklerIsDefaultPersistentId(m_persistentId, m_pickler))
                m_persistentId = { };
        }
        if (!isPlain) {
            m_reducerOverride = getAttrIfPresent(m_globalObject, m_pickler, internString(m_vm, "reducer_override"_s));
            RETURN_IF_EXCEPTION(scope, false);
        }

        if (m_proto >= 2) {
            writeOp(PickleOpProto);
            writeOp(static_cast<uint8_t>(m_proto));
            if (m_proto >= 4)
                m_framing = true;
        }
        save(object);
        RETURN_IF_EXCEPTION(scope, false);
        writeOp(PickleOpStop);
        commitFrame();
        m_framing = false;
        return true;
    }

    // Nothing, with a MemoryError: there was no room for them.
    JSValue takeBytes()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        commitFrame();
        JSValue result = makeBytes(m_globalObject, m_out);
        RETURN_IF_EXCEPTION(scope, { });
        m_out.shrink(0);
        m_frameStart = notFound;
        return result;
    }

    bool flushToFile()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue output = takeBytes();
        RETURN_IF_EXCEPTION(scope, false);
        call(m_globalObject, m_write, output);
        return !scope.exception();
    }

private:
    // ---- Output ----

    ALWAYS_INLINE void startFrameIfNeeded()
    {
        if (m_framing && m_frameStart == notFound) [[unlikely]] {
            m_frameStart = m_out.size();
            m_out.appendUninitialized(pickleFrameHeaderSize);
        }
    }

    ALWAYS_INLINE void writeOp(uint8_t op)
    {
        startFrameIfNeeded();
        m_out.append(op);
    }

    void write(std::span<const uint8_t> data)
    {
        startFrameIfNeeded();
        m_out.append(data);
    }

    void write(ASCIILiteral text) { write(byteCast<uint8_t>(text.span())); }

    void write(const String& ascii)
    {
        ASSERT(ascii.containsOnlyASCII());
        if (ascii.is8Bit()) {
            write(byteCast<uint8_t>(ascii.span8()));
            return;
        }
        startFrameIfNeeded();
        for (char16_t character : ascii.span16())
            m_out.append(static_cast<uint8_t>(character));
    }

    // Little-endian.
    void writeSize(uint64_t value, unsigned bytes)
    {
        startFrameIfNeeded();
        for (unsigned i = 0; i < bytes; ++i)
            m_out.append(static_cast<uint8_t>(value >> (8 * i)));
    }

    void commitFrame()
    {
        if (!m_framing || m_frameStart == notFound || m_out.hasOverflowed())
            return;
        size_t frameLength = m_out.size() - m_frameStart - pickleFrameHeaderSize;
        uint8_t* header = m_out.mutableSpan().data() + m_frameStart;
        if (frameLength >= pickleFrameSizeMin) {
            header[0] = PickleOpFrame;
            for (unsigned i = 0; i < 8; ++i)
                header[1 + i] = static_cast<uint8_t>(static_cast<uint64_t>(frameLength) >> (8 * i));
        } else {
            memmove(header, header + pickleFrameHeaderSize, frameLength);
            m_out.shrink(m_out.size() - pickleFrameHeaderSize);
        }
        m_frameStart = notFound;
    }

    // Between two opcodes: a frame that is long enough ends here, and goes to the file if there is one.
    ALWAYS_INLINE bool opcodeBoundary()
    {
        if (!m_framing || m_frameStart == notFound || m_out.size() - m_frameStart - pickleFrameHeaderSize < pickleFrameSizeTarget) [[likely]]
            return true;
        if (m_out.hasOverflowed()) [[unlikely]]
            return flushToFile();
        commitFrame();
        return !m_write || flushToFile();
    }

    // The header and the data of a bytes-like thing or a str. What is large is in no frame, and goes straight to the file.
    // `payload` is an object with the same data, if there is one.
    bool writeBytes(std::span<const uint8_t> header, std::span<const uint8_t> data, JSValue payload)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool bypassesBuffer = data.size() >= pickleFrameSizeTarget;
        bool framing = m_framing;
        if (bypassesBuffer) {
            commitFrame();
            m_framing = false;
        }
        write(header);
        if (bypassesBuffer && m_write) {
            if (!payload)
                payload = PyBytes::create(m_globalObject, data);
            flushToFile();
            RETURN_IF_EXCEPTION(scope, false);
            call(m_globalObject, m_write, payload);
            RETURN_IF_EXCEPTION(scope, false);
        } else
            write(data);
        m_framing = framing;
        return true;
    }

    // ---- The memo ----

    JSWeakMap* memoMap() const { return uncheckedDowncast<JSWeakMap>(m_pickler->get(PicklerMemoMap).asCell()); }

    // Empty: it is not there.
    ALWAYS_INLINE JSValue memoLookup(JSValue object) const
    {
        JSValue index = memoMap()->get(object.asCell());
        return index.isUndefined() ? JSValue() : index;
    }

    bool memoGet(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue found = memoLookup(object);
        if (!found) {
            raiseKeyError(m_globalObject, scope, object);
            return false;
        }
        double index = found.asNumber();
        if (!m_bin) {
            writeOp(PickleOpGet);
            write(makeString(static_cast<int64_t>(index), '\n'));
        } else if (index < 256) {
            writeOp(PickleOpBinGet);
            writeOp(static_cast<uint8_t>(index));
        } else if (index <= 0xffffffffU) {
            writeOp(PickleOpLongBinGet);
            writeSize(static_cast<uint64_t>(index), 4);
        } else
            RELEASE_AND_RETURN(scope, pickleRaise(m_globalObject, PickleStatePicklingError, "memo id too large for LONG_BINGET"_s));
        return true;
    }

    bool memoPut(JSValue object)
    {
        if (m_fast)
            return true;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned index = static_cast<unsigned>(m_pickler->get(PicklerMemoCount).asNumber());
        picklerMemoSet(m_globalObject, m_pickler, object.asCell(), index);
        RETURN_IF_EXCEPTION(scope, false);
        if (m_proto >= 4)
            writeOp(PickleOpMemoize);
        else if (!m_bin) {
            writeOp(PickleOpPut);
            write(makeString(index, '\n'));
        } else if (index < 256) {
            writeOp(PickleOpBinPut);
            writeOp(static_cast<uint8_t>(index));
        } else {
            writeOp(PickleOpLongBinPut);
            writeSize(index, 4);
        }
        return true;
    }

    // ---- In fast mode there is no memo, so a cycle is noticed otherwise, once things are nested deeply ----

    bool fastSaveEnter(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (++m_fastNesting < pickleFastNestingLimit)
            return true;
        if (!m_fastMemo)
            m_fastMemo = PyDict::create(m_globalObject);
        JSValue key = identityOf(m_globalObject, object);
        bool isThere = m_fastMemo->contains(m_globalObject, key);
        RETURN_IF_EXCEPTION(scope, false);
        if (isThere) {
            String address = addressForRepr(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(m_globalObject, scope, "fast mode: can't pickle cyclic objects including object type "_s, typeName(m_globalObject, object), " at "_s, address);
            m_fastNesting = -1;
            return false;
        }
        m_fastMemo->set(m_globalObject, key, jsUndefined());
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    bool fastSaveLeave(JSValue object)
    {
        if (m_fastNesting-- >= pickleFastNestingLimit)
            m_fastMemo->remove(m_globalObject, identityOf(m_globalObject, object));
        return true;
    }

    // ---- Notes ----

    NEVER_INLINE bool noteItem(JSValue container, uint64_t index)
    {
        pickleAddNote(m_globalObject, [&] {
            return makeString("when serializing "_s, pickleTypeName(m_globalObject, container), " item "_s, index);
        });
        return false;
    }

    NEVER_INLINE bool noteKey(JSValue container, JSValue key)
    {
        pickleAddNote(m_globalObject, [&]() -> String {
            String keyRepr = pickleRepr(m_globalObject, key);
            if (keyRepr.isNull())
                return String();
            return makeString("when serializing "_s, pickleTypeName(m_globalObject, container), " item "_s, keyRepr);
        });
        return false;
    }

    NEVER_INLINE bool note(JSValue object, ASCIILiteral what)
    {
        pickleAddNote(m_globalObject, [&] {
            return makeString("when serializing "_s, pickleTypeName(m_globalObject, object), ' ', what);
        });
        return false;
    }

    // ---- The types with opcodes of their own ----

    bool saveInt(JSValue value)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto small = tryInt64(value);
        if (small && *small >= INT32_MIN && *small <= INT32_MAX) {
            int64_t number = *small;
            if (!m_bin) {
                writeOp(PickleOpInt);
                write(makeString(number, '\n'));
            } else if (number < 0 || number > 0xffff) {
                writeOp(PickleOpBinInt);
                writeSize(static_cast<uint32_t>(number), 4);
            } else if (number > 0xff) {
                writeOp(PickleOpBinInt2);
                writeSize(number, 2);
            } else {
                writeOp(PickleOpBinInt1);
                writeOp(static_cast<uint8_t>(number));
            }
            return true;
        }

        if (m_proto < 2) {
            String text = intToString(m_globalObject, value);
            RETURN_IF_EXCEPTION(scope, false);
            writeOp(PickleOpLong);
            write(text);
            write("L\n"_s);
            return true;
        }

        // In two's complement, the least significant byte first, with room for the sign.
        uint64_t byteCount = (intBitLength(value) >> 3) + 1;
        if (byteCount > 0x7fffffff) {
            raiseOverflowError(m_globalObject, scope, "int too large to pickle"_s);
            return false;
        }
        bool isNegative = intSign(value) < 0;
        Vector<uint8_t, 32> bytes(byteCount);
        JSBigInt* magnitude = toBigInt(m_globalObject, value);
        RETURN_IF_EXCEPTION(scope, false);
        unsigned carry = 1;
        for (uint64_t i = 0; i < byteCount; ++i) {
            uint8_t byte = i / 8 < magnitude->length() ? static_cast<uint8_t>(magnitude->digit(i / 8) >> ((i % 8) * 8)) : 0;
            if (isNegative) {
                unsigned inverted = static_cast<uint8_t>(~byte) + carry;
                byte = static_cast<uint8_t>(inverted);
                carry = inverted >> 8;
            }
            bytes[i] = byte;
        }
        // -128 and the like need no byte for the sign alone.
        if (isNegative && byteCount > 1 && bytes[byteCount - 1] == 0xff && (bytes[byteCount - 2] & 0x80))
            --byteCount;
        if (byteCount < 256) {
            writeOp(PickleOpLong1);
            writeOp(static_cast<uint8_t>(byteCount));
        } else {
            writeOp(PickleOpLong4);
            writeSize(byteCount, 4);
        }
        write(bytes.span().first(byteCount));
        return true;
    }

    bool saveFloat(double value)
    {
        if (m_bin) {
            writeOp(PickleOpBinFloat);
            uint64_t bits = std::bit_cast<uint64_t>(value);
            for (int shift = 56; shift >= 0; shift -= 8)
                m_out.append(static_cast<uint8_t>(bits >> shift));
        } else {
            writeOp(PickleOpFloat);
            write(floatRepr(value));
            writeOp('\n');
        }
        return true;
    }

    bool saveBytesData(JSValue object, std::span<const uint8_t> data)
    {
        ASSERT(m_proto >= 3);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint8_t header[9];
        size_t headerSize;
        uint64_t size = data.size();
        if (size <= 0xff) {
            header[0] = PickleOpShortBinBytes;
            headerSize = 2;
        } else if (size <= 0xffffffffU) {
            header[0] = PickleOpBinBytes;
            headerSize = 5;
        } else if (m_proto >= 4) {
            header[0] = PickleOpBinBytes8;
            headerSize = 9;
        } else {
            raiseOverflowError(m_globalObject, scope, "serializing a bytes object larger than 4 GiB requires pickle protocol 4 or higher"_s);
            return false;
        }
        for (size_t i = 1; i < headerSize; ++i)
            header[i] = static_cast<uint8_t>(size >> (8 * (i - 1)));
        if (failed(scope, writeBytes({ header, headerSize }, data, object)))
            return false;
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    bool saveBytes(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto* bytes = PyBytes::from(object);
        if (m_proto >= 3)
            RELEASE_AND_RETURN(scope, saveBytesData(object, bytes->span()));
        // There is no opcode. What this does instead makes a str in Python 2, which is what its bytes are.
        PyTuple* reduced;
        if (!bytes->length())
            reduced = PyTuple::create(m_globalObject, { m_rt.bytesType(), PyTuple::create(m_globalObject, { }) });
        else {
            JSValue text = codecDecodeLatin1(m_globalObject, bytes->span());
            RETURN_IF_EXCEPTION(scope, false);
            reduced = PyTuple::create(m_globalObject, { m_state->get(PickleStateCodecsEncode), PyTuple::create(m_globalObject, { text, internString(m_vm, "latin1"_s) }) });
        }
        RELEASE_AND_RETURN(scope, saveReduce(reduced, object));
    }

    bool saveByteArrayData(JSValue object, std::span<const uint8_t> data)
    {
        ASSERT(m_proto >= 5);
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint8_t header[9];
        header[0] = PickleOpByteArray8;
        for (size_t i = 0; i < 8; ++i)
            header[1 + i] = static_cast<uint8_t>(static_cast<uint64_t>(data.size()) >> (8 * i));
        if (failed(scope, writeBytes({ header, 9 }, data, object)))
            return false;
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    bool saveByteArray(JSValue object)
    {
        auto* array = PyByteArray::from(object);
        if (m_proto >= 5)
            return saveByteArrayData(object, array->span());
        PyTuple* arguments = array->length() ? PyTuple::create(m_globalObject, { PyBytes::create(m_globalObject, array->span()) }) : PyTuple::create(m_globalObject, { });
        return saveReduce(PyTuple::create(m_globalObject, { m_rt.bytearrayType(), arguments }), object);
    }

    static bool isContiguous(const Buffer& buffer)
    {
        if (buffer.strides.isEmpty() || !buffer.length)
            return true;
        auto check = [&](bool lastVariesFastest) {
            int64_t expected = buffer.itemSize;
            for (unsigned i = 0; i < buffer.dimensions; ++i) {
                unsigned dimension = lastVariesFastest ? buffer.dimensions - 1 - i : i;
                if (buffer.shape[dimension] > 1 && buffer.strides[dimension] != expected)
                    return false;
                expected *= buffer.shape[dimension];
            }
            return true;
        };
        return check(true) || check(false);
    }

    bool savePickleBuffer(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_proto < 5)
            RELEASE_AND_RETURN(scope, pickleRaise(m_globalObject, PickleStatePicklingError, "PickleBuffer can only be pickled with protocol >= 5"_s));
        BufferHolder holder(m_globalObject);
        if (failed(scope, holder.get(object, BufferFullRO)))
            return false;
        if (!isContiguous(holder.buffer))
            RELEASE_AND_RETURN(scope, pickleRaise(m_globalObject, PickleStatePicklingError, "PickleBuffer can not be pickled when pointing to a non-contiguous buffer"_s));
        bool isInBand = true;
        if (m_bufferCallback) {
            JSValue result = call(m_globalObject, m_bufferCallback, object);
            RETURN_IF_EXCEPTION(scope, false);
            isInBand = isTrue(m_globalObject, result);
            RETURN_IF_EXCEPTION(scope, false);
            if (failed(scope, refreshBuffer(m_globalObject, holder.buffer)))
                return false;
        }
        if (isInBand)
            RELEASE_AND_RETURN(scope, holder.buffer.isReadOnly ? saveBytesData(object, holder.buffer.span()) : saveByteArrayData(object, holder.buffer.span()));
        writeOp(PickleOpNextBuffer);
        if (holder.buffer.isReadOnly)
            writeOp(PickleOpReadonlyBuffer);
        return true;
    }

    bool saveStr(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String text = asString(object)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!m_bin) {
            // raw-unicode-escape, but for the characters that would end the line or the file.
            writeOp(PickleOpUnicode);
            static constexpr char digits[] = "0123456789abcdef";
            StringView view(text);
            for (unsigned i = 0; i < view.length(); ++i) {
                char32_t c = view[i];
                if (U16_IS_LEAD(c) && i + 1 < view.length() && U16_IS_TRAIL(view[i + 1]))
                    c = U16_GET_SUPPLEMENTARY(c, view[++i]);
                if (c >= 0x10000) {
                    write("\\U"_s);
                    for (int shift = 28; shift >= 0; shift -= 4)
                        m_out.append(static_cast<uint8_t>(digits[(c >> shift) & 15]));
                } else if (c >= 256 || c == '\\' || !c || c == '\n' || c == '\r' || c == 0x1a) {
                    write("\\u"_s);
                    for (int shift = 12; shift >= 0; shift -= 4)
                        m_out.append(static_cast<uint8_t>(digits[(c >> shift) & 15]));
                } else
                    m_out.append(static_cast<uint8_t>(c));
            }
            writeOp('\n');
            RELEASE_AND_RETURN(scope, memoPut(object));
        }

        std::span<const uint8_t> data;
        ByteBuilder encoded;
        if (text.is8Bit() && text.containsOnlyASCII())
            data = byteCast<uint8_t>(text.span8());
        else {
            appendUTF8(encoded, text, true);
            if (encoded.hasOverflowed()) [[unlikely]] {
                raiseMemoryError(m_globalObject, scope, String());
                return false;
            }
            data = encoded.span();
        }
        uint8_t header[9];
        size_t headerSize;
        uint64_t size = data.size();
        if (size <= 0xff && m_proto >= 4) {
            header[0] = PickleOpShortBinUnicode;
            headerSize = 2;
        } else if (size <= 0xffffffffU) {
            header[0] = PickleOpBinUnicode;
            headerSize = 5;
        } else if (m_proto >= 4) {
            header[0] = PickleOpBinUnicode8;
            headerSize = 9;
        } else {
            raiseOverflowError(m_globalObject, scope, "serializing a string larger than 4 GiB requires pickle protocol 4 or higher"_s);
            return false;
        }
        for (size_t i = 1; i < headerSize; ++i)
            header[i] = static_cast<uint8_t>(size >> (8 * (i - 1)));
        if (failed(scope, writeBytes({ header, headerSize }, data, { })))
            return false;
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    bool saveTuple(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto* tuple = PyTuple::from(object);
        unsigned length = tuple->length();
        if (!length) {
            if (m_proto)
                writeOp(PickleOpEmptyTuple);
            else {
                writeOp(PickleOpMark);
                writeOp(PickleOpTuple);
            }
            return true;
        }

        // If the tuple is in the memo once its items are saved, one of them refers to it, and saving that one has made the tuple.
        bool isShort = length <= 3 && m_proto >= 2;
        if (!isShort)
            writeOp(PickleOpMark);
        for (unsigned i = 0; i < length; ++i) {
            if (failed(scope, save(tuple->at(i))))
                RELEASE_AND_RETURN(scope, noteItem(object, i));
        }
        if (memoLookup(object)) {
            if (!isShort && m_bin)
                writeOp(PickleOpPopMark);
            else {
                for (unsigned i = 0; i < length + !isShort; ++i)
                    writeOp(PickleOpPop);
            }
            RELEASE_AND_RETURN(scope, memoGet(object));
        }
        static constexpr uint8_t opcodes[] = { PickleOpEmptyTuple, PickleOpTuple1, PickleOpTuple2, PickleOpTuple3 };
        writeOp(isShort ? opcodes[length] : PickleOpTuple);
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // The items an iterator gives, appended in batches to what is on the stack.
    bool batchList(JSValue iterator, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint64_t total = 0;
        if (!m_proto) {
            for (;; ++total) {
                JSValue item = iterNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, false);
                if (!item)
                    return true;
                if (failed(scope, save(item)))
                    RELEASE_AND_RETURN(scope, noteItem(original, total));
                writeOp(PickleOpAppend);
            }
        }
        unsigned count;
        do {
            JSValue first = iterNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, false);
            if (!first)
                break;
            JSValue item = iterNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, false);
            if (!item) {
                // Only one is left.
                if (failed(scope, save(first)))
                    RELEASE_AND_RETURN(scope, noteItem(original, total));
                writeOp(PickleOpAppend);
                break;
            }
            writeOp(PickleOpMark);
            if (failed(scope, save(first)))
                RELEASE_AND_RETURN(scope, noteItem(original, total));
            ++total;
            count = 1;
            while (item) {
                if (failed(scope, save(item)))
                    RELEASE_AND_RETURN(scope, noteItem(original, total));
                ++total;
                if (++count == pickleBatchSize)
                    break;
                item = iterNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, false);
            }
            writeOp(PickleOpAppends);
        } while (count == pickleBatchSize);
        return true;
    }

    bool batchListExact(JSArray* list)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned size = listLength(list);
        unsigned total = 0;
        do {
            if (size - total == 1) {
                if (total >= listLength(list)) {
                    raiseIndexError(m_globalObject, scope, "list index out of range"_s);
                    RELEASE_AND_RETURN(scope, noteItem(list, total));
                }
                if (failed(scope, save(listGet(list, total))))
                    RELEASE_AND_RETURN(scope, noteItem(list, total));
                writeOp(PickleOpAppend);
                return true;
            }
            unsigned count = 0;
            writeOp(PickleOpMark);
            while (total < listLength(list)) {
                if (failed(scope, save(listGet(list, total))))
                    RELEASE_AND_RETURN(scope, noteItem(list, total));
                ++total;
                if (++count == pickleBatchSize)
                    break;
            }
            writeOp(PickleOpAppends);
            if (listLength(list) != size) {
                raiseRuntimeError(m_globalObject, scope, "list changed size during iteration"_s);
                return false;
            }
        } while (total < size);
        return true;
    }

    bool saveList(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* list = asList(object);
        if (m_fast && failed(scope, fastSaveEnter(object)))
            return false;
        if (m_bin)
            writeOp(PickleOpEmptyList);
        else {
            writeOp(PickleOpMark);
            writeOp(PickleOpList);
        }
        bool ok = !failed(scope, memoPut(object));
        if (ok && listLength(list)) {
            if (m_proto > 0)
                ok = !failed(scope, batchListExact(list));
            else {
                JSValue iterator = getIter(m_globalObject, object);
                ok = !scope.exception() && !failed(scope, batchList(iterator, object));
            }
        }
        scope.release();
        if (m_fast)
            fastSaveLeave(object);
        return ok;
    }

    bool saveItem(JSValue item, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto* pair = PyTuple::tryFrom(item);
        if (!pair || pair->length() != 2) {
            raiseTypeError(m_globalObject, scope, "dict items iterator must return 2-tuples"_s);
            return false;
        }
        if (failed(scope, save(pair->at(0))))
            return false;
        if (failed(scope, save(pair->at(1))))
            RELEASE_AND_RETURN(scope, noteKey(original, pair->at(0)));
        return true;
    }

    // The (key, value) pairs an iterator gives, set in batches in what is on the stack.
    bool batchDict(JSValue iterator, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_proto) {
            while (true) {
                JSValue item = iterNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, false);
                if (!item)
                    return true;
                if (failed(scope, saveItem(item, original)))
                    return false;
                writeOp(PickleOpSetItem);
            }
        }
        unsigned count;
        do {
            JSValue first = iterNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, false);
            if (!first)
                break;
            if (auto* pair = PyTuple::tryFrom(first); !pair || pair->length() != 2) {
                raiseTypeError(m_globalObject, scope, "dict items iterator must return 2-tuples"_s);
                return false;
            }
            JSValue item = iterNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, false);
            if (!item) {
                if (failed(scope, saveItem(first, original)))
                    return false;
                writeOp(PickleOpSetItem);
                break;
            }
            writeOp(PickleOpMark);
            if (failed(scope, saveItem(first, original)))
                return false;
            count = 1;
            while (item) {
                if (failed(scope, saveItem(item, original)))
                    return false;
                if (++count == pickleBatchSize)
                    break;
                item = iterNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, false);
            }
            writeOp(PickleOpSetItems);
        } while (count == pickleBatchSize);
        return true;
    }

    bool batchDictExact(PyDict* dict)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned size = dict->size();
        unsigned total = 0;
        unsigned position = 0;
        JSValue key, value;
        do {
            if (size - total == 1) {
                if (!dict->next(position, key, value))
                    return true;
                if (failed(scope, save(key)))
                    return false;
                if (failed(scope, save(value)))
                    RELEASE_AND_RETURN(scope, noteKey(dict, key));
                writeOp(PickleOpSetItem);
                return true;
            }
            unsigned count = 0;
            writeOp(PickleOpMark);
            while (dict->next(position, key, value)) {
                if (failed(scope, save(key)))
                    return false;
                if (failed(scope, save(value)))
                    RELEASE_AND_RETURN(scope, noteKey(dict, key));
                ++total;
                if (++count == pickleBatchSize)
                    break;
            }
            writeOp(PickleOpSetItems);
            if (dict->size() != size) {
                raiseRuntimeError(m_globalObject, scope, "dictionary changed size during iteration"_s);
                return false;
            }
        } while (total < size);
        return true;
    }

    bool saveDict(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto* dict = PyDict::from(object);
        if (m_fast && failed(scope, fastSaveEnter(object)))
            return false;
        if (m_bin)
            writeOp(PickleOpEmptyDict);
        else {
            writeOp(PickleOpMark);
            writeOp(PickleOpDict);
        }
        bool ok = !failed(scope, memoPut(object));
        if (ok && dict->size()) {
            if (m_proto > 0)
                ok = !failed(scope, batchDictExact(dict));
            else {
                JSValue items = callMethod(m_globalObject, object, m_rt.names.items());
                JSValue iterator = scope.exception() ? JSValue() : getIter(m_globalObject, items);
                ok = !scope.exception() && !failed(scope, batchDict(iterator, object));
            }
        }
        scope.release();
        if (m_fast)
            fastSaveLeave(object);
        return ok;
    }

    // type(list(object))
    bool saveSetAsCall(JSValue object, PyType* type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSArray* items = listFromIterable(m_globalObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, saveReduce(PyTuple::create(m_globalObject, { type, PyTuple::create(m_globalObject, { items }) }), object));
    }

    bool saveSet(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_proto < 4)
            RELEASE_AND_RETURN(scope, saveSetAsCall(object, m_rt.setType()));
        auto* set = PySet::from(object);
        writeOp(PickleOpEmptySet);
        if (failed(scope, memoPut(object)))
            return false;
        size_t size = set->size();
        if (!size)
            return true;
        size_t total = 0;
        size_t position = 0;
        do {
            unsigned count = 0;
            writeOp(PickleOpMark);
            JSValue item;
            while (set->next(position, item)) {
                if (failed(scope, save(item)))
                    RELEASE_AND_RETURN(scope, note(object, "element"_s));
                ++total;
                if (++count == pickleBatchSize)
                    break;
            }
            writeOp(PickleOpAddItems);
            if (set->size() != size) {
                raiseRuntimeError(m_globalObject, scope, "set changed size during iteration"_s);
                return false;
            }
        } while (total < size);
        return true;
    }

    bool saveFrozenSet(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_fast && failed(scope, fastSaveEnter(object)))
            return false;
        auto run = [&] {
            if (m_proto < 4)
                return saveSetAsCall(object, m_rt.frozensetType());
            writeOp(PickleOpMark);
            auto* set = PySet::from(object);
            JSValue item;
            for (size_t position = 0; set->next(position, item);) {
                if (failed(scope, save(item)))
                    return note(object, "element"_s);
            }
            // As for a tuple.
            if (memoLookup(object)) {
                writeOp(PickleOpPopMark);
                return memoGet(object);
            }
            writeOp(PickleOpFrozenSet);
            return memoPut(object);
        };
        bool ok = !failed(scope, run());
        scope.release();
        if (m_fast)
            fastSaveLeave(object);
        return ok;
    }

    // ---- What is pickled by its name ----

    static bool isPrivateName(const String& name) { return name.startsWith("__"_s) && !name.endsWith("__"_s) && !name.contains('.'); }

    // True if anything changed.
    static bool mangleDottedPath(Vector<String>& path)
    {
        bool changed = false;
        for (size_t i = path.size() - 1; i > 0; --i) {
            if (!isPrivateName(path[i]))
                continue;
            StringView parent = path[i - 1];
            unsigned underscores = 0;
            while (underscores < parent.length() && parent[underscores] == '_')
                ++underscores;
            if (underscores == parent.length())
                continue;
            path[i] = makeString('_', parent.substring(underscores), path[i]);
            changed = true;
        }
        return changed;
    }

    // 0: the module has the object at the path. -1: it has not, or there is an exception.
    int checkModule(JSValue moduleName, JSValue module, JSValue global, const Vector<String>& path)
    {
        if (isNone(module))
            return -1;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isStr(moduleName)) {
            String name = strValue(moduleName)->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, -1);
            if (name == "__main__"_s || name == "__mp_main__"_s)
                return -1;
        }
        JSValue candidate = pickleGetAttribute(m_globalObject, module, path, false);
        RETURN_IF_EXCEPTION(scope, -1);
        return candidate && candidate == global ? 0 : -1;
    }

    // The name of the module the object is to be found in at the path, having checked that it is. Empty: an exception.
    JSValue whichModule(JSValue global, JSValue globalName, const Vector<String>& path)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyType* picklingError = PyType::from(m_state->get(PickleStatePicklingError));
        for (const String& part : path) {
            if (part == "<locals>"_s) {
                String text = pickleRepr(m_globalObject, global);
                RETURN_IF_EXCEPTION(scope, { });
                scope.release();
                pickleRaise(m_globalObject, PickleStatePicklingError, "Can't pickle local object "_s, text);
                return { };
            }
        }
        JSValue moduleName = getAttrIfPresent(m_globalObject, global, m_rt.names.dModule());
        RETURN_IF_EXCEPTION(scope, { });
        if (!moduleName || isNone(moduleName)) {
            // It does not say. Then the first module that has it.
            moduleName = { };
            JSArray* names = listFromIterable(m_globalObject, m_rt.modules());
            RETURN_IF_EXCEPTION(scope, { });
            for (unsigned i = 0; i < listLength(names) && !moduleName; ++i) {
                JSValue module = m_rt.modules()->get(m_globalObject, listGet(names, i));
                RETURN_IF_EXCEPTION(scope, { });
                if (!module)
                    continue;
                int result = checkModule(listGet(names, i), module, global, path);
                RETURN_IF_EXCEPTION(scope, { });
                if (!result)
                    return listGet(names, i);
            }
            moduleName = m_rt.names.dMain();
        }

        JSValue module = pickleImport(m_globalObject, moduleName);
        if (scope.exception()) {
            if (pendingExceptionMatches(m_globalObject, m_rt.ImportError()) || pendingExceptionMatches(m_globalObject, m_rt.ValueError())) {
                scope.release();
                pickleReplaceException(m_globalObject, picklingError, [&](JSValue exception) -> String {
                    String text = pickleRepr(m_globalObject, global);
                    if (failed(scope, !text.isNull()))
                        return String();
                    String reason = pickleStr(m_globalObject, exception);
                    if (failed(scope, !reason.isNull()))
                        return String();
                    return makeString("Can't pickle "_s, text, ": "_s, reason);
                });
            }
            return { };
        }
        auto describe = [&](ASCIILiteral problem) -> String {
            String text = pickleRepr(m_globalObject, global);
            if (failed(scope, !text.isNull()))
                return String();
            String moduleText = pickleStr(m_globalObject, moduleName);
            if (failed(scope, !moduleText.isNull()))
                return String();
            String nameText = pickleStr(m_globalObject, globalName);
            if (failed(scope, !nameText.isNull()))
                return String();
            return makeString("Can't pickle "_s, text, problem, moduleText, '.', nameText);
        };
        JSValue actual = pickleGetAttribute(m_globalObject, module, path, true);
        if (scope.exception()) {
            if (pendingExceptionMatches(m_globalObject, m_rt.AttributeError())) {
                scope.release();
                pickleReplaceException(m_globalObject, picklingError, [&](JSValue) { return describe(": it's not found as "_s); });
            }
            return { };
        }
        if (actual != global) {
            String message = describe(": it's not the same object as "_s);
            RETURN_IF_EXCEPTION(scope, { });
            raise(m_globalObject, scope, picklingError, message);
            return { };
        }
        return moduleName;
    }

    // For protocols below 3: the names Python 2 knows things by.
    bool fixImports(JSValue& moduleName, JSValue& globalName)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue item = PyDict::from(m_state->get(PickleStateNameMapping3to2))->get(m_globalObject, PyTuple::create(m_globalObject, { moduleName, globalName }));
        RETURN_IF_EXCEPTION(scope, false);
        if (item) {
            auto* pair = PyTuple::tryFrom(item);
            if (!pair || pair->length() != 2) {
                raiseRuntimeError(m_globalObject, scope, "_compat_pickle.REVERSE_NAME_MAPPING values should be 2-tuples, not "_s, typeName(m_globalObject, item));
                return false;
            }
            if (!isStr(pair->at(0)) || !isStr(pair->at(1))) {
                raiseRuntimeError(m_globalObject, scope, "_compat_pickle.REVERSE_NAME_MAPPING values should be pairs of str, not ("_s, typeName(m_globalObject, pair->at(0)), ", "_s, typeName(m_globalObject, pair->at(1)), ')');
                return false;
            }
            moduleName = pair->at(0);
            globalName = pair->at(1);
            return true;
        }
        item = PyDict::from(m_state->get(PickleStateImportMapping3to2))->get(m_globalObject, moduleName);
        RETURN_IF_EXCEPTION(scope, false);
        if (item) {
            if (!isStr(item)) {
                raiseRuntimeError(m_globalObject, scope, "_compat_pickle.REVERSE_IMPORT_MAPPING values should be strings, not "_s, typeName(m_globalObject, item));
                return false;
            }
            moduleName = item;
        }
        return true;
    }

    // An identifier and a newline, in UTF-8 for protocol 3 and in ASCII for those that Python 2 reads.
    bool writeIdentifier(JSValue name, ASCIILiteral what)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isStr(name)) {
            raiseTypeError(m_globalObject, scope, "bad argument type for built-in operation"_s);
            return false;
        }
        JSValue encoded = m_proto == 3 ? codecEncodeUTF8(m_globalObject, strValue(name), "strict"_s) : codecEncodeASCII(m_globalObject, strValue(name), "strict"_s);
        if (scope.exception()) {
            if (pendingExceptionMatches(m_globalObject, m_rt.UnicodeEncodeError())) {
                scope.release();
                pickleReplaceException(m_globalObject, PyType::from(m_state->get(PickleStatePicklingError)), [&](JSValue) -> String {
                    String text = pickleRepr(m_globalObject, name);
                    if (text.isNull())
                        return String();
                    return makeString("can't pickle "_s, what, " identifier "_s, text, " using pickle protocol "_s, m_proto);
                });
            }
            return false;
        }
        write(PyBytes::from(encoded)->span());
        writeOp('\n');
        return true;
    }

    // `name` is empty, or what __reduce__ returned in place of a tuple.
    bool saveGlobal(JSValue object, JSValue name)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue globalName = name;
        if (!globalName) {
            globalName = getAttrIfPresent(m_globalObject, object, m_rt.names.dQualname());
            RETURN_IF_EXCEPTION(scope, false);
            if (!globalName) {
                globalName = getAttr(m_globalObject, object, m_rt.names.dName());
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
        if (!isStr(globalName)) {
            raiseTypeError(m_globalObject, scope, "must be str, not "_s, typeName(m_globalObject, globalName));
            return false;
        }
        String nameText = strValue(globalName)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, false);
        Vector<String> path = pickleDottedPath(nameText);
        if (mangleDottedPath(path)) {
            StrBuilder joined;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i)
                    joined.append('.');
                joined.append(path[i]);
            }
            globalName = makeStr(m_vm, joined.toString());
        }
        JSValue moduleName = whichModule(object, globalName, path);
        RETURN_IF_EXCEPTION(scope, false);

        if (m_proto >= 2) {
            // It may have a code, which is shorter.
            JSValue codeObject = PyDict::from(m_state->get(PickleStateExtensionRegistry))->get(m_globalObject, PyTuple::create(m_globalObject, { moduleName, globalName }));
            RETURN_IF_EXCEPTION(scope, false);
            if (codeObject) {
                int64_t code = toInt64(m_globalObject, codeObject);
                RETURN_IF_EXCEPTION(scope, false);
                if (code <= 0 || code > 0x7fffffff) {
                    raiseRuntimeError(m_globalObject, scope, "extension code "_s, code, " is out of range"_s);
                    return false;
                }
                if (code <= 0xff) {
                    writeOp(PickleOpExt1);
                    writeSize(code, 1);
                } else if (code <= 0xffff) {
                    writeOp(PickleOpExt2);
                    writeSize(code, 2);
                } else {
                    writeOp(PickleOpExt4);
                    writeSize(code, 4);
                }
                return true;
            }
        }

        if (m_proto >= 4) {
            if (failed(scope, save(moduleName)) || failed(scope, save(globalName)))
                return false;
            writeOp(PickleOpStackGlobal);
            RELEASE_AND_RETURN(scope, memoPut(object));
        }

        // What is nested in a class is getattr(getattr(..., name1), name2).
        if (path.size() > 1)
            globalName = makeStr(m_vm, path[0]);
        for (size_t i = 1; i < path.size(); ++i) {
            if (failed(scope, save(m_state->get(PickleStateGetAttr))))
                return false;
            if (m_proto < 2)
                writeOp(PickleOpMark);
        }
        writeOp(PickleOpGlobal);
        if (m_proto < 3 && m_fixImports && failed(scope, fixImports(moduleName, globalName)))
            return false;
        if (failed(scope, writeIdentifier(moduleName, "module"_s)) || failed(scope, writeIdentifier(globalName, "global"_s)))
            return false;
        for (size_t i = 1; i < path.size(); ++i) {
            if (failed(scope, save(makeStr(m_vm, path[i]))))
                return false;
            writeOp(m_proto < 2 ? PickleOpTuple : PickleOpTuple2);
            writeOp(PickleOpReduce);
        }
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    bool saveType(JSValue object)
    {
        JSValue singleton;
        if (object == JSValue(m_rt.noneType()))
            singleton = jsUndefined();
        else if (object == JSValue(m_rt.ellipsisType()))
            singleton = m_rt.ellipsis();
        else if (object == JSValue(m_rt.notImplementedType()))
            singleton = m_rt.notImplemented();
        else
            return saveGlobal(object, { });
        return saveReduce(PyTuple::create(m_globalObject, { m_rt.typeType(), PyTuple::create(m_globalObject, { singleton }) }), object);
    }

    // 1: the object has a persistent id, which is saved. 0: it has none. -1: an exception.
    int savePersistent(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue id = call(m_globalObject, m_persistentId, object);
        RETURN_IF_EXCEPTION(scope, -1);
        if (isNone(id))
            return 0;
        if (m_bin) {
            if (failed(scope, save(id, true)))
                return -1;
            writeOp(PickleOpBinPersId);
            return 1;
        }
        String text = pickleStr(m_globalObject, id);
        RETURN_IF_EXCEPTION(scope, -1);
        if (!text.containsOnlyASCII()) {
            scope.release();
            pickleRaise(m_globalObject, PickleStatePicklingError, "persistent IDs in protocol 0 must be ASCII strings"_s);
            return -1;
        }
        writeOp(PickleOpPersId);
        write(text);
        writeOp('\n');
        return 1;
    }

    // ---- What says itself how it is to be made again ----

    bool isIterator(JSValue value) { return !!typeOf(m_globalObject, value)->lookup(Special::Next); }

    // `arguments` is what __reduce__ returned. `object` is what is being saved.
    bool saveReduce(PyTuple* arguments, JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto fail = [&](auto... parts) { return pickleRaise(m_globalObject, PickleStatePicklingError, parts...); };
        unsigned size = arguments->length();
        if (size < 2 || size > 6)
            RELEASE_AND_RETURN(scope, fail("tuple returned by __reduce__ must contain 2 through 6 elements"_s));
        auto optional = [&](unsigned index) {
            JSValue value = index < size ? arguments->at(index) : JSValue();
            return value && isNone(value) ? JSValue() : value;
        };
        JSValue callable = arguments->at(0);
        JSValue argumentsValue = arguments->at(1);
        JSValue state = optional(2);
        JSValue listItems = optional(3);
        JSValue dictItems = optional(4);
        JSValue stateSetter = optional(5);

        if (!isCallable(m_globalObject, callable))
            RELEASE_AND_RETURN(scope, fail("first item of the tuple returned by __reduce__ must be callable, not "_s, pickleTypeName(m_globalObject, callable)));
        auto* argumentTuple = PyTuple::tryFrom(argumentsValue);
        if (!argumentTuple)
            RELEASE_AND_RETURN(scope, fail("second item of the tuple returned by __reduce__ must be a tuple, not "_s, pickleTypeName(m_globalObject, argumentsValue)));
        if (listItems && !isIterator(listItems))
            RELEASE_AND_RETURN(scope, fail("fourth item of the tuple returned by __reduce__ must be an iterator, not "_s, pickleTypeName(m_globalObject, listItems)));
        if (dictItems && !isIterator(dictItems))
            RELEASE_AND_RETURN(scope, fail("fifth item of the tuple returned by __reduce__ must be an iterator, not "_s, pickleTypeName(m_globalObject, dictItems)));
        if (stateSetter && !isCallable(m_globalObject, stateSetter))
            RELEASE_AND_RETURN(scope, fail("sixth item of the tuple returned by __reduce__ must be callable, not "_s, pickleTypeName(m_globalObject, stateSetter)));

        bool usesNewObj = false;
        bool usesNewObjEx = false;
        if (m_proto >= 2) {
            JSValue name = getAttrIfPresent(m_globalObject, callable, m_rt.names.dName());
            RETURN_IF_EXCEPTION(scope, false);
            if (name && isStr(name)) {
                String text = strValue(name)->value(m_globalObject);
                RETURN_IF_EXCEPTION(scope, false);
                usesNewObjEx = text == "__newobj_ex__"_s;
                usesNewObj = text == "__newobj__"_s;
            }
        }

        if (usesNewObjEx) {
            if (argumentTuple->length() != 3)
                RELEASE_AND_RETURN(scope, fail("__newobj_ex__ expected 3 arguments, got "_s, argumentTuple->length()));
            JSValue cls = argumentTuple->at(0);
            if (!PyType::is(cls))
                RELEASE_AND_RETURN(scope, fail("first argument to __newobj_ex__() must be a class, not "_s, pickleTypeName(m_globalObject, cls)));
            auto* args = PyTuple::tryFrom(argumentTuple->at(1));
            if (!args)
                RELEASE_AND_RETURN(scope, fail("second argument to __newobj_ex__() must be a tuple, not "_s, pickleTypeName(m_globalObject, argumentTuple->at(1))));
            JSValue kwargs = argumentTuple->at(2);
            if (!isDict(kwargs))
                RELEASE_AND_RETURN(scope, fail("third argument to __newobj_ex__() must be a dict, not "_s, pickleTypeName(m_globalObject, kwargs)));
            if (m_proto >= 4) {
                if (failed(scope, save(cls)))
                    RELEASE_AND_RETURN(scope, note(object, "class"_s));
                if (failed(scope, save(args)) || failed(scope, save(kwargs)))
                    RELEASE_AND_RETURN(scope, note(object, "__new__ arguments"_s));
                writeOp(PickleOpNewObjEx);
            } else {
                // functools.partial(cls.__new__, cls, *args, **kwargs)()
                JSValue newFunction = getAttr(m_globalObject, cls, m_rt.name(Special::New));
                RETURN_IF_EXCEPTION(scope, false);
                MarkedArgumentBuffer partialArguments;
                partialArguments.append(newFunction);
                partialArguments.append(cls);
                for (unsigned i = 0; i < args->length(); ++i)
                    partialArguments.append(args->at(i));
                JSValue partial = callWithTupleAndDict(m_globalObject, m_state->get(PickleStatePartial), PyTuple::create(m_globalObject, spanOf(partialArguments)), PyDict::from(kwargs));
                RETURN_IF_EXCEPTION(scope, false);
                if (failed(scope, save(partial)) || failed(scope, save(PyTuple::create(m_globalObject, { }))))
                    RELEASE_AND_RETURN(scope, note(object, "reconstructor"_s));
                writeOp(PickleOpReduce);
            }
        } else if (usesNewObj) {
            if (!argumentTuple->length())
                RELEASE_AND_RETURN(scope, fail("__newobj__ expected at least 1 argument, got 0"_s));
            JSValue cls = argumentTuple->at(0);
            if (!PyType::is(cls))
                RELEASE_AND_RETURN(scope, fail("first argument to __newobj__() must be a class, not "_s, pickleTypeName(m_globalObject, cls)));
            if (object) {
                JSValue objectClass = getAttrIfPresent(m_globalObject, object, m_rt.names.dClass());
                RETURN_IF_EXCEPTION(scope, false);
                if (!objectClass)
                    objectClass = typeOf(m_globalObject, object);
                if (objectClass != cls) {
                    String expected = pickleRepr(m_globalObject, objectClass);
                    RETURN_IF_EXCEPTION(scope, false);
                    String actual = pickleRepr(m_globalObject, cls);
                    RETURN_IF_EXCEPTION(scope, false);
                    RELEASE_AND_RETURN(scope, fail("first argument to __newobj__() must be "_s, expected, ", not "_s, actual));
                }
            }
            if (failed(scope, save(cls)))
                RELEASE_AND_RETURN(scope, note(object, "class"_s));
            if (failed(scope, save(PyTuple::create(m_globalObject, argumentTuple->span().subspan(1)))))
                RELEASE_AND_RETURN(scope, note(object, "__new__ arguments"_s));
            writeOp(PickleOpNewObj);
        } else {
            if (failed(scope, save(callable)))
                RELEASE_AND_RETURN(scope, note(object, "reconstructor"_s));
            if (failed(scope, save(argumentTuple)))
                RELEASE_AND_RETURN(scope, note(object, "reconstructor arguments"_s));
            writeOp(PickleOpReduce);
        }

        if (object) {
            // As for a tuple: what went into making it may have referred to it.
            if (memoLookup(object)) {
                writeOp(PickleOpPop);
                RELEASE_AND_RETURN(scope, memoGet(object));
            }
            if (failed(scope, memoPut(object)))
                return false;
        }
        if (listItems && failed(scope, batchList(listItems, object)))
            return false;
        if (dictItems && failed(scope, batchDict(dictItems, object)))
            return false;
        if (state) {
            if (!stateSetter) {
                if (failed(scope, save(state)))
                    RELEASE_AND_RETURN(scope, note(object, "state"_s));
                writeOp(PickleOpBuild);
            } else {
                // state_setter(object, state), the result of which is dropped.
                if (failed(scope, save(stateSetter)))
                    RELEASE_AND_RETURN(scope, note(object, "state setter"_s));
                if (failed(scope, save(object)))
                    return false;
                if (failed(scope, save(state)))
                    RELEASE_AND_RETURN(scope, note(object, "state"_s));
                writeOp(PickleOpTuple2);
                writeOp(PickleOpReduce);
                writeOp(PickleOpPop);
            }
        }
        return true;
    }

    // `isPersistentId`: the object is the persistent id of another, and is not asked for one of its own.
    bool save(JSValue object, bool isPersistentId = false)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, opcodeBoundary()))
            return false;
        if (m_persistentId && !isPersistentId) {
            int status = savePersistent(object);
            EXCEPTION_ASSERT(!!scope.exception() == (status < 0));
            if (status)
                return status > 0;
        }

        if (isNone(object)) {
            writeOp(PickleOpNone);
            return true;
        }
        if (object.isBoolean()) {
            if (m_proto >= 2)
                writeOp(object.asBoolean() ? PickleOpNewTrue : PickleOpNewFalse);
            else
                write(object.asBoolean() ? "I01\n"_s : "I00\n"_s);
            return true;
        }
        if (isExactInt(object))
            RELEASE_AND_RETURN(scope, saveInt(object));
        if (object.isNumber())
            return saveFloat(object.asNumber());
        PyType* type = m_rt.typeOf(object);
        if (type == m_rt.floatType())
            return saveFloat(floatValue(object));

        if (memoLookup(object))
            RELEASE_AND_RETURN(scope, memoGet(object));

        if (type == m_rt.strType())
            RELEASE_AND_RETURN(scope, saveStr(object));
        if (type == m_rt.bytesType())
            RELEASE_AND_RETURN(scope, saveBytes(object));

        if (!m_vm.isSafeToRecurseSoft()) [[unlikely]] {
            raiseRecursionError(m_globalObject, scope, "maximum recursion depth exceeded while pickling an object"_s);
            return false;
        }

        if (type == m_rt.dictType())
            RELEASE_AND_RETURN(scope, saveDict(object));
        if (type == m_rt.setType())
            RELEASE_AND_RETURN(scope, saveSet(object));
        if (type == m_rt.frozensetType())
            RELEASE_AND_RETURN(scope, saveFrozenSet(object));
        if (type == m_rt.listType())
            RELEASE_AND_RETURN(scope, saveList(object));
        if (type == m_rt.tupleType())
            RELEASE_AND_RETURN(scope, saveTuple(object));
        if (type == m_rt.bytearrayType())
            RELEASE_AND_RETURN(scope, saveByteArray(object));
        if (type == m_rt.typeFor(pickleBufferTypeDef))
            RELEASE_AND_RETURN(scope, savePickleBuffer(object));

        JSValue reduced;
        if (m_reducerOverride) {
            reduced = call(m_globalObject, m_reducerOverride, object);
            RETURN_IF_EXCEPTION(scope, false);
            if (reduced == m_rt.notImplemented())
                reduced = { };
        }
        if (!reduced) {
            if (type == m_rt.typeType())
                RELEASE_AND_RETURN(scope, saveType(object));
            if (type == m_rt.functionType())
                RELEASE_AND_RETURN(scope, saveGlobal(object, { }));

            JSValue reduceFunction;
            if (!m_dispatchTable)
                reduceFunction = PyDict::from(m_state->get(PickleStateDispatchTable))->get(m_globalObject, type);
            else {
                reduceFunction = getItem(m_globalObject, m_dispatchTable, type);
                if (scope.exception() && clearPendingExceptionIfMatches(m_globalObject, m_rt.KeyError()))
                    reduceFunction = { };
            }
            RETURN_IF_EXCEPTION(scope, false);
            if (reduceFunction)
                reduced = call(m_globalObject, reduceFunction, object);
            else if (type->isSubtypeOf(m_rt.typeType()))
                RELEASE_AND_RETURN(scope, saveGlobal(object, { }));
            else {
                reduceFunction = getAttrIfPresent(m_globalObject, object, m_rt.names.dReduceEx());
                RETURN_IF_EXCEPTION(scope, false);
                if (reduceFunction)
                    reduced = call(m_globalObject, reduceFunction, jsNumber(m_proto));
                else {
                    reduceFunction = getAttrIfPresent(m_globalObject, object, m_rt.names.dReduce());
                    RETURN_IF_EXCEPTION(scope, false);
                    if (!reduceFunction)
                        RELEASE_AND_RETURN(scope, pickleRaise(m_globalObject, PickleStatePicklingError, "Can't pickle "_s, pickleTypeName(m_globalObject, object), " object"_s));
                    reduced = call(m_globalObject, reduceFunction);
                }
            }
            RETURN_IF_EXCEPTION(scope, false);
        }

        if (isStr(reduced))
            RELEASE_AND_RETURN(scope, saveGlobal(object, reduced));
        auto* tuple = PyTuple::tryFrom(reduced);
        if (!tuple) {
            pickleRaise(m_globalObject, PickleStatePicklingError, "__reduce__ must return a string or tuple, not "_s, pickleTypeName(m_globalObject, reduced));
            EXCEPTION_ASSERT(scope.exception());
        } else if (!failed(scope, saveReduce(tuple, object)))
            return true;
        RELEASE_AND_RETURN(scope, note(object, "object"_s));
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_rt;
    VM& m_vm;
    PyRecord* m_pickler;
    PyRecord* m_state;
    int m_proto;
    bool m_bin;
    bool m_fast;
    bool m_fixImports;
    bool m_framing { false };
    JSValue m_write;
    JSValue m_dispatchTable;
    JSValue m_bufferCallback;
    JSValue m_persistentId;
    JSValue m_reducerOverride;
    PyDict* m_fastMemo { nullptr };
    int m_fastNesting { 0 };
    ByteBuilderWithRoom<256> m_out;
    size_t m_frameStart { notFound };
};

// Marks a Pickler or an Unpickler as in use for as long as it lives.
class PickleRunningScope {
public:
    PickleRunningScope(PyRecord* record, int64_t bit)
        : m_record(record)
        , m_bit(bit)
    {
        m_record->number |= m_bit;
    }
    ~PickleRunningScope() { m_record->number &= ~m_bit; }

private:
    PyRecord* m_record;
    int64_t m_bit;
};

static JSValue picklerNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&) { return picklerCreate(globalObject, PyType::from(cls)); }

static JSValue picklerInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "file"_s, "protocol"_s, "fix_imports"_s, "buffer_callback"_s };
    JSValue values[4];
    if (failed(scope, unpackKeywordArguments(globalObject, "Pickler"_s, keywords, 1, 4, false, args, values)))
        return { };
    bool fixImports = !values[2] || isTrue(globalObject, values[2]);
    RETURN_IF_EXCEPTION(scope, { });

    auto* pickler = PyRecord::from(self);
    if (pickler->number & PicklerIsRunning)
        return raiseRuntimeError(globalObject, scope, "Pickler object is already used"_s);
    PickleRunningScope running(pickler, PicklerIsRunning);
    if (pickler->get(PicklerWrite)) {
        for (unsigned field : { PicklerWrite, PicklerPersistentIdAttr, PicklerDispatchTable, PicklerBufferCallback })
            pickler->set(vm, field, { });
        picklerClearMemo(globalObject, pickler);
    }
    if (failed(scope, picklerSetProtocol(globalObject, pickler, values[1], fixImports))
        || failed(scope, picklerSetOutputStream(globalObject, pickler, values[0]))
        || failed(scope, picklerSetBufferCallback(globalObject, pickler, values[3])))
        return { };
    pickler->set(vm, PicklerFast, jsNumber(0));
    if (!pickler->get(PicklerDispatchTable)) {
        // A class derived from Pickler may have one.
        JSValue table = genericGetAttr(globalObject, self, internString(vm, "dispatch_table"_s), true);
        if (scope.exception() && clearPendingExceptionIfMatches(globalObject, runtime(globalObject).AttributeError()))
            table = { };
        RETURN_IF_EXCEPTION(scope, { });
        pickler->set(vm, PicklerDispatchTable, table);
    }
    return jsUndefined();
}

static JSValue picklerDump(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, picklePositionalOnly(globalObject, "dump"_s, args, 1)))
        return { };
    JSValue object = args[0];
    auto* pickler = PyRecord::from(self);
    if (!pickler->get(PicklerWrite)) {
        scope.release();
        pickleRaise(globalObject, PickleStatePicklingError, "Pickler.__init__() was not called by "_s, typeName(globalObject, self), ".__init__()"_s);
        return { };
    }
    if (pickler->number & PicklerIsRunning)
        return raiseRuntimeError(globalObject, scope, "Pickler object is already used"_s);
    PickleRunningScope running(pickler, PicklerIsRunning);
    PicklerRun run(globalObject, pickler);
    run.dump(object);
    RETURN_IF_EXCEPTION(scope, { });
    run.flushToFile();
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue picklerClearMemoMethod(JSGlobalObject* globalObject, JSValue self)
{
    picklerClearMemo(globalObject, PyRecord::from(self));
    return jsUndefined();
}

static JSValue picklerSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(sizeof(PyRecord) + PicklerFieldCount * sizeof(JSValue) + listLength(asList(PyRecord::from(self)->get(PicklerMemoKeys))) * 3 * sizeof(JSValue));
}

static bool pickleIsName(JSValue name, ASCIILiteral expected) { return isStr(name) && WTF::equal(strValue(name)->tryGetValue().data.impl(), expected); }

static JSValue pickleGenericGetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* nameString = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, nameString ? genericGetAttr(globalObject, self, nameString) : JSValue());
}

// An empty value deletes.
static JSValue pickleGenericSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* nameString = attributeName(globalObject, name);
    RETURN_IF_EXCEPTION(scope, { });
    genericSetAttr(globalObject, self, nameString, value);
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue picklerGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    if (pickleIsName(name, "persistent_id"_s)) {
        if (JSValue value = PyRecord::from(self)->get(PicklerPersistentIdAttr))
            return value;
    }
    return pickleGenericGetAttr(globalObject, self, name);
}

static JSValue picklerSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    if (pickleIsName(name, "persistent_id"_s)) {
        PyRecord::from(self)->set(getVM(globalObject), PicklerPersistentIdAttr, value);
        return jsUndefined();
    }
    return pickleGenericSetAttr(globalObject, self, name, value);
}

static JSValue picklerDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    if (pickleIsName(name, "persistent_id"_s)) {
        PyRecord::from(self)->set(getVM(globalObject), PicklerPersistentIdAttr, { });
        return jsUndefined();
    }
    return pickleGenericSetAttr(globalObject, self, name, { });
}

template<unsigned field>
static JSValue pickleGetIntMember(JSGlobalObject*, JSValue self) { return PyRecord::from(self)->get(field); }

template<unsigned field>
static void pickleSetIntMember(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value) {
        raiseTypeError(globalObject, scope, "can't delete numeric/char attribute"_s);
        return;
    }
    JSValue index = toIndex(globalObject, value);
    RETURN_IF_EXCEPTION(scope, void());
    auto number = tryInt64(index);
    if (!number) {
        raiseOverflowError(globalObject, scope, "Python int too large to convert to C long"_s);
        return;
    }
    PyRecord::from(self)->set(vm, field, jsNumber(static_cast<int32_t>(*number)));
}

static JSValue picklerGetDispatchTable(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (JSValue table = PyRecord::from(self)->get(PicklerDispatchTable))
        return table;
    return raise(globalObject, scope, runtime(globalObject).AttributeError(), makeString('\'', typeName(globalObject, self), "' object has no attribute 'dispatch_table'"_s));
}

static void picklerSetDispatchTable(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* pickler = PyRecord::from(self);
    if (!value && !pickler->get(PicklerDispatchTable)) {
        raise(globalObject, scope, runtime(globalObject).AttributeError(), "dispatch_table"_s);
        return;
    }
    pickler->set(vm, PicklerDispatchTable, value);
}

// ---- Pickler.memo ----

enum PickleProxyField : unsigned { PickleProxyOwner, PickleProxyFieldCount };

static JSValue picklerGetMemo(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* proxy = PyRecord::create(globalObject, pickleStateType(globalObject, PickleStatePicklerMemoProxyType), PickleProxyFieldCount);
    proxy->set(getVM(globalObject), PickleProxyOwner, self);
    return proxy;
}

static void picklerSetMemo(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* pickler = PyRecord::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "attribute deletion is not supported"_s);
        return;
    }
    // Made aside, so that an error leaves the memo as it was.
    PyRecord* scratch = PyRecord::create(globalObject, runtime(globalObject).objectType(), PicklerFieldCount);
    picklerClearMemo(globalObject, scratch);
    if (PyRecord* proxy = PyRecord::tryFrom(value, pickleStateType(globalObject, PickleStatePicklerMemoProxyType)); proxy && proxy->type() == pickleStateType(globalObject, PickleStatePicklerMemoProxyType)) {
        auto* other = PyRecord::from(proxy->get(PickleProxyOwner));
        JSArray* keys = picklerMemoKeysToGoThrough(globalObject, other);
        RETURN_IF_EXCEPTION(scope, void());
        auto* map = uncheckedDowncast<JSWeakMap>(other->get(PicklerMemoMap).asCell());
        for (unsigned i = 0; i < listLength(keys); ++i) {
            JSValue index = picklerMemoNumberOf(globalObject, map, listGet(keys, i));
            RETURN_IF_EXCEPTION(scope, void());
            picklerMemoSet(globalObject, scratch, listGet(keys, i).asCell(), index.asNumber());
            RETURN_IF_EXCEPTION(scope, void());
        }
        // (One object in the place of another that has a number too: the list is as long as it was, and each item has a number.)
        if (scratch->get(PicklerMemoCount) != other->get(PicklerMemoCount)) [[unlikely]] {
            scope.release();
            picklerRaiseMemoKeysChanged(globalObject);
            return;
        }
    } else if (isDict(value)) {
        auto* dict = PyDict::from(value);
        JSValue key, item;
        for (unsigned position = 0; dict->next(position, key, item);) {
            auto* pair = PyTuple::tryFrom(item);
            if (!pair || pair->length() != 2) {
                raiseTypeError(globalObject, scope, "'memo' values must be 2-item tuples"_s);
                return;
            }
            if (!isInt(pair->at(0))) {
                raiseTypeError(globalObject, scope, "an integer is required"_s);
                return;
            }
            auto index = tryInt64(intValue(pair->at(0)));
            if (!index) {
                raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
                return;
            }
            if (pair->at(1).isCell()) {
                picklerMemoSet(globalObject, scratch, pair->at(1).asCell(), static_cast<double>(*index));
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
    } else {
        raiseTypeError(globalObject, scope, "'memo' attribute must be a PicklerMemoProxy object or dict, not "_s, typeName(globalObject, value));
        return;
    }
    pickler->set(vm, PicklerMemoMap, scratch->get(PicklerMemoMap));
    pickler->set(vm, PicklerMemoKeys, scratch->get(PicklerMemoKeys));
    pickler->set(vm, PicklerMemoCount, scratch->get(PicklerMemoCount));
}

static JSValue picklerMemoProxyClear(JSGlobalObject* globalObject, JSValue self)
{
    picklerClearMemo(globalObject, PyRecord::from(PyRecord::from(self)->get(PickleProxyOwner)));
    return jsUndefined();
}

// {id(object): (number, object)}
static JSValue picklerMemoProxyCopy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* pickler = PyRecord::from(PyRecord::from(self)->get(PickleProxyOwner));
    JSArray* keys = picklerMemoKeysToGoThrough(globalObject, pickler);
    RETURN_IF_EXCEPTION(scope, { });
    auto* map = uncheckedDowncast<JSWeakMap>(pickler->get(PicklerMemoMap).asCell());
    PyDict* result = PyDict::create(globalObject);
    UncheckedKeyHashSet<JSCell*> seen;
    for (unsigned i = 0; i < listLength(keys); ++i) {
        JSValue object = listGet(keys, i);
        JSValue index = picklerMemoNumberOf(globalObject, map, object);
        RETURN_IF_EXCEPTION(scope, { });
        // (One object in the place of another that has a number too. By the cell, as the memo goes, not by the keys of the result: two strs of one text are two cells, and one object to id().)
        if (!seen.add(object.asCell()).isNewEntry) [[unlikely]]
            RELEASE_AND_RETURN(scope, picklerRaiseMemoKeysChanged(globalObject));
        JSValue identity = identityOf(globalObject, object);
        RETURN_IF_EXCEPTION(scope, { });
        result->set(globalObject, identity, PyTuple::create(globalObject, { index, object }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

template<JSValue (*copy)(JSGlobalObject*, JSValue)>
static JSValue pickleMemoProxyReduce(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue contents = copy(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, PyTuple::create(globalObject, { runtime(globalObject).dictType(), PyTuple::create(globalObject, { contents }) }));
}

static constexpr NativeDef picklerMemoProxyMethods[] = {
    PY_METHOD("clear", picklerMemoProxyClear, "($self, /)"),
    PY_METHOD("copy", picklerMemoProxyCopy, "($self, /)"),
    PY_METHOD("__reduce__", pickleMemoProxyReduce<picklerMemoProxyCopy>, "($self, /)"),
};

static constexpr TypeDef picklerMemoProxyTypeDef {
    "_pickle.PicklerMemoProxy"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, picklerMemoProxyMethods, { }, { }
};

static constexpr NativeDef picklerMethods[] = {
    PY_METHOD("persistent_id", picklerPersistentId, "($self, obj, /)"),
    PY_NEW(picklerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", picklerInit, "($self, *args, **kwargs)"),
    PY_SLOT("__getattribute__", picklerGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", picklerSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", picklerDelAttr, "($self, name, /)"),
    PY_METHOD("dump", picklerDump, "!($self, obj, /)"),
    PY_METHOD("clear_memo", picklerClearMemoMethod, "($self, /)"),
    PY_METHOD("__sizeof__", picklerSizeOf, "($self, /)"),
};

static bool picklerIsDefaultPersistentId(JSValue function, JSValue self)
{
    auto* native = PyNativeFunction::tryFrom(function);
    return native && &native->def() == &picklerMethods[0] && native->self() == self;
}

static constexpr NativeGetSet picklerGetSets[] = {
    PY_WRITABLE_MEMBER("bin", pickleGetIntMember<PicklerBin>, pickleSetIntMember<PicklerBin>),
    PY_WRITABLE_MEMBER("fast", pickleGetIntMember<PicklerFast>, pickleSetIntMember<PicklerFast>),
    PY_WRITABLE_MEMBER("dispatch_table", picklerGetDispatchTable, picklerSetDispatchTable),
    { "memo"_s, picklerGetMemo, picklerSetMemo },
};

static constexpr TypeDef picklerTypeDef {
    "_pickle.Pickler"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable }, picklerMethods, picklerGetSets,
    "This takes a binary file for writing a pickle data stream.\n\n"
    "The optional *protocol* argument tells the pickler to use the given\n"
    "protocol; supported protocols are 0, 1, 2, 3, 4 and 5.  The default\n"
    "protocol is 5. It was introduced in Python 3.8, and is incompatible\n"
    "with previous versions.\n\n"
    "Specifying a negative protocol version selects the highest protocol\n"
    "version supported.  The higher the protocol used, the more recent the\n"
    "version of Python needed to read the pickle produced.\n\n"
    "The *file* argument must have a write() method that accepts a single\n"
    "bytes argument. It can thus be a file object opened for binary\n"
    "writing, an io.BytesIO instance, or any other custom object that meets\n"
    "this interface.\n\n"
    "If *fix_imports* is True and protocol is less than 3, pickle will try\n"
    "to map the new Python 3 names to the old module names used in Python\n"
    "2, so that the pickle data stream is readable with Python 2.\n\n"
    "If *buffer_callback* is None (the default), buffer views are\n"
    "serialized into *file* as part of the pickle stream.\n\n"
    "If *buffer_callback* is not None, then it can be called any number\n"
    "of times with a buffer view.  If the callback returns a false value\n"
    "(such as None), the given buffer is out-of-band; otherwise the\n"
    "buffer is serialized in-band, i.e. inside the pickle stream.\n\n"
    "It is an error if *buffer_callback* is not None and *protocol*\n"
    "is None or smaller than 5."_s
};

// ---- Unpickler ----

enum UnpicklerField : unsigned {
    UnpicklerRead, // Empty: __init__ has not run, or the pickle is in memory.
    UnpicklerReadLine,
    UnpicklerPersistentLoadAttr,
    UnpicklerBuffers, // An iterator. Empty: none were given.
    UnpicklerEncoding,
    UnpicklerErrors,
    // The memo: an array with holes, and a dict for the numbers that are far beyond the others.
    UnpicklerMemo,
    UnpicklerMemoDict,
    UnpicklerMemoLength, // How many numbers the array has something for,
    UnpicklerMemoSize, // and how many there is room for, which decides what goes in the dict.
    // What has been read from the file and not used yet, from one load() to the next.
    UnpicklerInput, // bytes
    UnpicklerNext,
    UnpicklerInputLength,
    UnpicklerFrameEnd,
    UnpicklerSavedInputLength,
    UnpicklerFieldCount
};
// In `number`: the protocol, and above it
enum UnpicklerBit : int64_t { UnpicklerFixImports = 1 << 8, UnpicklerIsRunning = 1 << 9 };

static void unpicklerClearMemo(JSGlobalObject* globalObject, PyRecord* unpickler, unsigned size)
{
    VM& vm = getVM(globalObject);
    unpickler->set(vm, UnpicklerMemo, newList(globalObject));
    unpickler->set(vm, UnpicklerMemoDict, { });
    unpickler->set(vm, UnpicklerMemoLength, jsNumber(0));
    unpickler->set(vm, UnpicklerMemoSize, jsNumber(size));
}

static PyRecord* unpicklerCreate(JSGlobalObject* globalObject, PyType* type)
{
    PyRecord* unpickler = PyRecord::create(globalObject, type, UnpicklerFieldCount);
    unpicklerClearMemo(globalObject, unpickler, 32);
    return unpickler;
}

static void unpicklerMemoPut(JSGlobalObject* globalObject, PyRecord* unpickler, uint64_t index, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    uint64_t size = static_cast<uint64_t>(unpickler->get(UnpicklerMemoSize).asNumber());
    uint64_t length = static_cast<uint64_t>(unpickler->get(UnpicklerMemoLength).asNumber());
    if (index >= size) {
        if (index > length * 2) {
            JSValue dict = unpickler->get(UnpicklerMemoDict);
            if (!dict) {
                dict = PyDict::create(globalObject);
                unpickler->set(vm, UnpicklerMemoDict, dict);
            }
            scope.release();
            PyDict::from(dict)->set(globalObject, makeInt(globalObject, index), value);
            return;
        }
        unpickler->set(vm, UnpicklerMemoSize, jsNumber(static_cast<double>(index * 2)));
    }
    auto* memo = asList(unpickler->get(UnpicklerMemo));
    unsigned position = static_cast<unsigned>(index);
    bool isNew = position >= memo->length();
    if (!isNew) {
        isNew = !memo->getDirectIndex(globalObject, position);
        RETURN_IF_EXCEPTION(scope, void());
    }
    if (isNew)
        unpickler->set(vm, UnpicklerMemoLength, jsNumber(static_cast<double>(length + 1)));
    scope.release();
    memo->putDirectIndex(globalObject, position, value);
}

static bool unpicklerSetInputStream(JSGlobalObject* globalObject, PyRecord* unpickler, JSValue file)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    // CPython asks for these too, and in this order.
    getAttrIfPresent(globalObject, file, internString(vm, "peek"_s));
    RETURN_IF_EXCEPTION(scope, false);
    getAttrIfPresent(globalObject, file, internString(vm, "readinto"_s));
    RETURN_IF_EXCEPTION(scope, false);
    JSValue read = getAttrIfPresent(globalObject, file, rt.names.read());
    RETURN_IF_EXCEPTION(scope, false);
    JSValue readLine = getAttrIfPresent(globalObject, file, rt.names.readline());
    RETURN_IF_EXCEPTION(scope, false);
    if (!read || !readLine) {
        raiseTypeError(globalObject, scope, "file must have 'read' and 'readline' attributes"_s);
        return false;
    }
    unpickler->set(vm, UnpicklerRead, read);
    unpickler->set(vm, UnpicklerReadLine, readLine);
    return true;
}

static bool unpicklerSetBuffers(JSGlobalObject* globalObject, PyRecord* unpickler, JSValue buffers)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue iterator;
    if (buffers && !isNone(buffers)) {
        iterator = getIter(globalObject, buffers);
        RETURN_IF_EXCEPTION(scope, false);
    }
    unpickler->set(vm, UnpicklerBuffers, iterator);
    return true;
}

// An argument that CPython takes as a C string. Null: an exception.
static JSString* pickleStringArgument(JSGlobalObject* globalObject, ASCIILiteral function, ASCIILiteral name, JSValue value, ASCIILiteral defaultValue)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!value)
        return internString(vm, defaultValue);
    if (!isStr(value)) {
        raiseTypeError(globalObject, scope, function, "() argument '"_s, name, "' must be str, not "_s, typeNameOfArgument(globalObject, value));
        return nullptr;
    }
    String text = strValue(value)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, nullptr);
    if (text.contains(static_cast<char16_t>(0))) {
        raiseValueError(globalObject, scope, "embedded null character"_s);
        return nullptr;
    }
    return strValue(value);
}

static JSValue unpicklerFindClass(JSGlobalObject*, JSValue self, JSValue moduleName, JSValue globalName);

// One call of load().
class UnpicklerRun {
public:
    UnpicklerRun(JSGlobalObject* globalObject, PyRecord* unpickler)
        : m_globalObject(globalObject)
        , m_rt(runtime(globalObject))
        , m_vm(m_rt.vm())
        , m_unpickler(unpickler)
        , m_state(pickleState(globalObject))
        , m_read(unpickler->get(UnpicklerRead))
    {
        if (JSValue input = unpickler->get(UnpicklerInput)) {
            m_inputOwner = input;
            m_input = PyBytes::from(input)->data();
            m_next = static_cast<int64_t>(unpickler->get(UnpicklerNext).asNumber());
            m_inputLength = static_cast<int64_t>(unpickler->get(UnpicklerInputLength).asNumber());
            m_frameEnd = static_cast<int64_t>(unpickler->get(UnpicklerFrameEnd).asNumber());
            m_savedInputLength = static_cast<int64_t>(unpickler->get(UnpicklerSavedInputLength).asNumber());
        }
    }

    ~UnpicklerRun()
    {
        if (!m_read)
            return;
        m_unpickler->set(m_vm, UnpicklerInput, m_inputOwner);
        m_unpickler->set(m_vm, UnpicklerNext, jsNumber(static_cast<double>(m_next)));
        m_unpickler->set(m_vm, UnpicklerInputLength, jsNumber(static_cast<double>(m_inputLength)));
        m_unpickler->set(m_vm, UnpicklerFrameEnd, jsNumber(static_cast<double>(m_frameEnd)));
        m_unpickler->set(m_vm, UnpicklerSavedInputLength, jsNumber(static_cast<double>(m_savedInputLength)));
    }

    // The pickle is in memory, which the caller sees to staying where it is.
    void setInput(std::span<const uint8_t> data)
    {
        m_input = data.data();
        m_inputLength = data.size();
        m_next = 0;
        m_frameEnd = -1;
    }

    // Empty: an exception.
    JSValue load()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        setProto(0);
        bool isPlain = JSValue(m_unpickler->type()) == m_state->get(PickleStateUnpicklerType);
        m_callsFindClassDirectly = isPlain;
        m_persistentLoad = getAttr(m_globalObject, m_unpickler, internString(m_vm, "persistent_load"_s));
        RETURN_IF_EXCEPTION(scope, { });

        while (true) {
            const uint8_t* op = read(1);
            if (failed(scope, op)) {
                if (clearPendingExceptionIfMatches(m_globalObject, unpicklingError()))
                    raiseEOFError(m_globalObject, scope, "Ran out of input"_s);
                return { };
            }
            if (*op == PickleOpStop)
                break;
            if (failed(scope, dispatch(*op)))
                return { };
        }
        RELEASE_AND_RETURN(scope, pop());
    }

private:
    PyType* unpicklingError() const { return PyType::from(m_state->get(PickleStateUnpicklingError)); }

    template<typename... Parts>
    bool fail(Parts... parts) { return pickleRaise(m_globalObject, PickleStateUnpicklingError, parts...); }

    int proto() const { return static_cast<int>(m_unpickler->number & 0xff); }
    void setProto(int proto) { m_unpickler->number = (m_unpickler->number & ~static_cast<int64_t>(0xff)) | proto; }

    // ---- Input ----

    bool truncated() { return fail("pickle data was truncated"_s); }

    void endFrame()
    {
        m_inputLength = m_savedInputLength;
        m_frameEnd = -1;
    }

    // The end of the frame has been reached. `straddles`: by something that goes on beyond it.
    bool leaveFrame(bool straddles)
    {
        if (straddles)
            return fail("pickle exhausted before end of frame"_s);
        endFrame();
        return true;
    }

    // What the file returned becomes the input. -1: an exception.
    int64_t setFileInput(JSValue data)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        if (!PyBytes::is(data)) {
            BytesHolder holder(m_globalObject);
            if (failed(scope, holder.getLast(data)))
                return -1;
            data = PyBytes::create(m_globalObject, holder.span());
        }
        m_inputOwner = data;
        setInput(PyBytes::from(data)->span());
        return m_inputLength;
    }

    // Exactly `count` bytes from the file, to `destination`.
    bool readIntoFromFile(uint8_t* destination, int64_t count)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue data = call(m_globalObject, m_read, makeInt(m_globalObject, count));
        RETURN_IF_EXCEPTION(scope, false);
        auto* bytes = PyBytes::tryFrom(data);
        if (!bytes) {
            String type = pickleRepr(m_globalObject, typeOf(m_globalObject, data));
            RETURN_IF_EXCEPTION(scope, false);
            raiseValueError(m_globalObject, scope, "read() returned non-bytes object ("_s, type, ')');
            return false;
        }
        if (static_cast<int64_t>(bytes->length()) < count)
            RELEASE_AND_RETURN(scope, truncated());
        memcpy(destination, bytes->data(), count);
        return true;
    }

    // No more is taken from the file than is asked for: another pickle may follow this one. How many bytes there were, or -1.
    int64_t readFromFile(int64_t count)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        // Not all at once, lest a few bytes that lie about a length make for a huge allocation.
        int64_t size = std::min(count, pickleMinReadBufferSize);
        JSValue data = call(m_globalObject, m_read, makeInt(m_globalObject, size));
        RETURN_IF_EXCEPTION(scope, -1);
        if (size == count)
            RELEASE_AND_RETURN(scope, setFileInput(data));
        ByteBuilder all;
        {
            BytesHolder first(m_globalObject);
            if (failed(scope, first.getLast(data)))
                return -1;
            if (static_cast<int64_t>(first.span().size()) < size)
                RELEASE_AND_RETURN(scope, setFileInput(data));
            all.append(first.span().first(size));
        }
        while (size < count) {
            int64_t more = std::min(size, count - size);
            size += more;
            auto room = all.appendUninitialized(more);
            if (!room.data())
                break;
            if (failed(scope, readIntoFromFile(room.data(), more)))
                return -1;
        }
        PyBytes* bytes = makeBytes(m_globalObject, all);
        if (failed(scope, bytes))
            return -1;
        RELEASE_AND_RETURN(scope, setFileInput(bytes));
    }

    // The next `count` bytes, which stay put until the next read. Null: an exception.
    ALWAYS_INLINE const uint8_t* read(int64_t count)
    {
        if (count <= m_inputLength - m_next) [[likely]] {
            const uint8_t* result = m_input + m_next;
            m_next += count;
            return result ? result : reinterpret_cast<const uint8_t*>("");
        }
        return readSlow(count);
    }

    NEVER_INLINE const uint8_t* readSlow(int64_t count)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_next > std::numeric_limits<int64_t>::max() - count) {
            scope.release();
            fail("read would overflow (invalid bytecode)"_s);
            return nullptr;
        }
        if (m_frameEnd >= 0) {
            if (failed(scope, leaveFrame(m_next < m_frameEnd)))
                return nullptr;
            if (count <= m_inputLength - m_next) {
                const uint8_t* result = m_input + m_next;
                m_next += count;
                return result;
            }
        }
        if (!m_read) {
            scope.release();
            truncated();
            return nullptr;
        }
        int64_t found = readFromFile(count);
        if (failed(scope, found >= 0))
            return nullptr;
        if (found < count) {
            scope.release();
            truncated();
            return nullptr;
        }
        m_next = count;
        return m_input;
    }

    // Copies the next `count` bytes.
    bool readInto(uint8_t* destination, int64_t count)
    {
        auto takeFromBuffer = [&] {
            int64_t available = m_inputLength - m_next;
            if (available <= 0)
                return;
            int64_t taken = std::min(available, count);
            memcpy(destination, m_input + m_next, taken);
            m_next += taken;
            destination += taken;
            count -= taken;
        };
        bool hadSome = m_inputLength > m_next;
        takeFromBuffer();
        if (!count)
            return true;
        if (m_frameEnd >= 0) {
            if (!leaveFrame(hadSome))
                return false;
            takeFromBuffer();
            if (!count)
                return true;
        }
        if (!m_read)
            return truncated();
        return readIntoFromFile(destination, count);
    }

    // A line with its newline, in m_line, with a NUL after it. False: an exception.
    bool readLine()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto keep = [&](const uint8_t* start, int64_t length) {
            m_line.clear();
            m_line.reserveInitialCapacity(length + 1);
            m_line.append(std::span { start, static_cast<size_t>(length) });
            m_line.append(0);
            if (m_line.hasOverflowed()) [[unlikely]] {
                raiseMemoryError(m_globalObject, scope, String());
                return false;
            }
            m_line.shrink(length);
            return true;
        };
        while (true) {
            if (m_inputLength > m_next) {
                if (auto* newline = static_cast<const uint8_t*>(memchr(m_input + m_next, '\n', m_inputLength - m_next))) {
                    const uint8_t* start = m_input + m_next;
                    m_next = newline - m_input + 1;
                    return keep(start, newline - start + 1);
                }
            }
            if (m_frameEnd < 0)
                break;
            if (failed(scope, leaveFrame(m_next < m_frameEnd)))
                return false;
        }
        if (!m_read)
            RELEASE_AND_RETURN(scope, truncated());
        JSValue data = call(m_globalObject, m_unpickler->get(UnpicklerReadLine));
        RETURN_IF_EXCEPTION(scope, false);
        int64_t found = setFileInput(data);
        if (failed(scope, found >= 0))
            return false;
        if (!found || m_input[found - 1] != '\n')
            RELEASE_AND_RETURN(scope, truncated());
        m_next = found;
        return keep(m_input, found);
    }

    // A line that is to be a number, which is looked at as a text is.
    bool readLineOfText()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLine()))
            return false;
        if (m_line.size() > String::MaxLength) [[unlikely]] {
            raiseMemoryError(m_globalObject, scope, String());
            return false;
        }
        return true;
    }

    // ---- The stack ----

    size_t stackSize() const { return m_stack.size(); }

    bool underflow() { return fail(m_marks.isEmpty() ? "unpickling stack underflow"_s : "unexpected MARK found"_s); }

    ALWAYS_INLINE bool push(JSValue value)
    {
        m_stack.append(value);
        return true;
    }

    // Empty: an exception.
    JSValue pop()
    {
        if (stackSize() <= m_fence) {
            underflow();
            return { };
        }
        return m_stack.takeLast();
    }

    void truncateStack(size_t size)
    {
        while (m_stack.size() > size)
            m_stack.removeLast();
    }

    std::span<const JSValue> stackFrom(size_t start) { return spanOf(m_stack).subspan(start); }

    PyTuple* popTuple(size_t start)
    {
        if (start < m_fence) {
            underflow();
            return nullptr;
        }
        PyTuple* tuple = PyTuple::create(m_globalObject, stackFrom(start));
        truncateStack(start);
        return tuple;
    }

    JSArray* popList(size_t start)
    {
        JSArray* list = newList(m_globalObject, stackFrom(start));
        truncateStack(start);
        return list;
    }

    // Where the topmost mark is, which is taken off. nullopt: an exception.
    std::optional<size_t> marker()
    {
        if (m_marks.isEmpty()) {
            fail("could not find MARK"_s);
            return std::nullopt;
        }
        size_t mark = m_marks.takeLast();
        m_fence = m_marks.isEmpty() ? 0 : m_marks.last();
        return mark;
    }

    // ---- The memo ----

    bool memoGet(uint64_t index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        uint64_t size = static_cast<uint64_t>(m_unpickler->get(UnpicklerMemoSize).asNumber());
        auto* memo = asList(m_unpickler->get(UnpicklerMemo));
        if (index < size && index < memo->length()) {
            JSValue value = memo->tryGetIndexQuickly(static_cast<unsigned>(index));
            if (!value) {
                value = memo->getDirectIndex(m_globalObject, static_cast<unsigned>(index));
                RETURN_IF_EXCEPTION(scope, false);
            }
            if (value)
                return push(value);
        }
        if (JSValue dict = m_unpickler->get(UnpicklerMemoDict)) {
            JSValue value = PyDict::from(dict)->get(m_globalObject, makeInt(m_globalObject, index));
            RETURN_IF_EXCEPTION(scope, false);
            if (value)
                return push(value);
        }
        RELEASE_AND_RETURN(scope, fail("Memo value not found at index "_s, static_cast<int64_t>(index)));
    }

    bool memoPutTop(uint64_t index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unpicklerMemoPut(m_globalObject, m_unpickler, index, m_stack.last());
        RETURN_IF_EXCEPTION(scope, false);
        return true;
    }

    // ---- Numbers ----

    static int64_t binInt(const uint8_t* bytes, unsigned count)
    {
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        return count == 4 ? static_cast<int32_t>(value) : static_cast<int64_t>(value);
    }

    // Negative: it does not fit.
    static int64_t binSize(const uint8_t* bytes, unsigned count)
    {
        uint64_t value = 0;
        for (unsigned i = 0; i < count; ++i)
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? -1 : static_cast<int64_t>(value);
    }

    // int(text, 10), where the text ends at the first NUL. Empty: an exception.
    JSValue intFromLine(std::span<const uint8_t> text)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (size_t end = WTF::find(text, static_cast<uint8_t>(0)); end != notFound)
            text = text.first(end);
        JSValue value = intFromText(m_globalObject, StringView(byteCast<Latin1Character>(text)), 10, NumberTextOf::Bytes);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value) {
            JSValue shown = decodeUTF8(text.first(std::min<size_t>(text.size(), 200)), "strict"_s);
            RETURN_IF_EXCEPTION(scope, { });
            RELEASE_AND_RETURN(scope, raiseInvalidIntLiteral(m_globalObject, shown, 10));
        }
        return value;
    }

    bool loadInt()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLineOfText()))
            return false;
        if (m_line.size() < 2)
            RELEASE_AND_RETURN(scope, truncated());
        if (m_line.size() == 3 && m_line[0] == '0' && (m_line[1] == '0' || m_line[1] == '1'))
            return push(jsBoolean(m_line[1] == '1'));
        // What strtol() makes of it, if that is a long and all of the line. (A NUL follows the line.)
        errno = 0;
        char* end;
        long number = strtol(byteCast<char>(m_line.span()).data(), &end, 10);
        if (!errno && (*end == '\n' || !*end))
            RELEASE_AND_RETURN(scope, push(makeInt(m_globalObject, static_cast<int64_t>(number))));
        JSValue value = intFromLine(m_line.span());
        if (failed(scope, value)) {
            takePendingException(m_globalObject);
            raiseValueError(m_globalObject, scope, "could not convert string to int"_s);
            return false;
        }
        return push(value);
    }

    bool loadLong()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLineOfText()))
            return false;
        if (m_line.size() < 2)
            RELEASE_AND_RETURN(scope, truncated());
        if (m_line[m_line.size() - 2] == 'L')
            m_line[m_line.size() - 2] = 0;
        JSValue value = intFromLine(m_line.span());
        RETURN_IF_EXCEPTION(scope, false);
        return push(value);
    }

    bool loadCountedLong(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        int64_t size = binInt(bytes, sizeBytes);
        if (size < 0)
            RELEASE_AND_RETURN(scope, fail("LONG pickle has negative byte count"_s));
        if (!size)
            return push(jsNumber(0));
        bytes = read(size);
        if (failed(scope, bytes))
            return false;
        JSValue value = intFromByteSpan(m_globalObject, { bytes, static_cast<size_t>(size) }, true, true);
        RETURN_IF_EXCEPTION(scope, false);
        return push(value);
    }

    bool loadFloat()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLineOfText()))
            return false;
        if (m_line.size() < 2)
            RELEASE_AND_RETURN(scope, truncated());
        // The text ends at the first NUL, as that of an int.
        auto line = m_line.span();
        if (size_t end = WTF::find(line, static_cast<uint8_t>(0)); end != notFound)
            line = line.first(end);
        auto text = byteCast<Latin1Character>(line);
        double value;
        size_t used = scanDouble(text, value);
        bool isTooLarge = used && std::isinf(value) && !isASCIIAlpha(text[text[0] == '-' || text[0] == '+']);
        if (!used || isTooLarge) {
            // The first 200 bytes, without the beginning of a character that they may end with.
            size_t consumed;
            bool isCut = line.size() >= 200;
            JSValue shown = codecDecodeUTF8(m_globalObject, line.first(std::min<size_t>(line.size(), 200)), "replace"_s, isCut ? &consumed : nullptr);
            RETURN_IF_EXCEPTION(scope, false);
            if (isTooLarge)
                raiseOverflowError(m_globalObject, scope, "value too large to convert to float: '"_s, stringOf(asString(shown)), '\'');
            else
                raiseValueError(m_globalObject, scope, "could not convert string to float: '"_s, stringOf(asString(shown)), '\'');
            return false;
        }
        if (used < text.size() && text[used] != '\n') {
            raiseValueError(m_globalObject, scope, "could not convert string to float"_s);
            return false;
        }
        RELEASE_AND_RETURN(scope, push(makeFloat(m_globalObject, value)));
    }

    bool loadBinFloat()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(8);
        if (failed(scope, bytes))
            return false;
        uint64_t bits = 0;
        for (unsigned i = 0; i < 8; ++i)
            bits = (bits << 8) | bytes[i];
        RELEASE_AND_RETURN(scope, push(makeFloat(m_globalObject, std::bit_cast<double>(bits))));
    }

    // ---- Strings and bytes ----

    bool isEncodingBytes() { return stringOf(asString(m_unpickler->get(UnpicklerEncoding))) == "bytes"_s; }

    // A str of Python 2: bytes if the encoding is "bytes", or else decoded.
    bool pushOldString(JSValue bytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isEncodingBytes())
            return push(bytes);
        JSValue text = bytesDecode(m_globalObject, bytes, asString(m_unpickler->get(UnpicklerEncoding)), asString(m_unpickler->get(UnpicklerErrors)));
        RETURN_IF_EXCEPTION(scope, false);
        return push(text);
    }

    bool loadString()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLine()))
            return false;
        auto text = m_line.span().first(m_line.size() - 1);
        if (text.size() < 2 || text.front() != text.back() || (text.front() != '\'' && text.front() != '"'))
            RELEASE_AND_RETURN(scope, fail("the STRING opcode argument must be quoted"_s));
        JSValue codecs = importModule(m_globalObject, "_codecs"_s);
        RETURN_IF_EXCEPTION(scope, false);
        JSValue decoded = callMethod(m_globalObject, codecs, internString(m_vm, "escape_decode"_s), PyBytes::create(m_globalObject, text.subspan(1, text.size() - 2)));
        RETURN_IF_EXCEPTION(scope, false);
        RELEASE_AND_RETURN(scope, pushOldString(PyTuple::from(decoded)->at(0)));
    }

    bool loadCountedBinString(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        int64_t size = binInt(bytes, sizeBytes);
        if (size < 0)
            RELEASE_AND_RETURN(scope, fail("BINSTRING pickle has negative byte count"_s));
        bytes = read(size);
        if (failed(scope, bytes))
            return false;
        RELEASE_AND_RETURN(scope, pushOldString(PyBytes::create(m_globalObject, std::span { bytes, static_cast<size_t>(size) })));
    }

    template<typename Create>
    bool loadCountedBytes(unsigned sizeBytes, ASCIILiteral opcode, const Create& create)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        int64_t size = binSize(bytes, sizeBytes);
        if (size < 0) {
            raiseOverflowError(m_globalObject, scope, opcode, " exceeds system's maximum size of "_s, std::numeric_limits<int64_t>::max(), " bytes"_s);
            return false;
        }
        if (size <= m_inputLength - m_next) {
            JSValue result = create(std::span { m_input + m_next, static_cast<size_t>(size) });
            RETURN_IF_EXCEPTION(scope, false);
            m_next += size;
            return push(result);
        }
        // As much at a time as there has been so far: what the pickle says the size is need not be true.
        ByteBuilder data;
        int64_t current = std::min(size, pickleMinReadBufferSize);
        int64_t previous = 0;
        while (true) {
            auto room = data.appendUninitialized(current - previous);
            if (!room.data()) {
                raiseMemoryError(m_globalObject, scope, String());
                return false;
            }
            if (failed(scope, readInto(room.data(), current - previous)))
                return false;
            if (current >= size)
                break;
            previous = current;
            current += std::min(current, size - current);
        }
        JSValue result = create(data.span());
        RETURN_IF_EXCEPTION(scope, false);
        return push(result);
    }

    bool loadNextBuffer()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue buffers = m_unpickler->get(UnpicklerBuffers);
        if (!buffers)
            RELEASE_AND_RETURN(scope, fail("pickle stream refers to out-of-band data but no *buffers* argument was given"_s));
        JSValue buffer = iterNext(m_globalObject, buffers);
        RETURN_IF_EXCEPTION(scope, false);
        if (!buffer)
            RELEASE_AND_RETURN(scope, fail("not enough out-of-band buffers"_s));
        return push(buffer);
    }

    bool loadReadonlyBuffer()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (stackSize() <= m_fence)
            RELEASE_AND_RETURN(scope, underflow());
        JSValue view = call(m_globalObject, m_rt.memoryviewType(), m_stack.last());
        RETURN_IF_EXCEPTION(scope, false);
        JSValue isReadOnly = getAttr(m_globalObject, view, internString(m_vm, "readonly"_s));
        RETURN_IF_EXCEPTION(scope, false);
        if (isReadOnly == jsBoolean(true))
            return true;
        JSValue readOnlyView = callMethod(m_globalObject, view, internString(m_vm, "toreadonly"_s));
        RETURN_IF_EXCEPTION(scope, false);
        m_stack.removeLast();
        return push(readOnlyView);
    }

    bool loadUnicode()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLine()))
            return false;
        JSValue text = codecDecodeRawUnicodeEscape(m_globalObject, m_line.span().first(m_line.size() - 1), "strict"_s);
        RETURN_IF_EXCEPTION(scope, false);
        return push(text);
    }

    JSValue decodeUTF8(std::span<const uint8_t> bytes, ASCIILiteral errors)
    {
        if (charactersAreAllASCII(bytes) && bytes.size() <= String::MaxLength)
            return bytes.empty() ? JSValue(m_rt.names.empty()) : JSValue(makeStr(m_vm, String(byteCast<Latin1Character>(bytes))));
        return codecDecodeUTF8(m_globalObject, bytes, errors);
    }

    bool loadCountedBinUnicode(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        int64_t size = binSize(bytes, sizeBytes);
        if (size < 0) {
            raiseOverflowError(m_globalObject, scope, "BINUNICODE exceeds system's maximum size of "_s, std::numeric_limits<int64_t>::max(), " bytes"_s);
            return false;
        }
        bytes = read(size);
        if (failed(scope, bytes))
            return false;
        JSValue text = decodeUTF8({ bytes, static_cast<size_t>(size) }, "surrogatepass"_s);
        RETURN_IF_EXCEPTION(scope, false);
        return push(text);
    }

    // ---- Containers ----

    bool loadCountedTuple(size_t length)
    {
        if (stackSize() < length)
            return underflow();
        PyTuple* tuple = popTuple(stackSize() - length);
        return tuple && push(tuple);
    }

    bool loadTuple()
    {
        auto mark = marker();
        return mark && loadCountedTuple(stackSize() - *mark);
    }

    bool loadList()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        JSArray* list = popList(*mark);
        RETURN_IF_EXCEPTION(scope, false);
        return push(list);
    }

    bool loadDict()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        size_t size = stackSize();
        if ((size - *mark) % 2)
            RELEASE_AND_RETURN(scope, fail("odd number of items for DICT"_s));
        PyDict* dict = PyDict::create(m_globalObject);
        for (size_t i = *mark + 1; i < size; i += 2) {
            dict->set(m_globalObject, m_stack.at(i - 1), m_stack.at(i));
            RETURN_IF_EXCEPTION(scope, false);
        }
        truncateStack(*mark);
        return push(dict);
    }

    bool loadFrozenSet()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        PyTuple* items = popTuple(*mark);
        if (failed(scope, items))
            return false;
        PySet* set = PySet::createFrozen(m_globalObject);
        for (unsigned i = 0; i < items->length(); ++i) {
            set->add(m_globalObject, items->at(i));
            RETURN_IF_EXCEPTION(scope, false);
        }
        return push(set);
    }

    // ---- Objects ----

    JSValue findClass(JSValue moduleName, JSValue globalName)
    {
        if (m_callsFindClassDirectly)
            return unpicklerFindClass(m_globalObject, m_unpickler, moduleName, globalName);
        return callMethod(m_globalObject, m_unpickler, internString(m_vm, "find_class"_s), moduleName, globalName);
    }

    JSValue instantiate(JSValue cls, PyTuple* arguments)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!arguments->length() && PyType::is(cls)) {
            bool hasInitArgs = hasAttr(m_globalObject, cls, internString(m_vm, "__getinitargs__"_s));
            RETURN_IF_EXCEPTION(scope, { });
            if (!hasInitArgs)
                RELEASE_AND_RETURN(scope, callMethod(m_globalObject, cls, m_rt.name(Special::New), cls));
        }
        RELEASE_AND_RETURN(scope, callWithTupleAndDict(m_globalObject, cls, arguments, nullptr));
    }

    bool loadObj()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        if (stackSize() - *mark < 1)
            RELEASE_AND_RETURN(scope, underflow());
        PyTuple* arguments = popTuple(*mark + 1);
        if (failed(scope, arguments))
            return false;
        JSValue cls = pop();
        if (failed(scope, cls))
            return false;
        JSValue object = instantiate(cls, arguments);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadInst()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        JSValue names[2];
        for (JSValue& name : names) {
            if (failed(scope, readLine()))
                return false;
            if (m_line.size() < 2)
                RELEASE_AND_RETURN(scope, truncated());
            name = codecDecodeASCII(m_globalObject, m_line.span().first(m_line.size() - 1), "strict"_s);
            RETURN_IF_EXCEPTION(scope, false);
        }
        JSValue cls = findClass(names[0], names[1]);
        RETURN_IF_EXCEPTION(scope, false);
        PyTuple* arguments = popTuple(*mark);
        if (failed(scope, arguments))
            return false;
        JSValue object = instantiate(cls, arguments);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadNewObj(bool usesKeywords)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        ASCIILiteral opcode = usesKeywords ? "NEWOBJ_EX"_s : "NEWOBJ"_s;
        JSValue kwargs;
        if (usesKeywords) {
            kwargs = pop();
            if (failed(scope, kwargs))
                return false;
        }
        JSValue args = pop();
        if (failed(scope, args))
            return false;
        JSValue cls = pop();
        if (failed(scope, cls))
            return false;
        if (!PyType::is(cls))
            RELEASE_AND_RETURN(scope, fail(opcode, " class argument must be a type, not "_s, typeName(m_globalObject, cls)));
        if (!PyType::from(cls)->hasMaker())
            RELEASE_AND_RETURN(scope, fail(opcode, " class argument '"_s, typeName(m_globalObject, cls), "' doesn't have __new__"_s));
        auto* tuple = PyTuple::tryFrom(args);
        if (!tuple)
            RELEASE_AND_RETURN(scope, fail(opcode, " args argument must be a tuple, not "_s, typeName(m_globalObject, args)));
        if (usesKeywords && !isDict(kwargs))
            RELEASE_AND_RETURN(scope, fail(opcode, " kwargs argument must be a dict, not "_s, typeName(m_globalObject, kwargs)));

        // cls.__new__(cls, *args, **kwargs)
        JSValue newFunction = getAttr(m_globalObject, cls, m_rt.name(Special::New));
        RETURN_IF_EXCEPTION(scope, false);
        MarkedArgumentBuffer arguments;
        arguments.append(cls);
        for (unsigned i = 0; i < tuple->length(); ++i)
            arguments.append(tuple->at(i));
        JSValue object = callWithTupleAndDict(m_globalObject, newFunction, PyTuple::create(m_globalObject, spanOf(arguments)), usesKeywords ? PyDict::from(kwargs) : nullptr);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadGlobal()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue names[2];
        for (JSValue& name : names) {
            if (failed(scope, readLine()))
                return false;
            if (m_line.size() < 2)
                RELEASE_AND_RETURN(scope, truncated());
            name = decodeUTF8(m_line.span().first(m_line.size() - 1), "strict"_s);
            RETURN_IF_EXCEPTION(scope, false);
        }
        JSValue global = findClass(names[0], names[1]);
        RETURN_IF_EXCEPTION(scope, false);
        return push(global);
    }

    bool loadStackGlobal()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue globalName = pop();
        if (failed(scope, globalName))
            return false;
        JSValue moduleName = pop();
        if (failed(scope, moduleName))
            return false;
        if (!isExactStr(moduleName) || !isExactStr(globalName))
            RELEASE_AND_RETURN(scope, fail("STACK_GLOBAL requires str"_s));
        JSValue global = findClass(moduleName, globalName);
        RETURN_IF_EXCEPTION(scope, false);
        return push(global);
    }

    bool loadPersId()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLine()))
            return false;
        JSValue id = codecDecodeASCII(m_globalObject, m_line.span().first(m_line.size() - 1), "strict"_s);
        if (scope.exception()) {
            if (clearPendingExceptionIfMatches(m_globalObject, m_rt.UnicodeDecodeError()))
                RELEASE_AND_RETURN(scope, fail("persistent IDs in protocol 0 must be ASCII strings"_s));
            return false;
        }
        JSValue object = call(m_globalObject, m_persistentLoad, id);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadBinPersId()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue id = pop();
        if (failed(scope, id))
            return false;
        JSValue object = call(m_globalObject, m_persistentLoad, id);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadPop()
    {
        // A mark is on a stack of its own here. To the pickle it is on the one stack there is.
        if (!m_marks.isEmpty() && m_marks.last() == stackSize()) {
            m_marks.removeLast();
            m_fence = m_marks.isEmpty() ? 0 : m_marks.last();
            return true;
        }
        if (stackSize() <= m_fence)
            return underflow();
        m_stack.removeLast();
        return true;
    }

    bool loadPopMark()
    {
        auto mark = marker();
        if (!mark)
            return false;
        truncateStack(*mark);
        return true;
    }

    bool loadDup()
    {
        if (stackSize() <= m_fence)
            return underflow();
        return push(m_stack.last());
    }

    // The number on a line, for GET and PUT.
    bool readIndex(int64_t& index)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue key = intFromLine(m_line.span());
        RETURN_IF_EXCEPTION(scope, false);
        auto value = tryInt64(key);
        if (!value) {
            raiseOverflowError(m_globalObject, scope, "Python int too large to convert to C ssize_t"_s);
            return false;
        }
        index = *value;
        return true;
    }

    bool loadGet()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLineOfText()))
            return false;
        if (m_line.size() < 2)
            RELEASE_AND_RETURN(scope, truncated());
        int64_t index;
        if (failed(scope, readIndex(index)))
            return false;
        RELEASE_AND_RETURN(scope, memoGet(static_cast<uint64_t>(index)));
    }

    bool loadBinGet(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        RELEASE_AND_RETURN(scope, memoGet(binSize(bytes, sizeBytes)));
    }

    bool loadExtension(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        int64_t code = binInt(bytes, sizeBytes);
        if (code <= 0)
            RELEASE_AND_RETURN(scope, fail("EXT specifies code <= 0"_s));
        JSValue codeObject = jsNumber(static_cast<double>(code));
        auto* cache = PyDict::from(m_state->get(PickleStateExtensionCache));
        JSValue object = cache->get(m_globalObject, codeObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (object)
            return push(object);
        JSValue item = PyDict::from(m_state->get(PickleStateInvertedRegistry))->get(m_globalObject, codeObject);
        RETURN_IF_EXCEPTION(scope, false);
        if (!item) {
            raiseValueError(m_globalObject, scope, "unregistered extension code "_s, code);
            return false;
        }
        auto* pair = PyTuple::tryFrom(item);
        if (!pair || pair->length() != 2 || !isStr(pair->at(0)) || !isStr(pair->at(1))) {
            raiseValueError(m_globalObject, scope, "_inverted_registry["_s, code, "] isn't a 2-tuple of strings"_s);
            return false;
        }
        object = findClass(pair->at(0), pair->at(1));
        RETURN_IF_EXCEPTION(scope, false);
        cache->set(m_globalObject, codeObject, object);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadPut()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (failed(scope, readLineOfText()))
            return false;
        if (m_line.size() < 2)
            RELEASE_AND_RETURN(scope, truncated());
        if (stackSize() <= m_fence)
            RELEASE_AND_RETURN(scope, underflow());
        int64_t index;
        if (failed(scope, readIndex(index)))
            return false;
        if (index < 0) {
            raiseValueError(m_globalObject, scope, "negative PUT argument"_s);
            return false;
        }
        RELEASE_AND_RETURN(scope, memoPutTop(index));
    }

    bool loadBinPut(unsigned sizeBytes)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(sizeBytes);
        if (failed(scope, bytes))
            return false;
        if (stackSize() <= m_fence)
            RELEASE_AND_RETURN(scope, underflow());
        RELEASE_AND_RETURN(scope, memoPutTop(binSize(bytes, sizeBytes)));
    }

    bool loadMemoize()
    {
        if (stackSize() <= m_fence)
            return underflow();
        return memoPutTop(static_cast<uint64_t>(m_unpickler->get(UnpicklerMemoLength).asNumber()));
    }

    // Appends what is on the stack from `start` on to what is below it.
    bool doAppend(size_t start)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t size = stackSize();
        if (start > size || start <= m_fence)
            RELEASE_AND_RETURN(scope, underflow());
        JSValue list = m_stack.at(start - 1);
        if (isExactList(list)) {
            JSArray* array = asList(list);
            for (size_t i = start; i < size; ++i) {
                listAppend(m_globalObject, array, m_stack.at(i));
                RETURN_IF_EXCEPTION(scope, false);
            }
            truncateStack(start);
            return true;
        }
        JSValue extend = getAttrIfPresent(m_globalObject, list, internString(m_vm, "extend"_s));
        RETURN_IF_EXCEPTION(scope, false);
        if (extend) {
            JSArray* items = popList(start);
            RETURN_IF_EXCEPTION(scope, false);
            call(m_globalObject, extend, items);
            return !scope.exception();
        }
        JSValue append = getAttr(m_globalObject, list, internString(m_vm, "append"_s));
        RETURN_IF_EXCEPTION(scope, false);
        for (size_t i = start; i < size; ++i) {
            call(m_globalObject, append, m_stack.at(i));
            if (scope.exception())
                break;
        }
        truncateStack(start);
        return !scope.exception();
    }

    bool loadAppend()
    {
        if (stackSize() < 1 || stackSize() - 1 <= m_fence)
            return underflow();
        return doAppend(stackSize() - 1);
    }

    bool loadAppends()
    {
        auto mark = marker();
        return mark && doAppend(*mark);
    }

    bool doSetItems(int64_t start)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t size = stackSize();
        if (start > size || start <= static_cast<int64_t>(m_fence))
            RELEASE_AND_RETURN(scope, underflow());
        if ((size - start) % 2)
            RELEASE_AND_RETURN(scope, fail("odd number of items for SETITEMS"_s));
        JSValue dict = m_stack.at(start - 1);
        for (int64_t i = start + 1; i < size; i += 2) {
            setItem(m_globalObject, dict, m_stack.at(i - 1), m_stack.at(i));
            if (scope.exception())
                break;
        }
        truncateStack(start);
        return !scope.exception();
    }

    bool loadSetItems()
    {
        auto mark = marker();
        return mark && doSetItems(*mark);
    }

    bool loadAddItems()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto mark = marker();
        if (failed(scope, mark))
            return false;
        size_t size = stackSize();
        if (*mark > size || *mark <= m_fence)
            RELEASE_AND_RETURN(scope, underflow());
        JSValue set = m_stack.at(*mark - 1);
        if (auto* pySet = PySet::tryFrom(set); pySet && pySet->type()->isSubtypeOf(m_rt.setType())) {
            for (size_t i = *mark; i < size; ++i) {
                pySet->add(m_globalObject, m_stack.at(i));
                RETURN_IF_EXCEPTION(scope, false);
            }
            truncateStack(*mark);
            return true;
        }
        JSValue add = getAttr(m_globalObject, set, internString(m_vm, "add"_s));
        RETURN_IF_EXCEPTION(scope, false);
        for (size_t i = *mark; i < size; ++i) {
            call(m_globalObject, add, m_stack.at(i));
            if (scope.exception())
                break;
        }
        truncateStack(*mark);
        return !scope.exception();
    }

    bool loadBuild()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (static_cast<int64_t>(stackSize()) - 2 < static_cast<int64_t>(m_fence))
            RELEASE_AND_RETURN(scope, underflow());
        JSValue state = pop();
        if (failed(scope, state))
            return false;
        JSValue instance = m_stack.last();
        JSValue setState = getAttrIfPresent(m_globalObject, instance, m_rt.names.dSetState());
        RETURN_IF_EXCEPTION(scope, false);
        if (setState) {
            call(m_globalObject, setState, state);
            return !scope.exception();
        }

        // What __setstate__ would do if there were one. The state may be a pair: that of the __dict__ and that of the slots.
        JSValue slotState;
        if (auto* pair = PyTuple::tryFrom(state); pair && pair->length() == 2) {
            state = pair->at(0);
            slotState = pair->at(1);
        }
        if (!isNone(state)) {
            if (!isDict(state))
                RELEASE_AND_RETURN(scope, fail("state is not a dictionary"_s));
            JSValue dict = getAttr(m_globalObject, instance, m_rt.names.dDict());
            RETURN_IF_EXCEPTION(scope, false);
            JSValue key, value;
            for (unsigned position = 0; PyDict::from(state)->next(position, key, value);) {
                if (isExactStr(key)) {
                    key = internString(m_globalObject, asString(key));
                    RETURN_IF_EXCEPTION(scope, false);
                }
                setItem(m_globalObject, dict, key, value);
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
        if (slotState) {
            if (!isDict(slotState))
                RELEASE_AND_RETURN(scope, fail("slot state is not a dictionary"_s));
            JSValue key, value;
            for (unsigned position = 0; PyDict::from(slotState)->next(position, key, value);) {
                JSString* name = attributeName(m_globalObject, key);
                RETURN_IF_EXCEPTION(scope, false);
                setAttr(m_globalObject, instance, name, value);
                RETURN_IF_EXCEPTION(scope, false);
            }
        }
        return true;
    }

    bool loadMark()
    {
        m_marks.append(stackSize());
        m_fence = stackSize();
        return true;
    }

    bool loadReduce()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue arguments = pop();
        if (failed(scope, arguments))
            return false;
        JSValue callable = pop();
        if (failed(scope, callable))
            return false;
        auto* tuple = PyTuple::tryFrom(arguments);
        if (!tuple) {
            raiseTypeError(m_globalObject, scope, "argument list must be a tuple"_s);
            return false;
        }
        JSValue object = callWithTupleAndDict(m_globalObject, callable, tuple, nullptr);
        RETURN_IF_EXCEPTION(scope, false);
        return push(object);
    }

    bool loadProto()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(1);
        if (failed(scope, bytes))
            return false;
        if (*bytes > pickleHighestProtocol) {
            raiseValueError(m_globalObject, scope, "unsupported pickle protocol: "_s, static_cast<unsigned>(*bytes));
            return false;
        }
        setProto(*bytes);
        return true;
    }

    bool loadFrame()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        const uint8_t* bytes = read(8);
        if (failed(scope, bytes))
            return false;
        if (m_frameEnd >= 0) {
            if (m_next < m_frameEnd)
                RELEASE_AND_RETURN(scope, fail("beginning of a new frame before end of current frame"_s));
            endFrame();
        }
        int64_t length = binSize(bytes, 8);
        if (length < 0) {
            raiseOverflowError(m_globalObject, scope, "FRAME length exceeds system's maximum of "_s, std::numeric_limits<int64_t>::max(), " bytes"_s);
            return false;
        }
        if (failed(scope, read(length)))
            return false;
        // All of it is there. Nothing in it may reach beyond its end.
        m_next -= length;
        m_frameEnd = m_next + length;
        m_savedInputLength = m_inputLength;
        m_inputLength = m_frameEnd;
        return true;
    }

    bool dispatch(uint8_t op)
    {
        VM& vm = getVM(m_globalObject);
        auto scope = DECLARE_THROW_SCOPE(vm);
        switch (op) {
        case PickleOpNone:
            RELEASE_AND_RETURN(scope, push(jsUndefined()));
        case PickleOpBinInt:
        case PickleOpBinInt1:
        case PickleOpBinInt2: {
            unsigned size = op == PickleOpBinInt ? 4 : op == PickleOpBinInt1 ? 1 : 2;
            const uint8_t* bytes = read(size);
            if (failed(scope, bytes))
                return false;
            RELEASE_AND_RETURN(scope, push(jsNumber(static_cast<int32_t>(binInt(bytes, size)))));
        }
        case PickleOpInt:
            RELEASE_AND_RETURN(scope, loadInt());
        case PickleOpLong:
            RELEASE_AND_RETURN(scope, loadLong());
        case PickleOpLong1:
            RELEASE_AND_RETURN(scope, loadCountedLong(1));
        case PickleOpLong4:
            RELEASE_AND_RETURN(scope, loadCountedLong(4));
        case PickleOpFloat:
            RELEASE_AND_RETURN(scope, loadFloat());
        case PickleOpBinFloat:
            RELEASE_AND_RETURN(scope, loadBinFloat());
        case PickleOpShortBinBytes:
        case PickleOpBinBytes:
        case PickleOpBinBytes8:
            scope.release();
            return loadCountedBytes(op == PickleOpShortBinBytes ? 1 : op == PickleOpBinBytes ? 4 : 8, "BINBYTES"_s, [&](std::span<const uint8_t> data) -> JSValue {
                return PyBytes::create(m_globalObject, data);
            });
        case PickleOpByteArray8:
            scope.release();
            return loadCountedBytes(8, "BYTEARRAY8"_s, [&](std::span<const uint8_t> data) -> JSValue {
                return PyByteArray::create(m_globalObject, data);
            });
        case PickleOpNextBuffer:
            RELEASE_AND_RETURN(scope, loadNextBuffer());
        case PickleOpReadonlyBuffer:
            RELEASE_AND_RETURN(scope, loadReadonlyBuffer());
        case PickleOpShortBinString:
            RELEASE_AND_RETURN(scope, loadCountedBinString(1));
        case PickleOpBinString:
            RELEASE_AND_RETURN(scope, loadCountedBinString(4));
        case PickleOpString:
            RELEASE_AND_RETURN(scope, loadString());
        case PickleOpUnicode:
            RELEASE_AND_RETURN(scope, loadUnicode());
        case PickleOpShortBinUnicode:
            RELEASE_AND_RETURN(scope, loadCountedBinUnicode(1));
        case PickleOpBinUnicode:
            RELEASE_AND_RETURN(scope, loadCountedBinUnicode(4));
        case PickleOpBinUnicode8:
            RELEASE_AND_RETURN(scope, loadCountedBinUnicode(8));
        case PickleOpEmptyTuple:
            RELEASE_AND_RETURN(scope, loadCountedTuple(0));
        case PickleOpTuple1:
            RELEASE_AND_RETURN(scope, loadCountedTuple(1));
        case PickleOpTuple2:
            RELEASE_AND_RETURN(scope, loadCountedTuple(2));
        case PickleOpTuple3:
            RELEASE_AND_RETURN(scope, loadCountedTuple(3));
        case PickleOpTuple:
            RELEASE_AND_RETURN(scope, loadTuple());
        case PickleOpEmptyList:
            RELEASE_AND_RETURN(scope, push(newList(m_globalObject)));
        case PickleOpList:
            RELEASE_AND_RETURN(scope, loadList());
        case PickleOpEmptyDict:
            RELEASE_AND_RETURN(scope, push(PyDict::create(m_globalObject)));
        case PickleOpDict:
            RELEASE_AND_RETURN(scope, loadDict());
        case PickleOpEmptySet:
            RELEASE_AND_RETURN(scope, push(PySet::create(m_globalObject)));
        case PickleOpAddItems:
            RELEASE_AND_RETURN(scope, loadAddItems());
        case PickleOpFrozenSet:
            RELEASE_AND_RETURN(scope, loadFrozenSet());
        case PickleOpObj:
            RELEASE_AND_RETURN(scope, loadObj());
        case PickleOpInst:
            RELEASE_AND_RETURN(scope, loadInst());
        case PickleOpNewObj:
            RELEASE_AND_RETURN(scope, loadNewObj(false));
        case PickleOpNewObjEx:
            RELEASE_AND_RETURN(scope, loadNewObj(true));
        case PickleOpGlobal:
            RELEASE_AND_RETURN(scope, loadGlobal());
        case PickleOpStackGlobal:
            RELEASE_AND_RETURN(scope, loadStackGlobal());
        case PickleOpAppend:
            RELEASE_AND_RETURN(scope, loadAppend());
        case PickleOpAppends:
            RELEASE_AND_RETURN(scope, loadAppends());
        case PickleOpBuild:
            RELEASE_AND_RETURN(scope, loadBuild());
        case PickleOpDup:
            RELEASE_AND_RETURN(scope, loadDup());
        case PickleOpBinGet:
            RELEASE_AND_RETURN(scope, loadBinGet(1));
        case PickleOpLongBinGet:
            RELEASE_AND_RETURN(scope, loadBinGet(4));
        case PickleOpGet:
            RELEASE_AND_RETURN(scope, loadGet());
        case PickleOpMark:
            RELEASE_AND_RETURN(scope, loadMark());
        case PickleOpBinPut:
            RELEASE_AND_RETURN(scope, loadBinPut(1));
        case PickleOpLongBinPut:
            RELEASE_AND_RETURN(scope, loadBinPut(4));
        case PickleOpPut:
            RELEASE_AND_RETURN(scope, loadPut());
        case PickleOpMemoize:
            RELEASE_AND_RETURN(scope, loadMemoize());
        case PickleOpPop:
            RELEASE_AND_RETURN(scope, loadPop());
        case PickleOpPopMark:
            RELEASE_AND_RETURN(scope, loadPopMark());
        case PickleOpSetItem:
            RELEASE_AND_RETURN(scope, doSetItems(static_cast<int64_t>(stackSize()) - 2));
        case PickleOpSetItems:
            RELEASE_AND_RETURN(scope, loadSetItems());
        case PickleOpPersId:
            RELEASE_AND_RETURN(scope, loadPersId());
        case PickleOpBinPersId:
            RELEASE_AND_RETURN(scope, loadBinPersId());
        case PickleOpReduce:
            RELEASE_AND_RETURN(scope, loadReduce());
        case PickleOpProto:
            RELEASE_AND_RETURN(scope, loadProto());
        case PickleOpFrame:
            RELEASE_AND_RETURN(scope, loadFrame());
        case PickleOpExt1:
            RELEASE_AND_RETURN(scope, loadExtension(1));
        case PickleOpExt2:
            RELEASE_AND_RETURN(scope, loadExtension(2));
        case PickleOpExt4:
            RELEASE_AND_RETURN(scope, loadExtension(4));
        case PickleOpNewTrue:
            RELEASE_AND_RETURN(scope, push(jsBoolean(true)));
        case PickleOpNewFalse:
            RELEASE_AND_RETURN(scope, push(jsBoolean(false)));
        default:
            if (op >= 0x20 && op <= 0x7e && op != '\'' && op != '\\')
                RELEASE_AND_RETURN(scope, fail("invalid load key, '"_s, static_cast<char>(op), "'."_s));
            RELEASE_AND_RETURN(scope, fail("invalid load key, '\\x"_s, WTF::hex(op, 2, WTF::Lowercase), "'."_s));
        }
    }

    JSGlobalObject* m_globalObject;
    Runtime& m_rt;
    VM& m_vm;
    PyRecord* m_unpickler;
    PyRecord* m_state;
    JSValue m_read;
    JSValue m_persistentLoad;
    bool m_callsFindClassDirectly { false };

    JSValue m_inputOwner;
    const uint8_t* m_input { nullptr };
    int64_t m_next { 0 };
    int64_t m_inputLength { 0 }; // No further than the end of the frame, while there is one.
    int64_t m_frameEnd { -1 };
    int64_t m_savedInputLength { 0 };
    ByteBuilderWithRoom<64> m_line;

    MarkedArgumentBuffer m_stack;
    Vector<size_t, 16> m_marks;
    size_t m_fence { 0 }; // Where the topmost mark is: nothing but what takes the mark off reaches below.
};

static JSValue unpicklerNew(JSGlobalObject* globalObject, JSValue cls, const CallArgs&) { return unpicklerCreate(globalObject, PyType::from(cls)); }

// fix_imports, encoding, errors, buffers
static bool unpicklerSetOptions(JSGlobalObject* globalObject, ASCIILiteral function, PyRecord* unpickler, const JSValue* values)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSString* encoding = pickleStringArgument(globalObject, function, "encoding"_s, values[1], "ASCII"_s);
    RETURN_IF_EXCEPTION(scope, false);
    unpickler->set(vm, UnpicklerEncoding, encoding);
    JSString* errors = pickleStringArgument(globalObject, function, "errors"_s, values[2], "strict"_s);
    RETURN_IF_EXCEPTION(scope, false);
    unpickler->set(vm, UnpicklerErrors, errors);
    RELEASE_AND_RETURN(scope, unpicklerSetBuffers(globalObject, unpickler, values[3]));
}

// Converts the options, in the order of the parameters, before anything is done with any.
static bool unpicklerCheckOptions(JSGlobalObject* globalObject, ASCIILiteral function, const JSValue* values, bool& fixImports)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    fixImports = !values[0] || isTrue(globalObject, values[0]);
    RETURN_IF_EXCEPTION(scope, false);
    pickleStringArgument(globalObject, function, "encoding"_s, values[1], "ASCII"_s);
    RETURN_IF_EXCEPTION(scope, false);
    RELEASE_AND_RETURN(scope, !!pickleStringArgument(globalObject, function, "errors"_s, values[2], "strict"_s));
}

static JSValue unpicklerInit(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    static constexpr ASCIILiteral keywords[] = { "file"_s, "fix_imports"_s, "encoding"_s, "errors"_s, "buffers"_s };
    JSValue values[5];
    if (failed(scope, unpackKeywordArguments(globalObject, "Unpickler"_s, keywords, 1, 1, false, args, values)))
        return { };
    bool fixImports;
    if (failed(scope, unpicklerCheckOptions(globalObject, "Unpickler"_s, values + 1, fixImports)))
        return { };

    auto* unpickler = PyRecord::from(self);
    if (unpickler->number & UnpicklerIsRunning)
        return raiseRuntimeError(globalObject, scope, "Unpickler object is already used"_s);
    PickleRunningScope running(unpickler, UnpicklerIsRunning);
    if (unpickler->get(UnpicklerRead)) {
        for (unsigned field : { UnpicklerRead, UnpicklerReadLine, UnpicklerPersistentLoadAttr, UnpicklerBuffers, UnpicklerInput })
            unpickler->set(vm, field, { });
    }
    if (failed(scope, unpicklerSetInputStream(globalObject, unpickler, values[0])) || failed(scope, unpicklerSetOptions(globalObject, "Unpickler"_s, unpickler, values + 1)))
        return { };
    unpickler->number = UnpicklerIsRunning | (fixImports ? UnpicklerFixImports : 0);
    unpicklerClearMemo(globalObject, unpickler, 32);
    return jsUndefined();
}

static JSValue unpicklerLoad(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, picklePositionalOnly(globalObject, "load"_s, args, 0)))
        return { };
    auto* unpickler = PyRecord::from(self);
    if (!unpickler->get(UnpicklerRead)) {
        scope.release();
        pickleRaise(globalObject, PickleStateUnpicklingError, "Unpickler.__init__() was not called by "_s, typeName(globalObject, self), ".__init__()"_s);
        return { };
    }
    if (unpickler->number & UnpicklerIsRunning)
        return raiseRuntimeError(globalObject, scope, "Unpickler object is already used"_s);
    PickleRunningScope running(unpickler, UnpicklerIsRunning);
    UnpicklerRun run(globalObject, unpickler);
    RELEASE_AND_RETURN(scope, run.load());
}

static JSValue unpicklerPersistentLoad(JSGlobalObject* globalObject, JSValue, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, picklePositionalOnly(globalObject, "persistent_load"_s, args, 1)))
        return { };
    scope.release();
    pickleRaise(globalObject, PickleStateUnpicklingError, "A load persistent id instruction was encountered, but no persistent_load function was specified."_s);
    return { };
}

static JSValue unpicklerFindClass(JSGlobalObject* globalObject, JSValue self, JSValue moduleName, JSValue globalName)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* unpickler = PyRecord::from(self);
    int proto = static_cast<int>(unpickler->number & 0xff);
    if (failed(scope, audit(globalObject, "pickle.find_class"_s, moduleName, globalName)))
        return { };

    // What Python 2 called otherwise.
    if (proto < 3 && (unpickler->number & UnpicklerFixImports)) {
        JSValue item = pickleStateDict(globalObject, PickleStateNameMapping2to3)->get(globalObject, PyTuple::create(globalObject, { moduleName, globalName }));
        RETURN_IF_EXCEPTION(scope, { });
        if (item) {
            auto* pair = PyTuple::tryFrom(item);
            if (!pair || pair->length() != 2)
                return raiseRuntimeError(globalObject, scope, "_compat_pickle.NAME_MAPPING values should be 2-tuples, not "_s, typeName(globalObject, item));
            moduleName = pair->at(0);
            globalName = pair->at(1);
            if (!isStr(moduleName) || !isStr(globalName))
                return raiseRuntimeError(globalObject, scope, "_compat_pickle.NAME_MAPPING values should be pairs of str, not ("_s, typeName(globalObject, moduleName), ", "_s, typeName(globalObject, globalName), ')');
        } else {
            item = pickleStateDict(globalObject, PickleStateImportMapping2to3)->get(globalObject, moduleName);
            RETURN_IF_EXCEPTION(scope, { });
            if (item) {
                if (!isStr(item))
                    return raiseRuntimeError(globalObject, scope, "_compat_pickle.IMPORT_MAPPING values should be strings, not "_s, typeName(globalObject, item));
                moduleName = item;
            }
        }
    }

    JSValue module = pickleImport(globalObject, moduleName);
    RETURN_IF_EXCEPTION(scope, { });
    if (proto < 4) {
        JSString* name = attributeName(globalObject, globalName);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, getAttr(globalObject, module, name));
    }
    if (!isStr(globalName))
        return raiseTypeError(globalObject, scope, "must be str, not "_s, typeName(globalObject, globalName));
    String nameText = strValue(globalName)->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    Vector<String> path = pickleDottedPath(nameText);
    JSValue global = pickleGetAttribute(globalObject, module, path, true);
    if (scope.exception() && path.size() > 1) {
        scope.release();
        pickleReplaceException(globalObject, rt.AttributeError(), [&](JSValue) -> String {
            String name = pickleRepr(globalObject, globalName);
            if (failed(scope, !name.isNull()))
                return String();
            String moduleText = pickleRepr(globalObject, moduleName);
            if (failed(scope, !moduleText.isNull()))
                return String();
            return makeString("Can't resolve path "_s, name, " on module "_s, moduleText);
        });
        return { };
    }
    return global;
}

static JSValue unpicklerFindClassMethod(JSGlobalObject* globalObject, JSValue self, const CallArgs& args)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (failed(scope, picklePositionalOnly(globalObject, "find_class"_s, args, 2)))
        return { };
    RELEASE_AND_RETURN(scope, unpicklerFindClass(globalObject, self, args[0], args[1]));
}

static JSValue unpicklerSizeOf(JSGlobalObject*, JSValue self)
{
    return jsNumber(sizeof(PyRecord) + UnpicklerFieldCount * sizeof(JSValue) + PyRecord::from(self)->get(UnpicklerMemoSize).asNumber() * sizeof(JSValue));
}

static JSValue unpicklerGetAttribute(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    if (pickleIsName(name, "persistent_load"_s)) {
        if (JSValue value = PyRecord::from(self)->get(UnpicklerPersistentLoadAttr))
            return value;
    }
    return pickleGenericGetAttr(globalObject, self, name);
}

static JSValue unpicklerSetAttr(JSGlobalObject* globalObject, JSValue self, JSValue name, JSValue value)
{
    if (pickleIsName(name, "persistent_load"_s)) {
        PyRecord::from(self)->set(getVM(globalObject), UnpicklerPersistentLoadAttr, value);
        return jsUndefined();
    }
    return pickleGenericSetAttr(globalObject, self, name, value);
}

static JSValue unpicklerDelAttr(JSGlobalObject* globalObject, JSValue self, JSValue name)
{
    if (pickleIsName(name, "persistent_load"_s)) {
        PyRecord::from(self)->set(getVM(globalObject), UnpicklerPersistentLoadAttr, { });
        return jsUndefined();
    }
    return pickleGenericSetAttr(globalObject, self, name, { });
}

// ---- Unpickler.memo ----

static JSValue unpicklerGetMemo(JSGlobalObject* globalObject, JSValue self)
{
    PyRecord* proxy = PyRecord::create(globalObject, pickleStateType(globalObject, PickleStateUnpicklerMemoProxyType), PickleProxyFieldCount);
    proxy->set(getVM(globalObject), PickleProxyOwner, self);
    return proxy;
}

// {number: object}
static JSValue unpicklerMemoProxyCopy(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* unpickler = PyRecord::from(PyRecord::from(self)->get(PickleProxyOwner));
    auto* memo = asList(unpickler->get(UnpicklerMemo));
    PyDict* result = PyDict::create(globalObject);
    for (unsigned i = 0; i < memo->length(); ++i) {
        JSValue value = memo->getDirectIndex(globalObject, i);
        RETURN_IF_EXCEPTION(scope, { });
        if (!value)
            continue;
        result->set(globalObject, jsNumber(i), value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    return result;
}

static void unpicklerSetMemo(JSGlobalObject* globalObject, JSValue self, JSValue value)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* unpickler = PyRecord::from(self);
    if (!value) {
        raiseTypeError(globalObject, scope, "attribute deletion is not supported"_s);
        return;
    }
    PyType* proxyType = pickleStateType(globalObject, PickleStateUnpicklerMemoProxyType);
    JSValue contents = value;
    if (PyRecord* proxy = PyRecord::tryFrom(value, proxyType); proxy && proxy->type() == proxyType) {
        contents = unpicklerMemoProxyCopy(globalObject, value);
        RETURN_IF_EXCEPTION(scope, void());
    } else if (!isDict(value)) {
        raiseTypeError(globalObject, scope, "'memo' attribute must be an UnpicklerMemoProxy object or dict, not "_s, typeName(globalObject, value));
        return;
    }
    // Made aside, so that an error leaves the memo as it was.
    PyRecord* scratch = PyRecord::create(globalObject, runtime(globalObject).objectType(), UnpicklerFieldCount);
    unpicklerClearMemo(globalObject, scratch, std::max(32u, PyDict::from(contents)->size()));
    JSValue key, item;
    for (unsigned position = 0; PyDict::from(contents)->next(position, key, item);) {
        if (!isInt(key)) {
            raiseTypeError(globalObject, scope, "memo key must be integers"_s);
            return;
        }
        auto index = tryInt64(intValue(key));
        if (!index) {
            raiseOverflowError(globalObject, scope, "Python int too large to convert to C ssize_t"_s);
            return;
        }
        if (*index < 0) {
            raiseValueError(globalObject, scope, "memo key must be positive integers."_s);
            return;
        }
        unpicklerMemoPut(globalObject, scratch, *index, item);
        RETURN_IF_EXCEPTION(scope, void());
    }
    for (unsigned field : { UnpicklerMemo, UnpicklerMemoDict, UnpicklerMemoLength, UnpicklerMemoSize })
        unpickler->set(vm, field, scratch->get(field));
}

static JSValue unpicklerMemoProxyClear(JSGlobalObject* globalObject, JSValue self)
{
    auto* unpickler = PyRecord::from(PyRecord::from(self)->get(PickleProxyOwner));
    unpicklerClearMemo(globalObject, unpickler, static_cast<unsigned>(unpickler->get(UnpicklerMemoSize).asNumber()));
    return jsUndefined();
}

static constexpr NativeDef unpicklerMemoProxyMethods[] = {
    PY_METHOD("clear", unpicklerMemoProxyClear, "($self, /)"),
    PY_METHOD("copy", unpicklerMemoProxyCopy, "($self, /)"),
    PY_METHOD("__reduce__", pickleMemoProxyReduce<unpicklerMemoProxyCopy>, "($self, /)"),
};

static constexpr TypeDef unpicklerMemoProxyTypeDef {
    "_pickle.UnpicklerMemoProxy"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable, TypeFlag::DisallowInstantiation }, unpicklerMemoProxyMethods, { }, { }
};

static constexpr NativeDef unpicklerMethods[] = {
    PY_NEW(unpicklerNew, "($type, *args, **kwargs)"),
    PY_SLOT("__init__", unpicklerInit, "($self, *args, **kwargs)"),
    PY_SLOT("__getattribute__", unpicklerGetAttribute, "($self, name, /)"),
    PY_SLOT("__setattr__", unpicklerSetAttr, "($self, name, value, /)"),
    PY_SLOT("__delattr__", unpicklerDelAttr, "($self, name, /)"),
    PY_METHOD("persistent_load", unpicklerPersistentLoad, "!($self, pid, /)"),
    PY_METHOD("load", unpicklerLoad, "!($self, /)"),
    PY_METHOD("find_class", unpicklerFindClassMethod, "!($self, module_name, global_name, /)"),
    PY_METHOD("__sizeof__", unpicklerSizeOf, "($self, /)"),
};

static constexpr NativeGetSet unpicklerGetSets[] = {
    { "memo"_s, unpicklerGetMemo, unpicklerSetMemo },
};

static constexpr TypeDef unpicklerTypeDef {
    "_pickle.Unpickler"_s, { TypeFlag::Base, TypeFlag::OwnLayout, TypeFlag::Immutable }, unpicklerMethods, unpicklerGetSets,
    "This takes a binary file for reading a pickle data stream.\n\n"
    "The protocol version of the pickle is detected automatically, so no\n"
    "protocol argument is needed.  Bytes past the pickled object's\n"
    "representation are ignored.\n\n"
    "The argument *file* must have two methods, a read() method that takes\n"
    "an integer argument, and a readline() method that requires no\n"
    "arguments.  Both methods should return bytes.  Thus *file* can be a\n"
    "binary file object opened for reading, an io.BytesIO object, or any\n"
    "other custom object that meets this interface.\n\n"
    "Optional keyword arguments are *fix_imports*, *encoding* and *errors*,\n"
    "which are used to control compatibility support for pickle stream\n"
    "generated by Python 2.  If *fix_imports* is True, pickle will try to\n"
    "map the old Python 2 names to the new names used in Python 3.  The\n"
    "*encoding* and *errors* tell pickle how to decode 8-bit string\n"
    "instances pickled by Python 2; these default to 'ASCII' and 'strict',\n"
    "respectively.  The *encoding* can be 'bytes' to read these 8-bit\n"
    "string instances as bytes objects."_s
};

// ---- The functions of the module ----

static JSValue pickleDump(JSGlobalObject* globalObject, JSValue object, JSValue file, JSValue protocol, JSValue fixImportsValue, JSValue bufferCallback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool fixImports = !fixImportsValue || isTrue(globalObject, fixImportsValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* pickler = picklerCreate(globalObject, pickleStateType(globalObject, PickleStatePicklerType));
    if (failed(scope, picklerSetProtocol(globalObject, pickler, protocol, fixImports))
        || failed(scope, picklerSetOutputStream(globalObject, pickler, file))
        || failed(scope, picklerSetBufferCallback(globalObject, pickler, bufferCallback)))
        return { };
    PicklerRun run(globalObject, pickler);
    run.dump(object);
    RETURN_IF_EXCEPTION(scope, { });
    run.flushToFile();
    RETURN_IF_EXCEPTION(scope, { });
    return jsUndefined();
}

static JSValue pickleDumps(JSGlobalObject* globalObject, JSValue object, JSValue protocol, JSValue fixImportsValue, JSValue bufferCallback)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    bool fixImports = !fixImportsValue || isTrue(globalObject, fixImportsValue);
    RETURN_IF_EXCEPTION(scope, { });
    PyRecord* pickler = picklerCreate(globalObject, pickleStateType(globalObject, PickleStatePicklerType));
    if (failed(scope, picklerSetProtocol(globalObject, pickler, protocol, fixImports)) || failed(scope, picklerSetBufferCallback(globalObject, pickler, bufferCallback)))
        return { };
    PicklerRun run(globalObject, pickler);
    run.dump(object);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, run.takeBytes());
}

static JSValue pickleLoad(JSGlobalObject* globalObject, JSValue file, JSValue fixImportsValue, JSValue encoding, JSValue errors, JSValue buffers)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue options[] = { fixImportsValue, encoding, errors, buffers };
    bool fixImports;
    if (failed(scope, unpicklerCheckOptions(globalObject, "load"_s, options, fixImports)))
        return { };
    PyRecord* unpickler = unpicklerCreate(globalObject, pickleStateType(globalObject, PickleStateUnpicklerType));
    if (failed(scope, unpicklerSetInputStream(globalObject, unpickler, file)) || failed(scope, unpicklerSetOptions(globalObject, "load"_s, unpickler, options)))
        return { };
    unpickler->number = fixImports ? UnpicklerFixImports : 0;
    UnpicklerRun run(globalObject, unpickler);
    RELEASE_AND_RETURN(scope, run.load());
}

static JSValue pickleLoads(JSGlobalObject* globalObject, JSValue data, JSValue fixImportsValue, JSValue encoding, JSValue errors, JSValue buffers)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue options[] = { fixImportsValue, encoding, errors, buffers };
    bool fixImports;
    if (failed(scope, unpicklerCheckOptions(globalObject, "loads"_s, options, fixImports)))
        return { };
    PyRecord* unpickler = unpicklerCreate(globalObject, pickleStateType(globalObject, PickleStateUnpicklerType));
    BufferHolder holder(globalObject);
    if (failed(scope, holder.get(data, BufferContigRO)))
        return { };
    if (failed(scope, unpicklerSetOptions(globalObject, "loads"_s, unpickler, options)))
        return { };
    unpickler->number = fixImports ? UnpicklerFixImports : 0;
    UnpicklerRun run(globalObject, unpickler);
    run.setInput(holder.buffer.span());
    RELEASE_AND_RETURN(scope, run.load());
}

static constexpr NativeDef pickleFunctions[] = {
    PY_FUNCTION("dump", pickleDump, "(obj, file, protocol=None, *, fix_imports=True, buffer_callback=None)"),
    PY_FUNCTION("dumps", pickleDumps, "(obj, protocol=None, *, fix_imports=True, buffer_callback=None)"),
    PY_FUNCTION("load", pickleLoad, "(file, *, fix_imports=True, encoding='ASCII', errors='strict', buffers=())"),
    PY_FUNCTION("loads", pickleLoads, "(data, /, *, fix_imports=True, encoding='ASCII', errors='strict', buffers=())"),
};

static constexpr TypeDef pickleErrorTypeDef { "_pickle.PickleError"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };
static constexpr TypeDef picklingErrorTypeDef { "_pickle.PicklingError"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };
static constexpr TypeDef unpicklingErrorTypeDef { "_pickle.UnpicklingError"_s, { TypeFlag::Base, TypeFlag::Mutable, TypeFlag::HasWeakref }, { }, { }, { } };

void initializePickleModule(JSGlobalObject* globalObject, PyModule* module)
{
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    auto scope = DECLARE_THROW_SCOPE(vm);

    PyRecord* state = PyRecord::create(globalObject, rt.objectType(), PickleStateFieldCount);
    rt.setSlot(Runtime::Slot::Pickle, state);
    PyType* pickleError = rt.typeFor(pickleErrorTypeDef, rt.Exception());
    state->set(vm, PickleStatePickleError, pickleError);
    state->set(vm, PickleStatePicklingError, rt.typeFor(picklingErrorTypeDef, pickleError));
    state->set(vm, PickleStateUnpicklingError, rt.typeFor(unpicklingErrorTypeDef, pickleError));
    state->set(vm, PickleStatePicklerType, rt.typeFor(picklerTypeDef));
    state->set(vm, PickleStateUnpicklerType, rt.typeFor(unpicklerTypeDef));
    state->set(vm, PickleStatePicklerMemoProxyType, rt.typeFor(picklerMemoProxyTypeDef));
    state->set(vm, PickleStateUnpicklerMemoProxyType, rt.typeFor(unpicklerMemoProxyTypeDef));
    for (PickleStateField field : { PickleStatePicklerMemoProxyType, PickleStateUnpicklerMemoProxyType }) {
        PyType* proxyType = PyType::from(state->get(field));
        proxyType->dict()->set(vm, rt.name(Special::Hash), jsUndefined());
        proxyType->didChange(vm);
    }

    module->add(globalObject, "__doc__"_s, makeStr(vm, "Optimized C implementation for the Python pickle module."_s));
    module->addFunctions(globalObject, pickleFunctions);
    module->add(globalObject, "PickleBuffer"_s, rt.typeFor(pickleBufferTypeDef));
    RETURN_IF_EXCEPTION(scope, void());
    module->add(globalObject, "Pickler"_s, state->get(PickleStatePicklerType));
    module->add(globalObject, "Unpickler"_s, state->get(PickleStateUnpicklerType));
    module->add(globalObject, "PickleError"_s, pickleError);
    module->add(globalObject, "PicklingError"_s, state->get(PickleStatePicklingError));
    module->add(globalObject, "UnpicklingError"_s, state->get(PickleStateUnpicklingError));

    state->set(vm, PickleStateGetAttr, builtinNamed(globalObject, "getattr"_s));
    RETURN_IF_EXCEPTION(scope, void());
    struct Import {
        ASCIILiteral module;
        ASCIILiteral name;
        PickleStateField field;
        bool isDict;
    };
    static constexpr Import imports[] = {
        { "copyreg"_s, "dispatch_table"_s, PickleStateDispatchTable, true },
        { "copyreg"_s, "_extension_registry"_s, PickleStateExtensionRegistry, true },
        { "copyreg"_s, "_inverted_registry"_s, PickleStateInvertedRegistry, true },
        { "copyreg"_s, "_extension_cache"_s, PickleStateExtensionCache, true },
        { "_compat_pickle"_s, "NAME_MAPPING"_s, PickleStateNameMapping2to3, true },
        { "_compat_pickle"_s, "IMPORT_MAPPING"_s, PickleStateImportMapping2to3, true },
        { "_compat_pickle"_s, "REVERSE_NAME_MAPPING"_s, PickleStateNameMapping3to2, true },
        { "_compat_pickle"_s, "REVERSE_IMPORT_MAPPING"_s, PickleStateImportMapping3to2, true },
        { "codecs"_s, "encode"_s, PickleStateCodecsEncode, false },
        { "functools"_s, "partial"_s, PickleStatePartial, false },
    };
    for (const Import& import : imports) {
        JSValue imported = importModule(globalObject, import.module);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue value = getAttr(globalObject, imported, internString(vm, import.name));
        RETURN_IF_EXCEPTION(scope, void());
        if (import.isDict && typeOf(globalObject, value) != rt.dictType()) {
            raiseRuntimeError(globalObject, scope, import.module, '.', import.name, " should be a dict, not "_s, typeName(globalObject, value));
            return;
        }
        state->set(vm, import.field, value);
    }
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
