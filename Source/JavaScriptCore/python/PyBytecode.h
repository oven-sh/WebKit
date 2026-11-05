/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "VirtualRegister.h"
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

// An instruction that comes to call a function written in Python (__add__, __getitem__, the getter of a property) makes the frame and calls it itself, so that no
// frame of C++ is on the stack meanwhile. The frame goes below the registers of the caller: the stack grows for the call and shrinks
// after it, as for the getters that JSC's inline caches call (InlineCacheCompiler.cpp, "we temporarily grow the stack"). Only the
// interpreter makes such a frame (LowLevelInterpreter64.asm, "Calls that instructions make").
// How large it is, in registers, with the header, if it has so many registers from the first argument on. Even: the stack stays aligned.
constexpr unsigned sizeOfFrameMadeByInstruction(unsigned registerCount) { return (5 + 1 + registerCount + 1) & ~1u; }
// An operator may take several calls (__iadd__, __add__, __radd__), and what it goes by it keeps behind the arguments: how far it has
// got, and its operands, for a method can give the variables they came from other values. A frame has room for all the parameters,
// given or not: this one for two more than the operator passes (def __add__(self, other, context=None)).
constexpr unsigned parametersOperatorMethodMayHaveBeyondOperands = 2;
// Counted from the first argument.
namespace RegisterOfOperator {
enum : unsigned {
    Progress = 2 + parametersOperatorMethodMayHaveBeyondOperands,
    Left,
    Right,
    End,
};
}
// How far: what has been asked so far, for a + b, a += b
namespace OperatorProgress {
enum : unsigned {
    Nothing,
    InPlace, // __iadd__
    ReflectedFirst, // __radd__, and __add__ is still to be asked.
    Left, // __add__, and __radd__ is still to be asked.
    Everything,
    IsReflected = 8, // Added to these for whoever makes the call: the method is one of the right operand, method(b, a).
};
}
// and for a < b.
namespace ComparisonProgress {
enum : unsigned {
    Nothing,
    ReflectedFirst, // __gt__ of b, before __lt__ of a
    Left, // __lt__ of a
    Everything,
    IsReflected = OperatorProgress::IsReflected,
};
}
// The instruction that calls the getter of a property keeps the object behind the argument: an AttributeError out of the getter is told
// which object it was.
namespace RegisterOfGetter {
enum : unsigned {
    Object = 1,
    End,
};
}
// Those instructions, with the number of registers of the frame from the first argument on.
#define FOR_EACH_PYTHON_OPCODE_THAT_CALLS_METHODS(macro) \
    macro(op_py_binary, RegisterOfOperator::End) \
    macro(op_py_add, RegisterOfOperator::End) \
    macro(op_py_sub, RegisterOfOperator::End) \
    macro(op_py_mul, RegisterOfOperator::End) \
    macro(op_py_div, RegisterOfOperator::End) \
    macro(op_py_floor_div, RegisterOfOperator::End) \
    macro(op_py_mod, RegisterOfOperator::End) \
    macro(op_py_eq, RegisterOfOperator::End) \
    macro(op_py_ne, RegisterOfOperator::End) \
    macro(op_py_lt, RegisterOfOperator::End) \
    macro(op_py_le, RegisterOfOperator::End) \
    macro(op_py_gt, RegisterOfOperator::End) \
    macro(op_py_ge, RegisterOfOperator::End) \
    macro(op_py_jeq, RegisterOfOperator::End) \
    macro(op_py_jne, RegisterOfOperator::End) \
    macro(op_py_jlt, RegisterOfOperator::End) \
    macro(op_py_jle, RegisterOfOperator::End) \
    macro(op_py_jgt, RegisterOfOperator::End) \
    macro(op_py_jge, RegisterOfOperator::End) \
    macro(op_py_jneq, RegisterOfOperator::End) \
    macro(op_py_jnne, RegisterOfOperator::End) \
    macro(op_py_jnlt, RegisterOfOperator::End) \
    macro(op_py_jnle, RegisterOfOperator::End) \
    macro(op_py_jngt, RegisterOfOperator::End) \
    macro(op_py_jnge, RegisterOfOperator::End) \
    macro(op_py_get_attr, RegisterOfGetter::End) \
    macro(op_py_get_item, 2)

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
//     an `async for` loop has an item, or an exception in place of one: the two ways of a branch. afterNoLineMark: in a handler, CPython has
//     instructions of no line before what comes next, so that the line which begins there is told of even if the exception comes from it.
// Start: where the program's own code begins, after the frame has been set up.
// BlockBegin, BlockEnd: `value` numbers the block, `bits` is a TracedBlock.

constexpr unsigned hiddenJumpMark = 1;
constexpr unsigned asyncForGoesOnMark = 2;
constexpr unsigned asyncForEndsMark = 3;
constexpr unsigned afterNoLineMark = 4;

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
    // `ownsIterator`: see BlockPiece.
    static constexpr uint64_t encode(Kind kind, int registerOffset, bool ownsIterator) { return kind | ownsIterator << 7 | static_cast<uint64_t>(static_cast<uint32_t>(registerOffset)) << 8; }
    static constexpr Kind kindOf(uint64_t bits) { return static_cast<Kind>(bits & 0x7f); }
    static constexpr bool ownsIterator(uint64_t bits) { return bits & 0x80; }
    static constexpr int registerOffsetOf(uint64_t bits) { return static_cast<int32_t>(static_cast<uint32_t>(bits >> 8)); }
};

// A piece of the code of such a block. What the block holds is in use wherever the block is open, even where no instruction that
// reads the register can be reached any more. A trace function may send the frame back to where one can (Monitoring::jump()). And
// if an exception leaves a `for` loop that goes through the value of a call (`ownsIterator`), the unwinder reads the iterator
// (Runtime::releaseIteratorsOfLoopsLeft()). So whoever asks which registers are live has to add these.
struct BlockPiece {
    unsigned begin; // Offsets of instructions.
    unsigned end;
    unsigned number; // The same for all the pieces of a block.
    int held; // VirtualRegister::offset()
    bool holdsNextToo; // TracedBlock::With
    bool ownsIterator;
    bool contains(unsigned offset) const { return begin <= offset && offset < end; }
    template<typename Function>
    void forEachRegister(const Function& function) const
    {
        function(VirtualRegister(held));
        if (holdsNextToo)
            function(VirtualRegister(held - 1));
    }
};

// In the order of the code: of two that contain the same instruction, that of the inner block comes later.
template<typename Marks>
Vector<BlockPiece, 4> blockPiecesIn(const Marks& marks)
{
    using Mark = std::remove_cvref_t<decltype(marks[0])>;
    Vector<BlockPiece, 4> pieces;
    for (auto& mark : marks) {
        if ((mark.kind() != Mark::BlockBegin && mark.kind() != Mark::BlockEnd) || !VirtualRegister(TracedBlock::registerOffsetOf(mark.bits)).isValid())
            continue;
        if (mark.kind() == Mark::BlockBegin) {
            pieces.append({ mark.offset, std::numeric_limits<unsigned>::max(), mark.value(), TracedBlock::registerOffsetOf(mark.bits), TracedBlock::kindOf(mark.bits) == TracedBlock::With, TracedBlock::ownsIterator(mark.bits) });
            continue;
        }
        for (size_t i = pieces.size(); i--;) {
            if (pieces[i].number == mark.value()) {
                pieces[i].end = mark.offset;
                break;
            }
        }
    }
    return pieces;
}

// PythonMark::TemporaryMade, TemporaryUsed: see runtime/PyTemporaries.cpp. The value of a call or a subscript that is in no variable,
// and what is done with it, once.
struct TemporaryUse {
    enum Kind : uint8_t {
        Receiver, // A method of it is called. The register is a constant, the name of the method.
        SubscriptBase, // It is subscripted.
        Argument, // It is passed to what is in the register, which was there before the value was made.
        StoredValue, // It is stored in an item of what is in the register, a variable. No code runs between making it and that.
        ForHeader, // A `for` loop goes through it, `in` searches it, or it is taken apart (a, b = g()).
        KeywordArgument, // As Argument.
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

// RuntimeFunction::MakeFunction. Its first argument is the index of the code, shifted, and which of these follow it, in this order.
enum MakeFunctionPart : unsigned { HasDefaults = 1, HasKwDefaults = 2, HasAnnotate = 4 };
constexpr unsigned makeFunctionIndexShift = 3;

// op_py_runtime. name, number of arguments (-1: any). "empty" is the empty JSValue, for what is not there.
#define FOR_EACH_PY_RUNTIME_FUNCTION(v) \
    /* Building */ \
    v(BuildTuple, -1) \
    v(BuildSet, -1) \
    v(BuildMap, -1) /* key, value, ... */ \
    v(BuildString, -1) \
    v(BuildSlice, 3) \
    v(ListExtend, 2) \
    v(ListToTuple, 1) \
    v(SetAdd, 2) \
    v(SetUpdate, 2) \
    v(MapAdd, 3) \
    v(DictUpdate, 2) \
    v(DictMerge, 3) /* dict, mapping, the function being called */ \
    v(FormatValue, 3) /* value, conversion (0, 's', 'r' or 'a'), format spec or empty */ \
    v(FormatSimple, 1) /* value: no conversion, no format spec */ \
    v(ConvertValue, 2) /* value, conversion ('s', 'r' or 'a') */ \
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
    /* super(type, object).name: `super`, type, object, name. Zero: it is written super(). Method: the attribute is to be called, and what is to be */ \
    /* passed first, or the empty value, takes the place of the object. See loadSuperAttribute(). */ \
    v(LoadSuperAttr, 4) \
    v(LoadZeroSuperAttr, 4) \
    v(LoadSuperMethod, 4) \
    v(LoadZeroSuperMethod, 4) \
    v(LoadBuildClass, 0) \
    /* Functions */ \
    v(MakeFunction, -1) /* index and MakeFunctionParts, those parts, the cells of the closure... */ \
    v(SetFunctionTypeParams, 2) /* function, type params: the function */ \
    v(MakeGenerator, -1) /* how many slots it needs; for a generator expression: the iterator of its first iterable */ \
    v(CallArguments, 3) /* function, args, kwargs or empty: an array of the arguments, and sets aside the names of the keywords */ \
    v(TakeKeywordNames, 0) \
    /* Imports */ \
    v(ImportName, 4) /* name, fromlist, level, ImportMode */ \
    v(ImportFrom, 2) /* module, name */ \
    v(ImportStar, 2) /* module, namespace */ \
    /* Exceptions */ \
    v(Raise, 3) /* exception or empty; cause or empty; if the exception is what a call gave, with nothing in between: what was called */ \
    v(Reraise, 1) \
    v(ReraiseOutOfHandler, 2) /* exception, what was being handled before: that is put back first */ \
    v(ReraiseUnseen, 2) /* the same, at the end of a handler that CPython's code has not: tools hear nothing of it */ \
    v(ReraiseGroup, 1) /* at the end of `except*`: the frame is not shown where the exception was first raised */ \
    v(CatchException, 1) /* What was thrown, as a Python exception. */ \
    v(SetHandledException, 1) /* exception or empty: the one handled before, or empty */ \
    v(LeaveHandler, 1) /* what was being handled before the handler began, or empty */ \
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

// op_py_catch
enum class CatchMode : uint8_t {
    Thrown, // What was thrown, as it is: the handler tidies up and raises it again.
    Handled,
    // The same, and nothing in the handler names the exception, hands it on or can be suspended: see "Entries of tracebacks that are only
    // noted" in PyRuntime.h.
    HandledUnseen,
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
