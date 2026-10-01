/*
 * Copyright (C) 2026 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "PythonPickle.h"

#include "JSCInlines.h"
#include "PyObjects.h"
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonText.h"
#include <wtf/Scope.h>
#include <wtf/dtoa.h>

// Unpickler, load() and loads(): the second half of Modules/_pickle.c. See PythonPickle.h.

namespace JSC { namespace Python {

namespace {

constexpr unsigned initialMemoSize = 32;

// UnpicklerObject. The stack is not here: it lasts no longer than load() does, and is Loader's.
struct Unpickler final : NativeState {
    PYTHON_NATIVE_STATE(Unpickler);

    // What has been read and may be asked for again, by number. The numbers come one after another, so it is a row of places, some with nothing in them. It is of a fixed size, and gets another to grow, so that the
    // collector can look at it at any time. No program comes by it.
    WriteBarrier<PyTuple> memo;
    // What goes by a number that is far beyond the rest, which no pickle that was written by a Pickler has. CPython makes the row long enough, so that six bytes can ask for as many gigabytes. There is none until it
    // is wanted.
    WriteBarrier<PyDict> farMemo;
    size_t memoLength { 0 }; // How many numbers there are that something goes by

    WriteBarrier<Unknown> persistentLoad; // The method, while something is being read
    WriteBarrier<Unknown> persistentLoadAttribute; // What the attribute has been set to
    // The methods of what it reads from. There is no read() if that is bytes. The second and the last may not be there.
    WriteBarrier<Unknown> read;
    WriteBarrier<Unknown> readinto;
    WriteBarrier<Unknown> readline;
    WriteBarrier<Unknown> peek;
    WriteBarrier<Unknown> buffers; // What gives the buffers that are not in the pickle

    // What there is to read without asking for more: a copy, so that whatever is run meanwhile it stays where it is.
    ByteVector input;
    int64_t inputLength { 0 }; // How much of it may be read: all of it, or as far as the end of the frame
    int64_t nextReadIndex { 0 };
    int64_t prefetchedIndex { 0 }; // Where what was only looked ahead at begins
    int64_t frameEnd { -1 }; // Where the frame ends, or -1 if it is in none. What is read is not to go past it (PEP 3154).
    int64_t savedInputLength { 0 }; // What inputLength is to be when the frame ends
    Vector<uint8_t> inputLine;

    String encoding; // How to read a str of Python 2's. Null until it has been set up.
    String errors;
    Vector<int64_t> marks; // How much was on the stack at each MARK
    int protocol { 0 };
    bool fixesImports { false }; // Whether things are taken to be called what Python 2 calls them
    bool isRunning { false };
};

template<typename Visitor>
void Unpickler::visit(Visitor& visitor)
{
    visitor.append(memo);
    visitor.append(farMemo);
    visitor.append(persistentLoad);
    visitor.append(persistentLoadAttribute);
    visitor.append(read);
    visitor.append(readinto);
    visitor.append(readline);
    visitor.append(peek);
    visitor.append(buffers);
}

// _Unpickler_NewMemo(). Null if it raised.
PyTuple* newMemo(JSGlobalObject* globalObject, size_t size)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    if (size > std::numeric_limits<unsigned>::max()) {
        raiseMemoryError(globalObject, scope);
        return nullptr;
    }
    // One with no places is not the tuple that there is one of.
    PyTuple* memo = PyTuple::tryCreate(globalObject, std::max<unsigned>(size, 1));
    RETURN_IF_EXCEPTION(scope, nullptr);
    for (auto& place : memo->span())
        place.clear();
    return memo;
}

// _Unpickler_MemoGet(). Empty if there is nothing there.
JSValue memoGet(JSGlobalObject* globalObject, Unpickler& self, size_t index)
{
    if (index < self.memo->length()) [[likely]] {
        if (JSValue value = self.memo->at(index)) [[likely]]
            return value;
    }
    return self.farMemo ? self.farMemo->get(globalObject, intFromUInt64(globalObject, index)) : JSValue();
}

// _Unpickler_MemoPut()
void memoPut(JSGlobalObject* globalObject, PyStateObject* owner, Unpickler& self, size_t index, JSValue value)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (index >= self.memo->length() * static_cast<size_t>(2)) [[unlikely]] {
        // Further on than the row would reach if it were made twice as long
        if (!self.farMemo)
            self.farMemo.set(vm, owner, PyDict::create(globalObject));
        bool wasAdded;
        self.farMemo->add(globalObject, intFromUInt64(globalObject, index), value, &wasAdded);
        RETURN_IF_EXCEPTION(scope, void());
        self.memoLength += wasAdded;
        return;
    }
    if (index >= self.memo->length()) {
        PyTuple* larger = newMemo(globalObject, index * 2);
        RETURN_IF_EXCEPTION(scope, void());
        for (unsigned i = 0; i < self.memo->length(); ++i) {
            if (JSValue kept = self.memo->at(i))
                larger->initializeAt(vm, i, kept);
        }
        self.memo.set(vm, owner, larger);
    }
    if (!self.memo->at(index)) {
        // The row may have come to reach what was beyond it.
        JSValue wasFar = self.farMemo ? self.farMemo->remove(globalObject, intFromUInt64(globalObject, index)) : JSValue();
        RETURN_IF_EXCEPTION(scope, void());
        if (!wasFar)
            ++self.memoLength;
    }
    self.memo->initializeAt(vm, index, value);
}

// BEGIN_USING_UNPICKLER() and END_USING_UNPICKLER()
class UsingUnpickler {
public:
    UsingUnpickler(JSGlobalObject* globalObject, Unpickler& self)
        : m_self(self)
    {
        if (self.isRunning) {
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            raise(globalObject, scope, BuiltinType::RuntimeError, "Unpickler object is already used"_s);
            return;
        }
        self.isRunning = true;
        m_isUsing = true;
    }
    ~UsingUnpickler()
    {
        if (m_isUsing)
            m_self.isRunning = false;
    }

private:
    Unpickler& m_self;
    bool m_isUsing { false };
};

// _Unpickler_SetStringInput(): how much there is of it.
int64_t setStringInput(JSGlobalObject* globalObject, Unpickler& self, JSValue input)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Buffer buffer = bufferOf(globalObject, input);
    RETURN_IF_EXCEPTION(scope, -1);
    ByteVector copy;
    copy.append(buffer.span());
    if (copy.hasOverflowed()) {
        raiseMemoryError(globalObject, scope);
        return -1;
    }
    self.input = WTF::move(copy);
    self.inputLength = self.input.size();
    self.nextReadIndex = 0;
    self.prefetchedIndex = self.inputLength;
    self.frameEnd = -1;
    return self.inputLength;
}

// What is read while one object is, with all that is in it. Each function is the one of the same name, or near it, in _pickle.c.
class Loader {
public:
    Loader(JSGlobalObject* globalObject, PyStateObject* owner)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_realm(globalObject->pyRealm())
        , m_state(pickleModuleState(globalObject))
        , m_owner(owner)
        , m_self(owner->state<Unpickler>())
    {
    }

    // load(). Empty if it raised.
    JSValue load()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        m_self.marks.shrink(0);
        m_self.protocol = 0;
        auto forget = makeScopeExit([&] { m_self.persistentLoad.clear(); });
        JSValue persistentLoad = getAttribute(m_globalObject, m_owner, Identifier::fromString(m_vm, "persistent_load"_s));
        RETURN_IF_EXCEPTION(scope, { });
        m_self.persistentLoad.set(m_vm, m_owner, persistentLoad);

        while (true) {
            auto opcode = read(1);
            if (scope.exception()) [[unlikely]] {
                if (isRaised(m_state.unpicklingError.get()) && scope.tryClearException())
                    raise(m_globalObject, scope, BuiltinType::EOFError, "Ran out of input"_s);
                return { };
            }
            if (static_cast<Opcode>(opcode[0]) == Opcode::Stop)
                break;
            dispatch(opcode[0]);
            RETURN_IF_EXCEPTION(scope, { });
        }
        skipConsumed();
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, pop());
    }

private:
    void dispatch(uint8_t opcode)
    {
        switch (static_cast<Opcode>(opcode)) {
        case Opcode::None:
            return push(jsUndefined());
        case Opcode::BinInt:
            return loadBinInt(4);
        case Opcode::BinInt1:
            return loadBinInt(1);
        case Opcode::BinInt2:
            return loadBinInt(2);
        case Opcode::Int:
            return loadInt();
        case Opcode::Long:
            return loadLong();
        case Opcode::Long1:
            return loadCountedLong(1);
        case Opcode::Long4:
            return loadCountedLong(4);
        case Opcode::Float:
            return loadFloat();
        case Opcode::BinFloat:
            return loadBinFloat();
        case Opcode::ShortBinBytes:
            return loadCountedBinBytes(1);
        case Opcode::BinBytes:
            return loadCountedBinBytes(4);
        case Opcode::BinBytes8:
            return loadCountedBinBytes(8);
        case Opcode::ByteArray8:
            return loadCountedByteArray();
        case Opcode::NextBuffer:
            return loadNextBuffer();
        case Opcode::ReadOnlyBuffer:
            return loadReadOnlyBuffer();
        case Opcode::ShortBinString:
            return loadCountedBinString(1);
        case Opcode::BinString:
            return loadCountedBinString(4);
        case Opcode::String:
            return loadString();
        case Opcode::Unicode:
            return loadUnicode();
        case Opcode::ShortBinUnicode:
            return loadCountedBinUnicode(1);
        case Opcode::BinUnicode:
            return loadCountedBinUnicode(4);
        case Opcode::BinUnicode8:
            return loadCountedBinUnicode(8);
        case Opcode::EmptyTuple:
            return loadCountedTuple(0);
        case Opcode::Tuple1:
            return loadCountedTuple(1);
        case Opcode::Tuple2:
            return loadCountedTuple(2);
        case Opcode::Tuple3:
            return loadCountedTuple(3);
        case Opcode::Tuple:
            return loadTuple();
        case Opcode::EmptyList:
            return push(newList(m_globalObject));
        case Opcode::List:
            return loadList();
        case Opcode::EmptyDict:
            return push(PyDict::create(m_globalObject));
        case Opcode::Dict:
            return loadDict();
        case Opcode::EmptySet:
            return push(PySet::create(m_globalObject));
        case Opcode::AddItems:
            return loadAddItems();
        case Opcode::FrozenSet:
            return loadFrozenSet();
        case Opcode::Obj:
            return loadObj();
        case Opcode::Inst:
            return loadInst();
        case Opcode::NewObj:
            return loadNewObj(false);
        case Opcode::NewObjEx:
            return loadNewObj(true);
        case Opcode::Global:
            return loadGlobal();
        case Opcode::StackGlobal:
            return loadStackGlobal();
        case Opcode::Append:
            return loadAppend();
        case Opcode::Appends:
            return loadAppends();
        case Opcode::Build:
            return loadBuild();
        case Opcode::Dup:
            return loadDup();
        case Opcode::BinGet:
            return loadBinGet(1);
        case Opcode::LongBinGet:
            return loadBinGet(4);
        case Opcode::Get:
            return loadGet();
        case Opcode::Mark:
            return loadMark();
        case Opcode::BinPut:
            return loadBinPut(1);
        case Opcode::LongBinPut:
            return loadBinPut(4);
        case Opcode::Put:
            return loadPut();
        case Opcode::Memoize:
            return loadMemoize();
        case Opcode::Pop:
            return loadPop();
        case Opcode::PopMark:
            return loadPopMark();
        case Opcode::SetItem:
            return doSetItems(static_cast<int64_t>(m_stack.size()) - 2);
        case Opcode::SetItems:
            return loadSetItems();
        case Opcode::PersistentID:
            return loadPersistentID();
        case Opcode::BinPersistentID:
            return loadBinPersistentID();
        case Opcode::Reduce:
            return loadReduce();
        case Opcode::Proto:
            return loadProto();
        case Opcode::Frame:
            return loadFrame();
        case Opcode::Ext1:
            return loadExtension(1);
        case Opcode::Ext2:
            return loadExtension(2);
        case Opcode::Ext4:
            return loadExtension(4);
        case Opcode::NewTrue:
            return push(jsBoolean(true));
        case Opcode::NewFalse:
            return push(jsBoolean(false));
        case Opcode::Stop:
            break;
        }
        if (opcode >= 0x20 && opcode <= 0x7e && opcode != '\'' && opcode != '\\')
            return raiseUnpicklingError(makeString("invalid load key, '"_s, static_cast<char>(opcode), "'."_s));
        raiseUnpicklingError(makeString("invalid load key, '\\x"_s, hex(opcode, 2, Lowercase), "'."_s));
    }

    // ---- What goes wrong

    void raiseUnpicklingError(const String& message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, m_state.unpicklingError.get(), message);
    }

    // bad_readline()
    void raiseTruncated() { raiseUnpicklingError("pickle data was truncated"_s); }

    // PyErr_ExceptionMatches()
    bool isRaised(PyType* type)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        Exception* raised = scope.exception();
        return raised && !m_vm.isTerminationException(raised) && isInstance(m_globalObject, raised->value(), type);
    }

    // ---- The stack: Pdata

    int64_t stackSize() const { return m_stack.size(); }

    // Pdata_stack_underflow()
    void raiseStackUnderflow() { raiseUnpicklingError(m_isMarkSet ? "unexpected MARK found"_s : "unpickling stack underflow"_s); }

    // Pdata_push()
    void push(JSValue value)
    {
        m_stack.append(value);
        if (m_stack.hasOverflowed()) [[unlikely]] {
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            raiseMemoryError(m_globalObject, scope);
        }
    }

    // Pdata_pop(). Empty if it raised.
    JSValue pop()
    {
        if (stackSize() <= m_fence) {
            raiseStackUnderflow();
            return { };
        }
        return m_stack.takeLast();
    }

    // Pdata_clear(): no more than the first so many are kept.
    void clearStackTo(int64_t size)
    {
        while (stackSize() > size)
            m_stack.removeLast();
    }

    // Pdata_poptuple(): all from there up. Null if it raised.
    PyTuple* popTuple(int64_t start)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (start < m_fence) {
            raiseStackUnderflow();
            return nullptr;
        }
        PyTuple* tuple = PyTuple::tryCreate(m_globalObject, stackSize() - start);
        RETURN_IF_EXCEPTION(scope, nullptr);
        for (int64_t i = start; i < stackSize(); ++i)
            tuple->initializeAt(m_vm, i - start, m_stack.at(i));
        clearStackTo(start);
        return tuple;
    }

    // Pdata_poplist()
    JSArray* popList(int64_t start)
    {
        JSArray* list = newList(m_globalObject, ArgList(std::bit_cast<EncodedJSValue*>(m_stack.data()) + start, stackSize() - start));
        clearStackTo(start);
        return list;
    }

    void setFenceFromMarks()
    {
        m_isMarkSet = !m_self.marks.isEmpty();
        m_fence = m_isMarkSet ? m_self.marks.last() : 0;
    }

    // marker(): how much was on the stack at the last MARK, which is one no longer. -1 if it raised.
    int64_t marker()
    {
        if (m_self.marks.isEmpty()) {
            raiseUnpicklingError("could not find MARK"_s);
            return -1;
        }
        int64_t mark = m_self.marks.takeLast();
        setFenceFromMarks();
        return mark;
    }

    // ---- What is read from

    // _Unpickler_EndFrame()
    void endFrame()
    {
        ASSERT(m_self.frameEnd >= 0 && m_self.nextReadIndex == m_self.frameEnd);
        m_self.inputLength = m_self.savedInputLength;
        m_self.frameEnd = -1;
    }

    // _Unpickler_LeaveFrame(): the end of the frame has been come to. What is being read is not to lie on both sides of it.
    void leaveFrame(bool straddles)
    {
        if (straddles)
            return raiseUnpicklingError("pickle exhausted before end of frame"_s);
        endFrame();
    }

    // _Unpickler_SkipConsumed(): what was only looked ahead at, and has been used, is read and thrown away.
    void skipConsumed()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t consumed = m_self.nextReadIndex - m_self.prefetchedIndex;
        if (consumed <= 0)
            return;
        call(m_globalObject, m_self.read.get(), intFromInt64(m_globalObject, consumed));
        RETURN_IF_EXCEPTION(scope, void());
        m_self.prefetchedIndex = m_self.nextReadIndex;
    }

    static constexpr int64_t readWholeLine = -1;

    // _Unpickler_ReadFromFile(): no more is taken from a file than is wanted, since there may be another pickle after this one. What it gave is then what there is to read, and this is how much that is.
    int64_t readFromFile(int64_t count)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        skipConsumed();
        RETURN_IF_EXCEPTION(scope, -1);
        JSValue data;
        if (count == readWholeLine)
            data = call(m_globalObject, m_self.readline.get());
        else {
            // Looking ahead moves nothing on.
            if (m_self.peek && count < static_cast<int64_t>(picklePrefetch)) {
                data = call(m_globalObject, m_self.peek.get(), intFromInt64(m_globalObject, picklePrefetch));
                if (scope.exception()) {
                    // It may well be that it cannot be done.
                    if (!catchException(m_globalObject, BuiltinType::NotImplementedError))
                        return -1;
                    m_self.peek.clear();
                } else {
                    int64_t size = setStringInput(m_globalObject, m_self, data);
                    RETURN_IF_EXCEPTION(scope, -1);
                    m_self.prefetchedIndex = 0;
                    if (count <= size)
                        return count;
                }
            }
            data = call(m_globalObject, m_self.read.get(), intFromInt64(m_globalObject, count));
        }
        RETURN_IF_EXCEPTION(scope, -1);
        RELEASE_AND_RETURN(scope, setStringInput(m_globalObject, m_self, data));
    }

    std::span<const uint8_t> take(int64_t count)
    {
        auto result = m_self.input.span().subspan(m_self.nextReadIndex, count);
        m_self.nextReadIndex += count;
        return result;
    }

    // _Unpickler_Read(): the next so many bytes, which are good until more is read. If it raised there are none.
    std::span<const uint8_t> read(int64_t count)
    {
        if (count <= m_self.inputLength - m_self.nextReadIndex) [[likely]]
            return take(count);
        return readSlow(count);
    }

    // _Unpickler_ReadImpl()
    std::span<const uint8_t> readSlow(int64_t count)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_self.nextReadIndex > std::numeric_limits<int64_t>::max() - count) {
            raiseUnpicklingError("read would overflow (invalid bytecode)"_s);
            return { };
        }
        if (m_self.frameEnd >= 0) {
            leaveFrame(m_self.nextReadIndex < m_self.frameEnd);
            RETURN_IF_EXCEPTION(scope, { });
            if (count <= m_self.inputLength - m_self.nextReadIndex)
                return take(count);
        }
        if (!m_self.read) {
            raiseTruncated();
            return { };
        }
        int64_t size = readFromFile(count);
        RETURN_IF_EXCEPTION(scope, { });
        if (size < count) {
            raiseTruncated();
            return { };
        }
        m_self.nextReadIndex = count;
        return m_self.input.span().first(count);
    }

    // _Unpickler_ReadInto(): the next so many bytes, into something that has been made to take them. It is for a good many, which need then be copied no more than once, and it looks no further ahead.
    void readInto(JSUint8Array* target)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        size_t filled = 0;
        auto remaining = [&] { return static_cast<int64_t>(target->length() - filled); };
        auto takeWhatThereIs = [&] {
            int64_t inBuffer = m_self.inputLength - m_self.nextReadIndex;
            if (inBuffer <= 0)
                return;
            auto bytes = take(std::min(inBuffer, remaining()));
            memcpySpan(target->typedSpan().subspan(filled), bytes);
            filled += bytes.size();
        };
        bool hadSome = m_self.inputLength > m_self.nextReadIndex;
        takeWhatThereIs();
        if (!remaining())
            return;
        if (m_self.frameEnd >= 0) {
            leaveFrame(hadSome);
            RETURN_IF_EXCEPTION(scope, void());
            takeWhatThereIs();
            if (!remaining())
                return;
        }
        if (!m_self.read)
            return raiseTruncated();
        skipConsumed();
        RETURN_IF_EXCEPTION(scope, void());

        if (!m_self.readinto) {
            JSValue data = call(m_globalObject, m_self.read.get(), intFromInt64(m_globalObject, remaining()));
            RETURN_IF_EXCEPTION(scope, void());
            if (!isBytes(data)) {
                String shown = repr(m_globalObject, Python::typeOf(m_globalObject, data)->object());
                RETURN_IF_EXCEPTION(scope, void());
                raiseValueError(m_globalObject, scope, concatenate("read() returned non-bytes object ("_s, shown, ')'));
                return;
            }
            auto bytes = *builtinBufferOf(data);
            if (static_cast<int64_t>(bytes.size()) < remaining())
                return raiseTruncated();
            memcpySpan(target->typedSpan().subspan(filled), bytes.first(remaining()));
            return;
        }

        // What is given to write in is a bytearray of its own, since a bytes is not to be written in by anything that could keep hold of it.
        JSUint8Array* scratch = JSUint8Array::create(m_globalObject, m_realm->structureFor(BuiltinType::ByteArray), remaining());
        RETURN_IF_EXCEPTION(scope, void());
        PyMemoryView* view = memoryViewOf(m_globalObject, scratch, WritableBuffer);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue sizeObject = call(m_globalObject, m_self.readinto.get(), view);
        RETURN_IF_EXCEPTION(scope, void());
        auto size = toSsizeOfInt(m_globalObject, sizeObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (*size < 0) {
            raiseValueError(m_globalObject, scope, "readinto() returned negative size"_s);
            return;
        }
        if (*size < remaining() || static_cast<int64_t>(scratch->length()) < remaining())
            return raiseTruncated();
        memcpySpan(target->typedSpan().subspan(filled), std::span<const uint8_t>(scratch->typedSpan()).first(remaining()));
    }

    // _Unpickler_CopyLine()
    std::span<uint8_t> copyLine(std::span<const uint8_t> line)
    {
        m_self.inputLine.shrink(0);
        m_self.inputLine.append(line);
        return m_self.inputLine.mutableSpan();
    }

    // _Unpickler_Readline(): as far as the end of the line, and that. It is a copy, which can be written in. If it raised there is nothing.
    std::span<uint8_t> readLine()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        while (true) {
            auto unread = m_self.input.span().subspan(m_self.nextReadIndex, m_self.inputLength - m_self.nextReadIndex);
            if (size_t newline = WTF::find(unread, '\n'); newline != notFound)
                return copyLine(take(newline + 1));
            if (m_self.frameEnd < 0)
                break;
            leaveFrame(m_self.nextReadIndex < m_self.frameEnd);
            RETURN_IF_EXCEPTION(scope, { });
        }
        if (!m_self.read) {
            raiseTruncated();
            return { };
        }
        int64_t size = readFromFile(readWholeLine);
        RETURN_IF_EXCEPTION(scope, { });
        if (!size || m_self.input[size - 1] != '\n') {
            raiseTruncated();
            return { };
        }
        m_self.nextReadIndex = size;
        return copyLine(m_self.input.span().first(size));
    }

    // The same, where there is to be something on the line besides its end.
    std::span<uint8_t> readLineOfAtLeast(size_t length)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLine();
        RETURN_IF_EXCEPTION(scope, { });
        if (line.size() < length) {
            raiseTruncated();
            return { };
        }
        return line;
    }

    // ---- Numbers

    // calc_binsize(): so many bytes are a number, the least first. -1 if it is more than there can be of anything.
    static int64_t binSize(std::span<const uint8_t> bytes)
    {
        uint64_t value = 0;
        for (size_t i = 0; i < bytes.size(); ++i)
            value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
        return value > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ? -1 : static_cast<int64_t>(value);
    }

    // calc_binint(): the same, but that four of them may be less than nothing.
    static int64_t binInt(std::span<const uint8_t> bytes)
    {
        int64_t value = binSize(bytes);
        return bytes.size() == 4 ? static_cast<int32_t>(static_cast<uint32_t>(value)) : value;
    }

    // load_binint(), load_binint1() and load_binint2()
    void loadBinInt(unsigned size)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(size);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(jsNumber(static_cast<int32_t>(binInt(bytes)))));
    }

    // PyLong_FromString(s, NULL, 10), of what is as far as the first zero. Empty if it raised.
    JSValue intFromLine(std::span<const uint8_t> line)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (size_t zero = WTF::find(line, '\0'); zero != notFound)
            line = line.first(zero);
        JSValue value = parseInt(m_globalObject, StringView(byteCast<Latin1Character>(line)), 10);
        RETURN_IF_EXCEPTION(scope, { });
        if (value)
            return value;
        // What is shown is made a str of first, which it may not be fit for.
        JSValue text = decodeBytesToObject(m_globalObject, line.first(std::min<size_t>(line.size(), 200)), "utf-8"_s, "strict"_s);
        RETURN_IF_EXCEPTION(scope, { });
        String shown = repr(m_globalObject, text);
        RETURN_IF_EXCEPTION(scope, { });
        return raiseValueError(m_globalObject, scope, concatenate("invalid literal for int() with base 10: "_s, shown));
    }

    // load_int()
    void loadInt()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue value = intFromLine(line);
        if (scope.exception()) {
            if (!scope.tryClearException())
                return;
            raiseValueError(m_globalObject, scope, "could not convert string to int"_s);
            return;
        }
        // 00 and 01 are how False and True were written before there was a way to write them.
        if (Number number = classify(value); line.size() == 3 && number.kind == Number::Kind::Small && (!number.small || number.small == 1))
            value = jsBoolean(number.small);
        RELEASE_AND_RETURN(scope, push(value));
    }

    // load_long()
    void loadLong()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, void());
        // There is usually an L before the end of the line, but Python 3.0.0 wrote none.
        if (line[line.size() - 2] == 'L')
            line[line.size() - 2] = '\0';
        JSValue value = intFromLine(line);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(value));
    }

    // load_counted_long(): how many bytes, and then those, the least first, a negative one as what it falls short of a power of 256 by.
    void loadCountedLong(unsigned sizeOfCount)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto countBytes = read(sizeOfCount);
        RETURN_IF_EXCEPTION(scope, void());
        int64_t count = binInt(countBytes);
        if (count < 0)
            return raiseUnpicklingError("LONG pickle has negative byte count"_s);
        if (!count)
            RELEASE_AND_RETURN(scope, push(jsNumber(0)));
        auto bytes = read(count);
        RETURN_IF_EXCEPTION(scope, void());
        // _PyLong_FromByteArray()
        bool isNegative = bytes.back() & 0x80;
        Vector<uint64_t, 4> digits;
        digits.fill(0, (bytes.size() + 7) / 8);
        unsigned carry = 1;
        for (size_t i = 0; i < bytes.size(); ++i) {
            uint8_t byte = bytes[i];
            if (isNegative) {
                unsigned sum = static_cast<uint8_t>(~byte) + carry;
                byte = static_cast<uint8_t>(sum);
                carry = sum >> 8;
            }
            digits[i / 8] |= static_cast<uint64_t>(byte) << (8 * (i % 8));
        }
        JSValue value = intFromDigits(m_globalObject, digits.span(), isNegative);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(value));
    }

    // load_float()
    void loadFloat()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, void());
        auto text = std::span<const uint8_t>(line);
        if (size_t zero = WTF::find(text, '\0'); zero != notFound)
            text = text.first(zero);
        // PyOS_string_to_double(), and then there is to be nothing but the end of the line.
        double value;
        size_t parsed = parseFloatAtStart(text, value);
        auto shown = [&] { return String::fromUTF8ReplacingInvalidSequences(text.first(std::min<size_t>(text.size(), 200))); }; // As "%.200s" takes it
        if (!parsed) {
            raiseValueError(m_globalObject, scope, concatenate("could not convert string to float: '"_s, shown(), '\''));
            return;
        }
        if (std::isinf(value) && isASCIIDigit(text[parsed - 1])) {
            raise(m_globalObject, scope, BuiltinType::OverflowError, concatenate("value too large to convert to float: '"_s, shown(), '\''));
            return;
        }
        if (parsed < text.size() && text[parsed] != '\n') {
            raiseValueError(m_globalObject, scope, "could not convert string to float"_s);
            return;
        }
        RELEASE_AND_RETURN(scope, push(floatFromDouble(value)));
    }

    // _PyOS_ascii_strtod(): how much of the text, from its start, is a number, and the number. There is to be nothing before it.
    static size_t parseFloatAtStart(std::span<const uint8_t> text, double& value)
    {
        size_t at = 0;
        auto isAt = [&] (size_t index, char lower) { return index < text.size() && toASCIILower(text[index]) == lower; };
        auto follows = [&] (ASCIILiteral word) {
            for (size_t i = 0; i < word.length(); ++i) {
                if (!isAt(at + i, word[i]))
                    return false;
            }
            return true;
        };
        bool isNegative = isAt(0, '-');
        if (isNegative || isAt(0, '+'))
            ++at;
        // _Py_parse_inf_or_nan()
        if (follows("inf"_s)) {
            at += follows("infinity"_s) ? 8 : 3;
            value = isNegative ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            return at;
        }
        if (follows("nan"_s)) {
            value = std::copysign(std::numeric_limits<double>::quiet_NaN(), isNegative ? -1.0 : 1.0);
            return at + 3;
        }
        size_t start = at;
        auto skipDigits = [&] {
            size_t from = at;
            while (at < text.size() && isASCIIDigit(text[at]))
                ++at;
            return at - from;
        };
        size_t digits = skipDigits();
        if (isAt(at, '.')) {
            ++at;
            digits += skipDigits();
        }
        if (!digits)
            return 0;
        if (isAt(at, 'e')) {
            size_t beforeExponent = at++;
            if (isAt(at, '+') || isAt(at, '-'))
                ++at;
            if (!skipDigits())
                at = beforeExponent;
        }
        size_t parsed;
        value = WTF::parseDouble(byteCast<Latin1Character>(text.subspan(start, at - start)), parsed);
        if (isNegative)
            value = -value;
        return at;
    }

    // load_binfloat()
    void loadBinFloat()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(8);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(floatFromDouble(unpackFloat8(bytes.first<8>(), false))));
    }

    // ---- bytes, bytearray and str

    bool leavesStringsAsBytes() const { return m_self.encoding == "bytes"_s; }

    // load_string(): a str of Python 2's, as it would be written in a program.
    void loadString()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLine();
        RETURN_IF_EXCEPTION(scope, void());
        auto text = std::span<const uint8_t>(line).first(line.size() - 1);
        if (text.size() < 2 || text.front() != text.back() || (text.front() != '\'' && text.front() != '"'))
            return raiseUnpicklingError("the STRING opcode argument must be quoted"_s);
        auto bytes = decodeEscape(m_globalObject, text.subspan(1, text.size() - 2), String());
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = leavesStringsAsBytes() ? JSValue(newBytes(m_globalObject, *bytes)) : decodeBytesToObject(m_globalObject, bytes->span(), m_self.encoding, m_self.errors);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_counted_binstring()
    void loadCountedBinString(unsigned sizeOfCount)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto countBytes = read(sizeOfCount);
        RETURN_IF_EXCEPTION(scope, void());
        int64_t count = binInt(countBytes);
        if (count < 0)
            return raiseUnpicklingError("BINSTRING pickle has negative byte count"_s);
        auto bytes = read(count);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = leavesStringsAsBytes() ? JSValue(newBytes(m_globalObject, bytes)) : decodeBytesToObject(m_globalObject, bytes, m_self.encoding, m_self.errors);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // How many bytes are to come. -1 if it raised.
    int64_t readCount(unsigned sizeOfCount, ASCIILiteral opcode)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto countBytes = read(sizeOfCount);
        RETURN_IF_EXCEPTION(scope, -1);
        int64_t count = binSize(countBytes);
        if (count < 0)
            raise(m_globalObject, scope, BuiltinType::OverflowError, concatenate(opcode, " exceeds system's maximum size of "_s, std::numeric_limits<int64_t>::max(), " bytes"_s));
        return count;
    }

    // One of so many bytes, which are read into it.
    void loadBytesOfKind(int64_t count, BuiltinType kind)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        // Room need not be made for them to find that they are not all there to be had: readInto() would say the same.
        if (int64_t inBuffer = m_self.inputLength - m_self.nextReadIndex; count > inBuffer) {
            if (m_self.frameEnd >= 0 && inBuffer > 0)
                return leaveFrame(true);
            if (!m_self.read && count > (m_self.frameEnd >= 0 ? m_self.savedInputLength : m_self.inputLength) - m_self.nextReadIndex)
                return raiseTruncated();
        }
        if (!count && kind == BuiltinType::Bytes)
            RELEASE_AND_RETURN(scope, push(newBytes(m_globalObject, std::span<const uint8_t>())));
        if (static_cast<uint64_t>(count) > MAX_ARRAY_BUFFER_SIZE) {
            raiseMemoryError(m_globalObject, scope);
            return;
        }
        JSUint8Array* object = JSUint8Array::createUninitialized(m_globalObject, m_realm->structureFor(kind), count);
        RETURN_IF_EXCEPTION(scope, void());
        readInto(object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_counted_binbytes()
    void loadCountedBinBytes(unsigned sizeOfCount)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t count = readCount(sizeOfCount, "BINBYTES"_s);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, loadBytesOfKind(count, BuiltinType::Bytes));
    }

    // load_counted_bytearray()
    void loadCountedByteArray()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t count = readCount(8, "BYTEARRAY8"_s);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, loadBytesOfKind(count, BuiltinType::ByteArray));
    }

    // load_next_buffer()
    void loadNextBuffer()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_self.buffers)
            return raiseUnpicklingError("pickle stream refers to out-of-band data but no *buffers* argument was given"_s);
        JSValue buffer = iteratorNext(m_globalObject, m_self.buffers.get());
        RETURN_IF_EXCEPTION(scope, void());
        if (!buffer)
            return raiseUnpicklingError("not enough out-of-band buffers"_s);
        RELEASE_AND_RETURN(scope, push(buffer));
    }

    // load_readonly_buffer()
    void loadReadOnlyBuffer()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (stackSize() <= m_fence)
            return raiseStackUnderflow();
        PyMemoryView* view = memoryViewOf(m_globalObject, m_stack.last(), FullReadOnlyBuffer);
        RETURN_IF_EXCEPTION(scope, void());
        // What cannot be written to already will do as it is.
        if (view->isReadOnly())
            return;
        PyMemoryView::Layout layout = view->layout();
        layout.isReadOnly = true;
        m_stack.at(stackSize() - 1) = view->derive(m_globalObject, layout, view->dimensions());
    }

    // load_unicode()
    void loadUnicode()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(1);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue string = decodeBytesToObject(m_globalObject, std::span<const uint8_t>(line).first(line.size() - 1), "raw-unicode-escape"_s, String());
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(string));
    }

    // load_counted_binunicode()
    void loadCountedBinUnicode(unsigned sizeOfCount)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t count = readCount(sizeOfCount, "BINUNICODE"_s);
        RETURN_IF_EXCEPTION(scope, void());
        auto bytes = read(count);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue string;
        if (charactersAreAllASCII(bytes) && bytes.size() <= String::MaxLength)
            string = bytes.empty() ? jsEmptyString(m_vm) : jsString(m_vm, String(byteCast<Latin1Character>(bytes)));
        else
            string = decodeBytesToObject(m_globalObject, bytes, "utf-8"_s, "surrogatepass"_s);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(string));
    }

    // ---- What has other things in it

    // load_counted_tuple()
    void loadCountedTuple(int64_t length)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (stackSize() < length)
            return raiseStackUnderflow();
        PyTuple* tuple = popTuple(stackSize() - length);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(tuple));
    }

    // load_tuple()
    void loadTuple()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, loadCountedTuple(stackSize() - mark));
    }

    // load_list()
    void loadList()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        JSArray* list = popList(mark);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(list));
    }

    // load_dict()
    void loadDict()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        int64_t size = stackSize();
        if ((size - mark) % 2)
            return raiseUnpicklingError("odd number of items for DICT"_s);
        PyDict* dict = PyDict::create(m_globalObject);
        for (int64_t i = mark + 1; i < size; i += 2) {
            dict->set(m_globalObject, m_stack.at(i - 1), m_stack.at(i));
            RETURN_IF_EXCEPTION(scope, void());
        }
        clearStackTo(mark);
        RELEASE_AND_RETURN(scope, push(dict));
    }

    // load_frozenset()
    void loadFrozenSet()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        PyTuple* items = popTuple(mark);
        RETURN_IF_EXCEPTION(scope, void());
        PySet* set = setFromIterable(m_globalObject, m_realm->typeFrozenSet()->instanceStructure(), items);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(set));
    }

    // ---- What is made by calling something

    // PyObject_CallObject()
    JSValue callWithTuple(JSValue callable, JSValue arguments)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isTuple(arguments))
            return raiseTypeError(m_globalObject, scope, "argument list must be a tuple"_s);
        PyTuple* tuple = asTuple(arguments);
        RELEASE_AND_RETURN(scope, call(m_globalObject, callable, ArgList(std::bit_cast<EncodedJSValue*>(tuple->span().data()), tuple->length())));
    }

    // find_class()
    JSValue findClass(JSValue moduleName, JSValue globalName)
    {
        return callMethodNamed(m_globalObject, m_owner, Identifier::fromString(m_vm, "find_class"_s), moduleName, globalName);
    }

    // instantiate()
    JSValue instantiate(JSValue cls, PyTuple* arguments)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!arguments->length() && isClass(cls)) {
            JSValue getInitArgs = getAttributeIfPresent(m_globalObject, cls, Identifier::fromString(m_vm, "__getinitargs__"_s));
            RETURN_IF_EXCEPTION(scope, { });
            if (!getInitArgs)
                RELEASE_AND_RETURN(scope, callMethodNamed(m_globalObject, cls, m_vm.pythonNames().dunder_new, cls));
        }
        RELEASE_AND_RETURN(scope, callWithTuple(cls, arguments));
    }

    // load_obj()
    void loadObj()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        if (stackSize() - mark < 1)
            return raiseStackUnderflow();
        PyTuple* arguments = popTuple(mark + 1);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue cls = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = instantiate(cls, arguments);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // A line that is a name. Empty if it raised.
    JSValue readName(ASCIILiteral encoding)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, decodeBytesToObject(m_globalObject, std::span<const uint8_t>(line).first(line.size() - 1), encoding, "strict"_s));
    }

    // load_inst()
    void loadInst()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        // Only Python 2 wrote this, and had no names that are not ASCII.
        JSValue moduleName = readName("ascii"_s);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue className = readName("ascii"_s);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue cls = findClass(moduleName, className);
        RETURN_IF_EXCEPTION(scope, void());
        PyTuple* arguments = popTuple(mark);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = instantiate(cls, arguments);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_newobj(): cls.__new__(cls, *args, **kwargs)
    void loadNewObj(bool usesKeywords)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue keywords;
        if (usesKeywords) {
            keywords = pop();
            RETURN_IF_EXCEPTION(scope, void());
        }
        JSValue arguments = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue cls = pop();
        RETURN_IF_EXCEPTION(scope, void());
        ASCIILiteral opcode = usesKeywords ? "NEWOBJ_EX"_s : "NEWOBJ"_s;
        if (!isClass(cls))
            return raiseUnpicklingError(concatenate(opcode, " class argument must be a type, not "_s, typeName(m_globalObject, cls)));
        JSValue newOfClass = getAttributeIfPresent(m_globalObject, cls, m_vm.pythonNames().dunder_new);
        RETURN_IF_EXCEPTION(scope, void());
        if (!newOfClass)
            return raiseUnpicklingError(concatenate(opcode, " class argument '"_s, typeName(m_globalObject, cls), "' doesn't have __new__"_s));
        if (!isTuple(arguments))
            return raiseUnpicklingError(concatenate(opcode, " args argument must be a tuple, not "_s, typeName(m_globalObject, arguments)));
        if (usesKeywords && !isDict(keywords))
            return raiseUnpicklingError(concatenate(opcode, " kwargs argument must be a dict, not "_s, typeName(m_globalObject, keywords)));
        MarkedArgumentBuffer all;
        all.append(cls);
        for (unsigned i = 0; i < asTuple(arguments)->length(); ++i)
            all.append(asTuple(arguments)->at(i));
        JSValue object = callWithKeywordDict(m_globalObject, newOfClass, all, usesKeywords ? asDict(keywords) : nullptr);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_global()
    void loadGlobal()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue moduleName = readName("utf-8"_s);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue globalName = readName("utf-8"_s);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue global = findClass(moduleName, globalName);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(global));
    }

    // load_stack_global()
    void loadStackGlobal()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue globalName = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue moduleName = pop();
        RETURN_IF_EXCEPTION(scope, void());
        if (!moduleName.isString() || !globalName.isString())
            return raiseUnpicklingError("STACK_GLOBAL requires str"_s);
        JSValue global = findClass(moduleName, globalName);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(global));
    }

    // load_persid()
    void loadPersistentID()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = readLineOfAtLeast(1);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue identifier = decodeBytesToObject(m_globalObject, std::span<const uint8_t>(line).first(line.size() - 1), "ascii"_s, "strict"_s);
        if (scope.exception()) {
            if (catchException(m_globalObject, BuiltinType::UnicodeDecodeError))
                raiseUnpicklingError("persistent IDs in protocol 0 must be ASCII strings"_s);
            return;
        }
        JSValue object = call(m_globalObject, m_self.persistentLoad.get(), identifier);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_binpersid()
    void loadBinPersistentID()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue identifier = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = call(m_globalObject, m_self.persistentLoad.get(), identifier);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_reduce()
    void loadReduce()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue arguments = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue callable = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue object = callWithTuple(callable, arguments);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // load_extension(): what has been given a number to go by.
    void loadExtension(unsigned size)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(size);
        RETURN_IF_EXCEPTION(scope, void());
        int64_t code = binInt(bytes);
        if (code <= 0)
            return raiseUnpicklingError("EXT specifies code <= 0"_s);
        JSValue codeObject = intFromInt64(m_globalObject, code);
        JSValue object = m_state.extensionCache->get(m_globalObject, codeObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (object)
            RELEASE_AND_RETURN(scope, push(object));
        JSValue pair = m_state.invertedRegistry->get(m_globalObject, codeObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (!pair) {
            raiseValueError(m_globalObject, scope, concatenate("unregistered extension code "_s, code));
            return;
        }
        // A program can get at the registry.
        if (!isTuple(pair) || asTuple(pair)->length() != 2 || !stringIn(asTuple(pair)->at(0)) || !stringIn(asTuple(pair)->at(1))) {
            raiseValueError(m_globalObject, scope, concatenate("_inverted_registry["_s, code, "] isn't a 2-tuple of strings"_s));
            return;
        }
        object = findClass(asTuple(pair)->at(0), asTuple(pair)->at(1));
        RETURN_IF_EXCEPTION(scope, void());
        m_state.extensionCache->set(m_globalObject, codeObject, object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, push(object));
    }

    // ---- The stack, and the memo

    // load_pop(). In pickle.py the marks are on the stack with the rest, so it may be a mark that goes.
    void loadPop()
    {
        if (!m_self.marks.isEmpty() && m_self.marks.last() == stackSize()) {
            m_self.marks.removeLast();
            setFenceFromMarks();
        } else if (stackSize() <= m_fence)
            raiseStackUnderflow();
        else
            m_stack.removeLast();
    }

    // load_pop_mark()
    void loadPopMark()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        clearStackTo(mark);
    }

    // load_dup()
    void loadDup()
    {
        if (stackSize() <= m_fence)
            return raiseStackUnderflow();
        push(m_stack.last());
    }

    // load_mark()
    void loadMark()
    {
        m_isMarkSet = true;
        m_fence = stackSize();
        m_self.marks.append(m_fence);
    }

    void pushFromMemo(int64_t index)
    {
        JSValue value = index >= 0 ? memoGet(m_globalObject, m_self, index) : JSValue();
        if (!value)
            return raiseUnpicklingError(concatenate("Memo value not found at index "_s, index));
        push(value);
    }

    // A line that is a number. Nothing if it raised.
    std::optional<int64_t> readIndex()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto line = m_self.inputLine.span();
        JSValue key = intFromLine(line);
        RETURN_IF_EXCEPTION(scope, std::nullopt);
        RELEASE_AND_RETURN(scope, toSsizeOfInt(m_globalObject, key));
    }

    // load_get()
    void loadGet()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, void());
        auto index = readIndex();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, pushFromMemo(*index));
    }

    // load_binget() and load_long_binget()
    void loadBinGet(unsigned size)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(size);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, pushFromMemo(binSize(bytes)));
    }

    // load_put()
    void loadPut()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        readLineOfAtLeast(2);
        RETURN_IF_EXCEPTION(scope, void());
        if (stackSize() <= m_fence)
            return raiseStackUnderflow();
        auto index = readIndex();
        RETURN_IF_EXCEPTION(scope, void());
        if (*index < 0) {
            raiseValueError(m_globalObject, scope, "negative PUT argument"_s);
            return;
        }
        RELEASE_AND_RETURN(scope, memoPut(m_globalObject, m_owner, m_self, *index, m_stack.last()));
    }

    // load_binput() and load_long_binput()
    void loadBinPut(unsigned size)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(size);
        RETURN_IF_EXCEPTION(scope, void());
        if (stackSize() <= m_fence)
            return raiseStackUnderflow();
        RELEASE_AND_RETURN(scope, memoPut(m_globalObject, m_owner, m_self, binSize(bytes), m_stack.last()));
    }

    // load_memoize()
    void loadMemoize()
    {
        if (stackSize() <= m_fence)
            return raiseStackUnderflow();
        memoPut(m_globalObject, m_owner, m_self, m_self.memoLength, m_stack.last());
    }

    // ---- What is added to

    // do_append(): all from there up is appended to what is under it.
    void doAppend(int64_t start)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t size = stackSize();
        if (start > size || start <= m_fence)
            return raiseStackUnderflow();
        if (size == start)
            return;
        JSValue list = m_stack.at(start - 1);
        if (isExactly(m_globalObject, list, m_realm->typeList())) {
            for (int64_t i = start; i < size; ++i) {
                listAppend(m_globalObject, asList(list), m_stack.at(i));
                RETURN_IF_EXCEPTION(scope, void());
            }
            clearStackTo(start);
            return;
        }
        JSValue extend = getAttributeIfPresent(m_globalObject, list, Identifier::fromString(m_vm, "extend"_s));
        RETURN_IF_EXCEPTION(scope, void());
        if (extend) {
            JSArray* slice = popList(start);
            RETURN_IF_EXCEPTION(scope, void());
            scope.release();
            call(m_globalObject, extend, slice);
            return;
        }
        // PEP 307 asks for both, but it has always done with append() alone.
        JSValue append = getAttribute(m_globalObject, list, Identifier::fromString(m_vm, "append"_s));
        RETURN_IF_EXCEPTION(scope, void());
        for (int64_t i = start; i < size; ++i) {
            call(m_globalObject, append, m_stack.at(i));
            if (scope.exception())
                break;
        }
        clearStackTo(start);
    }

    // load_append()
    void loadAppend()
    {
        if (stackSize() - 1 <= m_fence)
            return raiseStackUnderflow();
        doAppend(stackSize() - 1);
    }

    // load_appends()
    void loadAppends()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, doAppend(mark));
    }

    // do_setitems(): all from there up is keys and values, for what is under them, which need not be a dict.
    void doSetItems(int64_t start)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t size = stackSize();
        if (start > size || start <= m_fence)
            return raiseStackUnderflow();
        if (size == start)
            return;
        if ((size - start) % 2)
            return raiseUnpicklingError("odd number of items for SETITEMS"_s);
        JSValue dict = m_stack.at(start - 1);
        bool isExactDict = isExactly(m_globalObject, dict, m_realm->typeDict());
        for (int64_t i = start + 1; i < size; i += 2) {
            if (isExactDict)
                asDict(dict)->set(m_globalObject, m_stack.at(i - 1), m_stack.at(i));
            else
                setItem(m_globalObject, dict, m_stack.at(i - 1), m_stack.at(i));
            if (scope.exception())
                break;
        }
        clearStackTo(start);
    }

    // load_setitems()
    void loadSetItems()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, doSetItems(mark));
    }

    // load_additems()
    void loadAddItems()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t mark = marker();
        RETURN_IF_EXCEPTION(scope, void());
        int64_t size = stackSize();
        if (mark > size || mark <= m_fence)
            return raiseStackUnderflow();
        if (size == mark)
            return;
        JSValue set = m_stack.at(mark - 1);
        if (isSet(set) && Python::typeOf(m_globalObject, set)->isSubtypeOf(m_realm->typeSet())) {
            PyTuple* items = popTuple(mark);
            RETURN_IF_EXCEPTION(scope, void());
            scope.release();
            setUpdate(m_globalObject, uncheckedDowncast<PySet>(set.asCell()), items);
            return;
        }
        JSValue add = getAttribute(m_globalObject, set, Identifier::fromString(m_vm, "add"_s));
        RETURN_IF_EXCEPTION(scope, void());
        for (int64_t i = mark; i < size; ++i) {
            call(m_globalObject, add, m_stack.at(i));
            if (scope.exception())
                break;
        }
        clearStackTo(mark);
    }

    // load_build(): the state of what is under it, which is left there.
    void loadBuild()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        if (stackSize() - 2 < m_fence)
            return raiseStackUnderflow();
        JSValue state = pop();
        RETURN_IF_EXCEPTION(scope, void());
        JSValue instance = m_stack.last();
        JSValue setState = getAttributeIfPresent(m_globalObject, instance, Identifier::fromString(m_vm, "__setstate__"_s));
        RETURN_IF_EXCEPTION(scope, void());
        if (setState) {
            // It sees to everything.
            scope.release();
            call(m_globalObject, setState, state);
            return;
        }

        // There may be the state of the slots besides, from protocol 2.
        JSValue slotState;
        if (isTuple(state) && asTuple(state)->length() == 2) {
            slotState = asTuple(state)->at(1);
            state = asTuple(state)->at(0);
        }
        if (!isNone(state)) {
            if (!isDict(state))
                return raiseUnpicklingError("state is not a dictionary"_s);
            JSValue dict = getAttribute(m_globalObject, instance, names.dunder_dict);
            RETURN_IF_EXCEPTION(scope, void());
            asDict(state)->forEach(m_globalObject, [&] (JSValue key, JSValue value) {
                // The names of attributes are as a rule the one str that there is for the name.
                if (key.isString()) {
                    key = m_realm->intern(m_globalObject, asString(key));
                    if (scope.exception())
                        return false;
                }
                setItem(m_globalObject, dict, key, value);
                return !scope.exception();
            });
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (!slotState)
            return;
        if (!isDict(slotState))
            return raiseUnpicklingError("slot state is not a dictionary"_s);
        asDict(slotState)->forEach(m_globalObject, [&] (JSValue key, JSValue value) {
            // PyObject_SetAttr()
            auto name = attributeName(m_globalObject, scope, key);
            if (scope.exception())
                return false;
            setAttribute(m_globalObject, instance, *name, value);
            return !scope.exception();
        });
    }

    // ---- What says something of the pickle

    // load_proto()
    void loadProto()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(1);
        RETURN_IF_EXCEPTION(scope, void());
        if (bytes[0] > highestPickleProtocol) {
            raiseValueError(m_globalObject, scope, concatenate("unsupported pickle protocol: "_s, static_cast<unsigned>(bytes[0])));
            return;
        }
        m_self.protocol = bytes[0];
    }

    // load_frame()
    void loadFrame()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto bytes = read(8);
        RETURN_IF_EXCEPTION(scope, void());
        int64_t length = binSize(bytes);
        // One is not to begin before the one before it has ended, though what says that it begins may be the last thing in that.
        if (m_self.frameEnd >= 0) {
            if (m_self.nextReadIndex < m_self.frameEnd)
                return raiseUnpicklingError("beginning of a new frame before end of current frame"_s);
            endFrame();
        }
        if (length < 0) {
            raise(m_globalObject, scope, BuiltinType::OverflowError, concatenate("FRAME length exceeds system's maximum of "_s, std::numeric_limits<int64_t>::max(), " bytes"_s));
            return;
        }
        read(length);
        RETURN_IF_EXCEPTION(scope, void());
        // Back to where it begins. Nothing is read past its end until it has all been read.
        m_self.nextReadIndex -= length;
        m_self.frameEnd = m_self.nextReadIndex + length;
        m_self.savedInputLength = m_self.inputLength;
        m_self.inputLength = m_self.frameEnd;
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRealm* m_realm;
    PickleModuleState& m_state;
    PyStateObject* m_owner;
    Unpickler& m_self;

    MarkedArgumentBuffer m_stack;
    bool m_isMarkSet { false };
    int64_t m_fence { 0 }; // Where the last MARK is, or 0. Nothing below it is to be taken off.
};

// ---- Setting one up

// _Unpickler_New()
PyStateObject* newUnpickler(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto* object = PyStateObject::create(vm, pickleModuleState(globalObject).unpicklerType->instanceStructure(), makeUnique<Unpickler>());
    PyTuple* memo = newMemo(globalObject, initialMemoSize);
    RETURN_IF_EXCEPTION(scope, nullptr);
    object->state<Unpickler>().memo.set(vm, object, memo);
    return object;
}

// _Unpickler_SetInputStream()
void setInputStream(JSGlobalObject* globalObject, PyStateObject* owner, Unpickler& self, JSValue file)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto& names = vm.pythonNames();
    auto forget = makeScopeExit([&] {
        self.read.clear();
        self.readinto.clear();
        self.readline.clear();
        self.peek.clear();
    });
    for (auto [slot, name] : { std::pair { &self.peek, &names.attribute_peek }, std::pair { &self.readinto, &names.attribute_readinto }, std::pair { &self.read, &names.attribute_read }, std::pair { &self.readline, &names.attribute_readline } }) {
        JSValue method = getAttributeIfPresent(globalObject, file, *name);
        RETURN_IF_EXCEPTION(scope, void());
        slot->set(vm, owner, method);
    }
    if (!self.readline || !self.read) {
        raiseTypeError(globalObject, scope, "file must have 'read' and 'readline' attributes"_s);
        return;
    }
    forget.release();
}

// _Unpickler_SetBuffers()
void setBuffers(JSGlobalObject* globalObject, PyStateObject* owner, Unpickler& self, JSValue buffers)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!buffers || isNone(buffers)) {
        self.buffers.clear();
        return;
    }
    JSValue iterator = getIterator(globalObject, buffers);
    RETURN_IF_EXCEPTION(scope, void());
    self.buffers.set(vm, owner, iterator);
}

// fix_imports=True, encoding='ASCII', errors='strict' and buffers=(), which all four ways in take, from the argument at `first`. They are looked at in that order before anything is done with any.
struct Options {
    bool fixesImports { true };
    String encoding { "ASCII"_s };
    String errors { "strict"_s };
    JSValue buffers;
};

Options optionsFrom(JSGlobalObject* globalObject, const NativeArguments& args, unsigned first, ASCIILiteral function)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    Options options;
    if (JSValue given = args.at(first)) {
        options.fixesImports = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (JSValue given = args.at(first + 1)) {
        auto text = toTextArgument(globalObject, given, function, "argument 'encoding'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        options.encoding = *text;
    }
    if (JSValue given = args.at(first + 2)) {
        auto text = toTextArgument(globalObject, given, function, "argument 'errors'"_s);
        RETURN_IF_EXCEPTION(scope, { });
        options.errors = *text;
    }
    options.buffers = args.at(first + 3);
    return options;
}

// Unpickler_clear()
void clearUnpickler(Unpickler& self)
{
    self.readline.clear();
    self.readinto.clear();
    self.read.clear();
    self.peek.clear();
    self.persistentLoad.clear();
    self.persistentLoadAttribute.clear();
    self.buffers.clear();
    self.input = ByteVector();
    self.inputLength = 0;
    self.memo.clear();
    self.farMemo.clear();
    self.marks.clear();
    self.inputLine.clear();
    self.encoding = String();
    self.errors = String();
}

// ---- Unpickler

PyStateObject* asUnpickler(JSValue value) { return uncheckedDowncast<PyStateObject>(value.asCell()); }

PYTHON_NATIVE(unpicklerNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Unpickler>()));
}

// Unpickler.__init__(file, *, fix_imports=True, encoding='ASCII', errors='strict', buffers=())
PYTHON_NATIVE(unpicklerInit)
{
    NATIVE_PROLOGUE();
    PyStateObject* owner = asUnpickler(args[0]);
    auto& self = owner->state<Unpickler>();
    Options options = optionsFrom(globalObject, args, 2, "Unpickler"_s);
    RETURN_IF_EXCEPTION(scope, { });
    UsingUnpickler usingUnpickler(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    // It may have been called before.
    if (self.read)
        clearUnpickler(self);
    setInputStream(globalObject, owner, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    self.encoding = options.encoding;
    self.errors = options.errors;
    setBuffers(globalObject, owner, self, options.buffers);
    RETURN_IF_EXCEPTION(scope, { });
    self.fixesImports = options.fixesImports;
    PyTuple* memo = newMemo(globalObject, initialMemoSize);
    RETURN_IF_EXCEPTION(scope, { });
    self.memo.set(vm, owner, memo);
    self.protocol = 0;
    RETURN_NONE();
}

// Unpickler.persistent_load(pid): the one that an Unpickler has unless it is given another.
PYTHON_NATIVE(unpicklerPersistentLoad)
{
    NATIVE_PROLOGUE();
    return JSValue::encode(raise(globalObject, scope, pickleModuleState(globalObject).unpicklingError.get(), "A load persistent id instruction was encountered, but no persistent_load function was specified."_s));
}

// Unpickler.load()
PYTHON_NATIVE(unpicklerLoad)
{
    NATIVE_PROLOGUE();
    PyStateObject* owner = asUnpickler(args[0]);
    auto& self = owner->state<Unpickler>();
    // A class derived from it may have an __init__() that does not call this one's.
    if (!self.read)
        return JSValue::encode(raise(globalObject, scope, pickleModuleState(globalObject).unpicklingError.get(), concatenate("Unpickler.__init__() was not called by "_s, typeName(globalObject, owner), ".__init__()"_s)));
    UsingUnpickler usingUnpickler(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    Loader loader(globalObject, owner);
    RELEASE_AND_RETURN(scope, JSValue::encode(loader.load()));
}

// Unpickler.find_class(module_name, global_name). It finds whatever is in a module, and not only classes.
PYTHON_NATIVE(unpicklerFindClass)
{
    NATIVE_PROLOGUE();
    auto& self = stateOf<Unpickler>(args[0]);
    auto& state = pickleModuleState(globalObject);
    JSValue moduleName = args[1];
    JSValue globalName = args[2];
    if (!audit(globalObject, "pickle.find_class"_s, moduleName, globalName))
        return { };

    // What Python 2 called it, if it may have been Python 2 that wrote this.
    if (self.protocol < 3 && self.fixesImports) {
        JSValue item = state.nameMapping2To3->get(globalObject, PyTuple::create(globalObject, { moduleName, globalName }));
        RETURN_IF_EXCEPTION(scope, { });
        if (item) {
            if (!isTuple(item) || asTuple(item)->length() != 2)
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.NAME_MAPPING values should be 2-tuples, not "_s, typeName(globalObject, item))));
            moduleName = asTuple(item)->at(0);
            globalName = asTuple(item)->at(1);
            if (!stringIn(moduleName) || !stringIn(globalName))
                return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.NAME_MAPPING values should be pairs of str, not ("_s, typeName(globalObject, moduleName), ", "_s, typeName(globalObject, globalName), ')')));
        } else {
            item = state.importMapping2To3->get(globalObject, moduleName);
            RETURN_IF_EXCEPTION(scope, { });
            if (item) {
                if (!stringIn(item))
                    return JSValue::encode(raise(globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.IMPORT_MAPPING values should be strings, not "_s, typeName(globalObject, item))));
                moduleName = item;
            }
        }
    }

    // PyImport_Import(), and not what sys.modules has, which may not have been run to its end.
    JSString* moduleNameString = stringIn(moduleName);
    if (!moduleNameString)
        return JSValue::encode(raiseTypeError(globalObject, scope, "module name must be a string"_s));
    String moduleNameText = moduleNameString->value(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue module = importModule(globalObject, moduleNameText);
    RETURN_IF_EXCEPTION(scope, { });

    if (self.protocol < 4) {
        auto name = attributeName(globalObject, scope, globalName);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, JSValue::encode(getAttribute(globalObject, module, *name)));
    }
    MarkedArgumentBuffer path;
    appendDottedPath(globalObject, globalName, path);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue global = getAttributeByPath(globalObject, module, path, true);
    if (scope.exception() && path.size() > 1) {
        Exception* raised = takeRaisedException(vm);
        if (!raised)
            return { };
        String shownName = repr(globalObject, globalName);
        if (!scope.exception()) {
            String shownModule = repr(globalObject, moduleName);
            if (!scope.exception())
                raise(globalObject, scope, BuiltinType::AttributeError, concatenate("Can't resolve path "_s, shownName, " on module "_s, shownModule));
        }
        chainRaisedExceptions(globalObject, raised);
        return { };
    }
    return JSValue::encode(global);
}

// Unpickler.__sizeof__(). The numbers are what they would be in CPython.
PYTHON_NATIVE(unpicklerSizeOf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& self = stateOf<Unpickler>(args[0]);
    int64_t size = Python::typeOf(globalObject, args[0])->basicSize();
    if (self.memo)
        size += self.memo->length() * 8;
    size += self.marks.capacity() * 8;
    if (!self.inputLine.isEmpty())
        size += self.inputLine.size() + 1;
    if (!self.encoding.isNull())
        size += self.encoding.utf8().length() + 1;
    if (!self.errors.isNull())
        size += self.errors.utf8().length() + 1;
    return JSValue::encode(intFromInt64(globalObject, size));
}

// Unpickler_getattr()
PYTHON_NATIVE(unpicklerGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSValue set = stateOf<Unpickler>(args[0]).persistentLoadAttribute.get(); set && name->string() == "persistent_load"_s)
        return JSValue::encode(set);
    RELEASE_AND_RETURN(scope, JSValue::encode(getAttributeAsObjectDoes(globalObject, args[0], *name)));
}

// Unpickler_setattr()
PYTHON_NATIVE(unpicklerSetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = args.at(2);
    if (name->string() == "persistent_load"_s)
        stateOf<Unpickler>(args[0]).persistentLoadAttribute.set(vm, args[0].asCell(), value);
    else {
        setAttributeAsObjectDoes(globalObject, args[0], *name, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- UnpicklerMemoProxy: so that unpickler.memo.clear() and unpickler.memo = saved go on working, as they did when it was a dict.

struct UnpicklerMemoProxy final : NativeState {
    PYTHON_NATIVE_STATE(UnpicklerMemoProxy);
    WriteBarrier<PyStateObject> unpickler;
};

template<typename Visitor> void UnpicklerMemoProxy::visit(Visitor& visitor) { visitor.append(unpickler); }

PYTHON_NATIVE(unpicklerMemoProxyClear)
{
    NATIVE_PROLOGUE();
    PyStateObject* unpickler = stateOf<UnpicklerMemoProxy>(args[0]).unpickler.get();
    auto& self = unpickler->state<Unpickler>();
    PyTuple* memo = newMemo(globalObject, self.memo ? self.memo->length() : 0);
    RETURN_IF_EXCEPTION(scope, { });
    self.memo.set(vm, unpickler, memo);
    self.farMemo.clear();
    RETURN_NONE();
}

// { number: object }. Empty if it raised.
JSValue copyOfMemo(JSGlobalObject* globalObject, Unpickler& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyDict* copy = PyDict::create(globalObject);
    for (unsigned i = 0; self.memo && i < self.memo->length(); ++i) {
        JSValue value = self.memo->at(i);
        if (!value)
            continue;
        copy->set(globalObject, intFromUInt64(globalObject, i), value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    if (self.farMemo) {
        copy->mergeFrom(globalObject, *self.farMemo.get());
        RETURN_IF_EXCEPTION(scope, { });
    }
    return copy;
}

PYTHON_NATIVE(unpicklerMemoProxyCopy)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(copyOfMemo(globalObject, stateOf<UnpicklerMemoProxy>(args[0]).unpickler->state<Unpickler>())));
}

PYTHON_NATIVE(unpicklerMemoProxyReduce)
{
    NATIVE_PROLOGUE();
    JSValue contents = copyOfMemo(globalObject, stateOf<UnpicklerMemoProxy>(args[0]).unpickler->state<Unpickler>());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { realm->typeDict()->object(), PyTuple::create(globalObject, { contents }) }));
}

// Unpickler_get_memo()
JSValue getMemoOfUnpickler(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto* proxy = PyStateObject::create(vm, pickleModuleState(globalObject).unpicklerMemoProxyType->instanceStructure(), makeUnique<UnpicklerMemoProxy>());
    proxy->state<UnpicklerMemoProxy>().unpickler.set(vm, proxy, asUnpickler(self));
    return proxy;
}

// Unpickler_set_memo()
void setMemoOfUnpickler(JSGlobalObject* globalObject, JSValue selfValue, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyStateObject* owner = asUnpickler(selfValue);
    auto& self = owner->state<Unpickler>();
    if (!given) {
        raiseTypeError(globalObject, scope, "attribute deletion is not supported"_s);
        return;
    }
    PyTuple* memo;
    PyDict* farMemo = nullptr;
    if (Python::typeOf(globalObject, given) == pickleModuleState(globalObject).unpicklerMemoProxyType.get()) {
        auto& from = stateOf<UnpicklerMemoProxy>(given).unpickler->state<Unpickler>();
        PyTuple* other = from.memo.get();
        if (from.farMemo) {
            farMemo = PyDict::create(globalObject);
            farMemo->mergeFrom(globalObject, *from.farMemo.get());
            RETURN_IF_EXCEPTION(scope, void());
        }
        memo = newMemo(globalObject, other->length());
        RETURN_IF_EXCEPTION(scope, void());
        for (unsigned i = 0; i < other->length(); ++i) {
            if (JSValue value = other->at(i))
                memo->initializeAt(vm, i, value);
        }
    } else if (isDict(given)) {
        memo = newMemo(globalObject, asDict(given)->size());
        RETURN_IF_EXCEPTION(scope, void());
        // CPython puts them in the memo that it is about to throw away, so that there is nothing in the one that it is left with. All that comes of it is that they are counted.
        asDict(given)->forEach(globalObject, [&] (JSValue key, JSValue value) {
            if (!classify(key).isInt()) {
                raiseTypeError(globalObject, scope, "memo key must be integers"_s);
                return false;
            }
            auto index = toSsizeOfInt(globalObject, key);
            if (scope.exception())
                return false;
            if (*index < 0) {
                raiseValueError(globalObject, scope, "memo key must be positive integers."_s);
                return false;
            }
            memoPut(globalObject, owner, self, *index, value);
            return !scope.exception();
        });
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        raiseTypeError(globalObject, scope, concatenate("'memo' attribute must be an UnpicklerMemoProxy object or dict, not "_s, typeName(globalObject, given)));
        return;
    }
    self.memo.set(vm, owner, memo);
    self.farMemo.setMayBeNull(vm, owner, farMemo);
}

// ---- load() and loads()

// load(file, *, fix_imports=True, encoding='ASCII', errors='strict', buffers=())
PYTHON_NATIVE(pickleLoad)
{
    NATIVE_PROLOGUE();
    Options options = optionsFrom(globalObject, args, 1, "load"_s);
    RETURN_IF_EXCEPTION(scope, { });
    PyStateObject* unpickler = newUnpickler(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = unpickler->state<Unpickler>();
    setInputStream(globalObject, unpickler, self, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    self.encoding = options.encoding;
    self.errors = options.errors;
    setBuffers(globalObject, unpickler, self, options.buffers);
    RETURN_IF_EXCEPTION(scope, { });
    self.fixesImports = options.fixesImports;
    Loader loader(globalObject, unpickler);
    RELEASE_AND_RETURN(scope, JSValue::encode(loader.load()));
}

// loads(data, /, *, fix_imports=True, encoding='ASCII', errors='strict', buffers=())
PYTHON_NATIVE(pickleLoads)
{
    NATIVE_PROLOGUE();
    Options options = optionsFrom(globalObject, args, 1, "loads"_s);
    RETURN_IF_EXCEPTION(scope, { });
    PyStateObject* unpickler = newUnpickler(globalObject);
    RETURN_IF_EXCEPTION(scope, { });
    auto& self = unpickler->state<Unpickler>();
    setStringInput(globalObject, self, args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    self.encoding = options.encoding;
    self.errors = options.errors;
    setBuffers(globalObject, unpickler, self, options.buffers);
    RETURN_IF_EXCEPTION(scope, { });
    self.fixesImports = options.fixesImports;
    Loader loader(globalObject, unpickler);
    RELEASE_AND_RETURN(scope, JSValue::encode(loader.load()));
}

} // namespace

void initializeUnpickler(JSGlobalObject* globalObject)
{
    VM& vm = globalObject->vm();
    PyRealm* realm = globalObject->pyRealm();
    using Kind = PyNativeFunction::Kind;
    using Arguments = PyNativeFunction::Arguments;
    auto& state = pickleModuleState(globalObject);
    auto make = [&] (WriteBarrier<PyType>& slot, ASCIILiteral name) {
        PyType* type = createBuiltinType(globalObject, name, realm->typeObject(), PyType::Layout::Native, PyType::IsBaseType);
        type->setInstanceStructure(vm, PyStateObject::createStructure(vm, globalObject, type));
        slot.set(vm, realm, type);
        return type;
    };

    PyType* unpickler = make(state.unpicklerType, "_pickle.Unpickler"_s);
    addMethods(globalObject, unpickler, {
        { "__new__"_s, unpicklerNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
        { "__init__"_s, unpicklerInit, Kind::Wrapper, 0, { }, Arguments::AreThoseOfTheClass },
        { "__getattribute__"_s, unpicklerGetAttribute },
        { "__setattr__"_s, unpicklerSetAttribute },
        { "__delattr__"_s, unpicklerSetAttribute },
        { "persistent_load"_s, unpicklerPersistentLoad, Kind::Method, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
        { "load"_s, unpicklerLoad, Kind::Method, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
        { "find_class"_s, unpicklerFindClass, Kind::Method, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
        { "__sizeof__"_s, unpicklerSizeOf },
    });
    addGetSet(globalObject, unpickler, "memo"_s, getMemoOfUnpickler, setMemoOfUnpickler);

    PyType* proxy = make(state.unpicklerMemoProxyType, "_pickle.UnpicklerMemoProxy"_s);
    addMethods(globalObject, proxy, {
        { "clear"_s, unpicklerMemoProxyClear },
        { "copy"_s, unpicklerMemoProxyCopy },
        { "__reduce__"_s, unpicklerMemoProxyReduce },
    });
    proxy->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
}

void addUnpicklerFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    addFunction(globalObject, module, "load"_s, pickleLoad);
    addFunction(globalObject, module, "loads"_s, pickleLoads);
}

} } // namespace JSC::Python
