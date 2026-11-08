/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "JSCellButterfly.h"
#include "PyBuffer.h"
#include "PyModuleHelpers.h"
#include "PyNative.h"
#include "PyType.h"

WTF_ALLOW_UNSAFE_BUFFER_USAGE_BEGIN

namespace JSC {
namespace Python {
namespace Ctypes {

// The module _ctypes. See README.md, "ctypes".
//
// PyCtypes.cpp        the state, the module and its functions
// PyCtypesTypes.cpp   the metaclasses: what a class statement makes of _type_, _fields_, _length_ and so on
// PyCtypesData.cpp    _CData and the kinds of instances; what keeps what alive; buffers, pickling
// PyCtypesFields.cpp  values to and from the bytes of C; CField
// PyCtypesCalls.cpp   CFuncPtr: arguments, calls, results, callbacks

class State;
class PyCData;

// ---- Values as C has them ----

// How the bytes of a simple type are read and written. (The integer types differ in size only.)
enum class Code : uint8_t {
    None, // An aggregate: there is no value but the instance.
    SignedInt, // b h i l q
    UnsignedInt, // B H I L Q
    Float, // f
    Double, // d
    LongDouble, // g
    ComplexFloat, // Zf
    ComplexDouble, // Zd
    ComplexLongDouble, // Zg
    Bool, // ?
    VariantBool, // v
    Char, // c
    WChar, // u
    CharPointer, // z
    WCharPointer, // Z
    VoidPointer, // P
    Object, // O
    CharArray, // s: a field that is an array of c_char
    WCharArray, // U: a field that is an array of c_wchar
};

struct Accessor {
    Code code { Code::None };
    bool isSwapped { false }; // The bytes are in the order that is not the machine's.
    explicit operator bool() const { return code != Code::None; }
    bool operator==(const Accessor&) const = default;
};

// What a function is told about an argument or a result: how the calling convention classifies it.
enum class ForeignKind : uint8_t {
    Void,
    SInt8,
    UInt8,
    SInt16,
    UInt16,
    SInt32,
    UInt32,
    SInt64,
    UInt64,
    Float,
    Double,
    LongDouble,
    ComplexFloat,
    ComplexDouble,
    ComplexLongDouble,
    Pointer,
    Aggregate,
};

struct CodeDescription {
    ASCIILiteral text; // What _type_ is.
    Code code;
    uint8_t size;
    uint8_t alignment;
    ForeignKind foreignKind;
    bool canBeSwapped;
    ASCIILiteral format; // In a buffer (PEP 3118), after the mark for the order of the bytes.
};
// Null: there is no such type.
const CodeDescription* describeCode(StringView);

// A value on its way into memory.
struct Packed {
    alignas(16) uint8_t bytes[32];
    JSValue keep; // What the bytes point into, which has to live while they are in use. None: nothing.
    std::span<const uint8_t> text; // CharArray and WCharArray: what to copy.
    JSCell* holder { nullptr }; // Whose memory `text` is: store() keeps it alive until it has copied.
};

// Which bits of the integer, for a bit field.
struct Bits {
    uint8_t size { 0 }; // 0: all of it.
    uint8_t offset { 0 };
};

// In two steps, because the first may run anything (__index__, __float__, __bool__), and that may move the memory.
// `size`: of the field, which is how many bytes store() will be given. False: an exception.
bool pack(JSGlobalObject*, Accessor, JSValue, int64_t size, Packed&);
void store(std::span<uint8_t> destination, Accessor, const Packed&, Bits = { });
// `hint`: for a py_object, what the memory is believed to refer to. Empty: an exception.
JSValue load(JSGlobalObject*, Accessor, std::span<const uint8_t> source, Bits = { }, JSValue hint = { });

// The 64 low bits of an int, in two's complement.
uint64_t lowBitsOfInt(JSValue exactInt);
// As PyLong_AsVoidPtr(). False: an exception.
bool pointerFromInt(JSGlobalObject*, JSValue, uintptr_t&);
JSValue intFromPointer(JSGlobalObject*, const void*);
// The text as wchar_t, with a NUL at the end, in memory of the collector. `length` does not count the NUL. Null: an exception.
PyByteStorage* wideCharactersOf(JSGlobalObject*, JSString*, size_t& length);
// Empty: an exception.
JSValue strFromWideCharacters(JSGlobalObject*, const wchar_t*, size_t length);
// A capsule, as CPython has one for such memory: something a program may find in _objects.
JSValue capsuleForMemory(JSGlobalObject*, PyByteStorage*);

// ---- What a ctypes type knows (StgInfo) ----

enum class TypeKind : uint8_t {
    None, // The __init__ of the metaclass has not got as far as what a kind promises, or has raised before: infoOf() does not hand it out.
    Simple,
    Structure,
    Union,
    Array,
    Pointer,
    Function,
};

enum TypeInfoFlag : unsigned {
    // Of functions: FUNCFLAG_*.
    FunctionCdecl = 0x1,
    FunctionHResult = 0x2,
    FunctionPythonAPI = 0x4,
    FunctionUseErrno = 0x8,
    FunctionUseLastError = 0x10,
    // Of data.
    IsPointer = 0x100,
    HasPointer = 0x200,
    HasUnion = 0x400,
    HasBitField = 0x800,
};

// It is the hidden field 0 of the type. A type of one of the metaclasses that has none is abstract: _SimpleCData, Structure,
// a class with _abstract_.
class PyCTypeInfo final : public PyObject {
public:
    PY_DECLARE_CELL(PyCTypeInfo, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCTypeInfo* create(VM&, State&);
    // All but what belongs to the one type: whether it is final, its pointer type.
    void copyFrom(VM&, const PyCTypeInfo&);

    TypeKind kind { TypeKind::None };
    Accessor accessor;
    ForeignKind foreignKind { ForeignKind::Aggregate };
    char letter { 0 }; // The first character of _type_.
    bool isFinal { false }; // Something was made that depends on the layout: _fields_ can no longer be set.
    unsigned flags { 0 };
    int64_t size { 0 };
    int64_t alignment { 0 };
    int64_t length { 0 }; // How many items or fields: how many places _objects needs.

    WriteBarrier<Unknown> proto; // Simple: the str _type_. Array, Pointer: the type of the items. Empty: not set.
    WriteBarrier<Unknown> argTypes; // Function: a tuple, or empty.
    WriteBarrier<Unknown> converters; // Function: the from_param of each, a tuple, or empty.
    WriteBarrier<Unknown> resultType; // Function: a type, a callable, None, or empty.
    WriteBarrier<Unknown> checker; // Function: _check_retval_ of the result type, or empty.
    WriteBarrier<Unknown> pointerType; // POINTER(type), once made. Empty: not yet.
    WriteBarrier<JSString> format; // Null: "B".
    WriteBarrier<PyTuple> shape; // Of ints. Its length is the number of dimensions.
    WriteBarrier<PyTuple> fields; // Structure, Union: every CField, those of the bases first.
    WriteBarrier<State> state;

private:
    PyCTypeInfo(VM&, Structure*, PyType*);
};

// Null, without an exception: not a ctypes type, or an abstract one. What it returns has all that its kind promises:
// Simple: proto is a str that describeCode() knows, and accessor, size and alignment are that code's. Array: proto is a type that had an info, and size, length and
// shape agree. Pointer, Function: size is that of a pointer; proto is a type that had an info, or empty. Structure, Union: fields is null or a tuple of PyCField.
PyCTypeInfo* infoOf(JSGlobalObject*, JSValue type);
PyCTypeInfo* infoOfInstance(JSGlobalObject*, JSValue object);
// Null: a SystemError, it has one.
PyCTypeInfo* createInfo(JSGlobalObject*, PyType*);

// ---- Instances ----

// Where the memory of an instance is.
// - Its own, or that of the instance it is a part of: a PyByteStorage, which the collector frees and never moves.
// - That of an object with a buffer (from_buffer): a memoryview, through which it is found again at each use. So it is noticed
//   when the view has been released, when memory of WebAssembly has grown, or when an audit hook had JS take the memory away
//   before from_buffer() was through. But for that the memory stays: that of Python's is exported, that of JS is pinned.
// - Anywhere (from_address, what a pointer points to, in_dll): nothing is known about it but the address.
class PyCData : public PyObject {
public:
    PY_DECLARE_CELL(PyCData, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCData* create(VM&, Structure*, PyType*);

    // Null: an exception. What it returns is good until code of the program runs.
    ALWAYS_INLINE uint8_t* memory(JSGlobalObject* globalObject) const
    {
        if (!m_isView) [[likely]]
            return std::bit_cast<uint8_t*>(m_address);
        return memoryOfView(globalObject);
    }

    JSCell* owner() const { return m_owner.get(); }
    bool isView() const { return m_isView; }
    int64_t offsetInView() const { return static_cast<int64_t>(m_address); }

    void setStorage(VM&, PyByteStorage*, size_t offset);
    // resize() has given the instance new memory. What it had lives as long as the instance: there may be addresses of it about, in pointers that keep the instance alive
    // and in arguments of a call that is being put together.
    void keepFormerStorage(VM&, PyByteStorage*);
    // Null: it has what it always had.
    JSCellButterfly* formerStorages() const { return m_isView || !m_past ? nullptr : uncheckedDowncast<JSCellButterfly>(m_past.get().asCell()); }
    // `memory`: where that is now.
    void setView(VM&, PyMemoryView*, int64_t offset, const uint8_t* memory);
    // Where the memory was when the view was taken. It tells what points there once the memory is there no more: the view is gone, or, which is rare, the memory has moved under
    // a view that is good. It is compared, and followed in one case only:
    uintptr_t whereViewWas() const { return static_cast<uintptr_t>(m_past.get().asNumber()); }
    // The view has been released. What it pins of the memory of JS stays where it was all the same, for as long as the cell of the view lives, and this holds the cell. Null: it is gone.
    uint8_t* memoryBehindReleasedView(JSGlobalObject*) const;
    void setAddress(const void*);
    // The same memory as the other's, from that offset on.
    void setMemoryOf(VM&, const PyCData*, int64_t offset);

    int64_t size { 0 }; // b_size
    int64_t length { 0 }; // b_length
    int64_t index { 0 }; // b_index: which part of the base it is.
    WriteBarrier<PyCData> base; // b_base
    WriteBarrier<Unknown> objects; // b_objects. Empty: not made yet.
    bool ownsMemory { false }; // b_needsfree

protected:
    PyCData(VM&, Structure*, PyType*);

private:
    uint8_t* memoryOfView(JSGlobalObject*) const;

    bool m_isView { false };
    WriteBarrier<JSCell> m_owner;
    uintptr_t m_address { 0 }; // For a view: the offset into it.
    WriteBarrier<Unknown> m_past; // For a view: whereViewWas(), a number. For memory of its own: formerStorages().
};

class PyCFuncPtr final : public PyCData {
public:
    PY_DECLARE_CELL(PyCFuncPtr, PyCData, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCFuncPtr* create(VM&, Structure*, PyType*);

    // Empty: not set, which leaves what the type says.
    WriteBarrier<Unknown> callable; // What a callback calls.
    WriteBarrier<Unknown> thunk; // What keeps the code of a callback.
    WriteBarrier<Unknown> converters;
    WriteBarrier<Unknown> argTypes;
    WriteBarrier<Unknown> resultType;
    WriteBarrier<Unknown> checker;
    WriteBarrier<Unknown> errorCheck;
    WriteBarrier<Unknown> paramFlags;

private:
    PyCFuncPtr(VM&, Structure*, PyType*);
};

class PyCField final : public PyObject {
public:
    PY_DECLARE_CELL(PyCField, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCField* create(VM&, Structure*, PyType*);

    int64_t byteOffset { 0 };
    int64_t byteSize { 0 };
    int64_t index { 0 };
    Bits bits;
    bool isAnonymous { false };
    Accessor accessor; // Only for arrays of characters, which read as bytes and str.
    WriteBarrier<JSString> name;
    WriteBarrier<PyType> proto;

private:
    PyCField(VM&, Structure*, PyType*);
};

// An argument as it is passed: what from_param() and byref() return.
class PyCArg final : public PyObject {
public:
    PY_DECLARE_CELL(PyCArg, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCArg* create(JSGlobalObject*);

    ForeignKind foreignKind { ForeignKind::Pointer };
    char tag { 0 };
    char tag2 { 0 }; // The second letter of the complex types.
    int64_t size { 0 }; // Of an aggregate: how many bytes the copy has.
    alignas(16) uint8_t value[32] { };
    WriteBarrier<Unknown> object; // What the value is of, or points into.
    WriteBarrier<Unknown> keep; // An aggregate is passed as a copy, `object`: the PyCTypeInfo by which the copy was made. (The instance may be given another class.)

    void setPointer(const void* pointer) { memcpy(value, &pointer, sizeof(pointer)); }
    void* pointer() const
    {
        void* result;
        memcpy(&result, value, sizeof(result));
        return result;
    }

private:
    PyCArg(VM&, Structure*, PyType*);
};

// What a callback is for C: the code that C calls, and what that code calls.
class PyCThunk final : public PyObject {
public:
    PY_DECLARE_CELL(PyCThunk, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static PyCThunk* create(VM&, Structure*, PyType*);

    unsigned flags { 0 };
    void* code { nullptr };
    int slot { -1 }; // For whatever made the code.
    bool givesReference { false }; // With its result: that is an object, by the prototype it was made with, and its interpreter is the one whose references are counted (CApi::isFor()).
    JSGlobalObject* globalObject { nullptr };
    WriteBarrier<Unknown> callable;
    WriteBarrier<PyTuple> argTypes;
    WriteBarrier<Unknown> resultType; // Empty: none.

private:
    PyCThunk(VM&, Structure*, PyType*);
};

// ---- The state of the module ----

#define FOR_EACH_CTYPES_NAME(v) \
    v(type, "_type_") \
    v(length, "_length_") \
    v(fields, "_fields_") \
    v(anonymous, "_anonymous_") \
    v(abstract, "_abstract_") \
    v(flags, "_flags_") \
    v(argtypes, "_argtypes_") \
    v(restype, "_restype_") \
    v(checkRetval, "_check_retval_") \
    v(asParameter, "_as_parameter_") \
    v(fromParam, "from_param") \
    v(fromOutParam, "__ctypes_from_outparam__") \
    v(ctypeBE, "__ctype_be__") \
    v(ctypeLE, "__ctype_le__") \
    v(handle, "_handle") \
    v(setState, "__setstate__") \
    v(size, "size") \
    v(align, "align") \
    v(formatSpec, "format_spec") \
    v(fieldsOfLayout, "fields") \
    v(getLayout, "get_layout") \
    v(isStruct, "is_struct") \
    v(base, "base") \
    v(raw, "raw") \
    v(value, "value")

class State final : public PyObject {
public:
    PY_DECLARE_CELL(State, PyObject, PyObjectType);
    DECLARE_VISIT_CHILDREN;

    static State* create(VM&, Structure*, PyType*);

    WriteBarrier<PyType> metaType; // CType_Type
    WriteBarrier<PyType> simpleMeta;
    WriteBarrier<PyType> structMeta;
    WriteBarrier<PyType> unionMeta;
    WriteBarrier<PyType> arrayMeta;
    WriteBarrier<PyType> pointerMeta;
    WriteBarrier<PyType> functionMeta;

    WriteBarrier<PyType> dataType; // _CData
    WriteBarrier<PyType> simpleType;
    WriteBarrier<PyType> structType;
    WriteBarrier<PyType> unionType;
    WriteBarrier<PyType> arrayType;
    WriteBarrier<PyType> pointerType;
    WriteBarrier<PyType> functionType;
    WriteBarrier<PyType> fieldType;
    WriteBarrier<PyType> argType;
    WriteBarrier<PyType> infoType;
    WriteBarrier<PyType> thunkType;
    WriteBarrier<PyType> argumentError;

    WriteBarrier<Structure> dataStructure;
    WriteBarrier<Structure> functionStructure;
    WriteBarrier<Structure> fieldStructure;
    WriteBarrier<Structure> argStructure;
    WriteBarrier<Structure> infoStructure;
    WriteBarrier<Structure> thunkStructure;

    WriteBarrier<PyDict> arrayTypes; // (type of the items, length) -> a weak reference to the array type
    unsigned arrayTypesSweepAt { 64 };
    WriteBarrier<PyDict> heldObjects; // id -> [object, count]: what Py_INCREF() has been called on
    WriteBarrier<Unknown> unpickle; // _ctypes._unpickle

#define DECLARE_NAME(id, text) WriteBarrier<JSString> id##Name;
    FOR_EACH_CTYPES_NAME(DECLARE_NAME)
#undef DECLARE_NAME

private:
    State(VM&, Structure*, PyType*);
};

State& stateOf(JSGlobalObject*);

extern const TypeDef metaTypeDef;
extern const TypeDef simpleMetaDef;
extern const TypeDef structMetaDef;
extern const TypeDef unionMetaDef;
extern const TypeDef arrayMetaDef;
extern const TypeDef pointerMetaDef;
extern const TypeDef functionMetaDef;
extern const TypeDef dataTypeDef;
extern const TypeDef simpleTypeDef;
extern const TypeDef structTypeDef;
extern const TypeDef unionTypeDef;
extern const TypeDef arrayTypeDef;
extern const TypeDef pointerTypeDef;
extern const TypeDef functionTypeDef;
extern const TypeDef fieldTypeDef;
extern const TypeDef argTypeDef;
extern const TypeDef thunkTypeDef;

// Whether an instance of the class is a PyCData. That goes by the built-in type that __base__ leads to, as do the methods that the instance will be handed to. Not by
// info->kind: that is what the __init__ of a metaclass wrote, which a metaclass derived from two of them, or type.__new__() and a call of the __init__, gets to run on any class.
ALWAYS_INLINE bool hasDataForInstances(State& state, PyType* type)
{
    PyType* layout = type->layoutBase();
    return layout == state.dataType.get() || layout == state.functionType.get();
}
ALWAYS_INLINE bool isInstanceOfData(JSValue value, PyType* type)
{
    PyCData* data = PyCData::tryFrom(value);
    return data && data->type()->isSubtypeOf(type);
}

// ---- Instances: PyCtypesData.cpp ----

// Where a value is: in the memory of an instance, or in what the instance (a pointer) points to.
struct Place {
    PyCData* object;
    int64_t offset { 0 };
    bool isThroughPointer { false }; // The offset counts from where the pointer points at the time that it is asked.

    // `size` bytes from there, and no byte of an instance is touched but in what this has returned. No data: an exception.
    // They are good until code of the program runs, which may free the memory, move it or make the pointer point elsewhere. So this is asked behind the last thing
    // that can run such code (converting a value, making an instance, an audit hook), and with the very number that the access goes by.
    std::span<uint8_t> resolve(JSGlobalObject*, int64_t size) const;
};

// An instance with zeroed memory of its own. __init__ is not called. Null: an exception ("abstract class", MemoryError).
PyCData* newData(JSGlobalObject*, PyType*);
// An instance that is the part `index` of another.
PyCData* dataAt(JSGlobalObject*, PyType*, const Place&, int64_t index);
// An instance for memory that nothing is known about.
PyCData* dataAtAddress(JSGlobalObject*, PyType*, const void*);
// An instance with a copy of that memory, or of as much of it as the instance has room for.
PyCData* dataFromCopy(JSGlobalObject*, PyType*, std::span<const uint8_t>);
// The __buffer__() of them all.
JSValue dataBuffer(JSGlobalObject*, JSValue self, JSValue flags);
// Whether an instance fits into so many bytes, from the offset on. If not, a ValueError. A structure may be given its fields until there is an instance of it, so
// this is asked, or asked again, where nothing of the program's runs any more before there is one: behind the audit hooks.
bool checkRoomFor(JSGlobalObject*, PyCTypeInfo*, int64_t length, int64_t offset);

// Gives the instance zeroed memory of its own, of that size. False: a MemoryError.
// `room`: how much it is given if that much is to be had.
bool allocateMemory(JSGlobalObject*, PyCData*, int64_t size, int64_t room = 0);

// Replaces the pending exception by one of that type, whose message is the prefix, the name of its class and its message.
void extendError(JSGlobalObject*, PyType* exceptionType, const String& prefix);

// What reading a field, an item or what a pointer points to gives. `accessor`: that of the field, if it has one.
JSValue getData(JSGlobalObject*, PyType*, Accessor, const Place&, int64_t index, int64_t size, Bits = { });
// False: an exception.
bool setData(JSGlobalObject*, PyType*, Accessor, const Place&, JSValue value, int64_t index, int64_t size, Bits = { });

// Remembers, in the _objects of the outermost instance, what part `index` of `target` points into. False: an exception.
// What has parts keeps something for each, in a dict; what has none keeps one thing, and lets go of it for the next. Which of the two it is, the caller says: the length of the
// cell is that of the class that it was made with, and it may have been given another.
enum class Kept : bool { ForPart, ForWhole };
bool keepAlive(JSGlobalObject*, PyCData* target, int64_t index, JSValue keep, Kept = Kept::ForPart);
// The _objects of an instance, made if need be. Empty: an exception.
JSValue keptObjects(JSGlobalObject*, PyCData*);
// What keepAlive() remembered last for that part. Empty: nothing.
JSValue keptFor(JSGlobalObject*, PyCData* target, int64_t index);

// Whether reading gives a plain value and not an instance: the class is derived from _SimpleCData directly.
bool isSimpleInstanceType(State&, PyType*);

// ---- Types: PyCtypesTypes.cpp ----

// type * length. Empty: an exception.
JSValue arrayTypeFor(JSGlobalObject*, JSValue itemType, int64_t length);
// The from_param of any of the metaclasses, called from C++.
JSValue dataTypeFromParam(JSGlobalObject*, JSValue type, JSValue value);
// _argtypes_ or argtypes: a tuple of the from_param methods. Empty: an exception.
JSValue convertersFor(JSGlobalObject*, JSValue argTypes);

// ---- Calls: PyCtypesCalls.cpp ----

// While the _as_parameter_ of an object is looked at in its place. An object may be its own, or lead back to itself.
class AsParameterScope {
    WTF_MAKE_NONCOPYABLE(AsParameterScope);
    WTF_FORBID_HEAP_ALLOCATION;
public:
    AsParameterScope() { ++depth(); }
    ~AsParameterScope() { --depth(); }
    // True: a RecursionError has been raised.
    bool isTooDeep(JSGlobalObject*) const;

private:
    static unsigned& depth();
};

// The instance as an argument. Null: an exception.
PyCArg* argumentFor(JSGlobalObject*, PyCData*);
// byref(object, offset)
JSValue makeReference(JSGlobalObject*, JSValue object, int64_t offset);

// function(*arguments). `converters`: a tuple of from_param methods, or empty. `resultType`: empty is int.
// `resultIsDropped`: the caller is not going to give the result to anybody.
JSValue callFunction(JSGlobalObject*, void* function, PyTuple* arguments, unsigned flags, JSValue converters, JSValue resultType, JSValue checker, bool resultIsDropped = false);

// The functions of C that Lib/ctypes calls through function pointers: their addresses are real, and a call to one runs
// here, without a foreign call.
uintptr_t addressOfMemmove();
uintptr_t addressOfMemset();
uintptr_t addressOfStringAt();
uintptr_t addressOfWStringAt();
uintptr_t addressOfMemoryViewAt();
uintptr_t addressOfCast();

// ctypes.get_errno(): a copy of errno for each thread, exchanged with the real one around calls that ask for it.
int& privateErrno();

// Keeps the object alive until it has been let go as many times: Py_INCREF() and Py_DECREF(). False: an exception.
bool holdObject(JSGlobalObject*, JSValue);

// A value is stored as a PyObject* by its identityBits(). 0 is NULL. And back:
// Empty: a ValueError, the bits are not those of a value of this heap. To find that out takes a while. It is not needed if they
// are those of `hint`, which is known to be alive: as a rule, what the instance keeps alive for this very reference.
JSValue objectFromBits(JSGlobalObject*, uint64_t, JSValue hint = { });

// ---- Foreign calls: PyCtypesForeign.cpp ----

// An argument or a result, as the calling convention sees it.
// (It lies in a frame under which the thread may wait: all of it is written.)
PY_BEGIN_WITHOUT_PADDING
struct ForeignValue {
    ForeignKind kind { ForeignKind::Void };
    uint8_t unused[7] { };
    PyCTypeInfo* aggregate { nullptr }; // What it is made of, if it is one.
    void* memory { nullptr }; // Where its bytes are, or are to be put. A result has room for 16 bytes at least.
    size_t size { 0 }; // Of an aggregate that is an argument: how many bytes there are.
};
PY_END_WITHOUT_PADDING

// Calls the function, without the GIL unless the flags say FunctionPythonAPI. The arguments from `fixedCount` on are those that
// a variadic function declares as "...". False: an exception: there is no way to make such a call here.
bool callForeignFunction(JSGlobalObject*, void* function, std::span<const ForeignValue> arguments, unsigned fixedCount, const ForeignValue& result, unsigned flags);
// Sets `code` to a function of C that calls runCallback() with the thunk. False: an exception.
bool allocateCallbackCode(JSGlobalObject*, PyCThunk*);
void releaseCallbackCode(PyCThunk*);
// Whether the address is one that allocateCallbackCode() gives out. Then `givesReference` is that of the thunk that it was last given to, in whatever interpreter.
bool isCallbackCode(void*, bool& givesReference);
// With the GIL. `arguments[i]` is where argument i is; the result, if any, is stored where `result` says. It raises nothing.
void runCallback(PyCThunk*, void** arguments, void* result);

} // namespace Ctypes
} // namespace Python
} // namespace JSC

WTF_ALLOW_UNSAFE_BUFFER_USAGE_END
