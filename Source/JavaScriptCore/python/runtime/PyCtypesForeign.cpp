/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "PyCtypes.h"

#include "JSCInlines.h"
#include "PyThreads.h"
#include <array>
#include <wtf/Lock.h>

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// Calls to and from functions of C whose signatures are known only when the program runs. See README.md, "ctypes".
//
// No code is generated. In the calling conventions of x86-64 (System V) and of AArch64, where an argument goes depends on its
// class and on how many of that class came before it: the first integers and pointers go into the integer registers, the first
// floats into the vector registers, all the rest onto the stack, in order. A function reads only what it declares. So one call
// that fills every argument register and a stretch of stack stands for all signatures: what it takes is to work out, for each
// argument, which register or which bytes of the stack are its own. The compiler makes that call, from a prototype with
// as many parameters as there are registers. The way back is the same: a function with such a prototype finds every register
// and the stack in its parameters.

#if (CPU(X86_64) || CPU(ARM64)) && !OS(WINDOWS)
#define CTYPES_HAS_FOREIGN_CALLS 1
#else
#define CTYPES_HAS_FOREIGN_CALLS 0
#endif

static bool raiseUnsupported(JSGlobalObject* globalObject, ASCIILiteral what)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    raiseNotImplementedError(globalObject, scope, "ctypes: "_s, what, " not supported on this platform yet"_s);
    return false;
}

#if CTYPES_HAS_FOREIGN_CALLS

#if CPU(X86_64)
static constexpr unsigned integerRegisterCount = 6;
#else
static constexpr unsigned integerRegisterCount = 8;
#endif
static constexpr unsigned vectorRegisterCount = 8;
static constexpr unsigned maxOutgoingStackSlots = 64;
static constexpr unsigned incomingStackSlots = 16;
// On Apple's AArch64, arguments on the stack take the room they need and no more, and what "..." stands for is all on the stack.
#if CPU(ARM64) && OS(DARWIN)
static constexpr bool packsStack = true;
#else
static constexpr bool packsStack = false;
#endif

static constexpr bool longDoubleIsDouble = sizeof(long double) == sizeof(double);

// ---- What an aggregate is made of ----

struct Scalar {
    ForeignKind kind;
    uint32_t offset;
};
using Scalars = Vector<Scalar, 16>;

static unsigned sizeOfScalar(ForeignKind kind)
{
    switch (kind) {
    case ForeignKind::SInt8:
    case ForeignKind::UInt8:
        return 1;
    case ForeignKind::SInt16:
    case ForeignKind::UInt16:
        return 2;
    case ForeignKind::SInt32:
    case ForeignKind::UInt32:
    case ForeignKind::Float:
        return 4;
    case ForeignKind::LongDouble:
        return sizeof(long double);
    default:
        return 8;
    }
}

// False: there are too many parts for it to go into registers anyway.
static bool flatten(JSGlobalObject* globalObject, PyCTypeInfo* info, int64_t offset, Scalars& result)
{
    if (result.size() > 64)
        return false;
    switch (info->kind) {
    case TypeKind::Structure:
    case TypeKind::Union: {
        PyTuple* fields = info->fields.get();
        for (unsigned i = 0; fields && i < fields->length(); ++i) {
            auto* field = PyCField::from(fields->at(i));
            PyCTypeInfo* fieldInfo = infoOf(globalObject, field->proto.get());
            if (!fieldInfo || !flatten(globalObject, fieldInfo, offset + field->byteOffset, result))
                return false;
        }
        return true;
    }
    case TypeKind::Array: {
        PyCTypeInfo* itemInfo = infoOf(globalObject, info->proto.get());
        if (!itemInfo || info->length > 64)
            return false;
        for (int64_t i = 0; i < info->length; ++i) {
            if (!flatten(globalObject, itemInfo, offset + i * itemInfo->size, result))
                return false;
        }
        return true;
    }
    case TypeKind::Pointer:
    case TypeKind::Function:
        result.append({ ForeignKind::Pointer, static_cast<uint32_t>(offset) });
        return true;
    case TypeKind::Simple:
        switch (info->foreignKind) {
        case ForeignKind::ComplexFloat:
            result.append({ ForeignKind::Float, static_cast<uint32_t>(offset) });
            result.append({ ForeignKind::Float, static_cast<uint32_t>(offset + 4) });
            return true;
        case ForeignKind::ComplexDouble:
            result.append({ ForeignKind::Double, static_cast<uint32_t>(offset) });
            result.append({ ForeignKind::Double, static_cast<uint32_t>(offset + 8) });
            return true;
        case ForeignKind::ComplexLongDouble:
            result.append({ ForeignKind::LongDouble, static_cast<uint32_t>(offset) });
            result.append({ ForeignKind::LongDouble, static_cast<uint32_t>(offset + sizeof(long double)) });
            return true;
        default:
            result.append({ info->foreignKind, static_cast<uint32_t>(offset) });
            return true;
        }
    }
    return false;
}

// How an aggregate travels.
struct Passing {
    enum Way : uint8_t {
        InMemory, // x86-64: a copy on the stack; a result: where the caller says. AArch64: by reference to a copy.
        InRegisters, // Eight bytes at a time, each in an integer or a vector register.
        AsFloats, // AArch64: up to four floats or doubles, each in a vector register of its own.
    };
    Way way { InMemory };
    unsigned count { 0 };
    bool isVector[2] { false, false }; // InRegisters
    unsigned floatSize { 0 }; // AsFloats
};

static Passing classify(JSGlobalObject* globalObject, PyCTypeInfo* info)
{
    Passing result;
    Scalars scalars;
#if CPU(X86_64)
    if (info->size > 16 || !flatten(globalObject, info, 0, scalars) || scalars.isEmpty())
        return result;
    bool isUsed[2] { false, false };
    bool isInteger[2] { false, false };
    for (auto& scalar : scalars) {
        unsigned size = sizeOfScalar(scalar.kind);
        if (scalar.kind == ForeignKind::LongDouble || scalar.offset % size || scalar.offset + size > 16)
            return result;
        unsigned part = scalar.offset / 8;
        isUsed[part] = true;
        if (scalar.kind != ForeignKind::Float && scalar.kind != ForeignKind::Double)
            isInteger[part] = true;
    }
    result.way = Passing::InRegisters;
    result.count = info->size > 8 ? 2 : 1;
    for (unsigned i = 0; i < result.count; ++i)
        result.isVector[i] = isUsed[i] && !isInteger[i];
#else
    if (info->size <= 64 && flatten(globalObject, info, 0, scalars) && !scalars.isEmpty()) {
        // Members of a union share their places.
        ForeignKind kind = scalars[0].kind;
        if (kind == ForeignKind::LongDouble && longDoubleIsDouble)
            kind = ForeignKind::Double;
        bool allSame = kind == ForeignKind::Float || kind == ForeignKind::Double;
        for (auto& scalar : scalars) {
            ForeignKind other = scalar.kind == ForeignKind::LongDouble && longDoubleIsDouble ? ForeignKind::Double : scalar.kind;
            allSame &= other == kind;
        }
        unsigned size = sizeOfScalar(kind);
        if (allSame && info->size % size == 0 && info->size / size <= 4) {
            result.way = Passing::AsFloats;
            result.count = static_cast<unsigned>(info->size / size);
            result.floatSize = size;
            return result;
        }
    }
    if (info->size > 16 || !info->size)
        return result;
    result.way = Passing::InRegisters;
    result.count = info->size > 8 ? 2 : 1;
#endif
    return result;
}

// ---- Outgoing calls ----

struct Outgoing {
    uint64_t integers[8] { };
    uint64_t vectors[8] { };
    alignas(16) uint64_t stack[maxOutgoingStackSlots] { };
    unsigned integersUsed { 0 };
    unsigned vectorsUsed { 0 };
    size_t stackUsed { 0 }; // In bytes.
    bool overflowed { false };

    void push(const void* bytes, size_t size, size_t alignment)
    {
        if (!packsStack) {
            alignment = std::max<size_t>(alignment, 8);
            stackUsed = roundUpToMultipleOf(alignment, stackUsed);
        } else
            stackUsed = roundUpToMultipleOf(alignment, stackUsed);
        if (stackUsed + size > sizeof(stack)) {
            overflowed = true;
            return;
        }
        memcpy(reinterpret_cast<uint8_t*>(stack) + stackUsed, bytes, size);
        stackUsed += packsStack ? size : roundUpToMultipleOf<8>(size);
    }
    void pushSlot(uint64_t bits)
    {
        stackUsed = roundUpToMultipleOf<8>(stackUsed);
        push(&bits, 8, 8);
    }
};

static uint64_t widen(ForeignKind kind, const void* memory)
{
    auto read = [&]<typename T>(T) {
        T value;
        memcpy(&value, memory, sizeof(T));
        return static_cast<uint64_t>(static_cast<int64_t>(value));
    };
    switch (kind) {
    case ForeignKind::SInt8:
        return read(int8_t { });
    case ForeignKind::UInt8:
        return read(uint8_t { });
    case ForeignKind::SInt16:
        return read(int16_t { });
    case ForeignKind::UInt16:
        return read(uint16_t { });
    case ForeignKind::SInt32:
        return read(int32_t { });
    case ForeignKind::UInt32:
        return read(uint32_t { });
    default:
        return read(int64_t { });
    }
}

static uint64_t partOf(const void* memory, size_t offset, size_t total)
{
    uint64_t bits = 0;
    memcpy(&bits, static_cast<const uint8_t*>(memory) + offset, std::min<size_t>(8, total - offset));
    return bits;
}

// False: an exception.
static bool place(JSGlobalObject* globalObject, Outgoing& out, const ForeignValue& value, bool isVariadic)
{
    bool onStackOnly = packsStack && isVariadic;
    ForeignKind kind = value.kind;
    if (kind == ForeignKind::LongDouble && longDoubleIsDouble)
        kind = ForeignKind::Double;
    if (kind == ForeignKind::ComplexLongDouble && longDoubleIsDouble)
        kind = ForeignKind::ComplexDouble;

    auto placeFloats = [&](unsigned count, unsigned size) {
        if (!onStackOnly && out.vectorsUsed + count <= vectorRegisterCount) {
            for (unsigned i = 0; i < count; ++i)
                out.vectors[out.vectorsUsed++] = partOf(value.memory, i * size, i * size + size);
            return;
        }
#if CPU(ARM64)
        out.vectorsUsed = vectorRegisterCount;
#endif
        if (onStackOnly)
            out.stackUsed = roundUpToMultipleOf<8>(out.stackUsed);
        out.push(value.memory, count * size, size);
    };

    switch (kind) {
    case ForeignKind::Void:
        return raiseUnsupported(globalObject, "an argument of type void is"_s);
    case ForeignKind::Float:
        placeFloats(1, 4);
        return true;
    case ForeignKind::Double:
        placeFloats(1, 8);
        return true;
    case ForeignKind::LongDouble:
#if CPU(X86_64)
        out.push(value.memory, 16, 16);
        return true;
#else
        return raiseUnsupported(globalObject, "long double arguments are"_s);
#endif
    case ForeignKind::ComplexFloat:
#if CPU(X86_64)
        placeFloats(1, 8);
#else
        placeFloats(2, 4);
#endif
        return true;
    case ForeignKind::ComplexDouble:
        placeFloats(2, 8);
        return true;
    case ForeignKind::ComplexLongDouble:
#if CPU(X86_64)
        out.push(value.memory, 32, 16);
        return true;
#else
        return raiseUnsupported(globalObject, "long double arguments are"_s);
#endif
    case ForeignKind::Aggregate: {
        PyCTypeInfo* info = value.aggregate;
        if (!info)
            return raiseUnsupported(globalObject, "this argument is"_s);
        if (!info->size)
            return true;
        size_t size = static_cast<size_t>(info->size);
        size_t alignment = static_cast<size_t>(std::max<int64_t>(info->alignment, 1));
        Passing passing = classify(globalObject, info);
        if (passing.way == Passing::AsFloats) {
            placeFloats(passing.count, passing.floatSize);
            return true;
        }
        if (passing.way == Passing::InMemory) {
#if CPU(X86_64)
            out.push(value.memory, size, alignment >= 16 ? 16 : 8);
#else
            // The bytes are a copy made for this call.
            uint64_t address = std::bit_cast<uintptr_t>(value.memory);
            if (!onStackOnly && out.integersUsed < integerRegisterCount)
                out.integers[out.integersUsed++] = address;
            else
                out.pushSlot(address);
#endif
            return true;
        }
        unsigned integersNeeded = 0;
        unsigned vectorsNeeded = 0;
        for (unsigned i = 0; i < passing.count; ++i)
            ++(passing.isVector[i] ? vectorsNeeded : integersNeeded);
#if CPU(ARM64)
        if (alignment >= 16 && !packsStack)
            out.integersUsed = roundUpToMultipleOf<2>(out.integersUsed);
#endif
        if (!onStackOnly && out.integersUsed + integersNeeded <= integerRegisterCount && out.vectorsUsed + vectorsNeeded <= vectorRegisterCount) {
            for (unsigned i = 0; i < passing.count; ++i) {
                uint64_t bits = partOf(value.memory, i * 8, size);
                if (passing.isVector[i])
                    out.vectors[out.vectorsUsed++] = bits;
                else
                    out.integers[out.integersUsed++] = bits;
            }
            return true;
        }
#if CPU(ARM64)
        out.integersUsed = integerRegisterCount;
#endif
        out.stackUsed = roundUpToMultipleOf<8>(out.stackUsed);
        out.push(value.memory, size, alignment >= 16 ? 16 : 8);
        out.stackUsed = roundUpToMultipleOf<8>(out.stackUsed);
        return true;
    }
    default: {
        uint64_t bits = widen(kind, value.memory);
        if (!onStackOnly && out.integersUsed < integerRegisterCount)
            out.integers[out.integersUsed++] = bits;
        else if (onStackOnly || !packsStack)
            out.pushSlot(bits);
        else {
            unsigned size = sizeOfScalar(kind);
            out.push(&bits, size, size);
        }
        return true;
    }
    }
}

struct TwoIntegers {
    uint64_t first;
    uint64_t second;
};
struct TwoDoubles {
    double first;
    double second;
};
struct IntegerAndDouble {
    uint64_t first;
    double second;
};
struct DoubleAndInteger {
    double first;
    uint64_t second;
};
struct FourDoubles {
    double values[4];
};
template<size_t size>
struct Block {
    uint8_t bytes[size];
};

#define CTYPES_V(i) std::bit_cast<double>(out.vectors[i])
#define CTYPES_VECTORS CTYPES_V(0), CTYPES_V(1), CTYPES_V(2), CTYPES_V(3), CTYPES_V(4), CTYPES_V(5), CTYPES_V(6), CTYPES_V(7)
#define CTYPES_S8(b) out.stack[b], out.stack[b + 1], out.stack[b + 2], out.stack[b + 3], out.stack[b + 4], out.stack[b + 5], out.stack[b + 6], out.stack[b + 7]
#define CTYPES_U8 uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t
#define CTYPES_D8 double, double, double, double, double, double, double, double

#if CPU(X86_64)
// As a variadic function is called, which differs in one thing: the count of vector registers in use is passed along, as such a
// function expects.
#define CTYPES_INTEGERS out.integers[0], out.integers[1], out.integers[2], out.integers[3], out.integers[4], out.integers[5]
#define CTYPES_PROTOTYPE(...) Result (*)(uint64_t, ...)
#else
#define CTYPES_INTEGERS out.integers[0], out.integers[1], out.integers[2], out.integers[3], out.integers[4], out.integers[5], out.integers[6], out.integers[7]
#define CTYPES_PROTOTYPE(...) Result (*)(CTYPES_U8, CTYPES_D8 __VA_ARGS__)
#endif

template<typename Result>
static NEVER_INLINE Result invoke(void* function, const Outgoing& out)
{
    if (!out.stackUsed)
        return reinterpret_cast<CTYPES_PROTOTYPE()>(function)(CTYPES_INTEGERS, CTYPES_VECTORS);
    if (out.stackUsed <= 8 * 8)
        return reinterpret_cast<CTYPES_PROTOTYPE(, CTYPES_U8)>(function)(CTYPES_INTEGERS, CTYPES_VECTORS, CTYPES_S8(0));
    return reinterpret_cast<CTYPES_PROTOTYPE(, CTYPES_U8, CTYPES_U8, CTYPES_U8, CTYPES_U8, CTYPES_U8, CTYPES_U8, CTYPES_U8, CTYPES_U8)>(function)(CTYPES_INTEGERS, CTYPES_VECTORS,
        CTYPES_S8(0), CTYPES_S8(8), CTYPES_S8(16), CTYPES_S8(24), CTYPES_S8(32), CTYPES_S8(40), CTYPES_S8(48), CTYPES_S8(56));
}

// A foreign call that this thread is in the middle of.
struct ActiveCall {
    std::optional<AllowThreads>* allowThreads;
    VM* vm;
    ActiveCall* previous;
};
static thread_local ActiveCall* activeCall;

bool callForeignFunction(JSGlobalObject* globalObject, void* function, std::span<const ForeignValue> arguments, unsigned fixedCount, const ForeignValue& result, unsigned flags)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    Outgoing out;

    ForeignKind resultKind = result.kind;
    if (resultKind == ForeignKind::LongDouble && longDoubleIsDouble)
        resultKind = ForeignKind::Double;
    if (resultKind == ForeignKind::ComplexLongDouble && longDoubleIsDouble)
        resultKind = ForeignKind::ComplexDouble;
    Passing resultPassing;
    size_t resultSize = 0;
    if (resultKind == ForeignKind::Aggregate) {
        resultPassing = classify(globalObject, result.aggregate);
        resultSize = static_cast<size_t>(result.aggregate->size);
#if CPU(X86_64)
        if (resultPassing.way == Passing::InMemory)
            out.integers[out.integersUsed++] = std::bit_cast<uintptr_t>(result.memory);
#else
        if (resultPassing.way == Passing::InMemory && resultSize > 4096)
            return raiseUnsupported(globalObject, "results of more than 4096 bytes are"_s);
#endif
    }
#if CPU(ARM64)
    if (resultKind == ForeignKind::LongDouble || resultKind == ForeignKind::ComplexLongDouble)
        return raiseUnsupported(globalObject, "long double results are"_s);
#else
    if (resultKind == ForeignKind::ComplexLongDouble)
        return raiseUnsupported(globalObject, "complex long double results are"_s);
#endif

    for (unsigned i = 0; i < arguments.size(); ++i) {
        if (failed(scope, place(globalObject, out, arguments[i], i >= fixedCount)))
            return false;
    }
    if (out.overflowed)
        return raiseUnsupported(globalObject, "so many arguments are"_s);

    auto* memory = static_cast<uint8_t*>(result.memory);
    auto store = [&](size_t offset, const void* bytes, size_t size) { memcpy(memory + offset, bytes, size); };
    {
        std::optional<AllowThreads> allowThreads;
        ActiveCall call { &allowThreads, &vm, activeCall };
        activeCall = &call;
        if (!(flags & FunctionPythonAPI))
            allowThreads.emplace(globalObject);
        if (flags & FunctionUseErrno)
            std::swap(errno, privateErrno());

        switch (resultKind) {
        case ForeignKind::Float:
        case ForeignKind::Double:
        case ForeignKind::ComplexFloat:
        case ForeignKind::ComplexDouble: {
#if CPU(X86_64)
            TwoDoubles values = invoke<TwoDoubles>(function, out);
            store(0, &values, sizeof(values));
#else
            FourDoubles values = invoke<FourDoubles>(function, out);
            if (resultKind == ForeignKind::ComplexFloat) {
                store(0, &values.values[0], 4);
                store(4, &values.values[1], 4);
            } else
                store(0, &values, 16);
#endif
            break;
        }
#if CPU(X86_64)
        case ForeignKind::LongDouble: {
            long double value = invoke<long double>(function, out);
            store(0, &value, sizeof(value));
            break;
        }
#endif
        case ForeignKind::Aggregate:
            if (!resultSize) {
                invoke<TwoIntegers>(function, out);
                break;
            }
            if (resultPassing.way == Passing::AsFloats) {
                FourDoubles values = invoke<FourDoubles>(function, out);
                for (unsigned i = 0; i < resultPassing.count; ++i)
                    store(i * resultPassing.floatSize, &values.values[i], resultPassing.floatSize);
                break;
            }
            if (resultPassing.way == Passing::InMemory) {
#if CPU(X86_64)
                invoke<TwoIntegers>(function, out);
#else
                // The address for the result travels in a register that is not an argument's. The compiler sets it for a
                // function that returns something large.
                if (resultSize <= 64) {
                    auto block = invoke<Block<64>>(function, out);
                    store(0, &block, resultSize);
                } else if (resultSize <= 512) {
                    auto block = invoke<Block<512>>(function, out);
                    store(0, &block, resultSize);
                } else {
                    auto block = invoke<Block<4096>>(function, out);
                    store(0, &block, resultSize);
                }
#endif
                break;
            }
            {
                uint64_t parts[2] { };
                if (!resultPassing.isVector[0] && !resultPassing.isVector[1]) {
                    TwoIntegers values = invoke<TwoIntegers>(function, out);
                    memcpy(parts, &values, sizeof(parts));
                } else if (resultPassing.isVector[0] && (resultPassing.count == 1 || resultPassing.isVector[1])) {
                    TwoDoubles values = invoke<TwoDoubles>(function, out);
                    memcpy(parts, &values, sizeof(parts));
                } else if (resultPassing.isVector[0]) {
                    DoubleAndInteger values = invoke<DoubleAndInteger>(function, out);
                    memcpy(parts, &values, sizeof(parts));
                } else {
                    IntegerAndDouble values = invoke<IntegerAndDouble>(function, out);
                    memcpy(parts, &values, sizeof(parts));
                }
                store(0, parts, resultSize);
            }
            break;
        default: {
            TwoIntegers values = invoke<TwoIntegers>(function, out);
            store(0, &values.first, sizeof(uint64_t));
            break;
        }
        }

        if (flags & FunctionUseErrno)
            std::swap(errno, privateErrno());
        allowThreads.reset();
        activeCall = call.previous;
    }
    return !scope.exception();
}

// ---- Callbacks ----

// The code of a callback has to be a function of its own, to tell it from the others, and none can be made while the program
// runs. So there is a fixed number of them, each tied to a place in a table that says which callback it is at the moment.
static constexpr unsigned callbackSlotCount = 512;

struct Incoming {
    uint64_t integers[8];
    uint64_t vectors[8];
    uint64_t stack[incomingStackSlots];
};

static Lock callbackLock;
static std::array<std::atomic<PyCThunk*>, callbackSlotCount> integerCallbacks;
static std::array<std::atomic<PyCThunk*>, callbackSlotCount> floatCallbacks;
// PyCThunk::givesReference, for who has the address of the code and may not look into the thunk: it may be another interpreter's.
static std::array<std::atomic<bool>, callbackSlotCount> integerCallbackGivesReference;

static void enterCallback(PyCThunk* thunk, Incoming& in, uint64_t (&result)[2])
{
    result[0] = result[1] = 0;
    if (!thunk) {
        fputs("ctypes: a callback was called after it had been freed; the call is ignored\n", stderr);
        return;
    }
    JSGlobalObject* globalObject = thunk->globalObject;
    VM& vm = getVM(globalObject);
    ActiveCall* call = activeCall;
    bool isInForeignCall = call && call->vm == &vm;
    if (!isInForeignCall && !vm.currentThreadIsHoldingAPILock()) {
        fputs("ctypes: a callback was called on a thread that was not started by Python; the call is ignored\n", stderr);
        return;
    }
    bool hadLetGo = isInForeignCall && call->allowThreads->has_value();
    if (hadLetGo)
        call->allowThreads->reset();

    {
        PyTuple* argTypes = thunk->argTypes.get();
        Vector<void*, 16> pointers;
        unsigned integersUsed = 0;
        unsigned vectorsUsed = 0;
        size_t stackUsed = 0;
        auto fromStack = [&](size_t size) -> void* {
            size_t alignment = packsStack ? size : 8;
            stackUsed = roundUpToMultipleOf(alignment, stackUsed);
            void* where = reinterpret_cast<uint8_t*>(in.stack) + stackUsed;
            stackUsed += packsStack ? size : 8;
            return where;
        };
        for (unsigned i = 0; i < argTypes->length(); ++i) {
            PyCTypeInfo* info = infoOf(globalObject, argTypes->at(i));
            ForeignKind kind = info->foreignKind;
            if (kind == ForeignKind::LongDouble && longDoubleIsDouble)
                kind = ForeignKind::Double;
            if (kind == ForeignKind::Float || kind == ForeignKind::Double)
                pointers.append(vectorsUsed < vectorRegisterCount ? static_cast<void*>(&in.vectors[vectorsUsed++]) : fromStack(sizeOfScalar(kind)));
            else
                pointers.append(integersUsed < integerRegisterCount ? static_cast<void*>(&in.integers[integersUsed++]) : fromStack(sizeOfScalar(kind)));
        }
        // Code in C is in between: nobody there looks for an exception, and the callback leaves none but the end of the program.
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        runCallback(thunk, pointers.mutableSpan().data(), result);
        EXCEPTION_ASSERT_UNUSED(scope, !scope.exception() || vm.hasPendingTerminationException());
    }

    if (hadLetGo)
        call->allowThreads->emplace(globalObject);
}

#if CPU(X86_64)
#define CTYPES_INCOMING_INTEGERS uint64_t i0, uint64_t i1, uint64_t i2, uint64_t i3, uint64_t i4, uint64_t i5
#define CTYPES_INCOMING_INTEGER_VALUES i0, i1, i2, i3, i4, i5, 0, 0
#else
#define CTYPES_INCOMING_INTEGERS uint64_t i0, uint64_t i1, uint64_t i2, uint64_t i3, uint64_t i4, uint64_t i5, uint64_t i6, uint64_t i7
#define CTYPES_INCOMING_INTEGER_VALUES i0, i1, i2, i3, i4, i5, i6, i7
#endif
#define CTYPES_INCOMING_PARAMETERS CTYPES_INCOMING_INTEGERS, \
    double v0, double v1, double v2, double v3, double v4, double v5, double v6, double v7, \
    uint64_t s0, uint64_t s1, uint64_t s2, uint64_t s3, uint64_t s4, uint64_t s5, uint64_t s6, uint64_t s7, \
    uint64_t s8, uint64_t s9, uint64_t s10, uint64_t s11, uint64_t s12, uint64_t s13, uint64_t s14, uint64_t s15
#define CTYPES_B(v) std::bit_cast<uint64_t>(v)
#define CTYPES_INCOMING_VALUES { CTYPES_INCOMING_INTEGER_VALUES }, \
    { CTYPES_B(v0), CTYPES_B(v1), CTYPES_B(v2), CTYPES_B(v3), CTYPES_B(v4), CTYPES_B(v5), CTYPES_B(v6), CTYPES_B(v7) }, \
    { s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12, s13, s14, s15 }

template<unsigned slot>
static uint64_t integerCallbackEntry(CTYPES_INCOMING_PARAMETERS)
{
    Incoming in { CTYPES_INCOMING_VALUES };
    uint64_t result[2];
    enterCallback(integerCallbacks[slot].load(), in, result);
    return result[0];
}

template<unsigned slot>
static double floatCallbackEntry(CTYPES_INCOMING_PARAMETERS)
{
    Incoming in { CTYPES_INCOMING_VALUES };
    uint64_t result[2];
    enterCallback(floatCallbacks[slot].load(), in, result);
    return std::bit_cast<double>(result[0]);
}

template<size_t... slots>
static constexpr std::array<void*, sizeof...(slots)> makeIntegerEntries(std::index_sequence<slots...>)
{
    return { reinterpret_cast<void*>(&integerCallbackEntry<slots>)... };
}

template<size_t... slots>
static constexpr std::array<void*, sizeof...(slots)> makeFloatEntries(std::index_sequence<slots...>)
{
    return { reinterpret_cast<void*>(&floatCallbackEntry<slots>)... };
}

static const std::array<void*, callbackSlotCount> integerEntries = makeIntegerEntries(std::make_index_sequence<callbackSlotCount>());
static const std::array<void*, callbackSlotCount> floatEntries = makeFloatEntries(std::make_index_sequence<callbackSlotCount>());

static bool returnsFloat(JSGlobalObject* globalObject, PyCThunk* thunk)
{
    PyCTypeInfo* info = thunk->resultType ? infoOf(globalObject, thunk->resultType.get()) : nullptr;
    if (!info)
        return false;
    return info->foreignKind == ForeignKind::Float || info->foreignKind == ForeignKind::Double || (info->foreignKind == ForeignKind::LongDouble && longDoubleIsDouble);
}

bool allocateCallbackCode(JSGlobalObject* globalObject, PyCThunk* thunk)
{
    VM& vm = getVM(globalObject);
    auto scope = DECLARE_THROW_SCOPE(vm);
    auto isSupported = [&](PyCTypeInfo* info) {
        if (!info)
            return false;
        switch (info->foreignKind) {
        case ForeignKind::Aggregate:
        case ForeignKind::ComplexFloat:
        case ForeignKind::ComplexDouble:
        case ForeignKind::ComplexLongDouble:
        case ForeignKind::Void:
            return false;
        case ForeignKind::LongDouble:
            return longDoubleIsDouble;
        default:
            return true;
        }
    };
    PyTuple* argTypes = thunk->argTypes.get();
    unsigned integers = 0;
    unsigned vectors = 0;
    for (unsigned i = 0; i < argTypes->length(); ++i) {
        PyCTypeInfo* info = infoOf(globalObject, argTypes->at(i));
        // What is not a ctypes type is found out when the callback is called, as in CPython: it is taken for an int.
        if (!info)
            return raiseUnsupported(globalObject, "callback parameters that are not ctypes types are"_s);
        if (!isSupported(info))
            return raiseUnsupported(globalObject, "callback parameters that are structures, complex numbers or long doubles are"_s);
        ++(info->foreignKind == ForeignKind::Float || info->foreignKind == ForeignKind::Double || info->foreignKind == ForeignKind::LongDouble ? vectors : integers);
    }
    unsigned onStack = (integers > integerRegisterCount ? integers - integerRegisterCount : 0) + (vectors > vectorRegisterCount ? vectors - vectorRegisterCount : 0);
    if (onStack > incomingStackSlots)
        return raiseUnsupported(globalObject, "callbacks with so many parameters are"_s);
    if (thunk->resultType && !isSupported(infoOf(globalObject, thunk->resultType.get())))
        return raiseUnsupported(globalObject, "callback results that are structures, complex numbers or long doubles are"_s);

    bool isFloat = returnsFloat(globalObject, thunk);
    auto& table = isFloat ? floatCallbacks : integerCallbacks;
    auto& entries = isFloat ? floatEntries : integerEntries;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        {
            Locker locker { callbackLock };
            for (unsigned i = 0; i < callbackSlotCount; ++i) {
                if (table[i].load())
                    continue;
                table[i].store(thunk);
                if (!isFloat)
                    integerCallbackGivesReference[i].store(thunk->givesReference);
                thunk->slot = static_cast<int>(i);
                thunk->code = entries[i];
                return true;
            }
        }
        // Those that the program has dropped are freed once the collector has noticed.
        if (!attempt) {
            collectAndFinalize(globalObject);
            RETURN_IF_EXCEPTION(scope, false);
        }
    }
    raiseMemoryError(globalObject, scope, "too many ctypes callbacks are alive"_s);
    return false;
}

bool isCallbackCode(void* function, bool& givesReference)
{
    // The linker puts them where it likes.
    struct Entry {
        void* code;
        std::atomic<bool>* givesReference;
    };
    static std::array<Entry, 2 * callbackSlotCount> sorted;
    static std::once_flag once;
    std::call_once(once, [] {
        for (unsigned i = 0; i < callbackSlotCount; ++i) {
            sorted[2 * i] = { integerEntries[i], &integerCallbackGivesReference[i] };
            sorted[2 * i + 1] = { floatEntries[i], nullptr };
        }
        std::ranges::sort(sorted, std::less<void*>(), &Entry::code);
    });
    auto found = std::ranges::lower_bound(sorted, function, std::less<void*>(), &Entry::code);
    if (found == sorted.end() || found->code != function)
        return false;
    givesReference = found->givesReference && found->givesReference->load();
    return true;
}

void releaseCallbackCode(PyCThunk* thunk)
{
    if (thunk->slot < 0)
        return;
    Locker locker { callbackLock };
    unsigned slot = static_cast<unsigned>(thunk->slot);
    auto& table = thunk->code == floatEntries[slot] ? floatCallbacks : integerCallbacks;
    if (table[slot].load() == thunk) {
        table[slot].store(nullptr);
        if (&table == &integerCallbacks)
            integerCallbackGivesReference[slot].store(false);
    }
    thunk->slot = -1;
    thunk->code = nullptr;
}

#else // !CTYPES_HAS_FOREIGN_CALLS

bool callForeignFunction(JSGlobalObject* globalObject, void*, std::span<const ForeignValue>, unsigned, const ForeignValue&, unsigned)
{
    return raiseUnsupported(globalObject, "calls of foreign functions are"_s);
}

bool allocateCallbackCode(JSGlobalObject* globalObject, PyCThunk*)
{
    return raiseUnsupported(globalObject, "callbacks are"_s);
}

bool isCallbackCode(void*, bool&)
{
    return false;
}

void releaseCallbackCode(PyCThunk*)
{
}

#endif

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
