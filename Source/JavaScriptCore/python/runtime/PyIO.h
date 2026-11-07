/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAbstract.h"
#include "PyBuffer.h"
#include "PyBytes.h"
#include "PyErrors.h"
#include "PyException.h"
#include "PyInternals.h"
#include "PyList.h"
#include "PyModule.h"
#include "PyNumbers.h"
#include "PyStr.h"
#include "PyThreads.h"
#include "PyTuple.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {

// The module _io. (Modules/_io in CPython is the reference.)
//
//   PyIOBase.cpp      the module, open(), and the abstract classes
//   PyFileIO.cpp      FileIO
//   PyBufferedIO.cpp  BufferedReader, BufferedWriter, BufferedRandom, BufferedRWPair
//   PyBytesIO.cpp     BytesIO
//   PyStringIO.cpp    StringIO
//   PyTextIO.cpp      IncrementalNewlineDecoder, TextIOWrapper

// ---- For the interpreter ----

// sys.stdin, sys.stdout or sys.stderr, over a file descriptor that stays open when the stream is closed: a TextIOWrapper, in
// UTF-8, over a BufferedReader or BufferedWriter over a FileIO. None if the descriptor is not open. Empty: an exception.
// Neither it nor its buffer is ever finalized, since whatever runs last may still have something to print.
JSValue makeStandardStream(JSGlobalObject*, int fd, bool writable, ASCIILiteral name, ASCIILiteral errors);
// For when the interpreter exits. Runtime::finalizeAllAtExit() closes, and so flushes, every other stream that is still open,
// a stream before the one it wraps. This flushes the standard streams, whatever sys.stdout and sys.stderr are by now.
void flushAllOpenFiles(JSGlobalObject*);
// For when something other than Python is about to write to the same descriptors: what print() has left in the buffers of the
// standard streams is written first, so that the output of a program in two languages is in the order it was made.
void flushStandardStreamsIfPending(JSGlobalObject*);
bool textIOHasPendingOutput(PyRecord*);
bool bufferedHasPendingOutput(PyRecord*);
// Whether a thread, which may be this one, is in the middle of an operation on it.
bool bufferedIsBusy(PyRecord*);
// For writeOutPendingOutput(): a TextIOWrapper, BufferedWriter or BufferedRandom, none of a class derived from those, over a FileIO
// itself, so that nothing of the program's runs when it is written out. The list keeps it alive until then.
void ioNotePendingOutput(JSGlobalObject*, PyRecord*);
// For PyTemporaries.cpp. A FileIO, BufferedReader, BufferedWriter, BufferedRandom or TextIOWrapper, none of a class derived from those.
bool ioIsOfTypeThatOpenMakes(JSGlobalObject*, JSCell*);
// What the collector would do when it found that nothing refers to the stream, and will not do again. An exception that is pending stays.
void ioFinalizeNow(JSGlobalObject*, PyRecord*);

// A method that keeps nothing of the stream it is called on (NativeFlag::KeepsNothing).
template<auto function> struct StreamUse;
template<typename... Arguments, JSValue (*function)(JSGlobalObject*, JSValue, Arguments...)>
struct StreamUse<function> {
    static JSValue call(JSGlobalObject* globalObject, JSValue self, Arguments... arguments)
    {
        JSValue result = function(globalObject, self, arguments...);
        if (Runtime& rt = runtime(globalObject); rt.ownsTemporaries()) [[unlikely]]
            rt.didCallMethod(self, result, false, reinterpret_cast<void*>(&call));
        return result;
    }
};
#define PY_STREAM_METHOD(name, function, signature) PY_METHOD_WITH_FLAGS(name, StreamUse<function>::call, signature, KeepsNothing)
void textIOWriteOutPending(JSGlobalObject*, PyRecord*);
void bufferedWriteOutPending(JSGlobalObject*, PyRecord*);

// ---- The types, and the names of the attributes the module looks up ----

static constexpr int64_t ioDefaultBufferSize = 128 * 1024;

#define FOR_EACH_IO_TYPE(v) \
    v(IOBase) \
    v(RawIOBase) \
    v(BufferedIOBase) \
    v(TextIOBase) \
    v(FileIO) \
    v(BytesIO) \
    v(BytesIOBuffer) \
    v(StringIO) \
    v(BufferedReader) \
    v(BufferedWriter) \
    v(BufferedRWPair) \
    v(BufferedRandom) \
    v(IncrementalNewlineDecoder) \
    v(TextIOWrapper) \
    v(BuiltinDecoder)

#define FOR_EACH_IO_NAME(v) \
    v(blksize, "_blksize") \
    v(closed, "closed") \
    v(deallocWarn, "_dealloc_warn") \
    v(decode, "decode") \
    v(encode, "encode") \
    v(fileno, "fileno") \
    v(finalizing, "_finalizing") \
    v(getstate, "getstate") \
    v(ioBaseClosed, "__IOBase_closed") \
    v(isatty, "isatty") \
    v(isattyOpenOnly, "_isatty_open_only") \
    v(mode, "mode") \
    v(newlines, "newlines") \
    v(peek, "peek") \
    v(raw, "raw") \
    v(read1, "read1") \
    v(readable, "readable") \
    v(readall, "readall") \
    v(readinto, "readinto") \
    v(readinto1, "readinto1") \
    v(reset, "reset") \
    v(seek, "seek") \
    v(seekable, "seekable") \
    v(setstate, "setstate") \
    v(tell, "tell") \
    v(truncate, "truncate") \
    v(writable, "writable")

enum class IOType : uint8_t {
#define PY_DEFINE_ENUMERATOR(id) id,
    FOR_EACH_IO_TYPE(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
    UnsupportedOperation,
    Count,
};

enum class IOName : uint8_t {
#define PY_DEFINE_ENUMERATOR(id, text) id,
    FOR_EACH_IO_NAME(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
    Count,
};

#define PY_DECLARE_TYPE_DEF(id) extern const TypeDef io##id##TypeDef;
FOR_EACH_IO_TYPE(PY_DECLARE_TYPE_DEF)
#undef PY_DECLARE_TYPE_DEF

// A tuple of the types, then the names, then a list for flushAllOpenFiles(), then one for writeOutPendingOutput(). The type _IOBase
// holds it.
PyTuple* ioMakeState(JSGlobalObject*);
ALWAYS_INLINE PyTuple* ioState(JSGlobalObject* globalObject)
{
    JSValue state = hiddenField(globalObject, runtime(globalObject).typeFor(ioIOBaseTypeDef), 0);
    if (state) [[likely]]
        return uncheckedDowncast<PyTuple>(state.asCell());
    return ioMakeState(globalObject);
}
ALWAYS_INLINE PyType* ioType(JSGlobalObject* globalObject, IOType type) { return uncheckedDowncast<PyType>(ioState(globalObject)->at(static_cast<unsigned>(type)).asCell()); }
ALWAYS_INLINE JSString* ioName(JSGlobalObject* globalObject, IOName name) { return asString(ioState(globalObject)->at(static_cast<unsigned>(IOType::Count) + static_cast<unsigned>(name))); }

// The record, if the value is an instance of that very type.
ALWAYS_INLINE PyRecord* ioTryExact(JSGlobalObject* globalObject, JSValue value, IOType type)
{
    auto* record = PyRecord::tryFrom(value);
    return record && record->type() == ioType(globalObject, type) ? record : nullptr;
}

// ---- Instances ----
//
// An instance of one of the concrete types is a record. What it keeps that is not a value is a C++ struct in a cell of its
// own, in the first field.

template<typename State>
ALWAYS_INLINE State& ioStateOf(PyRecord* record)
{
    return *reinterpret_cast<State*>(uncheckedDowncast<PyByteStorage>(record->get(0).asCell())->data());
}

template<typename State>
PyRecord* ioAllocate(JSGlobalObject* globalObject, JSValue cls, unsigned fieldCount)
{
    static_assert(std::is_trivially_destructible_v<State>);
    Runtime& rt = runtime(globalObject);
    VM& vm = rt.vm();
    PyByteStorage* storage = PyByteStorage::create(vm, rt.structure<PyByteStorage>(), sizeof(State));
    new (NotNull, storage->data()) State;
    PyRecord* record = PyRecord::create(vm, rt.structure<PyRecord>(), PyType::from(cls), fieldCount);
    record->set(vm, 0, storage);
    return record;
}

// ---- Finalization ----
//
// A stream nothing refers to any more is closed, by _IOBase.__del__, which takes its being registered with the runtime.
// One that is closed has no need of that, and one that another stream wraps is closed by that stream, which must have flushed
// what it holds first. So a stream is registered while it is open and nothing wraps it... unless its class has a __del__ of its
// own, which is to be called whatever happens, and in which this module has no say.

// In __new__.
void ioDidAllocate(JSGlobalObject*, PyRecord*);
void ioDidOpen(JSGlobalObject*, PyRecord*);
void ioDidClose(JSGlobalObject*, PyRecord*);
// `stream`, which may be any object, is or is no longer what another stream reads from and writes to.
void ioDidWrap(JSGlobalObject*, JSValue stream);
void ioDidUnwrap(JSGlobalObject*, JSValue stream);
// For close() of a stream that wraps others, once it has called theirs, with what came of that still pending. False: the host has ended it. Then
// the stream is still to be closed, and gives nothing up. Otherwise it is closed (ioDidClose()), and if theirs `hasRaised`, those that
// are still open are on their own again.
bool ioDidCloseWrapped(JSGlobalObject*, PyRecord* wrapper, bool hasRaised);
// The stream that a BufferedReader and the like, or a TextIOWrapper, wraps. Empty: none.
JSValue bufferedWrappedStream(PyRecord*);
JSValue textIOWrappedStream(PyRecord*);
// The BufferedReader and the BufferedWriter of a BufferedRWPair.
std::pair<JSValue, JSValue> bufferedRWPairWrappedStreams(PyRecord*);

// ---- Errors ----

JSValue ioRaiseUnsupported(JSGlobalObject*, ThrowScope&, const String& message);
// "I/O operation on closed file."
JSValue ioRaiseClosed(JSGlobalObject*, ThrowScope&);
// The exception propagates again, as it is.
void ioRestoreException(JSGlobalObject*, JSValue exception);
// After a second step that was taken although the first one failed with `saved` (empty: it did not): if the second failed too,
// its exception propagates, with `saved` as its context; otherwise `saved` does.
void ioChainExceptions(JSGlobalObject*, JSValue saved);
// Whether what propagates is an OSError for EINTR, which then propagates no more: the caller tries again.
bool ioTrapEINTR(JSGlobalObject*);
// __getstate__ of what cannot be pickled.
JSValue ioCannotPickle(JSGlobalObject*, JSValue self);
// The setter of _finalizing. False: an exception.
bool ioToFinalizing(JSGlobalObject*, JSValue value, bool& result);
// warnings.warn("unclosed file <source>", ResourceWarning, source=source). What that raises goes no further.
void ioWarnUnclosed(JSGlobalObject*, JSValue self, JSValue source);

// ---- What can be asked of any stream ----

// False: it is closed, a ValueError, or another exception.
bool ioCheckClosed(JSGlobalObject*, JSValue self);
// False: it says it is not, an UnsupportedOperation, or another exception.
bool ioCheckReadable(JSGlobalObject*, JSValue self);
bool ioCheckWritable(JSGlobalObject*, JSValue self);
bool ioCheckSeekable(JSGlobalObject*, JSValue self);
// stream.flush(). False: an exception.
bool ioFlush(JSGlobalObject*, JSValue stream);
// _IOBase.close(self)
JSValue ioBaseClose(JSGlobalObject*, JSValue self);

// ---- Arguments ----
// All return false having raised an exception.

// An int that fits in a C int.
bool ioToInt(JSGlobalObject*, JSValue, int& result);
// A size that may be None or absent, which leave `result` as it is.
bool ioToOptionalSize(JSGlobalObject*, JSValue, int64_t& result);
// A position in a file. What does not fit raises an exception of the given type; with none, it is clipped. -1: maybe an exception.
int64_t ioToOffset(JSGlobalObject*, JSValue, PyType* overflowError);
// A str, or None or nothing, which are a null string. `function` and `argument` are for the TypeError.
// CPython hands such arguments on as UTF-8. False: a UnicodeEncodeError, because of a lone surrogate.
bool ioCheckUTF8(JSGlobalObject*, JSString*);
bool ioToOptionalString(JSGlobalObject*, JSValue, ASCIILiteral function, ASCIILiteral argument, String& result);
// os.fspath(). Empty: an exception.
JSValue ioFSPath(JSGlobalObject*, JSValue);

// The memory of an argument: bytes, a bytearray, a memoryview... Until this goes, a bytearray and the like cannot be resized.
class IOBuffer {
    WTF_MAKE_NONCOPYABLE(IOBuffer);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    IOBuffer() = default;
    ~IOBuffer()
    {
        if (m_globalObject)
            releaseBuffer(m_globalObject, m_buffer);
    }

    // "a bytes-like object is required, not 'x'"
    bool acquire(JSGlobalObject*, JSValue);
    // "readinto() argument must be read-write bytes-like object, not x"
    bool acquireWritable(JSGlobalObject*, ASCIILiteral function, JSValue, ASCIILiteral argument = "argument"_s);
    // After anything that may have run code: memory of WebAssembly, which is not pinned, may have moved. False: a BufferError.
    bool refresh() { return !m_globalObject || refreshBuffer(m_globalObject, m_buffer); }

    size_t size() const { return m_buffer.length; }
    const uint8_t* data() const { return m_buffer.data; }
    uint8_t* mutableData() { return m_buffer.data; }
    std::span<const uint8_t> span() const { return m_buffer.span(); }
    std::span<uint8_t> mutableSpan() { return m_buffer.mutableSpan(); }
    // The cell the memory belongs to, and where in the memory of that cell it starts.
    JSCell* owner() const { return m_buffer.owner; }
    size_t offset() const { return m_buffer.offset; }

private:
    JSGlobalObject* m_globalObject { nullptr }; // Null: there is nothing to release.
    Buffer m_buffer;
};

// bytearray(size)
PyByteArray* ioNewByteArray(JSGlobalObject*, size_t size);
// With nothing in them yet. Null: a MemoryError.
PyBytes* ioTryNewBytes(JSGlobalObject*, size_t length);
PyByteStorage* ioTryNewStorage(JSGlobalObject*, size_t capacity);

// ---- FileIO, for the streams over one ----

bool fileIOIsClosed(PyRecord*);
// What readinto(), write(), seek() and tell() do, without the objects. -1: an exception. -2: it would block (None).
// The memory is `length` bytes of that of `owner`, from `offset` on, and no address: see locateMemory().
int64_t fileIOReadInto(JSGlobalObject*, PyRecord*, JSCell* owner, size_t offset, size_t length);
int64_t fileIOWrite(JSGlobalObject*, PyRecord*, JSCell* owner, size_t offset, size_t length);
int64_t fileIOSeek(JSGlobalObject*, PyRecord*, int64_t position, int whence);

// ---- Text ----

// IncrementalNewlineDecoder.decode(), for an instance of that type.
JSValue newlineDecoderDecode(JSGlobalObject*, PyRecord*, JSValue input, bool isFinal);
// False: a ValueError, "illegal newline value: ...". A null string, for None, is legal.
bool ioIsLegalNewline(const String&);

// A str counts code points; the text of one is in UTF-16 unless every character fits in a byte.
inline size_t ioCountCodePoints(std::span<const char16_t> units)
{
    size_t count = units.size();
    for (size_t i = 0; i + 1 < units.size(); ++i) {
        if (U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1])) {
            --count;
            ++i;
        }
    }
    return count;
}
inline size_t ioCountCodePoints(StringView text) { return text.is8Bit() ? text.length() : ioCountCodePoints(text.span16()); }
// Where in the text, in code units, the character `count` code points after the one at `start` is. There are that many.
inline size_t ioSkipCodePoints(StringView text, size_t start, size_t count)
{
    if (text.is8Bit())
        return start + count;
    auto units = text.span16();
    size_t i = start;
    for (; count; --count)
        i += i + 1 < units.size() && U16_IS_LEAD(units[i]) && U16_IS_TRAIL(units[i + 1]) ? 2 : 1;
    return i;
}

template<typename CharacterType>
ALWAYS_INLINE const CharacterType* ioFindCharacter(const CharacterType* start, const CharacterType* end, CharacterType character)
{
    if constexpr (sizeof(CharacterType) == 1)
        return static_cast<const CharacterType*>(memchr(start, character, end - start));
    else {
        for (; start < end; ++start) {
            if (*start == character)
                return start;
        }
        return nullptr;
    }
}

// How long the first line of the text is, with its ending. -1: it does not end here; then `consumed` is how much of the text
// is certainly no part of a line ending.
// `translated`: only "\n" ends a line. `universal`: "\n", "\r" and "\r\n" do. Otherwise `newline` does.
template<typename CharacterType>
int64_t ioFindLineEnding(bool translated, bool universal, std::span<const uint8_t> newline, std::span<const CharacterType> text, int64_t& consumed)
{
    static_assert(std::is_integral_v<CharacterType>);
    const CharacterType* start = text.data();
    const CharacterType* end = start + text.size();
    if (translated || (!universal && newline.size() == 1)) {
        if (auto* found = ioFindCharacter<CharacterType>(start, end, translated ? static_cast<uint8_t>('\n') : newline[0]))
            return found - start + 1;
        consumed = text.size();
        return -1;
    }
    if (universal) {
        // The decoder never leaves a "\r" at the end that a "\n" might follow.
        for (auto* s = start; s < end; ++s) {
            if (*s > '\r')
                continue;
            if (*s == '\n')
                return s - start + 1;
            if (*s == '\r')
                return s - start + (s + 1 < end && s[1] == '\n' ? 2 : 1);
        }
        consumed = text.size();
        return -1;
    }
    // The last characters may be the start of an ending.
    const CharacterType* limit = text.size() >= newline.size() - 1 ? end - (newline.size() - 1) : start;
    for (auto* s = start; s < limit;) {
        auto* found = ioFindCharacter<CharacterType>(s, end, newline[0]);
        if (!found || found >= limit)
            break;
        size_t i = 1;
        while (i < newline.size() && found[i] == newline[i])
            ++i;
        if (i == newline.size())
            return found - start + newline.size();
        s = found + 1;
    }
    auto* found = ioFindCharacter<CharacterType>(limit, end, newline[0]);
    consumed = found ? found - start : text.size();
    return -1;
}

} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
