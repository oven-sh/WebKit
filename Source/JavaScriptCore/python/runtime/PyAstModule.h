/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyAST.h"
#include "PyAbstract.h"
#include "PyParserHost.h"
#include <wtf/HashMap.h>

namespace JSC {
namespace Python {

class ParseTree;

// The syntax tree as Python objects: the module _ast.

// The classes of the abstract grammar (Parser/Python.asdl in CPython), in its order.
// id, name, base, what it is, the attributes its instances have besides the fields, the fields as the grammar writes them.
#define FOR_EACH_PY_AST_CLASS(v) \
    v(AnyMod, "mod", AST, Sum, None, "") \
    v(Module, "Module", AnyMod, Constructor, None, "stmt* body, type_ignore* type_ignores") \
    v(Interactive, "Interactive", AnyMod, Constructor, None, "stmt* body") \
    v(Expression, "Expression", AnyMod, Constructor, None, "expr body") \
    v(FunctionType, "FunctionType", AnyMod, Constructor, None, "expr* argtypes, expr returns") \
    v(AnyStmt, "stmt", AST, Sum, OptionalEnd, "") \
    v(FunctionDef, "FunctionDef", AnyStmt, Constructor, None, "identifier name, arguments args, stmt* body, expr* decorator_list, expr? returns, string? type_comment, type_param* type_params") \
    v(AsyncFunctionDef, "AsyncFunctionDef", AnyStmt, Constructor, None, "identifier name, arguments args, stmt* body, expr* decorator_list, expr? returns, string? type_comment, type_param* type_params") \
    v(ClassDef, "ClassDef", AnyStmt, Constructor, None, "identifier name, expr* bases, keyword* keywords, stmt* body, expr* decorator_list, type_param* type_params") \
    v(Return, "Return", AnyStmt, Constructor, None, "expr? value") \
    v(Delete, "Delete", AnyStmt, Constructor, None, "expr* targets") \
    v(Assign, "Assign", AnyStmt, Constructor, None, "expr* targets, expr value, string? type_comment") \
    v(TypeAlias, "TypeAlias", AnyStmt, Constructor, None, "expr name, type_param* type_params, expr value") \
    v(AugAssign, "AugAssign", AnyStmt, Constructor, None, "expr target, operator op, expr value") \
    v(AnnAssign, "AnnAssign", AnyStmt, Constructor, None, "expr target, expr annotation, expr? value, int simple") \
    v(For, "For", AnyStmt, Constructor, None, "expr target, expr iter, stmt* body, stmt* orelse, string? type_comment") \
    v(AsyncFor, "AsyncFor", AnyStmt, Constructor, None, "expr target, expr iter, stmt* body, stmt* orelse, string? type_comment") \
    v(While, "While", AnyStmt, Constructor, None, "expr test, stmt* body, stmt* orelse") \
    v(If, "If", AnyStmt, Constructor, None, "expr test, stmt* body, stmt* orelse") \
    v(With, "With", AnyStmt, Constructor, None, "withitem* items, stmt* body, string? type_comment") \
    v(AsyncWith, "AsyncWith", AnyStmt, Constructor, None, "withitem* items, stmt* body, string? type_comment") \
    v(Match, "Match", AnyStmt, Constructor, None, "expr subject, match_case* cases") \
    v(Raise, "Raise", AnyStmt, Constructor, None, "expr? exc, expr? cause") \
    v(Try, "Try", AnyStmt, Constructor, None, "stmt* body, excepthandler* handlers, stmt* orelse, stmt* finalbody") \
    v(TryStar, "TryStar", AnyStmt, Constructor, None, "stmt* body, excepthandler* handlers, stmt* orelse, stmt* finalbody") \
    v(Assert, "Assert", AnyStmt, Constructor, None, "expr test, expr? msg") \
    v(Import, "Import", AnyStmt, Constructor, None, "alias* names, int? is_lazy") \
    v(ImportFrom, "ImportFrom", AnyStmt, Constructor, None, "identifier? module, alias* names, int? level, int? is_lazy") \
    v(Global, "Global", AnyStmt, Constructor, None, "identifier* names") \
    v(Nonlocal, "Nonlocal", AnyStmt, Constructor, None, "identifier* names") \
    v(Expr, "Expr", AnyStmt, Constructor, None, "expr value") \
    v(Pass, "Pass", AnyStmt, Constructor, None, "") \
    v(Break, "Break", AnyStmt, Constructor, None, "") \
    v(Continue, "Continue", AnyStmt, Constructor, None, "") \
    v(AnyExpr, "expr", AST, Sum, OptionalEnd, "") \
    v(BoolOp, "BoolOp", AnyExpr, Constructor, None, "boolop op, expr* values") \
    v(NamedExpr, "NamedExpr", AnyExpr, Constructor, None, "expr target, expr value") \
    v(BinOp, "BinOp", AnyExpr, Constructor, None, "expr left, operator op, expr right") \
    v(UnaryOp, "UnaryOp", AnyExpr, Constructor, None, "unaryop op, expr operand") \
    v(Lambda, "Lambda", AnyExpr, Constructor, None, "arguments args, expr body") \
    v(IfExp, "IfExp", AnyExpr, Constructor, None, "expr test, expr body, expr orelse") \
    v(Dict, "Dict", AnyExpr, Constructor, None, "expr?* keys, expr* values") \
    v(Set, "Set", AnyExpr, Constructor, None, "expr* elts") \
    v(ListComp, "ListComp", AnyExpr, Constructor, None, "expr elt, comprehension* generators") \
    v(SetComp, "SetComp", AnyExpr, Constructor, None, "expr elt, comprehension* generators") \
    v(DictComp, "DictComp", AnyExpr, Constructor, None, "expr key, expr? value, comprehension* generators") \
    v(GeneratorExp, "GeneratorExp", AnyExpr, Constructor, None, "expr elt, comprehension* generators") \
    v(Await, "Await", AnyExpr, Constructor, None, "expr value") \
    v(Yield, "Yield", AnyExpr, Constructor, None, "expr? value") \
    v(YieldFrom, "YieldFrom", AnyExpr, Constructor, None, "expr value") \
    v(Compare, "Compare", AnyExpr, Constructor, None, "expr left, cmpop* ops, expr* comparators") \
    v(Call, "Call", AnyExpr, Constructor, None, "expr func, expr* args, keyword* keywords") \
    v(FormattedValue, "FormattedValue", AnyExpr, Constructor, None, "expr value, int conversion, expr? format_spec") \
    v(Interpolation, "Interpolation", AnyExpr, Constructor, None, "expr value, constant str, int conversion, expr? format_spec") \
    v(JoinedStr, "JoinedStr", AnyExpr, Constructor, None, "expr* values") \
    v(TemplateStr, "TemplateStr", AnyExpr, Constructor, None, "expr* values") \
    v(Constant, "Constant", AnyExpr, Constructor, None, "constant value, string? kind") \
    v(Attribute, "Attribute", AnyExpr, Constructor, None, "expr value, identifier attr, expr_context ctx") \
    v(Subscript, "Subscript", AnyExpr, Constructor, None, "expr value, expr slice, expr_context ctx") \
    v(Starred, "Starred", AnyExpr, Constructor, None, "expr value, expr_context ctx") \
    v(Name, "Name", AnyExpr, Constructor, None, "identifier id, expr_context ctx") \
    v(List, "List", AnyExpr, Constructor, None, "expr* elts, expr_context ctx") \
    v(Tuple, "Tuple", AnyExpr, Constructor, None, "expr* elts, expr_context ctx") \
    v(Slice, "Slice", AnyExpr, Constructor, None, "expr? lower, expr? upper, expr? step") \
    v(AnyExprContext, "expr_context", AST, Sum, None, "") \
    v(Load, "Load", AnyExprContext, Constructor, None, "") \
    v(Store, "Store", AnyExprContext, Constructor, None, "") \
    v(Del, "Del", AnyExprContext, Constructor, None, "") \
    v(AnyBoolOp, "boolop", AST, Sum, None, "") \
    v(And, "And", AnyBoolOp, Constructor, None, "") \
    v(Or, "Or", AnyBoolOp, Constructor, None, "") \
    v(AnyOperator, "operator", AST, Sum, None, "") \
    v(Add, "Add", AnyOperator, Constructor, None, "") \
    v(Sub, "Sub", AnyOperator, Constructor, None, "") \
    v(Mult, "Mult", AnyOperator, Constructor, None, "") \
    v(MatMult, "MatMult", AnyOperator, Constructor, None, "") \
    v(Div, "Div", AnyOperator, Constructor, None, "") \
    v(Mod, "Mod", AnyOperator, Constructor, None, "") \
    v(Pow, "Pow", AnyOperator, Constructor, None, "") \
    v(LShift, "LShift", AnyOperator, Constructor, None, "") \
    v(RShift, "RShift", AnyOperator, Constructor, None, "") \
    v(BitOr, "BitOr", AnyOperator, Constructor, None, "") \
    v(BitXor, "BitXor", AnyOperator, Constructor, None, "") \
    v(BitAnd, "BitAnd", AnyOperator, Constructor, None, "") \
    v(FloorDiv, "FloorDiv", AnyOperator, Constructor, None, "") \
    v(AnyUnaryOp, "unaryop", AST, Sum, None, "") \
    v(Invert, "Invert", AnyUnaryOp, Constructor, None, "") \
    v(Not, "Not", AnyUnaryOp, Constructor, None, "") \
    v(UAdd, "UAdd", AnyUnaryOp, Constructor, None, "") \
    v(USub, "USub", AnyUnaryOp, Constructor, None, "") \
    v(AnyCmpOp, "cmpop", AST, Sum, None, "") \
    v(Eq, "Eq", AnyCmpOp, Constructor, None, "") \
    v(NotEq, "NotEq", AnyCmpOp, Constructor, None, "") \
    v(Lt, "Lt", AnyCmpOp, Constructor, None, "") \
    v(LtE, "LtE", AnyCmpOp, Constructor, None, "") \
    v(Gt, "Gt", AnyCmpOp, Constructor, None, "") \
    v(GtE, "GtE", AnyCmpOp, Constructor, None, "") \
    v(Is, "Is", AnyCmpOp, Constructor, None, "") \
    v(IsNot, "IsNot", AnyCmpOp, Constructor, None, "") \
    v(In, "In", AnyCmpOp, Constructor, None, "") \
    v(NotIn, "NotIn", AnyCmpOp, Constructor, None, "") \
    v(Comprehension, "comprehension", AST, Product, None, "expr target, expr iter, expr* ifs, int is_async") \
    v(AnyExceptHandler, "excepthandler", AST, Sum, OptionalEnd, "") \
    v(ExceptHandler, "ExceptHandler", AnyExceptHandler, Constructor, None, "expr? type, identifier? name, stmt* body") \
    v(Arguments, "arguments", AST, Product, None, "arg* posonlyargs, arg* args, arg? vararg, arg* kwonlyargs, expr?* kw_defaults, arg? kwarg, expr* defaults") \
    v(Arg, "arg", AST, Product, OptionalEnd, "identifier arg, expr? annotation, string? type_comment") \
    v(Keyword, "keyword", AST, Product, OptionalEnd, "identifier? arg, expr value") \
    v(Alias, "alias", AST, Product, OptionalEnd, "identifier name, identifier? asname") \
    v(WithItem, "withitem", AST, Product, None, "expr context_expr, expr? optional_vars") \
    v(MatchCase, "match_case", AST, Product, None, "pattern pattern, expr? guard, stmt* body") \
    v(AnyPattern, "pattern", AST, Sum, RequiredEnd, "") \
    v(MatchValue, "MatchValue", AnyPattern, Constructor, None, "expr value") \
    v(MatchSingleton, "MatchSingleton", AnyPattern, Constructor, None, "constant value") \
    v(MatchSequence, "MatchSequence", AnyPattern, Constructor, None, "pattern* patterns") \
    v(MatchMapping, "MatchMapping", AnyPattern, Constructor, None, "expr* keys, pattern* patterns, identifier? rest") \
    v(MatchClass, "MatchClass", AnyPattern, Constructor, None, "expr cls, pattern* patterns, identifier* kwd_attrs, pattern* kwd_patterns") \
    v(MatchStar, "MatchStar", AnyPattern, Constructor, None, "identifier? name") \
    v(MatchAs, "MatchAs", AnyPattern, Constructor, None, "pattern? pattern, identifier? name") \
    v(MatchOr, "MatchOr", AnyPattern, Constructor, None, "pattern* patterns") \
    v(AnyTypeIgnore, "type_ignore", AST, Sum, None, "") \
    v(TypeIgnore, "TypeIgnore", AnyTypeIgnore, Constructor, None, "int lineno, string tag") \
    v(AnyTypeParam, "type_param", AST, Sum, RequiredEnd, "") \
    v(TypeVar, "TypeVar", AnyTypeParam, Constructor, None, "identifier name, expr? bound, expr? default_value") \
    v(ParamSpec, "ParamSpec", AnyTypeParam, Constructor, None, "identifier name, expr? default_value") \
    v(TypeVarTuple, "TypeVarTuple", AnyTypeParam, Constructor, None, "identifier name, expr? default_value")

enum class AstClass : uint8_t {
    AST,
#define PY_DEFINE_ENUMERATOR(id, name, base, kind, attributes, fields) id,
    FOR_EACH_PY_AST_CLASS(PY_DEFINE_ENUMERATOR)
#undef PY_DEFINE_ENUMERATOR
    Count,
};

static constexpr unsigned numberOfAstClasses = static_cast<unsigned>(AstClass::Count);

enum class AstName : uint8_t {
    Lineno,
    ColOffset,
    EndLineno,
    EndColOffset,
    Fields, // _fields
    Attributes, // _attributes
    FieldTypes, // _field_types
    Count,
};

// The classes and what goes with them, which are made the first time they are asked for. It is false if that failed, with an exception.
class AstState {
public:
    explicit AstState(JSGlobalObject*);

    explicit operator bool() const { return m_table; }

    PyType* type(AstClass id) const { return PyType::from(m_table->at(static_cast<unsigned>(id))); }
    // The names of the fields, whatever has become of _fields.
    PyTuple* fields(AstClass id) const { return uncheckedDowncast<PyTuple>(m_table->at(numberOfAstClasses + static_cast<unsigned>(id)).asCell()); }
    JSString* field(AstClass id, unsigned index) const { return asString(fields(id)->at(index)); }
    // For a class without fields such as Load or Add: the instance that trees made by the parser share.
    JSValue singleton(AstClass id) const { return m_table->at(2 * numberOfAstClasses + static_cast<unsigned>(id)); }
    JSString* name(AstName name) const { return asString(m_table->at(3 * numberOfAstClasses + static_cast<unsigned>(name))); }

    // A class the grammar has no instances of: AST itself, stmt, expr...
    bool isAbstract(PyType*) const;

private:
    PyTuple* m_table;
};

ASCIILiteral nameOfAstClass(AstClass);

// What the grammar says of a class and of its fields.

enum class AstClassKind : uint8_t {
    Sum, // stmt = FunctionDef(...) | ...
    Constructor,
    Product, // arguments = (...)
};

enum class AstAttributes : uint8_t {
    None,
    OptionalEnd, // int lineno, int col_offset, int? end_lineno, int? end_col_offset
    RequiredEnd, // int lineno, int col_offset, int end_lineno, int end_col_offset
};

struct AstField {
    enum class Type : uint8_t { Identifier, Int, String, Constant, Node };
    StringView name;
    Type type;
    AstClass node { AstClass::AST }; // Of a Node: a sum or a product.
    bool isOptional { false };
    bool isSequence { false }; // `expr?*` is a sequence and no more: what may be None in one is not the grammar's business.
};

struct AstClassInfo {
    ASCIILiteral name;
    AstClass base;
    AstClassKind kind;
    AstAttributes attributes; // A constructor has those of its sum.
    bool isSimpleSum { false }; // Such as operator: nothing but names.
    AstClass lastConstructor { AstClass::AST }; // Of a sum, whose constructors follow it.
    Vector<AstField, 7> fields;
};

const AstClassInfo& infoOfAstClass(AstClass);

// The tree as objects. Empty: an exception.
JSValue astFromTree(JSGlobalObject*, AST::Mod*);

// In a tree made from objects, what is where a constant should be and is none: the name of its type, by where it would be.
using AstInvalidConstants = UncheckedKeyHashMap<const AST::ConstantValue*, String>;

// Whether a tree made from objects is one the compiler can take (_PyAST_Validate() in CPython). False: an exception.
bool validateTree(JSGlobalObject*, AST::Mod*, const AstInvalidConstants&);

// What compile() does to a tree before it hands it out or compiles it (Python/ast_preprocess.c in CPython): with
// `optimize` 2 the docstrings go; unless `syntaxCheckOnly`, __debug__ becomes a constant, and so on. False: `error` says why.
bool preprocessTree(VM&, ParseTree&, int optimize, bool futureAnnotations, bool syntaxCheckOnly, SyntaxErrorInfo& error);

} // namespace Python
} // namespace JSC
