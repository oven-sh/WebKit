/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>
#include <wtf/Vector.h>

namespace JSC {
namespace Python {

// What the compiler and the interpreter's slow paths agree on: the immediate operands of the op_py_* instructions.

// op_py_load_cell, RuntimeFunction::DeleteCell: what to say about a variable without a value.
enum class UnboundKind : uint8_t {
    Local, // UnboundLocalError: cannot access local variable 'x' where it is not associated with a value
    Free, // NameError: cannot access free variable 'x' where it is not associated with a value in enclosing scope
};

// op_py_binary. In the order of ast.operator; the in-place forms follow.
enum class BinaryOperator : uint8_t {
    Add,
    Sub,
    Mult,
    MatMult,
    Div,
    Mod,
    Pow,
    LShift,
    RShift,
    BitOr,
    BitXor,
    BitAnd,
    FloorDiv,
    FirstInPlace,
    InPlaceAdd = FirstInPlace,
    InPlaceSub,
    InPlaceMult,
    InPlaceMatMult,
    InPlaceDiv,
    InPlaceMod,
    InPlacePow,
    InPlaceLShift,
    InPlaceRShift,
    InPlaceBitOr,
    InPlaceBitXor,
    InPlaceBitAnd,
    InPlaceFloorDiv,
};

// op_py_unary, in the order of ast.unaryop.
enum class UnaryOperator : uint8_t { Invert, Not, Positive, Negative };

// op_py_compare. The first ten in the order of ast.cmpop.
enum class CompareOperator : uint8_t {
    Eq,
    NotEq,
    Lt,
    LtE,
    Gt,
    GtE,
    Is,
    IsNot,
    In,
    NotIn,
    ExceptionMatch, // lhs: an exception; rhs: what follows `except`.
};

// ---- What the compiler tells whoever traces the program: PythonMark, in UnlinkedCodeBlock.h ----
//
// Line: `value` is lineMarkValue().
// NoLine: with `value` 0, what follows was not written by the program. With another, this says nothing about lines. hiddenJumpMark: the
//     jump that comes next is not one the program makes. asyncForGoesOnMark, asyncForEndsMark: what comes next does nothing, and is where
//     an `async for` loop has an item, or an exception in place of one: the two ways of a branch.
// Start: where the program's own code begins, after the frame has been set up.
// BlockBegin, BlockEnd: `value` numbers the block, `bits` is a TracedBlock.

constexpr unsigned hiddenJumpMark = 1;
constexpr unsigned asyncForGoesOnMark = 2;
constexpr unsigned asyncForEndsMark = 3;

// A statement begins: nothing is in a temporary register but what belongs to the blocks it is in.
constexpr unsigned lineMarkValue(int line, bool isStatementStart, bool isSilent = false) { return static_cast<unsigned>(line) << 2 | isSilent << 1 | isStatementStart; }
constexpr int lineOfMarkValue(unsigned value) { return static_cast<int>(value >> 2); }
constexpr bool isStatementStart(unsigned value) { return value & 1; }
// What follows is of the line, and to get here is not to begin it: nobody is told.
constexpr bool isSilentLine(unsigned value) { return value & 2; }

// A part of the code that has something in a temporary register from beginning to end, so that it cannot be entered but from the top.
struct TracedBlock {
    enum Kind : uint8_t {
        ForLoop, // The register holds the iterator,
        With, // __exit__, and the next register what to pass it first, or the empty value,
        ExceptionHandler, // the exception that was being handled before the one that is now.
        ExceptionHandlerWithMore, // The same, and there is more in other registers: `except*`, `finally` on the way of an exception.
        Other,
    };
    // `ownsIterator`: see OwnedLoop.
    static constexpr uint64_t encode(Kind kind, int registerOffset, bool ownsIterator) { return kind | ownsIterator << 7 | static_cast<uint64_t>(static_cast<uint32_t>(registerOffset)) << 8; }
    static constexpr Kind kindOf(uint64_t bits) { return static_cast<Kind>(bits & 0x7f); }
    static constexpr bool ownsIterator(uint64_t bits) { return bits & 0x80; }
    static constexpr int registerOffsetOf(uint64_t bits) { return static_cast<int32_t>(static_cast<uint32_t>(bits >> 8)); }
};

// A piece of the code of a `for` loop that goes through the value of a call. If an exception leaves the loop, the unwinder reads the
// iterator from its register (Runtime::releaseIteratorsOfLoopsLeft()). No instruction shows that use, so whoever asks which
// registers are live has to add the register wherever the loop is open.
struct OwnedLoop {
    unsigned begin; // Offsets of instructions.
    unsigned end;
    unsigned number; // The same for all the pieces of a loop.
    int iterator; // VirtualRegister::offset()
    bool contains(unsigned offset) const { return begin <= offset && offset < end; }
};

// In the order of the code: of two that contain the same instruction, the inner loop comes later.
template<typename Marks>
Vector<OwnedLoop, 4> ownedLoopsIn(const Marks& marks)
{
    using Mark = std::remove_cvref_t<decltype(marks[0])>;
    Vector<OwnedLoop, 4> loops;
    for (auto& mark : marks) {
        if ((mark.kind() != Mark::BlockBegin && mark.kind() != Mark::BlockEnd) || !TracedBlock::ownsIterator(mark.bits))
            continue;
        if (mark.kind() == Mark::BlockBegin) {
            loops.append({ mark.offset, std::numeric_limits<unsigned>::max(), mark.value(), TracedBlock::registerOffsetOf(mark.bits) });
            continue;
        }
        for (size_t i = loops.size(); i--;) {
            if (loops[i].number == mark.value()) {
                loops[i].end = mark.offset;
                break;
            }
        }
    }
    return loops;
}

// PythonMark::TemporaryMade, TemporaryUsed: see runtime/PyTemporaries.cpp. The value of a call or a subscript that is in no variable,
// and what is done with it, once.
struct TemporaryUse {
    enum Kind : uint8_t {
        Receiver, // A method of it is called. The register is a constant, the name of the method.
        SubscriptBase, // It is subscripted.
        Argument, // It is passed to what is in the register, which was there before the value was made.
        StoredValue, // It is stored in an item of what is in the register, a variable. No code runs between making it and that.
        ForHeader, // A `for` loop goes through it.
    };
    // `isLast`: no other mark for this value follows.
    static constexpr uint64_t encode(Kind kind, int registerOffset, bool isLast) { return kind | isLast << 7 | static_cast<uint64_t>(static_cast<uint32_t>(registerOffset)) << 8; }
    static constexpr Kind kindOf(uint64_t bits) { return static_cast<Kind>(bits & 0x7f); }
    static constexpr bool isLast(uint64_t bits) { return bits & 0x80; }
    static constexpr int registerOffsetOf(uint64_t bits) { return static_cast<int32_t>(static_cast<uint32_t>(bits >> 8)); }
};

// op_py_send. The mode is a JSGenerator::ResumeMode, or this plus 0, or 1 after __aenter__, or 2 after __aexit__: `iterator` holds
// what `await` was given, which the instruction replaces by what to iterate over, and then sends None to.
constexpr int firstSendOfAwait = 8;

// op_py_runtime. name, number of arguments (-1: any). "empty" is the empty JSValue, for what is not there.
#define FOR_EACH_PY_RUNTIME_FUNCTION(v) \
    /* Building */ \
    v(BuildTuple, -1) \
    v(BuildSet, -1) \
    v(BuildMap, -1) /* key, value, ... */ \
    v(BuildString, -1) \
    v(BuildSlice, 3) \
    v(ListAppend, 2) \
    v(ListExtend, 2) \
    v(ListToTuple, 1) \
    v(SetAdd, 2) \
    v(SetUpdate, 2) \
    v(MapAdd, 3) \
    v(DictUpdate, 2) \
    v(DictMerge, 3) /* dict, mapping, the function being called */ \
    v(FormatValue, 3) /* value, conversion (0, 's', 'r' or 'a'), format spec or empty */ \
    v(BuildInterpolation, 4) /* value, str, conversion, format spec or empty */ \
    v(BuildTemplate, 2) /* strings, interpolations */ \
    /* Names */ \
    v(DeleteName, 2) /* namespace, name */ \
    v(DeleteGlobal, 1) \
    v(DeleteCell, 3) /* cell, name, UnboundKind */ \
    v(DeleteAttr, 2) \
    v(DeleteItem, 2) \
    v(LoadFromDictOrGlobals, 2) /* mapping, name */ \
    v(LoadFromDictOrCell, 3) /* mapping, name, cell */ \
    v(MakeCell, 1) /* value or empty */ \
    v(LoadClosure, 1) /* index */ \
    v(LoadBuildClass, 0) \
    /* Functions */ \
    v(MakeFunction, -1) /* index, defaults or empty, kwdefaults or empty, annotate or empty, the cells of the closure... */ \
    v(SetFunctionTypeParams, 2) /* function, type params: the function */ \
    v(MakeGenerator, -1) /* how many slots it needs; for a generator expression: the iterator of its first iterable */ \
    v(CallArguments, 3) /* function, args, kwargs or empty: an array of the arguments, and sets aside the names of the keywords */ \
    v(TakeKeywordNames, 0) \
    /* Imports */ \
    v(ImportName, 4) /* name, fromlist, level, ImportMode */ \
    v(ImportFrom, 2) /* module, name */ \
    v(ImportStar, 2) /* module, namespace */ \
    /* Exceptions */ \
    v(Raise, 2) /* exception or empty, cause or empty */ \
    v(Reraise, 1) \
    v(CatchException, 1) /* What was thrown, as a Python exception. */ \
    v(SetHandledException, 1) /* exception or empty: the one handled before, or empty */ \
    v(MatchExceptionGroup, 2) /* exception, type: the match or None; the rest is set aside */ \
    v(TakeExceptionGroupRest, 0) \
    v(PrepareReraiseStar, 2) /* the original, a list of what was raised: what to raise, or None */ \
    /* with */ \
    v(WithExit, 3) /* __exit__, what to pass it first or empty, exception */ \
    /* Generators and coroutines */ \
    v(GetYieldFromIter, 1) \
    v(GetAIterOfLoop, 1) \
    v(LeaveAsyncFor, 2) /* iterator, exception or empty: what to await, or None */ \
    v(DropGenerator, 1) /* See generatorIsDropped(). */ \
    v(GetANext, 1) \
    v(EndAsyncFor, 2) /* exception, iterator: swallows StopAsyncIteration, throws anything else */ \
    v(AsyncGenWrap, 1) \
    v(ThrowIntoGenerator, 1) \
    /* match */ \
    v(MatchSequence, 1) \
    v(MatchMapping, 1) \
    v(MatchKeys, 2) /* subject, keys: a tuple of the values, or None */ \
    v(MatchClass, 4) /* subject, class, number of positional patterns, names: a tuple of attributes, or None */ \
    v(GetLength, 1) \
    v(CopyDictWithoutKeys, 2) \
    /* Typing */ \
    v(TypeVar, 1) \
    v(TypeVarWithBound, 2) \
    v(TypeVarWithConstraints, 2) \
    v(ParamSpec, 1) \
    v(TypeVarTuple, 1) \
    v(SetTypeParamDefault, 2) \
    v(SubscriptGeneric, 1) \
    v(TypeAlias, 3) /* name, type params or None, a function that computes the value */ \
    /* Other */ \
    v(PrintExpression, 1) \
    v(SetupAnnotations, 1) /* namespace */ \
    v(ReleaseIterator, 1) /* the iterator of a `for` loop that is left before its end */

enum class RuntimeFunction : uint8_t {
#define PY_DEFINE_RUNTIME_FUNCTION(name, argc) name,
    FOR_EACH_PY_RUNTIME_FUNCTION(PY_DEFINE_RUNTIME_FUNCTION)
#undef PY_DEFINE_RUNTIME_FUNCTION
};

// RuntimeFunction::LoadSpecial
// As the `name` of op_py_get_method, in place of a str: the method is looked for as `with` does it, in the class alone.
enum class SpecialMethodForWith : uint8_t { Enter, Exit, AEnter, AExit };

// RuntimeFunction::ImportName
enum class ImportMode : uint8_t {
    Default, // Lazy if the program asked for all imports that can be lazy to be.
    Lazy, // `lazy import`
    Eager, // In a function, a class or a `try`, or `from m import *`.
};

} // namespace Python
} // namespace JSC
