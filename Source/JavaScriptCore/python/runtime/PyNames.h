/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <wtf/text/ASCIILiteral.h>
#include <wtf/text/StringHasherInlines.h>

namespace JSC {
namespace Python {

// The methods the interpreter itself looks up on the type of an object. Every type caches what it finds for each.
#define FOR_EACH_PY_SPECIAL_METHOD(v) \
    v(New, "__new__") \
    v(Init, "__init__") \
    v(Del, "__del__") \
    v(Repr, "__repr__") \
    v(Str, "__str__") \
    v(Bytes, "__bytes__") \
    v(Format, "__format__") \
    v(Hash, "__hash__") \
    v(Bool, "__bool__") \
    v(Lt, "__lt__") \
    v(Le, "__le__") \
    v(Eq, "__eq__") \
    v(Ne, "__ne__") \
    v(Gt, "__gt__") \
    v(Ge, "__ge__") \
    v(GetAttribute, "__getattribute__") \
    v(GetAttr, "__getattr__") \
    v(SetAttr, "__setattr__") \
    v(DelAttr, "__delattr__") \
    v(Dir, "__dir__") \
    v(Get, "__get__") \
    v(Set, "__set__") \
    v(Delete, "__delete__") \
    v(SetName, "__set_name__") \
    v(InitSubclass, "__init_subclass__") \
    v(InstanceCheck, "__instancecheck__") \
    v(SubclassCheck, "__subclasscheck__") \
    v(ClassGetItem, "__class_getitem__") \
    v(MroEntries, "__mro_entries__") \
    v(Prepare, "__prepare__") \
    v(Call, "__call__") \
    v(Len, "__len__") \
    v(LengthHint, "__length_hint__") \
    v(GetItem, "__getitem__") \
    v(SetItem, "__setitem__") \
    v(DelItem, "__delitem__") \
    v(Missing, "__missing__") \
    v(Iter, "__iter__") \
    v(Next, "__next__") \
    v(Reversed, "__reversed__") \
    v(Contains, "__contains__") \
    /* In the order of BinaryOperator. */ \
    v(Add, "__add__") \
    v(Sub, "__sub__") \
    v(Mul, "__mul__") \
    v(MatMul, "__matmul__") \
    v(TrueDiv, "__truediv__") \
    v(Mod, "__mod__") \
    v(Pow, "__pow__") \
    v(LShift, "__lshift__") \
    v(RShift, "__rshift__") \
    v(Or, "__or__") \
    v(Xor, "__xor__") \
    v(And, "__and__") \
    v(FloorDiv, "__floordiv__") \
    v(RAdd, "__radd__") \
    v(RSub, "__rsub__") \
    v(RMul, "__rmul__") \
    v(RMatMul, "__rmatmul__") \
    v(RTrueDiv, "__rtruediv__") \
    v(RMod, "__rmod__") \
    v(RPow, "__rpow__") \
    v(RLShift, "__rlshift__") \
    v(RRShift, "__rrshift__") \
    v(ROr, "__ror__") \
    v(RXor, "__rxor__") \
    v(RAnd, "__rand__") \
    v(RFloorDiv, "__rfloordiv__") \
    v(IAdd, "__iadd__") \
    v(ISub, "__isub__") \
    v(IMul, "__imul__") \
    v(IMatMul, "__imatmul__") \
    v(ITrueDiv, "__itruediv__") \
    v(IMod, "__imod__") \
    v(IPow, "__ipow__") \
    v(ILShift, "__ilshift__") \
    v(IRShift, "__irshift__") \
    v(IOr, "__ior__") \
    v(IXor, "__ixor__") \
    v(IAnd, "__iand__") \
    v(IFloorDiv, "__ifloordiv__") \
    v(DivMod, "__divmod__") \
    v(RDivMod, "__rdivmod__") \
    v(Neg, "__neg__") \
    v(Pos, "__pos__") \
    v(Abs, "__abs__") \
    v(Invert, "__invert__") \
    v(Int, "__int__") \
    v(Float, "__float__") \
    v(Complex, "__complex__") \
    v(Index, "__index__") \
    v(Round, "__round__") \
    v(Trunc, "__trunc__") \
    v(Floor, "__floor__") \
    v(Ceil, "__ceil__") \
    v(Enter, "__enter__") \
    v(Exit, "__exit__") \
    v(Await, "__await__") \
    v(AIter, "__aiter__") \
    v(ANext, "__anext__") \
    v(AEnter, "__aenter__") \
    v(AExit, "__aexit__") \
    v(Buffer, "__buffer__") \
    v(ReleaseBuffer, "__release_buffer__") \
    v(FsPath, "__fspath__") \
    v(SizeOf, "__sizeof__")

enum class Special : uint8_t {
#define PY_DEFINE_SPECIAL(id, text) id,
    FOR_EACH_PY_SPECIAL_METHOD(PY_DEFINE_SPECIAL)
#undef PY_DEFINE_SPECIAL
};

#define PY_COUNT_SPECIAL(id, text) +1
static constexpr unsigned numberOfSpecials = 0 FOR_EACH_PY_SPECIAL_METHOD(PY_COUNT_SPECIAL);
#undef PY_COUNT_SPECIAL

// Other strings the runtime needs at hand.
#define FOR_EACH_PY_NAME(v) \
    v(dAbstractMethods, "__abstractmethods__") \
    v(dAll, "__all__") \
    v(dAnnotate, "__annotate__") \
    v(dAnnotateFunc, "__annotate_func__") \
    v(dAnnotations, "__annotations__") \
    v(dAnnotationsCache, "__annotations_cache__") \
    v(dBase, "__base__") \
    v(dBases, "__bases__") \
    v(dBuildClass, "__build_class__") \
    v(dBuiltins, "__builtins__") \
    v(dCached, "__cached__") \
    v(dCause, "__cause__") \
    v(dClass, "__class__") \
    v(dClassCell, "__classcell__") \
    v(dClassDictCell, "__classdictcell__") \
    v(dClosure, "__closure__") \
    v(dCode, "__code__") \
    v(dContext, "__context__") \
    v(dDefaults, "__defaults__") \
    v(dDict, "__dict__") \
    v(dDoc, "__doc__") \
    v(dFile, "__file__") \
    v(dFirstLineNo, "__firstlineno__") \
    v(dFunc, "__func__") \
    v(dGetNewArgs, "__getnewargs__") \
    v(dGetNewArgsEx, "__getnewargs_ex__") \
    v(dGetState, "__getstate__") \
    v(dGlobals, "__globals__") \
    v(dImport, "__import__") \
    v(dIsAbstractMethod, "__isabstractmethod__") \
    v(dKwDefaults, "__kwdefaults__") \
    v(dLazyModules, "__lazy_modules__") \
    v(dLoader, "__loader__") \
    v(dMain, "__main__") \
    v(dMatchArgs, "__match_args__") \
    v(dModule, "__module__") \
    v(dMro, "__mro__") \
    v(dName, "__name__") \
    v(dNotes, "__notes__") \
    v(dObjClass, "__objclass__") \
    v(dOrigBases, "__orig_bases__") \
    v(dPackage, "__package__") \
    v(dPath, "__path__") \
    v(dQualname, "__qualname__") \
    v(dReduce, "__reduce__") \
    v(dReduceEx, "__reduce_ex__") \
    v(dSelf, "__self__") \
    v(dSetState, "__setstate__") \
    v(dSlots, "__slots__") \
    v(dSpec, "__spec__") \
    v(dStaticAttributes, "__static_attributes__") \
    v(dSubclassHook, "__subclasshook__") \
    v(dSuppressContext, "__suppress_context__") \
    v(dTextSignature, "__text_signature__") \
    v(dTraceback, "__traceback__") \
    v(dTypeParams, "__type_params__") \
    v(dWeakref, "__weakref__") \
    v(dWrapped, "__wrapped__") \
    v(uFrozenImportlib, "_frozen_importlib") \
    v(uInitializing, "_initializing") \
    v(empty, "") \
    v(args, "args") \
    v(builtins, "builtins") \
    v(close, "close") \
    v(copy, "copy") \
    v(encoding, "encoding") \
    v(end, "end") \
    v(errors, "errors") \
    v(file, "file") \
    v(flush, "flush") \
    v(get, "get") \
    v(items, "items") \
    v(join, "join") \
    v(key, "key") \
    v(keys, "keys") \
    v(metaclass, "metaclass") \
    v(mro, "mro") \
    v(name, "name") \
    v(obj, "obj") \
    v(path, "path") \
    v(read, "read") \
    v(readline, "readline") \
    v(reverse, "reverse") \
    v(send, "send") \
    v(sep, "sep") \
    v(stderr_, "stderr") \
    v(stdin_, "stdin") \
    v(stdout_, "stdout") \
    v(strict, "strict") \
    v(sys, "sys") \
    v(throw_, "throw") \
    v(value, "value") \
    v(values, "values") \
    v(write, "write")

enum class NameID : uint16_t {
#define PY_DEFINE_NAME(id, text) id,
    FOR_EACH_PY_SPECIAL_METHOD(PY_DEFINE_NAME)
    FOR_EACH_PY_NAME(PY_DEFINE_NAME)
#undef PY_DEFINE_NAME
    Count,
};

static constexpr unsigned numberOfNames = static_cast<unsigned>(NameID::Count);

inline constexpr ASCIILiteral textsOfNames[] = {
#define PY_NAME_TEXT(id, text) text ""_s,
    FOR_EACH_PY_SPECIAL_METHOD(PY_NAME_TEXT)
    FOR_EACH_PY_NAME(PY_NAME_TEXT)
#undef PY_NAME_TEXT
};
static_assert(std::size(textsOfNames) == numberOfNames);

// What is known of a name in a static table when Bun is built: which of the names above it is, and the hash of its text, with
// which the table of atoms is asked for any other. See Runtime::nameIn().
struct KnownOfName {
    consteval KnownOfName(ASCIILiteral text)
        : hash(StringHasher::computeLiteralHashAndMaskTop8Bits(text))
    {
        for (unsigned i = 0; i < numberOfNames && id == NameID::Count; ++i) {
            if (textsOfNames[i].length() != text.length())
                continue;
            size_t same = 0;
            while (same < text.length() && textsOfNames[i][same] == text[same])
                ++same;
            if (same == text.length())
                id = static_cast<NameID>(i);
        }
    }

    // For a table that is made while the program runs, where to go through the names above would cost more than it saves.
    enum HashOnlyTag { HashOnly };
    KnownOfName(ASCIILiteral text, HashOnlyTag)
        : hash(StringHasher::computeLiteralHashAndMaskTop8Bits(text))
    {
    }

    unsigned hash;
    NameID id { NameID::Count };
};

} // namespace Python
} // namespace JSC
