/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyCompiler.h"

namespace JSC {
namespace Python {

// The compilation of one code object.
//
// An error does not stop it: the first is kept, the code that comes out is thrown away, and what is emitted after one only
// has to be well formed.
class Unit {
    WTF_MAKE_NONCOPYABLE(Unit);
public:
    using Reg = RefPtr<RegisterID>;
    using Name = AST::Name;
    using Range = AST::Range;
    using Expr = AST::Expr;
    using Stmt = AST::Stmt;
    using UnitKind = Compiler::UnitKind;
    using CodeSpec = Compiler::CodeSpec;

    enum class Part : uint8_t {
        Whole,
        GeneratorBinder, // Of a generator function: what binds the arguments and makes the generator.
        GeneratorBody,
    };

    Unit(Compiler&, Unit* parent, const CodeSpec&, BytecodeGenerator&, CodeInfo&, const SourceCode&, Part);

    BytecodeGenerator& generator() { return g; }
    const SourceCode& source() const { return m_source; }
    Unit* parent() const { return m_parent; }
    Scope* scope() const { return m_scope; }
    UnitKind kind() const { return m_kind; }
    const String& qualname() const { return m_info.qualname; }
    Name privateName() const { return m_private; }

    // ---- Entry and exit ----

    void emitPrologue();
    void emitMakeGenerator();
    void emitReturnNone();

    // ---- Bodies ----

    void compileModule(AST::Mod*);
    void compileFunctionBody(Seq<Stmt*>);
    void compileClassBody(AST::ClassDef*, int firstLineNumber);
    void statements(Seq<Stmt*>, unsigned first = 0);
    void statement(Stmt*);

    // ---- Expressions ----

    // The register the value is in: `dst` if there is one. Unless the caller holds a reference to it, the register is only
    // good until the next is allocated.
    RegisterID* expr(Expr*, RegisterID* dst = nullptr);
    void exprForEffect(Expr*);
    void jumpIf(Expr*, Label& target, bool condition);
    void store(Expr* target, RegisterID* value);
    void deleteTarget(Expr*);

    // ---- Names ----

    Name mangle(Name);
    RegisterID* loadName(Name, RegisterID* dst, const Range&);
    void storeName(Name, RegisterID* value, const Range&);
    void deleteName(Name, const Range&);

private:
    friend class Compiler;

    struct Variable {
        enum class Kind : uint8_t {
            Fast, // The register holds the value.
            Cell, // The register holds a cell that this code makes.
            Free, // The register holds a cell of the closure.
        };
        RegisterID* reg { nullptr };
        Kind kind { Kind::Fast };
        bool mayBeUnbound { true };
        // Its number in BytecodeGenerator::pythonBoundVariables(), by which most of the checks for a value are found to be
        // needless. -1: it has none. That is so for a variable that `del` or the end of an `except` clause takes the value from.
        int8_t boundBit { -1 };
    };
    using VariableMap = UncheckedKeyHashMap<UniquedStringImpl*, Variable>;

    struct Access {
        enum class Kind : uint8_t {
            Fast,
            Cell,
            ClassCell, // In a class body: the namespace first, then the cell.
            ClassDictCell, // Where the names of an enclosing class are visible: its namespace first, then the cell.
            Global,
            ClassDictGlobal, // Likewise, then the globals.
            Name, // The namespace, then the globals.
        };
        Kind kind;
        Variable* variable { nullptr };
    };

    enum class BlockKind : uint8_t {
        WhileLoop,
        ForLoop,
        AsyncForLoop,
        TryExcept,
        FinallyTry,
        FinallyEnd,
        With,
        AsyncWith,
        HandlerCleanup,
        ExceptionHandler,
        ExceptionGroupHandler,
        InlinedComprehension,
        MatchSubject, // While the patterns of a case are tried.
    };

    // What a `return`, `break` or `continue` has to leave in good order on its way out.
    struct Block {
        BlockKind kind;
        size_t tryDepth { 0 }; // How many try ranges of the generator were open before those of the block.
        Label* continueTarget { nullptr };
        Label* breakTarget { nullptr };
        Seq<Stmt*> finalBody { }; // FinallyTry
        RegisterID* saved { nullptr }; // Handlers: the exception that was being handled before. With: __exit__. ForLoop: the iterator.
        Name name { nullptr }; // HandlerCleanup: what follows `as`.
        Range range { }; // With
        Scope* comprehension { nullptr }; // InlinedComprehension
        unsigned number { 0 }; // For PythonMark.
    };

    struct PatternContext {
        struct Capture {
            Name name;
            Reg reg;
        };
        Vector<Capture, 4> captures;
        Label* fail { nullptr };
        bool allowIrrefutable { false };
    };

    // A run of registers, each one below the last, as op_py_runtime takes its arguments.
    class Arguments {
    public:
        Arguments(Unit&, unsigned count);
        RegisterID* operator[](unsigned index) { return m_registers[index].get(); }
        RegisterID* first() { return m_registers.isEmpty() ? nullptr : m_registers[0].get(); }
        unsigned size() const { return m_registers.size(); }

    private:
        Vector<Reg, 8> m_registers;
    };

    VM& vm() { return c.m_vm; }
    JSGlobalObject* globalObject() { return c.m_globalObject; }
    Runtime& runtime() { return c.m_runtime; }
    const SymbolTable::Names& names() { return c.m_symbols->names(); }
    bool isFunctionLike() const { return m_scope->isFunctionLike(); }
    bool isGeneratorLike() const { return m_scope->isGenerator || m_scope->isCoroutine; }
    bool isAsyncGenerator() const { return m_scope->isGenerator && m_scope->isCoroutine; }
    bool inInlinedComprehension() const { return !m_overlays.isEmpty(); }

    void fail(const Range& range, const String& message) { c.fail(range, message); }
    void warn(const Range& range, const String& message) { c.warn(range, message); }
    bool isSafeToRecurse();

    // Says where the instructions that follow come from.
    void loc(const Range&);
    // Only the line, which is what is told to whoever traces the program. A line of which no instruction comes is told of too.
    void line(int lineno);
    // What follows is of no line: the program did not write it, and it is not reached from what it wrote last.
    void noLine();
    // It is of this line, but nobody is told that the line begins.
    void silentLine(int lineno);
    // A jump that is there because of how the code is laid out here. In CPython there is none, or one that nobody is told of:
    // what handles exceptions is out of the way, at the end of the code.
    void hiddenJump(Label&);
    // Whatever follows it is never run.
    bool neverGetsPast(Stmt*);
    bool neverGetsPast(Seq<Stmt*>);
    // What is emitted meanwhile goes with the line before it, whatever loc() and line() are given.
    class SameLine {
        WTF_MAKE_NONCOPYABLE(SameLine);
    public:
        explicit SameLine(Unit& unit)
            : m_unit(unit)
        {
            ++unit.m_sameLineDepth;
        }
        ~SameLine() { --m_unit.m_sameLineDepth; }
    private:
        Unit& m_unit;
    };

    // ---- Registers and constants ----

    Reg newTemporary() { return g.newTemporary(); }
    RegisterID* constant(JSValue value) { return g.pythonConstant(value); }
    RegisterID* emptyValue() { return g.pythonConstant(JSValue()); }
    RegisterID* nameConstant(Name);
    RegisterID* usedName(Name);
    // For co_consts.
    void noteConstant(JSValue, bool isUsed = true);
    void noteFoldedConstant(AST::Expr*, JSValue);
    void noteCode(unsigned functionIndex);
    void finishConstants();
    RegisterID* stringConstant(const String&);
    RegisterID* load(RegisterID* dst, JSValue);
    JSValue constantValue(const AST::ConstantValue&);
    Vector<CodeInfo::Constant> m_foldedConstants;
    bool tryConstantValue(Expr*, JSValue&);
    JSValue valueCPythonsCompilerWorksOut(Expr*);
    std::optional<bool> constantTruth(Expr*);

    RegisterID* callRuntime(RegisterID* dst, RuntimeFunction, Arguments&);
    RegisterID* callRuntime(RegisterID* dst, RuntimeFunction);
    // The operands are copied to where the instruction wants them.
    RegisterID* callRuntime(RegisterID* dst, RuntimeFunction, std::initializer_list<RegisterID*>);
    void callRuntimeForEffect(RuntimeFunction function, std::initializer_list<RegisterID*> operands) { callRuntime(newTemporary().get(), function, operands); }

    // ---- Variables ----

    void declareVariables();
    void trackWhetherBound(Variable&);
    bool isKnownToBeBound(const Variable& variable) const { return variable.boundBit >= 0 && (g.pythonBoundVariables() >> variable.boundBit) & 1; }
    void noteBound(const Variable& variable)
    {
        if (variable.boundBit >= 0)
            g.setPythonBoundVariables(g.pythonBoundVariables() | static_cast<uint64_t>(1) << variable.boundBit);
    }
    void noteUnbound(const Variable& variable)
    {
        if (variable.boundBit >= 0)
            g.setPythonBoundVariables(g.pythonBoundVariables() & ~(static_cast<uint64_t>(1) << variable.boundBit));
    }
    void declareComprehensionVariables(Scope*);
    void recordVariable(Name, const Variable&, bool isHidden);
    bool isParameterRegister(RegisterID*);
    Access resolve(Name mangled);
    RegisterID* cellFor(Name);
    RegisterID* classDict(RegisterID* dst);
    Vector<Name> freeVariablesOf(Scope*);

    // ---- Functions and classes ----

    struct FunctionParts {
        Reg defaults;
        Reg kwDefaults;
        Reg annotate;
    };
    // Makes a function of the code. Its free variables are found in this unit.
    RegisterID* emitMakeFunction(RegisterID* dst, UnlinkedFunctionExecutable*, Scope*, const FunctionParts&);
    void emitDefaults(AST::Arguments*, FunctionParts&);
    void emitFunctionAnnotations(AST::Arguments*, Expr* returns, const Range&, FunctionParts&);
    void emitAnnotationsPrologue(const Range&);
    void functionDefinition(AST::FunctionDef*);
    RegisterID* emitFunctionObject(RegisterID* dst, AST::FunctionDef*, int firstLineNumber, FunctionParts&);
    void classDefinition(AST::ClassDef*);
    RegisterID* emitBuildClass(RegisterID* dst, AST::ClassDef*, int firstLineNumber, Name injectedBase);
    void typeAlias(AST::TypeAlias*);
    RegisterID* emitTypeAliasObject(RegisterID* dst, AST::TypeAlias*, RegisterID* typeParams);
    RegisterID* emitTypeParams(RegisterID* dst, Seq<AST::TypeParam*>);
    RegisterID* emitTypeParamExpression(RegisterID* dst, Expr*, Name, const void* key, bool allowStarred);
    RegisterID* lambda(AST::Lambda*, RegisterID* dst);
    void emitDecorators(Seq<Expr*>, Vector<Reg, 4>&);
    RegisterID* applyDecorators(Seq<Expr*>, Vector<Reg, 4>&, RegisterID* value);
    void collectDeferredAnnotations(Seq<Stmt*>, bool isConditional);
    void emitDeferredAnnotations(const Range&);
    String annotationText(Expr*);
    static String cleanDocstring(const String&);
    String docstringOf(Seq<Stmt*>);

    // ---- Statements ----

    void assign(AST::Assign*);
    void augmentedAssign(AST::AugAssign*);
    void annotatedAssign(AST::AnnAssign*);
    void checkAnnotationTarget(Expr*);
    void ifStatement(AST::If*);
    void whileLoop(AST::While*);
    void forLoop(AST::For*);
    void asyncForLoop(AST::For*);
    void returnStatement(AST::Return*);
    void breakOrContinue(Stmt*, bool isBreak);
    void raiseStatement(AST::Raise*);
    void tryStatement(AST::Try*);
    void tryFinally(AST::Try*);
    void tryExcept(AST::Try*);
    void tryStarExcept(AST::Try*);
    void withStatement(AST::With*, unsigned index);
    void assertStatement(AST::Assert*);
    void importStatement(AST::Import*);
    void importFrom(AST::ImportFrom*);
    void matchStatement(AST::Match*);
    ImportMode importMode(bool isLazy, bool isStar);

    // ---- Blocks ----

    void pushBlock(Block&&);
    void popBlock();
    void markBlock(const Block&, bool begins);
    // Leaves the blocks down to the innermost loop, or, without `stopAtLoop`, all of them. The code emitted from then on is
    // outside their try ranges, until `suspended` is given to resumeTry(). Returns the loop.
    std::optional<Block> unwindBlocks(const Range&, bool stopAtLoop, Vector<TryContext>& suspended);
    void unwindBlock(const Block&, Vector<TryContext>& suspended);
    void suspendTry(size_t depth, Vector<TryContext>& suspended);
    void resumeTry(Vector<TryContext>&& suspended) { g.resumeTryContexts(WTF::move(suspended)); }
    template<typename Function> void outsideTry(size_t depth, const Function&);
    bool inExceptionHandler() const;

    struct Try {
        TryData* data;
        Ref<Label> handler;
        size_t depthBefore;
    };
    Try beginTry();
    void endTry(Try&);
    // Starts the handler. Returns what was thrown.
    Reg beginHandler(Try&);
    // The same, as a Python exception that is now the one being handled. `previous`: the one that was.
    Reg beginExceptionHandler(Try&, Reg& previous);
    void restoreHandledException(RegisterID* previous);
    void reraise(RegisterID* exception);

    // ---- Expressions ----

    RegisterID* boolOp(AST::BoolOp*, RegisterID* dst);
    RegisterID* chain(AST::Expr*, RegisterID* dst);
    RegisterID* unaryOp(AST::UnaryOp*, RegisterID* dst);
    RegisterID* ifExp(AST::IfExp*, RegisterID* dst);
    RegisterID* compare(AST::Compare*, RegisterID* dst);
    RegisterID* emitCompare(RegisterID* dst, RegisterID* left, RegisterID* right, AST::CompareKind, Expr* rightExpr);
    void checkCompare(AST::Compare*);
    RegisterID* call(AST::Call*, RegisterID* dst);
    // callee(pushed..., *args, **keywords). `injected`: a variable to pass after the positional arguments.
    RegisterID* emitCall(RegisterID* dst, RegisterID* callee, std::span<RegisterID* const> pushed, Seq<Expr*> args, Seq<AST::Keyword*> keywords, Name injected, const Range&);
    RegisterID* emitSimpleCall(RegisterID* dst, RegisterID* callee, std::initializer_list<RegisterID*> args, const Range&);
    void validateKeywords(Seq<AST::Keyword*>);
    JSValue keywordNames(Seq<AST::Keyword*>);
    void checkCaller(Expr*);
    void checkSubscripter(Expr*);
    void checkIndex(Expr* value, Expr* slice);
    ASCIILiteral inferTypeName(Expr*);
    bool looksLikeClass(Expr*);
    bool looksLikeModule(Expr*);
    RegisterID* slice(AST::Slice*, RegisterID* dst);
    enum class SequenceKind : uint8_t { List, Tuple, Set };
    RegisterID* sequence(SequenceKind, Seq<Expr*>, RegisterID* dst, const Range&, std::span<RegisterID* const> pushed = { }, Name injected = nullptr);
    RegisterID* dict(AST::Dict*, RegisterID* dst);
    RegisterID* joinedStr(AST::JoinedStr*, RegisterID* dst);
    bool checkConversion(int conversion);
    RegisterID* formattedValue(AST::FormattedValue*, RegisterID* dst);
    RegisterID* templateStr(AST::TemplateStr*, RegisterID* dst);
    RegisterID* interpolation(AST::Interpolation*, RegisterID* dst);
    RegisterID* namedExpr(AST::NamedExpr*, RegisterID* dst);
    RegisterID* yield(AST::Yield*, RegisterID* dst);
    RegisterID* yieldFrom(AST::YieldFrom*, RegisterID* dst);
    RegisterID* await(AST::Await*, RegisterID* dst);
    RegisterID* emitYield(RegisterID* dst, RegisterID* value);
    void keepVariablesAcrossYield();
    // `yield from` an iterator, or `await` one: the value it finishes with.
    RegisterID* emitDelegate(RegisterID* dst, RegisterID* iterator);
    RegisterID* emitAwait(RegisterID* dst, RegisterID* awaitable, int where);
    void unpack(Seq<Expr*> targets, RegisterID* value, const Range&);

    RegisterID* comprehension(Expr*, ComprehensionKind, Seq<AST::Comprehension*>, Expr* element, Expr* value, RegisterID* dst);
    void comprehensionGenerator(ComprehensionKind, Seq<AST::Comprehension*>, unsigned index, Expr* element, Expr* value, RegisterID* result, RegisterID* firstIterator, const Range&);
    void comprehensionElement(ComprehensionKind, Expr* element, Expr* value, RegisterID* result);

    // ---- Patterns ----

    void pattern(AST::Pattern*, RegisterID* subject, PatternContext&);
    void subpattern(AST::Pattern*, RegisterID* subject, PatternContext&);
    void capture(Name, RegisterID* value, const Range&, PatternContext&);
    void patternValue(AST::MatchValue*, RegisterID* subject, PatternContext&);
    void patternSingleton(AST::MatchSingleton*, RegisterID* subject, PatternContext&);
    void patternSequence(AST::MatchSequence*, RegisterID* subject, PatternContext&);
    void patternMapping(AST::MatchMapping*, RegisterID* subject, PatternContext&);
    void patternClass(AST::MatchClass*, RegisterID* subject, PatternContext&);
    void patternAs(AST::MatchAs*, RegisterID* subject, PatternContext&);
    void patternOr(AST::MatchOr*, RegisterID* subject, PatternContext&);

    Compiler& c;
    Unit* m_parent;
    BytecodeGenerator& g;
    CodeInfo& m_info;
    SourceCode m_source;
    Scope* m_scope;
    UnitKind m_kind;
    Part m_part;
    Name m_private;
    AST::Arguments* m_arguments;
    CodeSpec::IterateFirstParameter m_iterateFirstParameter;
    RegisterID* m_firstIterator { nullptr }; // In the body of a generator expression: what the binder got from the parameter.

    UncheckedKeyHashMap<Scope*, std::unique_ptr<VariableMap>> m_variables;
    Vector<Scope*, 2> m_overlays; // The inlined comprehensions being compiled, the innermost last.
    RegisterID* m_namespace { nullptr }; // Module, class: where the names live.
    bool m_copyLocals;
    struct CellParameter {
        RegisterID* reg;
        Name name;
    };
    Vector<CellParameter, 2> m_cellParameters; // The parameters that enclosed functions use.
    unsigned m_boundBitCount { 0 };
    int m_markedLine { -1 }; // What line() was last given, and where in the instructions.
    size_t m_markedPosition { 0 };
    unsigned m_sameLineDepth { 0 };
    size_t m_statementStart { std::numeric_limits<size_t>::max() }; // Where in the instructions the last statement begins.
    unsigned m_blockCount { 0 };

    Vector<Block, 8> m_blocks;

    // For "'return' in a 'finally' block".
    struct FinallyContext {
        bool inFinally;
        bool inLoop;
    };
    Vector<FinallyContext, 4> m_finallyContexts;

    // Module, class: the annotations of variables, evaluated on demand.
    struct DeferredAnnotation {
        AST::AnnAssign* statement;
        int conditionalIndex; // -1: it always applies.
    };
    Vector<DeferredAnnotation> m_deferredAnnotations;
    UncheckedKeyHashMap<AST::AnnAssign*, int> m_conditionalAnnotationIndices;
    int m_nextConditionalAnnotationIndex { 0 };

    // Class: the X of every `self.X = ...` in its methods.
    Vector<Name> m_staticAttributes;
};

template<typename Function>
void Unit::outsideTry(size_t depth, const Function& function)
{
    Vector<TryContext> suspended = g.suspendTryContexts(depth);
    function();
    g.resumeTryContexts(WTF::move(suspended));
}

} // namespace Python
} // namespace JSC
