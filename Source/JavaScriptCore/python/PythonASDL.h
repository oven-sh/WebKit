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

#pragma once

#include <span>
#include <wtf/text/ASCIILiteral.h>

namespace JSC { namespace Python {

// The classes of the syntax tree as a program sees it, in the module _ast: CPython's Parser/Python.asdl, which this follows line for line. What the parser makes is in PythonAST.h, and is not quite
// the same: it has one kind of node for `def` and `async def`, and so on. PythonASTWalker.h goes through the one as if it were the other.

#define FOR_EACH_PYTHON_AST_CLASS(v) \
    v(mod) v(Module) v(Interactive) v(Expression) v(FunctionType) v(stmt) v(FunctionDef) v(AsyncFunctionDef) v(ClassDef) v(Return) v(Delete) v(Assign) v(TypeAlias) v(AugAssign) v(AnnAssign) \
    v(For) v(AsyncFor) v(While) v(If) v(With) v(AsyncWith) v(Match) v(Raise) v(Try) v(TryStar) v(Assert) v(Import) v(ImportFrom) v(Global) v(Nonlocal) v(Expr) v(Pass) v(Break) v(Continue) \
    v(expr) v(BoolOp) v(NamedExpr) v(BinOp) v(UnaryOp) v(Lambda) v(IfExp) v(Dict) v(Set) v(ListComp) v(SetComp) v(DictComp) v(GeneratorExp) v(Await) v(Yield) v(YieldFrom) v(Compare) v(Call) \
    v(FormattedValue) v(Interpolation) v(JoinedStr) v(TemplateStr) v(Constant) v(Attribute) v(Subscript) v(Starred) v(Name) v(List) v(Tuple) v(Slice) v(expr_context) v(Load) v(Store) v(Del) \
    v(boolop) v(And) v(Or) v(operator_) v(Add) v(Sub) v(Mult) v(MatMult) v(Div) v(Mod) v(Pow) v(LShift) v(RShift) v(BitOr) v(BitXor) v(BitAnd) v(FloorDiv) v(unaryop) v(Invert) v(Not) v(UAdd) \
    v(USub) v(cmpop) v(Eq) v(NotEq) v(Lt) v(LtE) v(Gt) v(GtE) v(Is) v(IsNot) v(In) v(NotIn) v(comprehension) v(excepthandler) v(ExceptHandler) v(arguments) v(arg) v(keyword) v(alias) \
    v(withitem) v(match_case) v(pattern) v(MatchValue) v(MatchSingleton) v(MatchSequence) v(MatchMapping) v(MatchClass) v(MatchStar) v(MatchAs) v(MatchOr) v(type_ignore) v(TypeIgnore) \
    v(type_param) v(TypeVar) v(ParamSpec) v(TypeVarTuple)

enum class ASTClass : uint8_t {
    AST,
#define DECLARE(name) name,
    FOR_EACH_PYTHON_AST_CLASS(DECLARE)
#undef DECLARE
    // What a field can be besides a node. There are no classes for these.
    identifier,
    int_,
    string,
    constant,
};
static constexpr unsigned numberOfASTClasses = static_cast<unsigned>(ASTClass::identifier);

struct ASDLField {
    enum class Quantifier : uint8_t {
        One,
        Optional, // expr?
        Sequence, // expr*
        SequenceOfOptional, // expr?*
    };

    ASTClass type;
    Quantifier quantifier;
    ASCIILiteral name;
};

struct ASDLClass {
    enum class Kind : uint8_t {
        Sum, // stmt = FunctionDef(...) | ..., which nothing is an instance of but by being one of those
        Constructor, // One of those.
        Product, // arguments = (...)
    };

    Kind kind;
    ASCIILiteral name;
    ASTClass base;
    std::span<const ASDLField> fields;
    // lineno and so on. What a sum has, all that it is the sum of have.
    std::span<const ASDLField> attributes;
};

namespace ASDL {

using enum ASDLField::Quantifier;

inline constexpr ASDLField fieldsOf_Module[] = { { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::type_ignore, Sequence, "type_ignores"_s } };
inline constexpr ASDLField fieldsOf_Interactive[] = { { ASTClass::stmt, Sequence, "body"_s } };
inline constexpr ASDLField fieldsOf_Expression[] = { { ASTClass::expr, One, "body"_s } };
inline constexpr ASDLField fieldsOf_FunctionType[] = { { ASTClass::expr, Sequence, "argtypes"_s }, { ASTClass::expr, One, "returns"_s } };
inline constexpr ASDLField attributesOf_stmt[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_FunctionDef[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::arguments, One, "args"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::expr, Sequence, "decorator_list"_s }, { ASTClass::expr, Optional, "returns"_s }, { ASTClass::string, Optional, "type_comment"_s }, { ASTClass::type_param, Sequence, "type_params"_s } };
inline constexpr ASDLField fieldsOf_AsyncFunctionDef[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::arguments, One, "args"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::expr, Sequence, "decorator_list"_s }, { ASTClass::expr, Optional, "returns"_s }, { ASTClass::string, Optional, "type_comment"_s }, { ASTClass::type_param, Sequence, "type_params"_s } };
inline constexpr ASDLField fieldsOf_ClassDef[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::expr, Sequence, "bases"_s }, { ASTClass::keyword, Sequence, "keywords"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::expr, Sequence, "decorator_list"_s }, { ASTClass::type_param, Sequence, "type_params"_s } };
inline constexpr ASDLField fieldsOf_Return[] = { { ASTClass::expr, Optional, "value"_s } };
inline constexpr ASDLField fieldsOf_Delete[] = { { ASTClass::expr, Sequence, "targets"_s } };
inline constexpr ASDLField fieldsOf_Assign[] = { { ASTClass::expr, Sequence, "targets"_s }, { ASTClass::expr, One, "value"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField fieldsOf_TypeAlias[] = { { ASTClass::expr, One, "name"_s }, { ASTClass::type_param, Sequence, "type_params"_s }, { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_AugAssign[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::operator_, One, "op"_s }, { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_AnnAssign[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::expr, One, "annotation"_s }, { ASTClass::expr, Optional, "value"_s }, { ASTClass::int_, One, "simple"_s } };
inline constexpr ASDLField fieldsOf_For[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::expr, One, "iter"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::stmt, Sequence, "orelse"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField fieldsOf_AsyncFor[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::expr, One, "iter"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::stmt, Sequence, "orelse"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField fieldsOf_While[] = { { ASTClass::expr, One, "test"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::stmt, Sequence, "orelse"_s } };
inline constexpr ASDLField fieldsOf_If[] = { { ASTClass::expr, One, "test"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::stmt, Sequence, "orelse"_s } };
inline constexpr ASDLField fieldsOf_With[] = { { ASTClass::withitem, Sequence, "items"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField fieldsOf_AsyncWith[] = { { ASTClass::withitem, Sequence, "items"_s }, { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField fieldsOf_Match[] = { { ASTClass::expr, One, "subject"_s }, { ASTClass::match_case, Sequence, "cases"_s } };
inline constexpr ASDLField fieldsOf_Raise[] = { { ASTClass::expr, Optional, "exc"_s }, { ASTClass::expr, Optional, "cause"_s } };
inline constexpr ASDLField fieldsOf_Try[] = { { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::excepthandler, Sequence, "handlers"_s }, { ASTClass::stmt, Sequence, "orelse"_s }, { ASTClass::stmt, Sequence, "finalbody"_s } };
inline constexpr ASDLField fieldsOf_TryStar[] = { { ASTClass::stmt, Sequence, "body"_s }, { ASTClass::excepthandler, Sequence, "handlers"_s }, { ASTClass::stmt, Sequence, "orelse"_s }, { ASTClass::stmt, Sequence, "finalbody"_s } };
inline constexpr ASDLField fieldsOf_Assert[] = { { ASTClass::expr, One, "test"_s }, { ASTClass::expr, Optional, "msg"_s } };
inline constexpr ASDLField fieldsOf_Import[] = { { ASTClass::alias, Sequence, "names"_s } };
inline constexpr ASDLField fieldsOf_ImportFrom[] = { { ASTClass::identifier, Optional, "module"_s }, { ASTClass::alias, Sequence, "names"_s }, { ASTClass::int_, Optional, "level"_s } };
inline constexpr ASDLField fieldsOf_Global[] = { { ASTClass::identifier, Sequence, "names"_s } };
inline constexpr ASDLField fieldsOf_Nonlocal[] = { { ASTClass::identifier, Sequence, "names"_s } };
inline constexpr ASDLField fieldsOf_Expr[] = { { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField attributesOf_expr[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_BoolOp[] = { { ASTClass::boolop, One, "op"_s }, { ASTClass::expr, Sequence, "values"_s } };
inline constexpr ASDLField fieldsOf_NamedExpr[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_BinOp[] = { { ASTClass::expr, One, "left"_s }, { ASTClass::operator_, One, "op"_s }, { ASTClass::expr, One, "right"_s } };
inline constexpr ASDLField fieldsOf_UnaryOp[] = { { ASTClass::unaryop, One, "op"_s }, { ASTClass::expr, One, "operand"_s } };
inline constexpr ASDLField fieldsOf_Lambda[] = { { ASTClass::arguments, One, "args"_s }, { ASTClass::expr, One, "body"_s } };
inline constexpr ASDLField fieldsOf_IfExp[] = { { ASTClass::expr, One, "test"_s }, { ASTClass::expr, One, "body"_s }, { ASTClass::expr, One, "orelse"_s } };
inline constexpr ASDLField fieldsOf_Dict[] = { { ASTClass::expr, SequenceOfOptional, "keys"_s }, { ASTClass::expr, Sequence, "values"_s } };
inline constexpr ASDLField fieldsOf_Set[] = { { ASTClass::expr, Sequence, "elts"_s } };
inline constexpr ASDLField fieldsOf_ListComp[] = { { ASTClass::expr, One, "elt"_s }, { ASTClass::comprehension, Sequence, "generators"_s } };
inline constexpr ASDLField fieldsOf_SetComp[] = { { ASTClass::expr, One, "elt"_s }, { ASTClass::comprehension, Sequence, "generators"_s } };
inline constexpr ASDLField fieldsOf_DictComp[] = { { ASTClass::expr, One, "key"_s }, { ASTClass::expr, One, "value"_s }, { ASTClass::comprehension, Sequence, "generators"_s } };
inline constexpr ASDLField fieldsOf_GeneratorExp[] = { { ASTClass::expr, One, "elt"_s }, { ASTClass::comprehension, Sequence, "generators"_s } };
inline constexpr ASDLField fieldsOf_Await[] = { { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_Yield[] = { { ASTClass::expr, Optional, "value"_s } };
inline constexpr ASDLField fieldsOf_YieldFrom[] = { { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_Compare[] = { { ASTClass::expr, One, "left"_s }, { ASTClass::cmpop, Sequence, "ops"_s }, { ASTClass::expr, Sequence, "comparators"_s } };
inline constexpr ASDLField fieldsOf_Call[] = { { ASTClass::expr, One, "func"_s }, { ASTClass::expr, Sequence, "args"_s }, { ASTClass::keyword, Sequence, "keywords"_s } };
inline constexpr ASDLField fieldsOf_FormattedValue[] = { { ASTClass::expr, One, "value"_s }, { ASTClass::int_, One, "conversion"_s }, { ASTClass::expr, Optional, "format_spec"_s } };
inline constexpr ASDLField fieldsOf_Interpolation[] = { { ASTClass::expr, One, "value"_s }, { ASTClass::constant, One, "str"_s }, { ASTClass::int_, One, "conversion"_s }, { ASTClass::expr, Optional, "format_spec"_s } };
inline constexpr ASDLField fieldsOf_JoinedStr[] = { { ASTClass::expr, Sequence, "values"_s } };
inline constexpr ASDLField fieldsOf_TemplateStr[] = { { ASTClass::expr, Sequence, "values"_s } };
inline constexpr ASDLField fieldsOf_Constant[] = { { ASTClass::constant, One, "value"_s }, { ASTClass::string, Optional, "kind"_s } };
inline constexpr ASDLField fieldsOf_Attribute[] = { { ASTClass::expr, One, "value"_s }, { ASTClass::identifier, One, "attr"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_Subscript[] = { { ASTClass::expr, One, "value"_s }, { ASTClass::expr, One, "slice"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_Starred[] = { { ASTClass::expr, One, "value"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_Name[] = { { ASTClass::identifier, One, "id"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_List[] = { { ASTClass::expr, Sequence, "elts"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_Tuple[] = { { ASTClass::expr, Sequence, "elts"_s }, { ASTClass::expr_context, One, "ctx"_s } };
inline constexpr ASDLField fieldsOf_Slice[] = { { ASTClass::expr, Optional, "lower"_s }, { ASTClass::expr, Optional, "upper"_s }, { ASTClass::expr, Optional, "step"_s } };
inline constexpr ASDLField fieldsOf_comprehension[] = { { ASTClass::expr, One, "target"_s }, { ASTClass::expr, One, "iter"_s }, { ASTClass::expr, Sequence, "ifs"_s }, { ASTClass::int_, One, "is_async"_s } };
inline constexpr ASDLField attributesOf_excepthandler[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_ExceptHandler[] = { { ASTClass::expr, Optional, "type"_s }, { ASTClass::identifier, Optional, "name"_s }, { ASTClass::stmt, Sequence, "body"_s } };
inline constexpr ASDLField fieldsOf_arguments[] = { { ASTClass::arg, Sequence, "posonlyargs"_s }, { ASTClass::arg, Sequence, "args"_s }, { ASTClass::arg, Optional, "vararg"_s }, { ASTClass::arg, Sequence, "kwonlyargs"_s }, { ASTClass::expr, SequenceOfOptional, "kw_defaults"_s }, { ASTClass::arg, Optional, "kwarg"_s }, { ASTClass::expr, Sequence, "defaults"_s } };
inline constexpr ASDLField fieldsOf_arg[] = { { ASTClass::identifier, One, "arg"_s }, { ASTClass::expr, Optional, "annotation"_s }, { ASTClass::string, Optional, "type_comment"_s } };
inline constexpr ASDLField attributesOf_arg[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_keyword[] = { { ASTClass::identifier, Optional, "arg"_s }, { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField attributesOf_keyword[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_alias[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::identifier, Optional, "asname"_s } };
inline constexpr ASDLField attributesOf_alias[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, Optional, "end_lineno"_s }, { ASTClass::int_, Optional, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_withitem[] = { { ASTClass::expr, One, "context_expr"_s }, { ASTClass::expr, Optional, "optional_vars"_s } };
inline constexpr ASDLField fieldsOf_match_case[] = { { ASTClass::pattern, One, "pattern"_s }, { ASTClass::expr, Optional, "guard"_s }, { ASTClass::stmt, Sequence, "body"_s } };
inline constexpr ASDLField attributesOf_pattern[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, One, "end_lineno"_s }, { ASTClass::int_, One, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_MatchValue[] = { { ASTClass::expr, One, "value"_s } };
inline constexpr ASDLField fieldsOf_MatchSingleton[] = { { ASTClass::constant, One, "value"_s } };
inline constexpr ASDLField fieldsOf_MatchSequence[] = { { ASTClass::pattern, Sequence, "patterns"_s } };
inline constexpr ASDLField fieldsOf_MatchMapping[] = { { ASTClass::expr, Sequence, "keys"_s }, { ASTClass::pattern, Sequence, "patterns"_s }, { ASTClass::identifier, Optional, "rest"_s } };
inline constexpr ASDLField fieldsOf_MatchClass[] = { { ASTClass::expr, One, "cls"_s }, { ASTClass::pattern, Sequence, "patterns"_s }, { ASTClass::identifier, Sequence, "kwd_attrs"_s }, { ASTClass::pattern, Sequence, "kwd_patterns"_s } };
inline constexpr ASDLField fieldsOf_MatchStar[] = { { ASTClass::identifier, Optional, "name"_s } };
inline constexpr ASDLField fieldsOf_MatchAs[] = { { ASTClass::pattern, Optional, "pattern"_s }, { ASTClass::identifier, Optional, "name"_s } };
inline constexpr ASDLField fieldsOf_MatchOr[] = { { ASTClass::pattern, Sequence, "patterns"_s } };
inline constexpr ASDLField fieldsOf_TypeIgnore[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::string, One, "tag"_s } };
inline constexpr ASDLField attributesOf_type_param[] = { { ASTClass::int_, One, "lineno"_s }, { ASTClass::int_, One, "col_offset"_s }, { ASTClass::int_, One, "end_lineno"_s }, { ASTClass::int_, One, "end_col_offset"_s } };
inline constexpr ASDLField fieldsOf_TypeVar[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::expr, Optional, "bound"_s }, { ASTClass::expr, Optional, "default_value"_s } };
inline constexpr ASDLField fieldsOf_ParamSpec[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::expr, Optional, "default_value"_s } };
inline constexpr ASDLField fieldsOf_TypeVarTuple[] = { { ASTClass::identifier, One, "name"_s }, { ASTClass::expr, Optional, "default_value"_s } };

// In the order of ASTClass.
inline constexpr ASDLClass classes[] = {
    { ASDLClass::Kind::Sum, "AST"_s, ASTClass::AST, { }, { } },
    { ASDLClass::Kind::Sum, "mod"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Module"_s, ASTClass::mod, { fieldsOf_Module }, {  } },
    { ASDLClass::Kind::Constructor, "Interactive"_s, ASTClass::mod, { fieldsOf_Interactive }, {  } },
    { ASDLClass::Kind::Constructor, "Expression"_s, ASTClass::mod, { fieldsOf_Expression }, {  } },
    { ASDLClass::Kind::Constructor, "FunctionType"_s, ASTClass::mod, { fieldsOf_FunctionType }, {  } },
    { ASDLClass::Kind::Sum, "stmt"_s, ASTClass::AST, {  }, { attributesOf_stmt } },
    { ASDLClass::Kind::Constructor, "FunctionDef"_s, ASTClass::stmt, { fieldsOf_FunctionDef }, {  } },
    { ASDLClass::Kind::Constructor, "AsyncFunctionDef"_s, ASTClass::stmt, { fieldsOf_AsyncFunctionDef }, {  } },
    { ASDLClass::Kind::Constructor, "ClassDef"_s, ASTClass::stmt, { fieldsOf_ClassDef }, {  } },
    { ASDLClass::Kind::Constructor, "Return"_s, ASTClass::stmt, { fieldsOf_Return }, {  } },
    { ASDLClass::Kind::Constructor, "Delete"_s, ASTClass::stmt, { fieldsOf_Delete }, {  } },
    { ASDLClass::Kind::Constructor, "Assign"_s, ASTClass::stmt, { fieldsOf_Assign }, {  } },
    { ASDLClass::Kind::Constructor, "TypeAlias"_s, ASTClass::stmt, { fieldsOf_TypeAlias }, {  } },
    { ASDLClass::Kind::Constructor, "AugAssign"_s, ASTClass::stmt, { fieldsOf_AugAssign }, {  } },
    { ASDLClass::Kind::Constructor, "AnnAssign"_s, ASTClass::stmt, { fieldsOf_AnnAssign }, {  } },
    { ASDLClass::Kind::Constructor, "For"_s, ASTClass::stmt, { fieldsOf_For }, {  } },
    { ASDLClass::Kind::Constructor, "AsyncFor"_s, ASTClass::stmt, { fieldsOf_AsyncFor }, {  } },
    { ASDLClass::Kind::Constructor, "While"_s, ASTClass::stmt, { fieldsOf_While }, {  } },
    { ASDLClass::Kind::Constructor, "If"_s, ASTClass::stmt, { fieldsOf_If }, {  } },
    { ASDLClass::Kind::Constructor, "With"_s, ASTClass::stmt, { fieldsOf_With }, {  } },
    { ASDLClass::Kind::Constructor, "AsyncWith"_s, ASTClass::stmt, { fieldsOf_AsyncWith }, {  } },
    { ASDLClass::Kind::Constructor, "Match"_s, ASTClass::stmt, { fieldsOf_Match }, {  } },
    { ASDLClass::Kind::Constructor, "Raise"_s, ASTClass::stmt, { fieldsOf_Raise }, {  } },
    { ASDLClass::Kind::Constructor, "Try"_s, ASTClass::stmt, { fieldsOf_Try }, {  } },
    { ASDLClass::Kind::Constructor, "TryStar"_s, ASTClass::stmt, { fieldsOf_TryStar }, {  } },
    { ASDLClass::Kind::Constructor, "Assert"_s, ASTClass::stmt, { fieldsOf_Assert }, {  } },
    { ASDLClass::Kind::Constructor, "Import"_s, ASTClass::stmt, { fieldsOf_Import }, {  } },
    { ASDLClass::Kind::Constructor, "ImportFrom"_s, ASTClass::stmt, { fieldsOf_ImportFrom }, {  } },
    { ASDLClass::Kind::Constructor, "Global"_s, ASTClass::stmt, { fieldsOf_Global }, {  } },
    { ASDLClass::Kind::Constructor, "Nonlocal"_s, ASTClass::stmt, { fieldsOf_Nonlocal }, {  } },
    { ASDLClass::Kind::Constructor, "Expr"_s, ASTClass::stmt, { fieldsOf_Expr }, {  } },
    { ASDLClass::Kind::Constructor, "Pass"_s, ASTClass::stmt, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Break"_s, ASTClass::stmt, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Continue"_s, ASTClass::stmt, {  }, {  } },
    { ASDLClass::Kind::Sum, "expr"_s, ASTClass::AST, {  }, { attributesOf_expr } },
    { ASDLClass::Kind::Constructor, "BoolOp"_s, ASTClass::expr, { fieldsOf_BoolOp }, {  } },
    { ASDLClass::Kind::Constructor, "NamedExpr"_s, ASTClass::expr, { fieldsOf_NamedExpr }, {  } },
    { ASDLClass::Kind::Constructor, "BinOp"_s, ASTClass::expr, { fieldsOf_BinOp }, {  } },
    { ASDLClass::Kind::Constructor, "UnaryOp"_s, ASTClass::expr, { fieldsOf_UnaryOp }, {  } },
    { ASDLClass::Kind::Constructor, "Lambda"_s, ASTClass::expr, { fieldsOf_Lambda }, {  } },
    { ASDLClass::Kind::Constructor, "IfExp"_s, ASTClass::expr, { fieldsOf_IfExp }, {  } },
    { ASDLClass::Kind::Constructor, "Dict"_s, ASTClass::expr, { fieldsOf_Dict }, {  } },
    { ASDLClass::Kind::Constructor, "Set"_s, ASTClass::expr, { fieldsOf_Set }, {  } },
    { ASDLClass::Kind::Constructor, "ListComp"_s, ASTClass::expr, { fieldsOf_ListComp }, {  } },
    { ASDLClass::Kind::Constructor, "SetComp"_s, ASTClass::expr, { fieldsOf_SetComp }, {  } },
    { ASDLClass::Kind::Constructor, "DictComp"_s, ASTClass::expr, { fieldsOf_DictComp }, {  } },
    { ASDLClass::Kind::Constructor, "GeneratorExp"_s, ASTClass::expr, { fieldsOf_GeneratorExp }, {  } },
    { ASDLClass::Kind::Constructor, "Await"_s, ASTClass::expr, { fieldsOf_Await }, {  } },
    { ASDLClass::Kind::Constructor, "Yield"_s, ASTClass::expr, { fieldsOf_Yield }, {  } },
    { ASDLClass::Kind::Constructor, "YieldFrom"_s, ASTClass::expr, { fieldsOf_YieldFrom }, {  } },
    { ASDLClass::Kind::Constructor, "Compare"_s, ASTClass::expr, { fieldsOf_Compare }, {  } },
    { ASDLClass::Kind::Constructor, "Call"_s, ASTClass::expr, { fieldsOf_Call }, {  } },
    { ASDLClass::Kind::Constructor, "FormattedValue"_s, ASTClass::expr, { fieldsOf_FormattedValue }, {  } },
    { ASDLClass::Kind::Constructor, "Interpolation"_s, ASTClass::expr, { fieldsOf_Interpolation }, {  } },
    { ASDLClass::Kind::Constructor, "JoinedStr"_s, ASTClass::expr, { fieldsOf_JoinedStr }, {  } },
    { ASDLClass::Kind::Constructor, "TemplateStr"_s, ASTClass::expr, { fieldsOf_TemplateStr }, {  } },
    { ASDLClass::Kind::Constructor, "Constant"_s, ASTClass::expr, { fieldsOf_Constant }, {  } },
    { ASDLClass::Kind::Constructor, "Attribute"_s, ASTClass::expr, { fieldsOf_Attribute }, {  } },
    { ASDLClass::Kind::Constructor, "Subscript"_s, ASTClass::expr, { fieldsOf_Subscript }, {  } },
    { ASDLClass::Kind::Constructor, "Starred"_s, ASTClass::expr, { fieldsOf_Starred }, {  } },
    { ASDLClass::Kind::Constructor, "Name"_s, ASTClass::expr, { fieldsOf_Name }, {  } },
    { ASDLClass::Kind::Constructor, "List"_s, ASTClass::expr, { fieldsOf_List }, {  } },
    { ASDLClass::Kind::Constructor, "Tuple"_s, ASTClass::expr, { fieldsOf_Tuple }, {  } },
    { ASDLClass::Kind::Constructor, "Slice"_s, ASTClass::expr, { fieldsOf_Slice }, {  } },
    { ASDLClass::Kind::Sum, "expr_context"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Load"_s, ASTClass::expr_context, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Store"_s, ASTClass::expr_context, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Del"_s, ASTClass::expr_context, {  }, {  } },
    { ASDLClass::Kind::Sum, "boolop"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "And"_s, ASTClass::boolop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Or"_s, ASTClass::boolop, {  }, {  } },
    { ASDLClass::Kind::Sum, "operator"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Add"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Sub"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Mult"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "MatMult"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Div"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Mod"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Pow"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "LShift"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "RShift"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "BitOr"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "BitXor"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "BitAnd"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Constructor, "FloorDiv"_s, ASTClass::operator_, {  }, {  } },
    { ASDLClass::Kind::Sum, "unaryop"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Invert"_s, ASTClass::unaryop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Not"_s, ASTClass::unaryop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "UAdd"_s, ASTClass::unaryop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "USub"_s, ASTClass::unaryop, {  }, {  } },
    { ASDLClass::Kind::Sum, "cmpop"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Eq"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "NotEq"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Lt"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "LtE"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Gt"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "GtE"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "Is"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "IsNot"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "In"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Constructor, "NotIn"_s, ASTClass::cmpop, {  }, {  } },
    { ASDLClass::Kind::Product, "comprehension"_s, ASTClass::AST, { fieldsOf_comprehension }, {  } },
    { ASDLClass::Kind::Sum, "excepthandler"_s, ASTClass::AST, {  }, { attributesOf_excepthandler } },
    { ASDLClass::Kind::Constructor, "ExceptHandler"_s, ASTClass::excepthandler, { fieldsOf_ExceptHandler }, {  } },
    { ASDLClass::Kind::Product, "arguments"_s, ASTClass::AST, { fieldsOf_arguments }, {  } },
    { ASDLClass::Kind::Product, "arg"_s, ASTClass::AST, { fieldsOf_arg }, { attributesOf_arg } },
    { ASDLClass::Kind::Product, "keyword"_s, ASTClass::AST, { fieldsOf_keyword }, { attributesOf_keyword } },
    { ASDLClass::Kind::Product, "alias"_s, ASTClass::AST, { fieldsOf_alias }, { attributesOf_alias } },
    { ASDLClass::Kind::Product, "withitem"_s, ASTClass::AST, { fieldsOf_withitem }, {  } },
    { ASDLClass::Kind::Product, "match_case"_s, ASTClass::AST, { fieldsOf_match_case }, {  } },
    { ASDLClass::Kind::Sum, "pattern"_s, ASTClass::AST, {  }, { attributesOf_pattern } },
    { ASDLClass::Kind::Constructor, "MatchValue"_s, ASTClass::pattern, { fieldsOf_MatchValue }, {  } },
    { ASDLClass::Kind::Constructor, "MatchSingleton"_s, ASTClass::pattern, { fieldsOf_MatchSingleton }, {  } },
    { ASDLClass::Kind::Constructor, "MatchSequence"_s, ASTClass::pattern, { fieldsOf_MatchSequence }, {  } },
    { ASDLClass::Kind::Constructor, "MatchMapping"_s, ASTClass::pattern, { fieldsOf_MatchMapping }, {  } },
    { ASDLClass::Kind::Constructor, "MatchClass"_s, ASTClass::pattern, { fieldsOf_MatchClass }, {  } },
    { ASDLClass::Kind::Constructor, "MatchStar"_s, ASTClass::pattern, { fieldsOf_MatchStar }, {  } },
    { ASDLClass::Kind::Constructor, "MatchAs"_s, ASTClass::pattern, { fieldsOf_MatchAs }, {  } },
    { ASDLClass::Kind::Constructor, "MatchOr"_s, ASTClass::pattern, { fieldsOf_MatchOr }, {  } },
    { ASDLClass::Kind::Sum, "type_ignore"_s, ASTClass::AST, {  }, {  } },
    { ASDLClass::Kind::Constructor, "TypeIgnore"_s, ASTClass::type_ignore, { fieldsOf_TypeIgnore }, {  } },
    { ASDLClass::Kind::Sum, "type_param"_s, ASTClass::AST, {  }, { attributesOf_type_param } },
    { ASDLClass::Kind::Constructor, "TypeVar"_s, ASTClass::type_param, { fieldsOf_TypeVar }, {  } },
    { ASDLClass::Kind::Constructor, "ParamSpec"_s, ASTClass::type_param, { fieldsOf_ParamSpec }, {  } },
    { ASDLClass::Kind::Constructor, "TypeVarTuple"_s, ASTClass::type_param, { fieldsOf_TypeVarTuple }, {  } },
};
static_assert(std::size(classes) == numberOfASTClasses);

} // namespace ASDL

inline const ASDLClass& descriptionOf(ASTClass astClass) { return ASDL::classes[static_cast<unsigned>(astClass)]; }

} } // namespace JSC::Python
