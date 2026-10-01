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
#include "PyRealm.h"
#include "PyTuple.h"
#include "PythonBuiltins.h"
#include "PythonBytes.h"
#include "PythonIO.h"
#include "PythonImport.h"
#include "PythonNumbers.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonStructMember.h"
#include "PythonText.h"
#include "TopExceptionScope.h"
#include <wtf/Scope.h>

// Pickler, dump() and dumps(): the first half of Modules/_pickle.c. See PythonPickle.h.

namespace JSC { namespace Python {

namespace {

// PicklerObject
struct Pickler final : NativeState {
    PYTHON_NATIVE_STATE(Pickler);

    // PyMemoTable: what has been written already, and what it is known by, so that what is come to twice, or is inside itself, is written once. It goes by which object it is. The list is what keeps them.
    UncheckedKeyHashMap<JSCell*, int64_t> memo;
    WriteBarrier<JSArray> memoKeys;
    bool hasMemo { false }; // It has none until __init__() has been called.

    WriteBarrier<Unknown> persistentID; // The method, while something is being written. It may be empty.
    WriteBarrier<Unknown> persistentIDAttribute; // What the attribute has been set to
    WriteBarrier<Unknown> dispatchTable; // Its own
    WriteBarrier<Unknown> reducerOverride;
    WriteBarrier<Unknown> write; // The write() of what it writes to
    WriteBarrier<Unknown> fastMemo;
    WriteBarrier<Unknown> bufferCallback;

    ByteVector output; // What has not been given to write() yet
    int protocol { 0 };
    int isBinary { 0 }; // Whether the protocol is other than 0
    bool isFraming { false }; // From protocol 4
    std::optional<size_t> frameStart; // Where in the output the frame begins that is being written
    int isFast { 0 }; // With no memo: nothing superfluous is written, and nothing can be inside itself.
    int fastNesting { 0 };
    bool fixesImports { false }; // Whether things are called what Python 2 calls them
    bool isRunning { false };
};

template<typename Visitor>
void Pickler::visit(Visitor& visitor)
{
    visitor.append(memoKeys);
    visitor.append(persistentID);
    visitor.append(persistentIDAttribute);
    visitor.append(dispatchTable);
    visitor.append(reducerOverride);
    visitor.append(write);
    visitor.append(fastMemo);
    visitor.append(bufferCallback);
}

// PyMemoTable_Clear() and PyMemoTable_New()
void clearMemo(JSGlobalObject* globalObject, PyStateObject* owner, Pickler& self)
{
    self.memo.clear();
    self.memoKeys.set(globalObject->vm(), owner, newList(globalObject));
    self.hasMemo = true;
}

// PyMemoTable_Set()
void setMemo(JSGlobalObject* globalObject, Pickler& self, JSValue key, int64_t value)
{
    if (self.memo.set(key.asCell(), value).isNewEntry)
        listAppend(globalObject, self.memoKeys.get(), key);
}

// BEGIN_USING_PICKLER() and END_USING_PICKLER()
class UsingPickler {
public:
    UsingPickler(JSGlobalObject* globalObject, Pickler& self)
        : m_self(self)
    {
        if (self.isRunning) {
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            raise(globalObject, scope, BuiltinType::RuntimeError, "Pickler object is already used"_s);
            return;
        }
        self.isRunning = true;
        m_isUsing = true;
    }
    ~UsingPickler()
    {
        if (m_isUsing)
            m_self.isRunning = false;
    }

private:
    Pickler& m_self;
    bool m_isUsing { false };
};

JSC_DECLARE_HOST_FUNCTION(picklerPersistentID);

// What is written while one object is, with all that is in it. Each function is the one of the same name, or near it, in _pickle.c.
class Saver {
public:
    Saver(JSGlobalObject* globalObject, PyStateObject* owner)
        : m_globalObject(globalObject)
        , m_vm(globalObject->vm())
        , m_realm(globalObject->pyRealm())
        , m_state(pickleModuleState(globalObject))
        , m_owner(owner)
        , m_self(owner->state<Pickler>())
    {
    }

    // _Pickler_ClearBuffer()
    void clearBuffer()
    {
        m_self.output = ByteVector();
        m_self.frameStart = std::nullopt;
    }

    // _Pickler_GetString(). Empty if it raised.
    JSValue takeString()
    {
        commitFrame();
        ByteVector output = std::exchange(m_self.output, ByteVector());
        return newBytes(m_globalObject, output);
    }

    // _Pickler_FlushToFile()
    void flushToFile()
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue output = takeString();
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        call(m_globalObject, m_self.write.get(), output);
    }

    // dump()
    void dump(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        // They are methods of the Pickler, which has everything that it has written. So it is not left with them.
        auto forget = makeScopeExit([&] {
            m_self.isFraming = false;
            m_self.persistentID.clear();
            m_self.reducerOverride.clear();
        });

        JSValue persistentID = getAttribute(m_globalObject, m_owner, Identifier::fromString(m_vm, "persistent_id"_s));
        RETURN_IF_EXCEPTION(scope, void());
        // The one that does nothing is not called.
        if (auto* bound = tryBoundMethod(persistentID); bound && bound->self() == JSValue(m_owner)) {
            if (auto* function = dynamicDowncast<PyNativeFunction>(bound->function()); function && function->nativeFunction() == TaggedNativeFunction(picklerPersistentID))
                persistentID = JSValue();
        }
        m_self.persistentID.set(m_vm, m_owner, persistentID);

        JSValue reducerOverride = getAttributeIfPresent(m_globalObject, m_owner, Identifier::fromString(m_vm, "reducer_override"_s));
        RETURN_IF_EXCEPTION(scope, void());
        m_self.reducerOverride.set(m_vm, m_owner, reducerOverride);
        UNUSED_PARAM(names);

        if (m_self.protocol >= 2) {
            write(Opcode::Proto);
            write(static_cast<uint8_t>(m_self.protocol));
            if (m_self.protocol >= 4)
                m_self.isFraming = true;
        }
        save(object);
        RETURN_IF_EXCEPTION(scope, void());
        write(Opcode::Stop);
        commitFrame();
    }

private:
    // ---- What is written to

    // _Pickler_Write(). If there is no room it is found out when a bytes is made of it.
    void write(std::span<const uint8_t> data)
    {
        if (m_self.isFraming && !m_self.frameStart) {
            m_self.frameStart = m_self.output.size();
            // What is not valid, until it is known how long the frame is
            m_self.output.appendFill(0xfe, frameHeaderSize);
        }
        m_self.output.append(data);
    }
    void write(uint8_t byte) { write(std::span<const uint8_t> { &byte, 1 }); }
    void write(Opcode opcode) { write(static_cast<uint8_t>(opcode)); }
    void write(ASCIILiteral text) { write(byteCast<uint8_t>(text.span8())); }
    void write(const String& ascii)
    {
        ASSERT(ascii.containsOnlyASCII());
        write(byteCast<uint8_t>(ascii.span8()));
    }
    void writeLittleEndian(uint64_t value, unsigned count)
    {
        std::array<uint8_t, 8> bytes;
        for (unsigned i = 0; i < count; ++i)
            bytes[i] = static_cast<uint8_t>(value >> (8 * i));
        write(std::span<const uint8_t>(bytes).first(count));
    }

    // _Pickler_CommitFrame()
    void commitFrame()
    {
        if (!m_self.isFraming || !m_self.frameStart || m_self.output.hasOverflowed())
            return;
        size_t start = *m_self.frameStart;
        size_t length = m_self.output.size() - start - frameHeaderSize;
        auto frame = m_self.output.mutableSpan().subspan(start);
        if (length >= frameSizeMinimum) {
            frame[0] = static_cast<uint8_t>(Opcode::Frame);
            for (unsigned i = 0; i < 8; ++i)
                frame[1 + i] = static_cast<uint8_t>(static_cast<uint64_t>(length) >> (8 * i));
        } else {
            memmoveSpan(frame, frame.subspan(frameHeaderSize));
            m_self.output.shrink(m_self.output.size() - frameHeaderSize);
        }
        m_self.frameStart = std::nullopt;
    }

    // _Pickler_OpcodeBoundary()
    void opcodeBoundary()
    {
        if (!m_self.isFraming || !m_self.frameStart) [[likely]]
            return;
        if (m_self.output.size() - *m_self.frameStart - frameHeaderSize < frameSizeTarget) [[likely]]
            return;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        commitFrame();
        // What is being written to a file is given it a frame at a time, so that no more than that is kept.
        if (m_self.write) {
            flushToFile();
            RETURN_IF_EXCEPTION(scope, void());
            clearBuffer();
        }
    }

    // _Pickler_write_bytes(): what says what comes, and then that, which if it is long goes straight to the file and is in no frame. `payload` is an object that has the same bytes, if there is one.
    void writeBytes(std::span<const uint8_t> header, std::span<const uint8_t> data, JSValue payload)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        bool bypassesBuffer = data.size() >= frameSizeTarget;
        bool wasFraming = m_self.isFraming;
        if (bypassesBuffer) {
            commitFrame();
            m_self.isFraming = false;
        }
        write(header);
        if (bypassesBuffer && m_self.write) {
            // Where the bytes are is not to be relied on once anything has been called.
            if (!payload) {
                payload = newBytes(m_globalObject, data);
                RETURN_IF_EXCEPTION(scope, void());
            }
            flushToFile();
            RETURN_IF_EXCEPTION(scope, void());
            call(m_globalObject, m_self.write.get(), payload);
            RETURN_IF_EXCEPTION(scope, void());
            clearBuffer();
        } else
            write(data);
        m_self.isFraming = wasFraming;
    }

    // ---- What goes wrong

    void raisePicklingError(const String& message)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, m_state.picklingError.get(), message);
    }

    String typeNameOf(JSValue value) { return fullyQualifiedTypeName(m_globalObject, value); } // %T

    // _Py_EnterRecursiveCall(" while pickling an object")
    void checkDepth()
    {
        if (m_vm.isSafeToRecurse()) [[likely]]
            return;
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        raise(m_globalObject, scope, BuiltinType::RecursionError, "maximum recursion depth exceeded while pickling an object"_s);
    }

    // save(), and if it raises, _PyErr_FormatNote().
    template<typename MakeNote>
    void save(JSValue object, const MakeNote& makeNote)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        save(object);
        if (scope.exception()) [[unlikely]]
            addNoteToRaised(m_globalObject, [&] () -> String { return makeNote(); });
    }

    // ---- The memo

    bool isInMemo(JSValue object) { return m_self.memo.contains(object.asCell()); }

    // memo_get(): writes what gets it back.
    void memoGet(JSValue key)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto found = m_self.memo.find(key.asCell());
        if (found == m_self.memo.end()) {
            raise(m_globalObject, scope, BuiltinType::KeyError, key);
            return;
        }
        int64_t value = found->value;
        if (!m_self.isBinary) {
            write(Opcode::Get);
            write(makeString(value, '\n'));
        } else if (value < 256) {
            write(Opcode::BinGet);
            write(static_cast<uint8_t>(value));
        } else if (static_cast<uint64_t>(value) <= 0xffffffff) {
            write(Opcode::LongBinGet);
            writeLittleEndian(value, 4);
        } else
            raisePicklingError("memo id too large for LONG_BINGET"_s);
    }

    // memo_put(): it is known from now on by how many there were before it, and what says so is written.
    void memoPut(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_self.isFast)
            return;
        int64_t index = m_self.memo.size();
        setMemo(m_globalObject, m_self, object, index);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_self.protocol >= 4)
            write(Opcode::Memoize);
        else if (!m_self.isBinary) {
            write(Opcode::Put);
            write(makeString(index, '\n'));
        } else if (index < 256) {
            write(Opcode::BinPut);
            write(static_cast<uint8_t>(index));
        } else if (static_cast<uint64_t>(index) <= 0xffffffff) {
            write(Opcode::LongBinPut);
            writeLittleEndian(index, 4);
        } else
            raisePicklingError("memo id too large for LONG_BINPUT"_s);
    }

    // ---- With no memo

    JSValue identityOf(JSValue object) { return intFromInt64(m_globalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(object.asCell()))); }

    // fast_save_enter() and fast_save_leave(): past a certain depth, what is being written is kept note of, so that what has itself in it is said to, which is better than running out of stack.
    void fastSaveEnter(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (++m_self.fastNesting < fastNestingLimit)
            return;
        if (!m_self.fastMemo)
            m_self.fastMemo.set(m_vm, m_owner, PyDict::create(m_globalObject));
        PyDict* memo = asDict(m_self.fastMemo.get());
        JSValue key = identityOf(object);
        bool isThere = memo->contains(m_globalObject, key);
        if (!scope.exception()) {
            if (isThere)
                raiseValueError(m_globalObject, scope, concatenate("fast mode: can't pickle cyclic objects including object type "_s, typeName(m_globalObject, object), " at "_s, addressOf(object.asCell())));
            else
                memo->set(m_globalObject, key, jsUndefined());
        }
        if (scope.exception())
            m_self.fastNesting = -1;
    }

    void fastSaveLeave(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_self.fastNesting-- < fastNestingLimit)
            return;
        JSValue key = identityOf(object);
        JSValue removed = asDict(m_self.fastMemo.get())->remove(m_globalObject, key);
        RETURN_IF_EXCEPTION(scope, void());
        if (!removed)
            raise(m_globalObject, scope, BuiltinType::KeyError, key);
    }

    // Calls the function between the two, if there is no memo. What the second raises is what is raised, unless something has been already.
    template<typename Function>
    void guardingAgainstCycles(JSValue object, const Function& function)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_self.isFast)
            RELEASE_AND_RETURN(scope, function());
        fastSaveEnter(object);
        if (!scope.exception())
            function();
        Exception* raised = takeRaisedException(m_vm);
        fastSaveLeave(object);
        if (raised && (!scope.exception() || scope.tryClearException()))
            restoreRaisedException(m_globalObject, raised);
    }

    // ---- What is written as it is

    // save_bool()
    void saveBool(bool value)
    {
        if (m_self.protocol >= 2)
            write(value ? Opcode::NewTrue : Opcode::NewFalse);
        else
            write(value ? "I01\n"_s : "I00\n"_s); // An int, to what was written before there were any
    }

    // save_long()
    void saveLong(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        Number number = classify(object);
        if (number.kind == Number::Kind::Small) {
            int32_t value = number.small;
            if (!m_self.isBinary) {
                write(Opcode::Int);
                write(makeString(value, '\n'));
                return;
            }
            auto bits = static_cast<uint32_t>(value);
            if (bits >> 16) {
                write(Opcode::BinInt);
                writeLittleEndian(bits, 4);
            } else if (bits >> 8) {
                write(Opcode::BinInt2);
                writeLittleEndian(bits, 2);
            } else {
                write(Opcode::BinInt1);
                write(static_cast<uint8_t>(bits));
            }
            return;
        }

        if (m_self.protocol < 2) {
            // As it is written out, which takes time that goes by the square of how long it is, both ways. The L is for Python 2.
            String text = reprOfInt(m_globalObject, number);
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::Long);
            write(text);
            write("L\n"_s);
            return;
        }

        // The bytes of it, the least first, a negative one as what it falls short of a power of 256 by. There is a byte more than it has bits for, since the top bit says which it is, and that is taken back if it
        // turns out not to have been needed: for -(2**(8*j-1)).
        bool isNegative = JSBigInt::compare(number.big, static_cast<int64_t>(0)) == JSBigInt::ComparisonResult::LessThan;
        size_t count = static_cast<size_t>(bitLengthOfInt(number) >> 3) + 1;
        if (count > 0x7fffffff) {
            raise(m_globalObject, scope, BuiltinType::OverflowError, "int too large to pickle"_s);
            return;
        }
        ByteVector bytes;
        auto digits = digitsOfInt(number);
        for (size_t i = 0; i < count; ++i)
            bytes.append(i / 8 < digits.size() ? static_cast<uint8_t>(digits[i / 8] >> (8 * (i % 8))) : 0);
        if (bytes.hasOverflowed()) {
            raiseMemoryError(m_globalObject, scope);
            return;
        }
        if (isNegative) {
            unsigned carry = 1;
            for (auto& byte : bytes.mutableSpan()) {
                unsigned sum = static_cast<uint8_t>(~byte) + carry;
                byte = static_cast<uint8_t>(sum);
                carry = sum >> 8;
            }
            if (count > 1 && bytes[count - 1] == 0xff && (bytes[count - 2] & 0x80))
                --count;
        }
        if (count < 256) {
            write(Opcode::Long1);
            write(static_cast<uint8_t>(count));
        } else {
            write(Opcode::Long4);
            writeLittleEndian(count, 4);
        }
        write(bytes.span().first(count));
    }

    // save_float()
    void saveFloat(double value)
    {
        if (m_self.isBinary) {
            std::array<uint8_t, 8> bytes;
            packFloat8(value, bytes, false);
            write(Opcode::BinFloat);
            write(bytes);
            return;
        }
        write(Opcode::Float);
        write(reprOfDouble(value));
        write('\n');
    }

    // ---- bytes, bytearray and PickleBuffer

    // _save_bytes_data()
    void saveBytesData(JSValue object, std::span<const uint8_t> data)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        ASSERT(m_self.protocol >= 3);
        std::array<uint8_t, 9> header;
        size_t headerSize;
        uint64_t size = data.size();
        auto fill = [&] (Opcode opcode, unsigned count) {
            header[0] = static_cast<uint8_t>(opcode);
            for (unsigned i = 0; i < count; ++i)
                header[1 + i] = static_cast<uint8_t>(size >> (8 * i));
            headerSize = 1 + count;
        };
        if (size <= 0xff)
            fill(Opcode::ShortBinBytes, 1);
        else if (size <= 0xffffffff)
            fill(Opcode::BinBytes, 4);
        else if (m_self.protocol >= 4)
            fill(Opcode::BinBytes8, 8);
        else {
            raise(m_globalObject, scope, BuiltinType::OverflowError, "serializing a bytes object larger than 4 GiB requires pickle protocol 4 or higher"_s);
            return;
        }
        writeBytes(std::span<const uint8_t>(header).first(headerSize), data, object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // save_bytes()
    void saveBytes(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto data = *builtinBufferOf(object);
        if (m_self.protocol >= 3)
            RELEASE_AND_RETURN(scope, saveBytesData(object, data));
        // There is no way to write one. It is written as what makes one, in Python 3, and in Python 2 a str, which is what bytes was there.
        JSValue reduceValue;
        if (data.empty())
            reduceValue = PyTuple::create(m_globalObject, { m_realm->typeBytes()->object(), PyTuple::create(m_globalObject, 0) });
        else {
            String text = textOfBytes(m_globalObject, data);
            RETURN_IF_EXCEPTION(scope, void());
            reduceValue = PyTuple::create(m_globalObject, { m_state.codecsEncode.get(), PyTuple::create(m_globalObject, { jsString(m_vm, text), internedString(m_vm, Identifier::fromString(m_vm, "latin1"_s)) }) });
        }
        RELEASE_AND_RETURN(scope, saveReduce(reduceValue, object));
    }

    // _save_bytearray_data()
    void saveByteArrayData(JSValue object, std::span<const uint8_t> data)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        ASSERT(m_self.protocol >= 5);
        std::array<uint8_t, 9> header;
        header[0] = static_cast<uint8_t>(Opcode::ByteArray8);
        for (unsigned i = 0; i < 8; ++i)
            header[1 + i] = static_cast<uint8_t>(static_cast<uint64_t>(data.size()) >> (8 * i));
        writeBytes(header, data, object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // save_bytearray()
    void saveByteArray(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto data = *builtinBufferOf(object);
        if (m_self.protocol >= 5)
            RELEASE_AND_RETURN(scope, saveByteArrayData(object, data));
        JSValue arguments;
        if (data.empty())
            arguments = PyTuple::create(m_globalObject, 0);
        else {
            JSValue bytes = newBytes(m_globalObject, data);
            RETURN_IF_EXCEPTION(scope, void());
            arguments = PyTuple::create(m_globalObject, { bytes });
        }
        RELEASE_AND_RETURN(scope, saveReduce(PyTuple::create(m_globalObject, { m_realm->typeByteArray()->object(), arguments }), object));
    }

    // save_picklebuffer()
    void savePickleBuffer(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_self.protocol < 5)
            RELEASE_AND_RETURN(scope, raisePicklingError("PickleBuffer can only be pickled with protocol >= 5"_s));
        // A view of its own, since what is called below may release the PickleBuffer.
        PyMemoryView* view = memoryViewOf(m_globalObject, object, FullReadOnlyBuffer);
        RETURN_IF_EXCEPTION(scope, void());
        auto release = makeScopeExit([&] { view->release(m_globalObject); });
        if (!view->isCContiguous() && !view->isFortranContiguous())
            RELEASE_AND_RETURN(scope, raisePicklingError("PickleBuffer can not be pickled when pointing to a non-contiguous buffer"_s));
        bool isInBand = true;
        if (m_self.bufferCallback) {
            JSValue result = call(m_globalObject, m_self.bufferCallback.get(), object);
            RETURN_IF_EXCEPTION(scope, void());
            isInBand = isTrue(m_globalObject, result);
            RETURN_IF_EXCEPTION(scope, void());
        }
        bool isReadOnly = view->isReadOnly();
        if (isInBand) {
            // As they lie
            auto data = view->span().value_or(std::span<const uint8_t>());
            if (isReadOnly)
                RELEASE_AND_RETURN(scope, saveBytesData(object, data));
            RELEASE_AND_RETURN(scope, saveByteArrayData(object, data));
        }
        write(Opcode::NextBuffer);
        if (isReadOnly)
            write(Opcode::ReadOnlyBuffer);
    }

    // ---- str

    // As UTF-8, but that half of a surrogate pair is written as any other character is: "surrogatepass".
    static void appendUTF8(ByteVector& out, StringView text)
    {
        for (char32_t c : text.codePoints()) {
            if (c < 0x80)
                out.append(static_cast<uint8_t>(c));
            else if (c < 0x800)
                out.appendList({ static_cast<uint8_t>(0xc0 | c >> 6), static_cast<uint8_t>(0x80 | (c & 0x3f)) });
            else if (c < 0x10000)
                out.appendList({ static_cast<uint8_t>(0xe0 | c >> 12), static_cast<uint8_t>(0x80 | (c >> 6 & 0x3f)), static_cast<uint8_t>(0x80 | (c & 0x3f)) });
            else
                out.appendList({ static_cast<uint8_t>(0xf0 | c >> 18), static_cast<uint8_t>(0x80 | (c >> 12 & 0x3f)), static_cast<uint8_t>(0x80 | (c >> 6 & 0x3f)), static_cast<uint8_t>(0x80 | (c & 0x3f)) });
        }
    }

    // write_unicode_binary()
    void writeUnicodeBinary(const String& text)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        ByteVector encoded;
        std::span<const uint8_t> data;
        if (text.is8Bit() && charactersAreAllASCII(text.span8()))
            data = byteCast<uint8_t>(text.span8());
        else {
            appendUTF8(encoded, text);
            if (encoded.hasOverflowed()) {
                raiseMemoryError(m_globalObject, scope);
                return;
            }
            data = encoded.span();
        }
        std::array<uint8_t, 9> header;
        size_t headerSize;
        uint64_t size = data.size();
        auto fill = [&] (Opcode opcode, unsigned count) {
            header[0] = static_cast<uint8_t>(opcode);
            for (unsigned i = 0; i < count; ++i)
                header[1 + i] = static_cast<uint8_t>(size >> (8 * i));
            headerSize = 1 + count;
        };
        if (size <= 0xff && m_self.protocol >= 4)
            fill(Opcode::ShortBinUnicode, 1);
        else if (size <= 0xffffffff)
            fill(Opcode::BinUnicode, 4);
        else if (m_self.protocol >= 4)
            fill(Opcode::BinUnicode8, 8);
        else {
            raise(m_globalObject, scope, BuiltinType::OverflowError, "serializing a string larger than 4 GiB requires pickle protocol 4 or higher"_s);
            return;
        }
        RELEASE_AND_RETURN(scope, writeBytes(std::span<const uint8_t>(header).first(headerSize), data, JSValue()));
    }

    // raw_unicode_escape(): PyUnicode_AsRawUnicodeEscapeString(), but that a backslash and the end of a line are escaped too.
    void writeRawUnicodeEscape(StringView text)
    {
        constexpr char hexDigits[] = "0123456789abcdef";
        for (char32_t c : text.codePoints()) {
            if (c >= 0x10000) {
                write("\\U"_s);
                for (int shift = 28; shift >= 0; shift -= 4)
                    write(static_cast<uint8_t>(hexDigits[c >> shift & 0xf]));
            } else if (c >= 256 || c == '\\' || !c || c == '\n' || c == '\r' || c == 0x1a) {
                write("\\u"_s);
                for (int shift = 12; shift >= 0; shift -= 4)
                    write(static_cast<uint8_t>(hexDigits[c >> shift & 0xf]));
            } else
                write(static_cast<uint8_t>(c));
        }
    }

    // save_unicode()
    void saveUnicode(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        String text = asString(object)->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, void());
        if (m_self.isBinary) {
            writeUnicodeBinary(text);
            RETURN_IF_EXCEPTION(scope, void());
        } else {
            write(Opcode::Unicode);
            writeRawUnicodeEscape(text);
            write('\n');
        }
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // ---- tuple

    // store_tuple_elements()
    void storeTupleElements(PyTuple* tuple)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        for (unsigned i = 0; i < tuple->length(); ++i) {
            save(tuple->at(i), [&] { return concatenate("when serializing "_s, typeNameOf(tuple), " item "_s, i); });
            RETURN_IF_EXCEPTION(scope, void());
        }
    }

    // save_tuple(). It is the one thing that cannot be changed and can have itself in it all the same, which is found out when what is in it has been written: it is then in the memo. All that was written for it is
    // thrown away then, and it is got from there.
    void saveTuple(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        PyTuple* tuple = asTuple(object);
        unsigned length = tuple->length();
        if (!length) {
            if (m_self.protocol)
                write(Opcode::EmptyTuple);
            else {
                write(Opcode::Mark);
                write(Opcode::Tuple);
            }
            return;
        }

        if (length <= 3 && m_self.protocol >= 2) {
            storeTupleElements(tuple);
            RETURN_IF_EXCEPTION(scope, void());
            if (isInMemo(object)) {
                for (unsigned i = 0; i < length; ++i)
                    write(Opcode::Pop);
                RELEASE_AND_RETURN(scope, memoGet(object));
            }
            constexpr Opcode opcodes[] = { Opcode::EmptyTuple, Opcode::Tuple1, Opcode::Tuple2, Opcode::Tuple3 };
            write(opcodes[length]);
            RELEASE_AND_RETURN(scope, memoPut(object));
        }

        write(Opcode::Mark);
        storeTupleElements(tuple);
        RETURN_IF_EXCEPTION(scope, void());
        if (isInMemo(object)) {
            if (m_self.isBinary)
                write(Opcode::PopMark);
            else {
                // One more than there are, for the mark.
                for (unsigned i = 0; i <= length; ++i)
                    write(Opcode::Pop);
            }
            RELEASE_AND_RETURN(scope, memoGet(object));
        }
        write(Opcode::Tuple);
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // ---- list

    // batch_list(): MARK item item ... item APPENDS, so many at a time, of what an iterator gives. What they are to be appended to has been written.
    void batchList(JSValue iterator, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        int64_t total = 0;
        auto note = [&] { return concatenate("when serializing "_s, typeNameOf(original), " item "_s, total); };

        if (!m_self.protocol) {
            // One at a time
            for (;; ++total) {
                JSValue item = iteratorNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, void());
                if (!item)
                    return;
                save(item, note);
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::Append);
            }
        }

        unsigned count;
        do {
            JSValue first = iteratorNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, void());
            if (!first)
                return;
            JSValue item = iteratorNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, void());
            if (!item) {
                // There is only the one.
                save(first, note);
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::Append);
                return;
            }

            write(Opcode::Mark);
            save(first, note);
            RETURN_IF_EXCEPTION(scope, void());
            ++total;
            count = 1;
            while (item) {
                save(item, note);
                RETURN_IF_EXCEPTION(scope, void());
                ++total;
                if (++count == pickleBatchSize)
                    break;
                item = iteratorNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, void());
            }
            write(Opcode::Appends);
        } while (count == pickleBatchSize);
    }

    // batch_list_exact(): the same, of a list that is of no class derived from list, and not for protocol 0.
    void batchListExact(JSArray* list)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned size = list->length();
        unsigned total = 0;
        auto note = [&] { return concatenate("when serializing "_s, typeNameOf(list), " item "_s, total); };
        if (size == 1) {
            JSValue item = listGet(m_globalObject, list, 0);
            RETURN_IF_EXCEPTION(scope, void());
            save(item, note);
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::Append);
            return;
        }
        do {
            unsigned inThisBatch = 0;
            write(Opcode::Mark);
            while (total < list->length()) {
                JSValue item = listGet(m_globalObject, list, total);
                RETURN_IF_EXCEPTION(scope, void());
                save(item, note);
                RETURN_IF_EXCEPTION(scope, void());
                ++total;
                if (++inThisBatch == pickleBatchSize)
                    break;
            }
            write(Opcode::Appends);
            if (list->length() != size) {
                raise(m_globalObject, scope, BuiltinType::RuntimeError, "list changed size during iteration"_s);
                return;
            }
        } while (total < size);
    }

    // save_list()
    void saveList(JSValue object)
    {
        guardingAgainstCycles(object, [&] {
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            if (m_self.isBinary)
                write(Opcode::EmptyList);
            else {
                write(Opcode::Mark);
                write(Opcode::List);
            }
            memoPut(object);
            RETURN_IF_EXCEPTION(scope, void());
            if (!asList(object)->length())
                return;
            if (m_self.protocol > 0) {
                checkDepth();
                RETURN_IF_EXCEPTION(scope, void());
                RELEASE_AND_RETURN(scope, batchListExact(asList(object)));
            }
            JSValue iterator = getIterator(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, void());
            checkDepth();
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_AND_RETURN(scope, batchList(iterator, object));
        });
    }

    // ---- dict

    void saveItem(JSValue key, JSValue value, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        save(key);
        RETURN_IF_EXCEPTION(scope, void());
        scope.release();
        save(value, [&] { return concatenate("when serializing "_s, typeNameOf(original), " item "_s, repr(m_globalObject, key)); });
    }

    // What an iterator gave, which is to be a key and a value.
    void savePair(JSValue pair, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!isTuple(pair) || asTuple(pair)->length() != 2) {
            raiseTypeError(m_globalObject, scope, "dict items iterator must return 2-tuples"_s);
            return;
        }
        RELEASE_AND_RETURN(scope, saveItem(asTuple(pair)->at(0), asTuple(pair)->at(1), original));
    }

    // batch_dict(): MARK key value ... key value SETITEMS, so many at a time, of what an iterator gives.
    void batchDict(JSValue iterator, JSValue original)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (!m_self.protocol) {
            while (true) {
                JSValue pair = iteratorNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, void());
                if (!pair)
                    return;
                savePair(pair, original);
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::SetItem);
            }
        }

        auto checkPair = [&] (JSValue pair) {
            if (!isTuple(pair) || asTuple(pair)->length() != 2)
                raiseTypeError(m_globalObject, scope, "dict items iterator must return 2-tuples"_s);
        };
        unsigned count;
        do {
            JSValue first = iteratorNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, void());
            if (!first)
                return;
            checkPair(first);
            RETURN_IF_EXCEPTION(scope, void());
            JSValue pair = iteratorNext(m_globalObject, iterator);
            RETURN_IF_EXCEPTION(scope, void());
            if (!pair) {
                savePair(first, original);
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::SetItem);
                return;
            }

            write(Opcode::Mark);
            savePair(first, original);
            RETURN_IF_EXCEPTION(scope, void());
            count = 1;
            while (pair) {
                savePair(pair, original);
                RETURN_IF_EXCEPTION(scope, void());
                if (++count == pickleBatchSize)
                    break;
                pair = iteratorNext(m_globalObject, iterator);
                RETURN_IF_EXCEPTION(scope, void());
            }
            write(Opcode::SetItems);
        } while (count == pickleBatchSize);
    }

    // batch_dict_exact(): the same, of a dict that is of no class derived from dict, and not for protocol 0.
    void batchDictExact(PyDict* dict)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        unsigned size = dict->size();
        auto checkSize = [&] {
            if (dict->size() != size)
                raise(m_globalObject, scope, BuiltinType::RuntimeError, "dictionary changed size during iteration"_s);
        };
        if (size == 1) {
            dict->forEach(m_globalObject, [&] (JSValue key, JSValue value) {
                saveItem(key, value, dict);
                return false;
            });
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::SetItem);
            return;
        }
        // A batch that is full is followed by another, though there may be nothing to go in it.
        unsigned inThisBatch = 0;
        write(Opcode::Mark);
        dict->forEach(m_globalObject, [&] (JSValue key, JSValue value) {
            saveItem(key, value, dict);
            if (scope.exception()) [[unlikely]]
                return false;
            if (++inThisBatch < pickleBatchSize)
                return true;
            write(Opcode::SetItems);
            checkSize();
            if (scope.exception()) [[unlikely]]
                return false;
            write(Opcode::Mark);
            inThisBatch = 0;
            return true;
        });
        RETURN_IF_EXCEPTION(scope, void());
        write(Opcode::SetItems);
        checkSize();
    }

    // save_dict()
    void saveDict(JSValue object)
    {
        guardingAgainstCycles(object, [&] {
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            if (m_self.isBinary)
                write(Opcode::EmptyDict);
            else {
                write(Opcode::Mark);
                write(Opcode::Dict);
            }
            memoPut(object);
            RETURN_IF_EXCEPTION(scope, void());
            if (!asDict(object)->size())
                return;
            if (m_self.protocol > 0) {
                checkDepth();
                RETURN_IF_EXCEPTION(scope, void());
                RELEASE_AND_RETURN(scope, batchDictExact(asDict(object)));
            }
            JSValue items = callMethodNamed(m_globalObject, object, Identifier::fromString(m_vm, "items"_s));
            RETURN_IF_EXCEPTION(scope, void());
            JSValue iterator = getIterator(m_globalObject, items);
            RETURN_IF_EXCEPTION(scope, void());
            checkDepth();
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_AND_RETURN(scope, batchDict(iterator, object));
        });
    }

    // ---- set and frozenset

    // Before there was a way to write one: set(list) or frozenset(list).
    void saveSetByReduce(JSValue object, PyType* type)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue items = listFromIterable(m_globalObject, object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, saveReduce(PyTuple::create(m_globalObject, { type->object(), PyTuple::create(m_globalObject, { items }) }), object));
    }

    // save_set()
    void saveSet(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (m_self.protocol < 4)
            RELEASE_AND_RETURN(scope, saveSetByReduce(object, m_realm->typeSet()));
        write(Opcode::EmptySet);
        memoPut(object);
        RETURN_IF_EXCEPTION(scope, void());
        auto* set = uncheckedDowncast<PySet>(object.asCell());
        unsigned size = set->size();
        if (!size)
            return;
        unsigned entry = set->firstEntry();
        unsigned inThisBatch;
        do {
            inThisBatch = 0;
            write(Opcode::Mark);
            for (; entry < set->entryCount(); ++entry) {
                JSValue item = set->keyAt(entry);
                if (!item)
                    continue;
                save(item, [&] { return concatenate("when serializing "_s, typeNameOf(object), " element"_s); });
                RETURN_IF_EXCEPTION(scope, void());
                if (++inThisBatch == pickleBatchSize) {
                    ++entry;
                    break;
                }
            }
            write(Opcode::AddItems);
            if (set->size() != size) {
                raise(m_globalObject, scope, BuiltinType::RuntimeError, "set changed size during iteration"_s);
                return;
            }
        } while (inThisBatch == pickleBatchSize);
    }

    // save_frozenset()
    void saveFrozenSet(JSValue object)
    {
        guardingAgainstCycles(object, [&] {
            auto scope = DECLARE_THROW_SCOPE(m_vm);
            if (m_self.protocol < 4)
                RELEASE_AND_RETURN(scope, saveSetByReduce(object, m_realm->typeFrozenSet()));
            write(Opcode::Mark);
            auto* set = uncheckedDowncast<PySet>(object.asCell());
            for (unsigned entry = set->firstEntry(); entry < set->entryCount(); ++entry) {
                JSValue item = set->keyAt(entry);
                if (!item)
                    continue;
                save(item, [&] { return concatenate("when serializing "_s, typeNameOf(object), " element"_s); });
                RETURN_IF_EXCEPTION(scope, void());
            }
            // As for a tuple
            if (isInMemo(object)) {
                write(Opcode::PopMark);
                RELEASE_AND_RETURN(scope, memoGet(object));
            }
            write(Opcode::FrozenSet);
            RELEASE_AND_RETURN(scope, memoPut(object));
        });
    }

    // ---- What is written as where it is to be found

    // check_dotted_path()
    void checkDottedPath(JSValue object, const MarkedArgumentBuffer& path)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        for (size_t i = 0; i < path.size(); ++i) {
            if (asString(path.at(i))->tryGetValue().data != "<locals>"_s)
                continue;
            String shown = repr(m_globalObject, object);
            RETURN_IF_EXCEPTION(scope, void());
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("Can't pickle local object "_s, shown)));
        }
    }

    // _checkmodule(): whether it is in the module that it is to be found. It may raise, and is then false.
    bool isFoundIn(JSValue moduleName, JSValue module, JSValue global, const MarkedArgumentBuffer& path)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        if (isNone(module))
            return false;
        if (JSString* name = stringIn(moduleName)) {
            String text = name->value(m_globalObject);
            RETURN_IF_EXCEPTION(scope, false);
            if (text == "__main__"_s)
                return false;
        }
        JSValue candidate = getAttributeByPath(m_globalObject, module, path, false);
        RETURN_IF_EXCEPTION(scope, false);
        return candidate && candidate == global;
    }

    // whichmodule(): the name of the module that something is in, having made sure that it is to be found there by its name. Empty if it raised.
    JSValue whichModule(JSValue global, JSValue globalName, const MarkedArgumentBuffer& path)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        checkDottedPath(global, path);
        RETURN_IF_EXCEPTION(scope, { });
        JSValue moduleName = getAttributeIfPresent(m_globalObject, global, names.dunder_module);
        RETURN_IF_EXCEPTION(scope, { });
        if (!moduleName || isNone(moduleName)) {
            // It does not say, as a method of something built in may not. So every module is looked in.
            moduleName = JSValue();
            JSValue modules = sysAttribute(m_globalObject, "modules"_s);
            if (!modules)
                return raise(m_globalObject, scope, BuiltinType::RuntimeError, "lost sys.modules"_s);
            if (isExactly(m_globalObject, modules, m_realm->typeDict())) {
                asDict(modules)->forEach(m_globalObject, [&] (JSValue name, JSValue module) {
                    bool isHere = isFoundIn(name, module, global, path);
                    if (scope.exception()) [[unlikely]]
                        return false;
                    if (isHere)
                        moduleName = name;
                    return !isHere;
                });
                RETURN_IF_EXCEPTION(scope, { });
            } else {
                JSValue iterator = getIterator(m_globalObject, modules);
                RETURN_IF_EXCEPTION(scope, { });
                while (true) {
                    JSValue name = iteratorNext(m_globalObject, iterator);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (!name)
                        break;
                    JSValue module = getItem(m_globalObject, modules, name);
                    RETURN_IF_EXCEPTION(scope, { });
                    bool isHere = isFoundIn(name, module, global, path);
                    RETURN_IF_EXCEPTION(scope, { });
                    if (isHere) {
                        moduleName = name;
                        break;
                    }
                }
            }
            if (moduleName)
                return moduleName;
            moduleName = internedString(m_vm, Identifier::fromString(m_vm, "__main__"_s));
        }

        JSValue module = importModuleNamed(moduleName);
        if (scope.exception()) [[unlikely]] {
            if (isRaised(BuiltinType::ImportError) || isRaised(BuiltinType::ValueError)) {
                Exception* raised = takeRaisedException(m_vm);
                raiseWithShown("Can't pickle "_s, global, ": "_s, str(m_globalObject, raised->value()));
                chainRaisedExceptions(m_globalObject, raised);
            }
            return { };
        }
        JSValue actual = getAttributeByPath(m_globalObject, module, path, true);
        if (scope.exception()) [[unlikely]] {
            if (isRaised(BuiltinType::AttributeError)) {
                Exception* raised = takeRaisedException(m_vm);
                raiseWithShown("Can't pickle "_s, global, ": it's not found as "_s, concatenate(str(m_globalObject, moduleName), '.', str(m_globalObject, globalName)));
                chainRaisedExceptions(m_globalObject, raised);
            }
            return { };
        }
        if (actual != global) {
            raiseWithShown("Can't pickle "_s, global, ": it's not the same object as "_s, concatenate(str(m_globalObject, moduleName), '.', str(m_globalObject, globalName)));
            return { };
        }
        return moduleName;
    }

    // PyErr_ExceptionMatches()
    bool isRaised(BuiltinType type)
    {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(m_vm);
        Exception* raised = scope.exception();
        return raised && !m_vm.isTerminationException(raised) && isInstance(m_globalObject, raised->value(), m_realm->type(type));
    }

    // PicklingError, of what is said before the repr() of something and what is said after. If working either out raised, that is what is raised.
    void raiseWithShown(ASCIILiteral before, JSValue object, ASCIILiteral between, const String& after)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        RETURN_IF_EXCEPTION(scope, void());
        String shown = repr(m_globalObject, object);
        RETURN_IF_EXCEPTION(scope, void());
        RELEASE_AND_RETURN(scope, raisePicklingError(concatenate(before, shown, between, after)));
    }

    // PyImport_Import()
    JSValue importModuleNamed(JSValue name)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSString* string = stringIn(name);
        if (!string)
            return raiseTypeError(m_globalObject, scope, "module name must be a string"_s);
        String text = string->value(m_globalObject);
        RETURN_IF_EXCEPTION(scope, { });
        RELEASE_AND_RETURN(scope, importModule(m_globalObject, text));
    }

    // fix_imports(): what Python 2 calls it.
    void fixImports(JSValue& moduleName, JSValue& globalName)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue item = m_state.nameMapping3To2->get(m_globalObject, PyTuple::create(m_globalObject, { moduleName, globalName }));
        RETURN_IF_EXCEPTION(scope, void());
        if (item) {
            if (!isTuple(item) || asTuple(item)->length() != 2) {
                raise(m_globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.REVERSE_NAME_MAPPING values should be 2-tuples, not "_s, typeName(m_globalObject, item)));
                return;
            }
            JSValue fixedModuleName = asTuple(item)->at(0);
            JSValue fixedGlobalName = asTuple(item)->at(1);
            if (!stringIn(fixedModuleName) || !stringIn(fixedGlobalName)) {
                raise(m_globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.REVERSE_NAME_MAPPING values should be pairs of str, not ("_s, typeName(m_globalObject, fixedModuleName), ", "_s, typeName(m_globalObject, fixedGlobalName), ')'));
                return;
            }
            moduleName = fixedModuleName;
            globalName = fixedGlobalName;
            return;
        }
        item = m_state.importMapping3To2->get(m_globalObject, moduleName);
        RETURN_IF_EXCEPTION(scope, void());
        if (!item)
            return;
        if (!stringIn(item)) {
            raise(m_globalObject, scope, BuiltinType::RuntimeError, concatenate("_compat_pickle.REVERSE_IMPORT_MAPPING values should be strings, not "_s, typeName(m_globalObject, item)));
            return;
        }
        moduleName = item;
    }

    // The name of a module or of what is in one, and the end of the line. There are names now that are not ASCII, and nothing older than protocol 3 is to be given one.
    void writeIdentifier(JSValue name, ASCIILiteral kind)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto encoded = encodeString(m_globalObject, name, m_self.protocol == 3 ? "utf-8"_s : "ascii"_s, "strict"_s);
        if (scope.exception()) [[unlikely]] {
            if (isRaised(BuiltinType::UnicodeEncodeError)) {
                Exception* raised = takeRaisedException(m_vm);
                raiseWithShown(kind, name, " using pickle protocol "_s, String::number(m_self.protocol));
                chainRaisedExceptions(m_globalObject, raised);
            }
            return;
        }
        write(encoded->span());
        write('\n');
    }

    // save_global(). `name` is what it is to be found by, if that has been said.
    void saveGlobal(JSValue object, JSValue name)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        JSValue globalName = name;
        if (!globalName) {
            globalName = getAttributeIfPresent(m_globalObject, object, names.dunder_qualname);
            RETURN_IF_EXCEPTION(scope, void());
            if (!globalName) {
                globalName = getAttribute(m_globalObject, object, names.dunder_name);
                RETURN_IF_EXCEPTION(scope, void());
            }
        }
        MarkedArgumentBuffer path;
        appendDottedPath(m_globalObject, globalName, path);
        RETURN_IF_EXCEPTION(scope, void());
        JSValue moduleName = whichModule(object, globalName, path);
        RETURN_IF_EXCEPTION(scope, void());

        if (m_self.protocol >= 2) {
            // It may have been given a number to go by.
            JSValue codeObject = m_state.extensionRegistry->get(m_globalObject, PyTuple::create(m_globalObject, { moduleName, globalName }));
            RETURN_IF_EXCEPTION(scope, void());
            if (codeObject) {
                auto code = toCLong(m_globalObject, codeObject);
                RETURN_IF_EXCEPTION(scope, void());
                if (*code <= 0 || *code > 0x7fffffff) {
                    // copyreg.add_extension() sees that it is not.
                    raise(m_globalObject, scope, BuiltinType::RuntimeError, concatenate("extension code "_s, *code, " is out of range"_s));
                    return;
                }
                if (*code <= 0xff) {
                    write(Opcode::Ext1);
                    writeLittleEndian(*code, 1);
                } else if (*code <= 0xffff) {
                    write(Opcode::Ext2);
                    writeLittleEndian(*code, 2);
                } else {
                    write(Opcode::Ext4);
                    writeLittleEndian(*code, 4);
                }
                return;
            }
        }

        if (m_self.protocol >= 4) {
            save(moduleName);
            RETURN_IF_EXCEPTION(scope, void());
            save(globalName);
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::StackGlobal);
        } else {
            // What is inside something else is getattr(getattr(..., name1), name2).
            if (path.size() > 1)
                globalName = path.at(0);
            for (size_t i = 1; i < path.size(); ++i) {
                save(m_state.getattr.get());
                RETURN_IF_EXCEPTION(scope, void());
                if (m_self.protocol < 2)
                    write(Opcode::Mark);
            }
            write(Opcode::Global);
            if (m_self.protocol < 3 && m_self.fixesImports) {
                fixImports(moduleName, globalName);
                RETURN_IF_EXCEPTION(scope, void());
            }
            writeIdentifier(moduleName, "can't pickle module identifier "_s);
            RETURN_IF_EXCEPTION(scope, void());
            writeIdentifier(globalName, "can't pickle global identifier "_s);
            RETURN_IF_EXCEPTION(scope, void());
            for (size_t i = 1; i < path.size(); ++i) {
                save(path.at(i));
                RETURN_IF_EXCEPTION(scope, void());
                write(m_self.protocol < 2 ? Opcode::Tuple : Opcode::Tuple2);
                write(Opcode::Reduce);
            }
        }
        RELEASE_AND_RETURN(scope, memoPut(object));
    }

    // save_type()
    void saveType(JSValue object)
    {
        // save_singleton_type(): the class of what there is one of has no name to be found by. It is type(None).
        auto singleton = [&] () -> JSValue {
            if (object == JSValue(m_realm->type(BuiltinType::NoneType)->object()))
                return jsUndefined();
            if (object == JSValue(m_realm->type(BuiltinType::Ellipsis)->object()))
                return m_realm->ellipsis();
            if (object == JSValue(m_realm->type(BuiltinType::NotImplementedType)->object()))
                return m_realm->notImplemented();
            return { };
        }();
        if (singleton)
            return saveReduce(PyTuple::create(m_globalObject, { m_realm->typeType()->object(), PyTuple::create(m_globalObject, { singleton }) }), object);
        saveGlobal(object, JSValue());
    }

    // save_pers(): whether it was written as what persistent_id() says that it is known by elsewhere.
    bool savePersistent(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue identifier = call(m_globalObject, m_self.persistentID.get(), object);
        RETURN_IF_EXCEPTION(scope, false);
        if (isNone(identifier))
            return false;
        if (m_self.isBinary) {
            save(identifier, IsPersistentID::Yes);
            RETURN_IF_EXCEPTION(scope, false);
            write(Opcode::BinPersistentID);
            return true;
        }
        String text = str(m_globalObject, identifier);
        RETURN_IF_EXCEPTION(scope, false);
        if (!text.containsOnlyASCII()) {
            raisePicklingError("persistent IDs in protocol 0 must be ASCII strings"_s);
            return false;
        }
        write(Opcode::PersistentID);
        write(text);
        write('\n');
        return true;
    }

    // ---- What is written as how to make it

    // get_class()
    JSValue classOf(JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        JSValue result = getAttributeIfPresent(m_globalObject, object, m_vm.pythonNames().dunder_class);
        RETURN_IF_EXCEPTION(scope, { });
        return result ? result : JSValue(Python::typeOf(m_globalObject, object)->object());
    }

    // PyIter_Check()
    bool isIterator(JSValue value) { return !!Python::typeOf(m_globalObject, value)->lookup(m_vm, m_vm.pythonNames().dunder_next); }

    // save_reduce(): `arguments` is what __reduce__() gave for `object`, from two to six things.
    void saveReduce(JSValue reduceValue, JSValue object)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        PyTuple* given = asTuple(reduceValue);
        unsigned size = given->length();
        if (size < 2 || size > 6)
            RELEASE_AND_RETURN(scope, raisePicklingError("tuple returned by __reduce__ must contain 2 through 6 elements"_s));
        auto optional = [&] (unsigned index) -> JSValue { return index < size && !isNone(given->at(index)) ? given->at(index) : JSValue(); };
        JSValue callable = given->at(0);
        JSValue argumentTuple = given->at(1);
        JSValue state = optional(2);
        JSValue listItems = optional(3);
        JSValue dictItems = optional(4);
        JSValue stateSetter = optional(5);

        if (!isCallable(m_globalObject, callable))
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("first item of the tuple returned by __reduce__ must be callable, not "_s, typeNameOf(callable))));
        if (!isTuple(argumentTuple))
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("second item of the tuple returned by __reduce__ must be a tuple, not "_s, typeNameOf(argumentTuple))));
        if (listItems && !isIterator(listItems))
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("fourth item of the tuple returned by __reduce__ must be an iterator, not "_s, typeNameOf(listItems))));
        if (dictItems && !isIterator(dictItems))
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("fifth item of the tuple returned by __reduce__ must be an iterator, not "_s, typeNameOf(dictItems))));
        if (stateSetter && !isCallable(m_globalObject, stateSetter))
            RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("sixth item of the tuple returned by __reduce__ must be callable, not "_s, typeNameOf(stateSetter))));
        PyTuple* arguments = asTuple(argumentTuple);

        bool usesNewObj = false;
        bool usesNewObjEx = false;
        if (m_self.protocol >= 2) {
            JSValue name = getAttributeIfPresent(m_globalObject, callable, names.dunder_name);
            RETURN_IF_EXCEPTION(scope, void());
            if (JSString* string = name ? stringIn(name) : nullptr) {
                String text = string->value(m_globalObject);
                RETURN_IF_EXCEPTION(scope, void());
                usesNewObjEx = text == "__newobj_ex__"_s;
                usesNewObj = text == "__newobj__"_s;
            }
        }
        auto noteOf = [&] (ASCIILiteral what) { return [this, object, what] { return concatenate("when serializing "_s, typeNameOf(object), ' ', what); }; };

        if (usesNewObjEx) {
            if (arguments->length() != 3)
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("__newobj_ex__ expected 3 arguments, got "_s, arguments->length())));
            JSValue cls = arguments->at(0);
            if (!isClass(cls))
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("first argument to __newobj_ex__() must be a class, not "_s, typeNameOf(cls))));
            JSValue positional = arguments->at(1);
            if (!isTuple(positional))
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("second argument to __newobj_ex__() must be a tuple, not "_s, typeNameOf(positional))));
            JSValue keywords = arguments->at(2);
            if (!isDict(keywords))
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("third argument to __newobj_ex__() must be a dict, not "_s, typeNameOf(keywords))));

            if (m_self.protocol >= 4) {
                save(cls, noteOf("class"_s));
                RETURN_IF_EXCEPTION(scope, void());
                save(positional, noteOf("__new__ arguments"_s));
                RETURN_IF_EXCEPTION(scope, void());
                save(keywords, noteOf("__new__ arguments"_s));
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::NewObjEx);
            } else {
                // functools.partial(cls.__new__, cls, *args, **kwargs)()
                JSValue newOfClass = getAttribute(m_globalObject, cls, names.dunder_new);
                RETURN_IF_EXCEPTION(scope, void());
                MarkedArgumentBuffer newArguments;
                newArguments.append(newOfClass);
                newArguments.append(cls);
                for (unsigned i = 0; i < asTuple(positional)->length(); ++i)
                    newArguments.append(asTuple(positional)->at(i));
                JSValue partial = callWithKeywordDict(m_globalObject, m_state.partial.get(), newArguments, asDict(keywords));
                RETURN_IF_EXCEPTION(scope, void());
                save(partial, noteOf("reconstructor"_s));
                RETURN_IF_EXCEPTION(scope, void());
                save(PyTuple::create(m_globalObject, 0), noteOf("reconstructor"_s));
                RETURN_IF_EXCEPTION(scope, void());
                write(Opcode::Reduce);
            }
        } else if (usesNewObj) {
            if (arguments->length() < 1)
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("__newobj__ expected at least 1 argument, got "_s, arguments->length())));
            JSValue cls = arguments->at(0);
            if (!isClass(cls))
                RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("first argument to __newobj__() must be a class, not "_s, typeNameOf(cls))));
            if (object) {
                JSValue classOfObject = classOf(object);
                RETURN_IF_EXCEPTION(scope, void());
                if (classOfObject != cls) {
                    String expected = repr(m_globalObject, classOfObject);
                    RETURN_IF_EXCEPTION(scope, void());
                    String shown = repr(m_globalObject, cls);
                    RETURN_IF_EXCEPTION(scope, void());
                    RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("first argument to __newobj__() must be "_s, expected, ", not "_s, shown)));
                }
            }
            save(cls, noteOf("class"_s));
            RETURN_IF_EXCEPTION(scope, void());
            PyTuple* rest = PyTuple::create(m_globalObject, arguments->length() - 1);
            for (unsigned i = 1; i < arguments->length(); ++i)
                rest->initializeAt(m_vm, i - 1, arguments->at(i));
            save(rest, noteOf("__new__ arguments"_s));
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::NewObj);
        } else {
            save(callable, noteOf("reconstructor"_s));
            RETURN_IF_EXCEPTION(scope, void());
            save(argumentTuple, noteOf("reconstructor arguments"_s));
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::Reduce);
        }

        if (object) {
            // If it is in the memo by now it has itself in it. What was written is thrown away, and it is got from there.
            if (isInMemo(object)) {
                write(Opcode::Pop);
                RELEASE_AND_RETURN(scope, memoGet(object));
            }
            memoPut(object);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (listItems) {
            batchList(listItems, object);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (dictItems) {
            batchDict(dictItems, object);
            RETURN_IF_EXCEPTION(scope, void());
        }
        if (!state)
            return;
        if (!stateSetter) {
            save(state, noteOf("state"_s));
            RETURN_IF_EXCEPTION(scope, void());
            write(Opcode::Build);
            return;
        }
        // state_setter(obj, state), and what comes of that is thrown away, so that the stack is as it was.
        save(stateSetter, noteOf("state setter"_s));
        RETURN_IF_EXCEPTION(scope, void());
        save(object);
        RETURN_IF_EXCEPTION(scope, void());
        save(state, noteOf("state"_s));
        RETURN_IF_EXCEPTION(scope, void());
        write(Opcode::Tuple2);
        write(Opcode::Reduce);
        write(Opcode::Pop);
    }

    // ---- Anything

    enum class IsPersistentID : bool { No, Yes };

    // save(). What persistent_id() gave is not itself asked about.
    void save(JSValue object, IsPersistentID isPersistentID = IsPersistentID::No)
    {
        auto scope = DECLARE_THROW_SCOPE(m_vm);
        auto& names = m_vm.pythonNames();
        opcodeBoundary();
        RETURN_IF_EXCEPTION(scope, void());
        if (isPersistentID == IsPersistentID::No && m_self.persistentID) {
            bool wasSaved = savePersistent(object);
            RETURN_IF_EXCEPTION(scope, void());
            if (wasSaved)
                return;
        }

        // What is not kept in the memo
        if (isNone(object))
            return write(Opcode::None);
        if (object.isBoolean())
            return saveBool(object.asBoolean());
        if (object.isNumber() || object.isHeapBigInt()) {
            // Which of the two a number is does not go by how it is encoded: see TaggedArithmetic.h.
            if (Number number = classify(object); number.kind == Number::Kind::Float)
                return saveFloat(number.real);
            RELEASE_AND_RETURN(scope, saveLong(object));
        }

        if (isInMemo(object))
            RELEASE_AND_RETURN(scope, memoGet(object));
        if (object.isString())
            RELEASE_AND_RETURN(scope, saveUnicode(object));
        PyType* type = Python::typeOf(m_globalObject, object);
        if (type == m_realm->typeBytes())
            RELEASE_AND_RETURN(scope, saveBytes(object));

        checkDepth();
        RETURN_IF_EXCEPTION(scope, void());
        if (type == m_realm->typeDict())
            RELEASE_AND_RETURN(scope, saveDict(object));
        if (type == m_realm->typeSet())
            RELEASE_AND_RETURN(scope, saveSet(object));
        if (type == m_realm->typeFrozenSet())
            RELEASE_AND_RETURN(scope, saveFrozenSet(object));
        if (type == m_realm->typeList())
            RELEASE_AND_RETURN(scope, saveList(object));
        if (type == m_realm->typeTuple())
            RELEASE_AND_RETURN(scope, saveTuple(object));
        if (type == m_realm->typeByteArray())
            RELEASE_AND_RETURN(scope, saveByteArray(object));
        if (type == m_state.pickleBufferType.get())
            RELEASE_AND_RETURN(scope, savePickleBuffer(object));

        JSValue reduceValue;
        if (m_self.reducerOverride) {
            reduceValue = call(m_globalObject, m_self.reducerOverride.get(), object);
            RETURN_IF_EXCEPTION(scope, void());
            if (reduceValue == JSValue(m_realm->notImplemented()))
                reduceValue = JSValue();
        }
        if (!reduceValue) {
            if (type == m_realm->typeType())
                RELEASE_AND_RETURN(scope, saveType(object));
            if (type == m_realm->type(BuiltinType::Function))
                RELEASE_AND_RETURN(scope, saveGlobal(object, JSValue()));

            // What says how to make it again: from the dispatch_table of the Pickler or that of copyreg, or its __reduce_ex__(), or its __reduce__().
            JSValue reduceFunction;
            if (!m_self.dispatchTable)
                reduceFunction = m_state.dispatchTable->get(m_globalObject, type->object());
            else {
                // PyMapping_GetOptionalItem()
                reduceFunction = getItem(m_globalObject, m_self.dispatchTable.get(), type->object());
                if (scope.exception() && catchException(m_globalObject, BuiltinType::KeyError))
                    reduceFunction = JSValue();
            }
            RETURN_IF_EXCEPTION(scope, void());
            if (reduceFunction)
                reduceValue = call(m_globalObject, reduceFunction, object);
            else if (type->isSubtypeOf(m_realm->typeType()))
                RELEASE_AND_RETURN(scope, saveGlobal(object, JSValue()));
            else {
                reduceFunction = getAttributeIfPresent(m_globalObject, object, names.dunder_reduce_ex);
                RETURN_IF_EXCEPTION(scope, void());
                if (reduceFunction)
                    reduceValue = call(m_globalObject, reduceFunction, jsNumber(m_self.protocol));
                else {
                    reduceFunction = getAttributeIfPresent(m_globalObject, object, names.dunder_reduce);
                    RETURN_IF_EXCEPTION(scope, void());
                    if (!reduceFunction)
                        RELEASE_AND_RETURN(scope, raisePicklingError(concatenate("Can't pickle "_s, typeNameOf(object), " object"_s)));
                    reduceValue = call(m_globalObject, reduceFunction);
                }
            }
            RETURN_IF_EXCEPTION(scope, void());
        }

        if (stringIn(reduceValue))
            RELEASE_AND_RETURN(scope, saveGlobal(object, reduceValue));
        if (!isTuple(reduceValue))
            raisePicklingError(concatenate("__reduce__ must return a string or tuple, not "_s, typeNameOf(reduceValue)));
        else
            saveReduce(reduceValue, object);
        if (scope.exception()) [[unlikely]]
            addNoteToRaised(m_globalObject, concatenate("when serializing "_s, typeNameOf(object), " object"_s));
    }

    JSGlobalObject* m_globalObject;
    VM& m_vm;
    PyRealm* m_realm;
    PickleModuleState& m_state;
    PyStateObject* m_owner;
    Pickler& m_self;
};

// ---- Setting one up

// _Pickler_New()
PyStateObject* newPickler(JSGlobalObject* globalObject)
{
    auto* object = PyStateObject::create(globalObject->vm(), pickleModuleState(globalObject).picklerType->instanceStructure(), makeUnique<Pickler>());
    clearMemo(globalObject, object, object->state<Pickler>());
    return object;
}

// _Pickler_SetProtocol()
void setProtocol(JSGlobalObject* globalObject, Pickler& self, JSValue given, bool fixesImports)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    int64_t protocol = defaultPickleProtocol;
    if (!isNone(given)) {
        auto converted = toCLong(globalObject, given);
        RETURN_IF_EXCEPTION(scope, void());
        protocol = *converted;
        if (protocol < 0)
            protocol = highestPickleProtocol;
        else if (protocol > highestPickleProtocol) {
            raiseValueError(globalObject, scope, concatenate("pickle protocol must be <= "_s, highestPickleProtocol));
            return;
        }
    }
    self.protocol = static_cast<int>(protocol);
    self.isBinary = protocol > 0;
    self.fixesImports = fixesImports && protocol < 3;
}

// _Pickler_SetOutputStream()
void setOutputStream(JSGlobalObject* globalObject, PyStateObject* owner, Pickler& self, JSValue file)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    JSValue write = getAttributeIfPresent(globalObject, file, vm.pythonNames().attribute_write);
    RETURN_IF_EXCEPTION(scope, void());
    if (!write) {
        raiseTypeError(globalObject, scope, "file must have a 'write' attribute"_s);
        return;
    }
    self.write.set(vm, owner, write);
}

// _Pickler_SetBufferCallback()
void setBufferCallback(JSGlobalObject* globalObject, PyStateObject* owner, Pickler& self, JSValue callback)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    if (!callback || isNone(callback)) {
        self.bufferCallback.clear();
        return;
    }
    if (self.protocol < 5) {
        raiseValueError(globalObject, scope, "buffer_callback needs protocol >= 5"_s);
        return;
    }
    self.bufferCallback.set(vm, owner, callback);
}

// Pickler_clear()
void clearPickler(Pickler& self)
{
    self.output = ByteVector();
    self.write.clear();
    self.persistentID.clear();
    self.persistentIDAttribute.clear();
    self.dispatchTable.clear();
    self.fastMemo.clear();
    self.reducerOverride.clear();
    self.bufferCallback.clear();
    self.memo.clear();
    self.memoKeys.clear();
    self.hasMemo = false;
}

// ---- Pickler

PyStateObject* asPickler(JSValue value) { return uncheckedDowncast<PyStateObject>(value.asCell()); }

PYTHON_NATIVE(picklerNew)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    return JSValue::encode(PyStateObject::create(vm, asType(args[0])->instanceStructure(), makeUnique<Pickler>()));
}

// Pickler.__init__(file, protocol=None, fix_imports=True, buffer_callback=None)
PYTHON_NATIVE(picklerInit)
{
    NATIVE_PROLOGUE();
    PyStateObject* owner = asPickler(args[0]);
    auto& self = owner->state<Pickler>();
    bool fixesImports = true;
    if (JSValue given = args.at(3)) {
        fixesImports = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    UsingPickler usingPickler(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    // It may have been called before.
    if (self.write)
        clearPickler(self);
    setProtocol(globalObject, self, args.at(2) ? args.at(2) : jsUndefined(), fixesImports);
    RETURN_IF_EXCEPTION(scope, { });
    setOutputStream(globalObject, owner, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    setBufferCallback(globalObject, owner, self, args.at(4));
    RETURN_IF_EXCEPTION(scope, { });
    if (!self.hasMemo)
        clearMemo(globalObject, owner, self);
    self.output = ByteVector();
    self.isFast = 0;
    self.fastNesting = 0;
    self.fastMemo.clear();
    if (!self.dispatchTable) {
        JSValue table = getAttributeIfPresent(globalObject, owner, Identifier::fromString(vm, "dispatch_table"_s));
        RETURN_IF_EXCEPTION(scope, { });
        self.dispatchTable.set(vm, owner, table);
    }
    RETURN_NONE();
}

// persistent_id(): the one that a Pickler has unless it is given another, which says of nothing that it is known elsewhere.
JSC_DEFINE_HOST_FUNCTION(picklerPersistentID, (JSGlobalObject* globalObject, CallFrame* callFrame))
{
    if (!checkArguments(globalObject, callFrame)) [[unlikely]]
        return { };
    RETURN_NONE();
}

// Pickler.clear_memo()
PYTHON_NATIVE(picklerClearMemo)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    PyStateObject* owner = asPickler(args[0]);
    if (owner->state<Pickler>().hasMemo)
        clearMemo(globalObject, owner, owner->state<Pickler>());
    RETURN_NONE();
}

// Pickler.dump(obj)
PYTHON_NATIVE(picklerDump)
{
    NATIVE_PROLOGUE();
    PyStateObject* owner = asPickler(args[0]);
    auto& self = owner->state<Pickler>();
    // A class derived from it may have an __init__() that does not call this one's.
    if (!self.write)
        return JSValue::encode(raise(globalObject, scope, pickleModuleState(globalObject).picklingError.get(), concatenate("Pickler.__init__() was not called by "_s, typeName(globalObject, owner), ".__init__()"_s)));
    UsingPickler usingPickler(globalObject, self);
    RETURN_IF_EXCEPTION(scope, { });
    Saver saver(globalObject, owner);
    saver.clearBuffer();
    saver.dump(args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    saver.flushToFile();
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// Pickler.__sizeof__(). The numbers are what they would be in CPython.
PYTHON_NATIVE(picklerSizeOf)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    auto& self = stateOf<Pickler>(args[0]);
    int64_t size = Python::typeOf(globalObject, args[0])->basicSize();
    if (self.hasMemo) {
        // PyMemoTable, which has room for a power of two, of which no more than two thirds are taken.
        size_t allocated = 8;
        while (self.memo.size() * 3 >= allocated * 2)
            allocated <<= 1;
        size += 32 + allocated * 16;
        // And a bytes of WRITE_BUF_SIZE
        size += 33 + std::max<size_t>(4096, self.output.size());
    }
    return JSValue::encode(intFromInt64(globalObject, size));
}

// Pickler_getattr()
PYTHON_NATIVE(picklerGetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    if (JSValue set = stateOf<Pickler>(args[0]).persistentIDAttribute.get(); set && name->string() == "persistent_id"_s)
        return JSValue::encode(set);
    RELEASE_AND_RETURN(scope, JSValue::encode(getAttributeAsObjectDoes(globalObject, args[0], *name)));
}

// Pickler_setattr()
PYTHON_NATIVE(picklerSetAttribute)
{
    NATIVE_PROLOGUE();
    auto name = attributeName(globalObject, scope, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    JSValue value = args.at(2);
    if (name->string() == "persistent_id"_s)
        stateOf<Pickler>(args[0]).persistentIDAttribute.set(vm, args[0].asCell(), value);
    else {
        setAttributeAsObjectDoes(globalObject, args[0], *name, value);
        RETURN_IF_EXCEPTION(scope, { });
    }
    RETURN_NONE();
}

// ---- PicklerMemoProxy: so that pickler.memo.clear() and pickler.memo = saved go on working, as they did when it was a dict.

struct PicklerMemoProxy final : NativeState {
    PYTHON_NATIVE_STATE(PicklerMemoProxy);
    WriteBarrier<PyStateObject> pickler;
};

template<typename Visitor> void PicklerMemoProxy::visit(Visitor& visitor) { visitor.append(pickler); }

PYTHON_NATIVE(picklerMemoProxyClear)
{
    NATIVE_PROLOGUE();
    UNUSED_PARAM(scope);
    PyStateObject* pickler = stateOf<PicklerMemoProxy>(args[0]).pickler.get();
    if (pickler->state<Pickler>().hasMemo)
        clearMemo(globalObject, pickler, pickler->state<Pickler>());
    RETURN_NONE();
}

// { id(object): (what it is known by, object) }. Empty if it raised.
JSValue copyOfMemo(JSGlobalObject* globalObject, Pickler& self)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
    PyDict* copy = PyDict::create(globalObject);
    for (auto& [key, value] : self.memo) {
        copy->set(globalObject, intFromInt64(globalObject, static_cast<int64_t>(std::bit_cast<uintptr_t>(key))), PyTuple::create(globalObject, { intFromInt64(globalObject, value), key }));
        RETURN_IF_EXCEPTION(scope, { });
    }
    return copy;
}

PYTHON_NATIVE(picklerMemoProxyCopy)
{
    NATIVE_PROLOGUE();
    RELEASE_AND_RETURN(scope, JSValue::encode(copyOfMemo(globalObject, stateOf<PicklerMemoProxy>(args[0]).pickler->state<Pickler>())));
}

PYTHON_NATIVE(picklerMemoProxyReduce)
{
    NATIVE_PROLOGUE();
    JSValue contents = copyOfMemo(globalObject, stateOf<PicklerMemoProxy>(args[0]).pickler->state<Pickler>());
    RETURN_IF_EXCEPTION(scope, { });
    return JSValue::encode(PyTuple::create(globalObject, { realm->typeDict()->object(), PyTuple::create(globalObject, { contents }) }));
}

// Pickler_get_memo()
JSValue getMemoOfPickler(JSGlobalObject* globalObject, JSValue self)
{
    VM& vm = globalObject->vm();
    auto* proxy = PyStateObject::create(vm, pickleModuleState(globalObject).picklerMemoProxyType->instanceStructure(), makeUnique<PicklerMemoProxy>());
    proxy->state<PicklerMemoProxy>().pickler.set(vm, proxy, asPickler(self));
    return proxy;
}

// Pickler_set_memo()
void setMemoOfPickler(JSGlobalObject* globalObject, JSValue selfValue, JSValue given)
{
    VM& vm = globalObject->vm();
    auto scope = DECLARE_THROW_SCOPE(vm);
    PyStateObject* owner = asPickler(selfValue);
    auto& self = owner->state<Pickler>();
    if (!given) {
        raiseTypeError(globalObject, scope, "attribute deletion is not supported"_s);
        return;
    }
    // All of it is gathered before any of it is kept.
    MarkedArgumentBuffer keys;
    Vector<int64_t> values;
    if (Python::typeOf(globalObject, given) == pickleModuleState(globalObject).picklerMemoProxyType.get()) {
        for (auto& [key, value] : stateOf<PicklerMemoProxy>(given).pickler->state<Pickler>().memo) {
            keys.append(key);
            values.append(value);
        }
    } else if (isDict(given)) {
        asDict(given)->forEach(globalObject, [&] (JSValue, JSValue value) {
            if (!isTuple(value) || asTuple(value)->length() != 2) {
                raiseTypeError(globalObject, scope, "'memo' values must be 2-item tuples"_s);
                return false;
            }
            auto identifier = toSsizeOfInt(globalObject, asTuple(value)->at(0));
            if (scope.exception())
                return false;
            // What is not kept anywhere is never looked for.
            if (asTuple(value)->at(1).isCell()) {
                keys.append(asTuple(value)->at(1));
                values.append(*identifier);
            }
            return true;
        });
        RETURN_IF_EXCEPTION(scope, void());
    } else {
        raiseTypeError(globalObject, scope, concatenate("'memo' attribute must be a PicklerMemoProxy object or dict, not "_s, typeName(globalObject, given)));
        return;
    }
    clearMemo(globalObject, owner, self);
    for (size_t i = 0; i < keys.size(); ++i) {
        setMemo(globalObject, self, keys.at(i), values[i]);
        RETURN_IF_EXCEPTION(scope, void());
    }
}

// ---- dump() and dumps()

// dump(obj, file, protocol=None, *, fix_imports=True, buffer_callback=None)
PYTHON_NATIVE(pickleDump)
{
    NATIVE_PROLOGUE();
    bool fixesImports = true;
    if (JSValue given = args.at(3)) {
        fixesImports = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyStateObject* pickler = newPickler(globalObject);
    auto& self = pickler->state<Pickler>();
    setProtocol(globalObject, self, args.at(2) ? args.at(2) : jsUndefined(), fixesImports);
    RETURN_IF_EXCEPTION(scope, { });
    setOutputStream(globalObject, pickler, self, args[1]);
    RETURN_IF_EXCEPTION(scope, { });
    setBufferCallback(globalObject, pickler, self, args.at(4));
    RETURN_IF_EXCEPTION(scope, { });
    Saver saver(globalObject, pickler);
    saver.dump(args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    saver.flushToFile();
    RETURN_IF_EXCEPTION(scope, { });
    RETURN_NONE();
}

// dumps(obj, protocol=None, *, fix_imports=True, buffer_callback=None)
PYTHON_NATIVE(pickleDumps)
{
    NATIVE_PROLOGUE();
    bool fixesImports = true;
    if (JSValue given = args.at(2)) {
        fixesImports = isTrue(globalObject, given);
        RETURN_IF_EXCEPTION(scope, { });
    }
    PyStateObject* pickler = newPickler(globalObject);
    auto& self = pickler->state<Pickler>();
    setProtocol(globalObject, self, args.at(1) ? args.at(1) : jsUndefined(), fixesImports);
    RETURN_IF_EXCEPTION(scope, { });
    setBufferCallback(globalObject, pickler, self, args.at(3));
    RETURN_IF_EXCEPTION(scope, { });
    Saver saver(globalObject, pickler);
    saver.dump(args[0]);
    RETURN_IF_EXCEPTION(scope, { });
    RELEASE_AND_RETURN(scope, JSValue::encode(saver.takeString()));
}

} // namespace

void initializePickler(JSGlobalObject* globalObject)
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

    PyType* pickler = make(state.picklerType, "_pickle.Pickler"_s);
    addMethods(globalObject, pickler, {
        { "__new__"_s, picklerNew, Kind::New, 0, "($type, /, *args, **kwargs)"_s, Arguments::AreNotChecked },
        { "__init__"_s, picklerInit, Kind::Wrapper, 0, { }, Arguments::AreThoseOfTheClass },
        { "__getattribute__"_s, picklerGetAttribute },
        { "__setattr__"_s, picklerSetAttribute },
        { "__delattr__"_s, picklerSetAttribute },
        { "persistent_id"_s, picklerPersistentID },
        { "dump"_s, picklerDump, Kind::Method, 0, { }, Arguments::AreCheckedAsWithDefiningClass },
        { "clear_memo"_s, picklerClearMemo },
        { "__sizeof__"_s, picklerSizeOf },
    });
    addMember(globalObject, pickler, "bin"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Pickler>(self).isBinary); },
        [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setMember(globalObject, value, stateOf<Pickler>(self).isBinary); });
    addMember(globalObject, pickler, "fast"_s, [] (JSGlobalObject*, JSValue self) -> JSValue { return jsNumber(stateOf<Pickler>(self).isFast); },
        [] (JSGlobalObject* globalObject, JSValue self, JSValue value) { setMember(globalObject, value, stateOf<Pickler>(self).isFast); });
    // Py_T_OBJECT_EX: there is no such attribute while there is nothing there.
    addMember(globalObject, pickler, "dispatch_table"_s, [] (JSGlobalObject* globalObject, JSValue self) -> JSValue {
        if (JSValue table = stateOf<Pickler>(self).dispatchTable.get())
            return table;
        auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
        return raise(globalObject, scope, BuiltinType::AttributeError, concatenate('\'', typeName(globalObject, self), "' object has no attribute 'dispatch_table'"_s));
    }, [] (JSGlobalObject* globalObject, JSValue self, JSValue value) {
        auto& table = stateOf<Pickler>(self).dispatchTable;
        if (!value && !table) {
            auto scope = DECLARE_THROW_SCOPE(globalObject->vm());
            raise(globalObject, scope, BuiltinType::AttributeError, "dispatch_table"_s);
            return;
        }
        table.set(globalObject->vm(), self.asCell(), value);
    });
    addGetSet(globalObject, pickler, "memo"_s, getMemoOfPickler, setMemoOfPickler);

    PyType* proxy = make(state.picklerMemoProxyType, "_pickle.PicklerMemoProxy"_s);
    addMethods(globalObject, proxy, {
        { "clear"_s, picklerMemoProxyClear },
        { "copy"_s, picklerMemoProxyCopy },
        { "__reduce__"_s, picklerMemoProxyReduce },
    });
    proxy->putDirect(vm, vm.pythonNames().dunder_hash, jsUndefined());
}

void addPicklerFunctions(JSGlobalObject* globalObject, JSObject* module)
{
    addFunction(globalObject, module, "dump"_s, pickleDump);
    addFunction(globalObject, module, "dumps"_s, pickleDumps);
}

} } // namespace JSC::Python
