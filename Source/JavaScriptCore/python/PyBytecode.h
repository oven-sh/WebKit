/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stdint.h>

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

// A generator is laid out so that op_get_internal_field and op_put_internal_field reach these. (Field 0 is its type.)
// ---- What the compiler tells whoever traces the program: PythonMark, in UnlinkedCodeBlock.h ----
//
// Line: `value` is lineMarkValue().
// NoLine: with `value` 0, what follows was not written by the program. With hiddenJumpMark, this says nothing about lines: the jump that
//     comes next is not one the program makes.
// Start: where the program's own code begins, after the frame has been set up.
// BlockBegin, BlockEnd: `value` numbers the block, `bits` is a TracedBlock.

constexpr unsigned hiddenJumpMark = 1;

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
        With, // __exit__,
        ExceptionHandler, // the exception that was being handled before the one that is now.
        ExceptionHandlerWithMore, // The same, and there is more in other registers: `except*`, `finally` on the way of an exception.
        Other,
    };
    static constexpr uint64_t encode(Kind kind, int registerOffset) { return kind | static_cast<uint64_t>(static_cast<uint32_t>(registerOffset)) << 8; }
    static constexpr Kind kindOf(uint64_t bits) { return static_cast<Kind>(bits & 0xff); }
    static constexpr int registerOffsetOf(uint64_t bits) { return static_cast<int32_t>(static_cast<uint32_t>(bits >> 8)); }
};

static constexpr unsigned generatorStateField = 1; // As JSGenerator::Field::State.
static constexpr unsigned generatorBodyField = 2; // The function to call to resume it.
static constexpr unsigned generatorFrameField = 3; // The environment its registers are kept in while it is suspended.
static constexpr unsigned generatorArgumentsField = 4; // Until it starts: a tuple, the values of the parameters.
static constexpr unsigned numberOfGeneratorFields = 4;

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
    v(MakeGenerator, -1) /* for a generator expression: the iterator of its first iterable */ \
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
    v(LoadSpecial, 2) /* object, SpecialMethodForWith: bound */ \
    v(WithExit, 2) /* __exit__, exception */ \
    /* Generators and coroutines */ \
    v(GetYieldFromIter, 1) \
    v(GetAwaitable, 2) /* object, where: 0, or 1 after __aenter__, 2 after __aexit__ */ \
    v(GetAIter, 1) \
    v(GetANext, 1) \
    v(EndAsyncFor, 1) /* exception: swallows StopAsyncIteration, throws anything else */ \
    v(Send, 3) /* iterator, value, resume mode: what it yields, or empty when it is done */ \
    v(TakeSendResult, 1) /* iterator, for whoever traces the program */ \
    v(AsyncGenWrap, 1) \
    v(ThrowIntoGenerator, 1) \
    v(KeepVariables, -1) /* does nothing with them: see Unit::keepVariablesAcrossYield() */ \
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
    v(SetupAnnotations, 1) /* namespace */

enum class RuntimeFunction : uint8_t {
#define PY_DEFINE_RUNTIME_FUNCTION(name, argc) name,
    FOR_EACH_PY_RUNTIME_FUNCTION(PY_DEFINE_RUNTIME_FUNCTION)
#undef PY_DEFINE_RUNTIME_FUNCTION
};

// RuntimeFunction::LoadSpecial
enum class SpecialMethodForWith : uint8_t { Enter, Exit, AEnter, AExit };

// RuntimeFunction::ImportName
enum class ImportMode : uint8_t {
    Default, // Lazy if the program asked for all imports that can be lazy to be.
    Lazy, // `lazy import`
    Eager, // In a function, a class or a `try`, or `from m import *`.
};

} // namespace Python
} // namespace JSC
