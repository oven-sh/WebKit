/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "PyCompiler.h"
#include "PyFormatParts.h"

namespace JSC {
namespace Python {

// See "What CPython's compiler copies".
constexpr unsigned tooManyToCopy = 5;

// Whether tools are told of a jump to where a statement ends. See jumpAsLaidOutMark, jumpForwardsAsLaidOutMark.
// OfTheProgram: they are, come what may. The order is that of Last::inPlaceOfJump().
enum class JumpIs : uint8_t { Hidden, ToldOfForwards, ToldOf, OfTheProgram };

// What comes after a statement: something else, the end of the function, the end of the body of a loop, or a jump. See Unit::leaveStatement().
struct Last {
    enum Kind : uint8_t { No, OfFunction, OfLoopBody, BeforeJump };
    Kind kind { No };
    unsigned loop { 0 }; // OfLoopBody: which of Unit::m_blocks.
    // BeforeJump: where to, and how many blocks and `try` ranges are open there.
    Label* target { nullptr };
    unsigned blockCount { 0 };
    unsigned tryDepth { 0 };
    // OfFunction: it is not the function that ends there but a `finally` clause that an exception has led to, and the exception goes on.
    // These are for Unit::reraiseOutOfHandler(), the two above as for BeforeJump.
    RegisterID* exception { nullptr };
    RegisterID* previous { nullptr };
    // OfLoopBody: a jump comes first, over an `else`, the other cases or the handlers. In CPython's compiler a jump to such a jump goes where that
    // goes, and a branch does not.
    bool isBehindJump { false };
    unsigned jumpsBefore { 0 }; // How many such jumps, one leading to the next.
    JumpIs jumpIs { JumpIs::Hidden }; // BeforeJump
    bool isInPlaceOfJump { false }; // What is left like this would be a jump to the jump, and not the jump itself.
    // BeforeJump, for Unit::isReplacedByCopy(): how many instructions come after the jump if the function ends with them, and how many each of the
    // jumps comes with that lead one to the next, the last of them first. (More than there is room for: nothing is copied.)
    unsigned instructionsAfterJump { std::numeric_limits<unsigned>::max() };
    static constexpr unsigned maxJumps = 6;
    std::array<uint8_t, maxJumps> instructionsOfJumps { };
    // Where they lead as the program has them, which is where the next of them is, and what `blockCount` and `tryDepth` are for that.
    std::array<Label*, maxJumps> targetsOfJumps { };
    std::array<uint16_t, maxJumps> blockCountsOfJumps { };
    std::array<uint16_t, maxJumps> tryDepthsOfJumps { };
    explicit operator bool() const { return kind != No; }
    Last onlyIf(bool condition) const { return condition ? *this : Last { }; }
    // A jump to a jump is of the kind that tools are told of whichever way it goes if either is.
    Last inPlaceOfJump(JumpIs first) const
    {
        Last result = *this;
        result.jumpIs = std::max(first, jumpIs);
        result.isInPlaceOfJump = true;
        return result;
    }
    Last behindJump(JumpIs first, unsigned instructions, Label* targetOfJump, size_t blockCountOfJump = 0, size_t tryDepthOfJump = 0) const
    {
        Last result = inPlaceOfJump(first);
        result.isInPlaceOfJump = false;
        result.isBehindJump = true;
        if (result.jumpsBefore < maxJumps) {
            result.instructionsOfJumps[result.jumpsBefore] = instructions;
            result.targetsOfJumps[result.jumpsBefore] = blockCountOfJump <= UINT16_MAX && tryDepthOfJump <= UINT16_MAX ? targetOfJump : nullptr;
            result.blockCountsOfJumps[result.jumpsBefore] = blockCountOfJump;
            result.tryDepthsOfJumps[result.jumpsBefore] = tryDepthOfJump;
        }
        ++result.jumpsBefore;
        return result;
    }
};

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
    void emitMakeGenerator(unsigned slotCount);
    void emitReturnNone();

    // ---- Bodies ----

    void compileModule(AST::Mod*);
    void compileFunctionBody(Seq<Stmt*>);
    void compileClassBody(AST::ClassDef*, int firstLineNumber);
    void statements(Seq<Stmt*>, unsigned first = 0, const Last& = { });
    void statement(Stmt*, const Last&);

    // ---- Expressions ----

    // The register the value is in: `dst` if there is one. Unless the caller holds a reference to it, the register is only
    // good until the next is allocated.
    RegisterID* expr(Expr*, RegisterID* dst = nullptr);
    void exprForEffect(Expr*);
    void jumpIf(Expr*, Label& target, bool condition);
    void jumpIfTo(Expr*, Label& target, bool condition, bool isTurnedRound = false);
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
        bool isAssignedInExpression { false }; // By :=.
        unsigned firstUse { 0 }; // It is the so-manieth of the variables that code was emitted for. 0: there has been none.
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
        RegisterID* savedSelf { nullptr }; // With: what to pass __exit__ first, or the empty value. The register after `saved`.
        Name name { nullptr }; // HandlerCleanup: what follows `as`.
        Range range { }; // With
        Scope* comprehension { nullptr }; // InlinedComprehension
        unsigned number { 0 }; // For PythonMark.
        Last isLast { }; // A loop: what comes after it.
        RegisterID* valueToReturn { nullptr }; // A loop: m_valueToReturn where it stands. (It may stand in a `finally` that a `return` is running.)
        bool ownsIterator { false }; // ForLoop: it goes through the value of a call. See beginTemporary().
        bool isMarked { true }; // There is a PythonMark where it begins and where it ends.
        unsigned instructionsAfterLoop { tooManyToCopy }; // Last::instructionsAfterJump, for a `break`.
        bool hasBreak { false }; // A loop.
    };

    // Where the ways out of a statement go that leave it on different lines.
    struct WayOut {
        int lineno;
        Ref<Label> label;
        Range place;
    };
    using ReturnsByLine = Vector<WayOut, 4>;

    struct PatternContext {
        struct Capture {
            Name name;
            Reg reg;
        };
        Vector<Capture, 4> captures;
        Label* fail { nullptr };
        // For a pattern written over several lines. CPython's code has on its stack what is still to be looked at and what has been
        // captured; where a pattern fails with something there, that is dropped first, which is of the line the pattern begins on.
        Label* failOnFirstLine { nullptr };
        unsigned stillToLookAt { 0 };
        // If the statement is the last of something: what comes then is of the line that the pattern failed on.
        ReturnsByLine* failsByLine { nullptr };
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
    void line(const Range&);
    void line(int lineno);
    // A jump of the program's. Its place is that of whatever came before it, as in CPython. (One that goes back looks for signals, so a
    // handler may find the frame there, and a traceback show the place.)
    void jump(Label&);
    // What follows is of no line: the program did not write it, and it is not reached from what it wrote last.
    void noLine();
    // It is of this line, but nobody is told that the line begins.
    void silentLine(const Range&);
    void silentLine(int lineno);
    // All that gets here is on this line, and what is laid out in front is of it too.
    void isStillOnLine(const Range&);
    // A jump that is there because of how the code is laid out here. In CPython there is none, or one that nobody is told of:
    // what handles exceptions is out of the way, at the end of the code.
    static Range rangeOfAttributeName(AST::Attribute*);
    void hiddenJump(Label&);
    void jump(Label&, JumpIs, bool takesInMore = false);
    // The last jump that came of a test of a constant. CPython's compiler finds out late that it is a jump: see jump().
    RefPtr<Label> m_targetOfJumpOfConstant;
    size_t m_whereBranchOfConstantLeads { notFound };
    bool jumpOfConstantLeadsHere()
    {
        size_t position = g.instructions().size();
        return m_whereBranchOfConstantLeads == position || (m_targetOfJumpOfConstant && !m_targetOfJumpOfConstant->isForward() && m_targetOfJumpOfConstant->location() == position);
    }
    void leaveStatement(Label& end, const Last&, JumpIs);
    void endOfStatement(Label& end, const Last& = { }, int lineOfBranch = 0);
    bool cannotGetHere();
    void emitImplicitReturn(const Last&, const Range& = { });
    void emitImplicitContinue(unsigned loop, const Range& = { });
    void emitImplicitJump(Last, const Range& = { });
    void leave(const Last&, const Range& = { });
    Last lastBeforeJump(Label&, const Last&, JumpIs, unsigned instructions = 1);
    bool isReplacedByCopy(const Last&, unsigned* jumpItStaysInFrontOf = nullptr);
    // Jumps that lead to a jump and stay what they are: isReplacedByCopy() has found one that they stay in front of. Where they are; or, of one that goes further
    // here than it does in CPython, where the jump leads that it stays in front of there.
    Vector<unsigned, 4> m_jumpsThatStay;
    Vector<Label*, 4> m_targetsOfJumpsStillJumpedTo;
    Label* targetReplacedByCopy(Label& end, const Last&);
    Label* m_targetReplacedByCopy { nullptr };
    bool canBranchTo(const Last&);
    void jumpIfOrLeave(Expr* test, Label& target, const Last&, ReturnsByLine&, bool condition = false);
    void emitReturnsByLine(ReturnsByLine&, const Last&);
    Label& labelForLine(ReturnsByLine&, int lineno, const Range& place = { });
    ReturnsByLine* m_endsOfClassBody { nullptr };
    Label& endOfClassBodyForLine(int lineno);
    // What jumpIf() does about the branches that go to the label.
    struct BranchTarget {
        Label* label { nullptr };
        Range placeOfJumpThere { }; // They are of this place: see comprehensionGenerator().
        ReturnsByLine* returns { nullptr }; // Each goes to one of these instead, by the line it is of.
        // Each is turned round, to jump over a jump
        std::optional<unsigned> loop { }; // to where this loop, one of m_blocks, goes on,
        Label* loopOfComprehension { nullptr }; // or here.
        bool returnsAreJumps { false }; // `returns`: by its place as well, which the jump is of.
    };
    BranchTarget m_branchTarget;
    Last m_leavingAs { }; // Blocks are being left by a `return` or a `continue`, or as if by one. OfFunction:
    RegisterID* m_valueToReturn { nullptr }; // with this. Null: None.
    void lineOfImplicitReturn(int lineno, const Last&);
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
    RegisterID* constant(JSValue value) { return g.pythonConstant(value && value.isCell() ? merged(value.asCell()) : value); }
    // Constants of the same type and value are one object, as in CPython: `a = (1, 2); b = (1, 2)`, and then `a is b`.
    JSCell* merged(JSCell*, JSCell** twin = nullptr);
    RegisterID* emptyValue() { return g.pythonConstant(JSValue()); }
    RegisterID* nameConstant(Name);
    // A name that is a value: the key of a dict of keyword arguments. One of co_consts.
    RegisterID* nameAsValue(Name);
    RegisterID* usedName(Name);
    // For co_consts.
    void noteConstant(JSValue, bool isUsed = true);
    void noteConstant(const CodeInfo::Constant&);
    void noteFoldedConstant(AST::Expr*, JSValue);
    void noteWhatConstantIsMadeOf(AST::Expr*);
    void noteConstantMade(JSValue);
    void finishConstants();
    RegisterID* stringConstant(const String&);
    RegisterID* load(RegisterID* dst, JSValue);
    JSValue constantValue(const AST::ConstantValue&);
    JSValue makeConstantValue(const AST::ConstantValue&);
    Vector<CodeInfo::Constant> m_foldedConstants;
    UncheckedKeyHashMap<String, JSCell*> m_mergedConstants;
    // Where they are in m_info.constants, by register, and what is in m_info.names.
    UncheckedKeyHashMap<int, unsigned> m_indexOfConstant;
    UncheckedKeyHashSet<UniquedStringImpl*> m_usedNames;
    bool m_hasKeywordNames { false }; // Among m_info.constants.
    bool tryConstantValue(Expr*, JSValue&);
    JSValue valueCPythonsCompilerWorksOut(Expr*);
    JSValue valueCPythonsCompilerWorksOut(AST::Operator, JSValue left, JSValue right);
    JSValue itemCPythonsCompilerWorksOut(JSValue base, Expr* index);
    JSValue constantSlice(AST::Slice*);
    static bool isMadeOfLiterals(AST::Slice*);
    PyTuple* tupleOfConstants(Seq<Expr*>);
    PySet* frozensetOf(PyTuple*);
    bool areConstants(Seq<Expr*>);
    RegisterID* exprToIterateOverOrSearch(Expr*, RegisterID* dst = nullptr);
    bool isAttributeOfSuper(AST::Attribute*);
    RegisterID* attributeOfSuper(AST::Attribute*, RegisterID* dst, RegisterID* self);
    // They cannot be reached: see statements().
    void deadStatements(Seq<Stmt*>, unsigned first = 0);
    void deadExpr(Expr*);
    // The same for a part of a statement. What comes next may be more of the statement, or the end of the function: it is of the line before.
    template<typename Function>
    void deadCode(const Function& function)
    {
        int lineBefore = m_markedLine;
        if (!m_sameLineDepth)
            noLine();
        {
            SameLine sameLine(*this);
            SetForScope cannotBeReached(m_cannotBeReached, true);
            function();
            g.emitUnreachable();
        }
        m_endOfDeadCode = g.instructions().size();
        line(lineBefore);
    }
    size_t m_endOfDeadCode { 0 };
    size_t m_whereLineOfBranchIsMarked { notFound };
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
    // Code that reads, assigns to or deletes it is about to be emitted, or has just been: see finishVariables().
    void noteUse(Variable& variable)
    {
        if (!variable.firstUse)
            variable.firstUse = ++m_usedVariableCount;
    }
    void noteBound(Variable& variable)
    {
        noteUse(variable);
        if (variable.boundBit >= 0)
            g.setPythonBoundVariables(g.pythonBoundVariables() | static_cast<uint64_t>(1) << variable.boundBit);
    }
    void noteUnbound(const Variable& variable)
    {
        if (variable.boundBit >= 0)
            g.setPythonBoundVariables(g.pythonBoundVariables() & ~(static_cast<uint64_t>(1) << variable.boundBit));
    }
    void declareComprehensionVariables(Scope*);
    void finishVariables();
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
    // What is written once may be compiled several times: a `finally` clause, for each way into it. Code in there is compiled once, and is one
    // constant (in CPython two that are equal, which makes them one).
    struct CompiledCode {
        UnlinkedFunctionExecutable* executable;
        std::optional<unsigned> index; // Of the functions of the code block.
        unsigned constant; // Of m_info.constants.
    };
    UncheckedKeyHashMap<Scope*, CompiledCode> m_compiledCode;
    void emitDefaults(AST::Arguments*, FunctionParts&, const Range&);
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
    void ifStatement(AST::If*, const Last&);
    void whileLoop(AST::While*, const Last&);
    void forLoop(AST::For*, const Last&);
    void asyncForLoop(AST::For*, const Last&);
    // The loop is left before its end. `exception`: by that, or null.
    void leaveAsyncFor(RegisterID* iterator, RegisterID* exception);
    void returnStatement(AST::Return*);
    void breakOrContinue(Stmt*, bool isBreak);
    void raiseStatement(AST::Raise*);
    void tryStatement(AST::Try*, const Last&);
    void tryFinally(AST::Try*, const Last&);
    void tryExcept(AST::Try*, const Last&);
    void tryStarExcept(AST::Try*, const Last&);
    void withStatement(AST::With*, unsigned index, const Last&);
    void assertStatement(AST::Assert*, const Last&);
    void importStatement(AST::Import*);
    void importFrom(AST::ImportFrom*);
    void matchStatement(AST::Match*, const Last&);
    ImportMode importMode(bool isLazy, bool isStar);

    // ---- Blocks ----

    void pushBlock(Block&&);
    void popBlock();
    void markBlock(const Block&, bool begins);
    // Leaves the blocks down to the innermost loop, or, without `stopAtLoop`, all of them. The code emitted from then on is
    // outside their try ranges, until `suspended` is given to resumeTry(). Returns the loop.
    void unwindBlocks(const Range&, size_t count, Vector<TryContext>& suspended);
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
    // SynthesizedCatch: CPython's code has no such handler, and tools are not told that it catches.
    Try beginTry(HandlerType = HandlerType::Catch);
    void endTry(Try&);
    // Starts the handler. Returns what was thrown.
    Reg beginHandler(Try&, Reg* previous = nullptr, bool isOfLine = false);
    // What follows the code of an `async for` that `whole` covers: an exception leaves the loop.
    void asyncForHandler(Try& whole, RegisterID* iterator);
    // The same, as a Python exception that is now the one being handled. `previous`: the one that was.
    Reg beginExceptionHandler(Try&, Reg& previous, bool isOfLine = false);
    void restoreHandledException(RegisterID* previous);
    void reraise(RegisterID* exception);
    void reraiseOutOfHandler(RegisterID* exception, RegisterID* previous);

    // ---- Expressions ----

    RegisterID* boolOp(AST::BoolOp*, RegisterID* dst, bool* jumpThatFollowsIsOfNoLine = nullptr);
    void boolOperand(Expr*, RegisterID* result, Label* ifTrue, Label* ifFalse, const Range& rangeOfTest, const Range& rangeOfOperation, Label& whenConstantSettles, Label* endOfNested);
    RegisterID* chain(AST::Expr*, RegisterID* dst);
    static bool takeFormatApart(AST::BinOp*, Vector<PartOfFormat, 8>&);
    RegisterID* formatTakenApart(AST::BinOp*, const Vector<PartOfFormat, 8>&, RegisterID* dst);
    RegisterID* unaryOp(AST::UnaryOp*, RegisterID* dst);
    struct EndOfIfExp {
        Label& label;
        bool isReachedFromNoLine { false };
        bool jumpThereIsOfNoLine { false }; // The one that comes after the expression that is given this.
        bool isCopied { false }; // isCopiedWhatComesAfter()
    };
    bool isCopiedWhatComesAfter(Expr*);
    bool isCopiedWhatComesAfterStatement();
    // What comes after a statement.
    struct StatementsAfter {
        Seq<Stmt*> body;
        unsigned first { 0 };
        const Last& isLast;
        unsigned blockCount { 0 }; // The blocks that they are in.
        // The statements are part of another statement, which these come after.
        const StatementsAfter* enclosing { nullptr };
        // Where the last of them ends, the other statement ends: the statements of an `if` without `else`, those of an `else`.
        bool endsWhereEnclosingEnds { false };
        unsigned instructionsBefore { 0 }; // What CPython's compiler has in front of them: see endOfProtectedStatements().
    };
    enum class ComesFirst : bool { PartOfStatement, Nothing };
    unsigned instructionsUntilFunctionEnds(const StatementsAfter&, ComesFirst);
    unsigned instructionsOnTheWayOut(bool withValue, unsigned firstBlock, unsigned blockCount);
    enum class JumpsHere : bool { Count, AreTakenIn };
    std::optional<int> lineOfOnlyWayHere(JumpsHere);
    struct {
        size_t position { std::numeric_limits<size_t>::max() };
        int lineno { 0 };
        bool hasBreak { false };
    } m_endOfForLoop;
    void endOfProtectedStatements(JumpsHere = JumpsHere::Count, Label* end = nullptr);
    // The statement that is being compiled, and what comes after it. (Of a compound statement: the last of its statements that was.)
    Stmt* m_statement { nullptr };
    const StatementsAfter* m_statementsAfter { nullptr };
    bool m_statementsEndWhereStatementEnds { false }; // For the next call of statements().
    Expr* m_returnedExpression { nullptr }; // The body of a lambda.
    RegisterID* ifExp(AST::IfExp*, RegisterID* dst, EndOfIfExp* enclosing = nullptr);
    RegisterID* compare(AST::Compare*, RegisterID* dst);
    RegisterID* emitCompare(RegisterID* dst, RegisterID* left, RegisterID* right, AST::CompareKind, Expr* rightExpr);
    void checkCompare(AST::Compare*);
    // `callee`: where to leave what is called.
    RegisterID* call(AST::Call*, RegisterID* dst, RegisterID* callee = nullptr);
    enum class GoesThrough : uint8_t { All, Any, Tuple, List, Set };
    RegisterID* callOnGeneratorExpression(AST::Call*, RegisterID* callee, JSValue builtin, GoesThrough, RegisterID* dst, unsigned temporary);
    // callee(pushed..., *args, **keywords). `injected`: a variable to pass after the positional arguments.
    RegisterID* emitCall(RegisterID* dst, RegisterID* callee, std::span<RegisterID* const> pushed, Seq<Expr*> args, Seq<AST::Keyword*> keywords, Name injected, const Range&, unsigned temporary = 0);

    // Marks for runtime/PyTemporaries.cpp: the value of a call, a subscript or a generator expression that goes in a temporary register and is
    // used once.
    static bool makesTemporary(Expr* node) { return node->is<AST::Call>() || node->is<AST::Subscript>() || node->is<AST::GeneratorExp>(); }
    // For what is about to compile `node` as an operand of its own: the number of the value, which it passes to markTemporaryUsed()
    // right before each instruction that may turn out to be the use. 0: the node is not of that kind.
    unsigned beginTemporary(Expr* node);
    // For a loop that is about to compile what it goes through, and for `in`, what it searches. Nothing but a call or a generator expression: in `for x in (g := f()):` there
    // is a variable as well.
    static bool mayMakeGenerator(Expr* node) { return node->is<AST::Call>() || node->is<AST::GeneratorExp>(); }
    unsigned beginTemporaryOfLoop(Expr* node) { return mayMakeGenerator(node) ? beginTemporary(node) : 0; }
    // For what compiles `node`, first thing: the number, which it passes to markTemporaryMade() right before each instruction that may
    // turn out to make the value. 0: nobody asked.
    unsigned takeTemporary(Expr* node);
    void markTemporaryMade(unsigned number);
    void markTemporaryUsed(unsigned number, TemporaryUse::Kind, RegisterID*, bool isLast);
    Expr* m_temporaryNode { nullptr };
    unsigned m_temporaryNumber { 0 };
    unsigned m_unpackedTemporary { 0 }; // From assign() to unpack().
    unsigned m_temporaryCount { 0 };
    Vector<unsigned> m_yieldPointsWhenTemporaryWasMade; // By number.
    unsigned m_storedTemporary { 0 }; // For store(): the number of the value, if it is to mark the store as its use.
    RegisterID* emitSimpleCall(RegisterID* dst, RegisterID* callee, std::initializer_list<RegisterID*> args, const Range&, unsigned temporary = 0, unsigned temporaryToGoThrough = 0);
    // What op_py_get_method gave is called: with `self` before the arguments, unless it is the empty value.
    RegisterID* emitMethodCall(RegisterID* dst, RegisterID* callee, RegisterID* self, std::initializer_list<RegisterID*> args, const Range&);
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
    RegisterID* yield(AST::Yield*, RegisterID* dst, bool isForEffect = false);
    RegisterID* yieldFrom(AST::YieldFrom*, RegisterID* dst);
    RegisterID* await(AST::Await*, RegisterID* dst);
    RegisterID* emitYield(RegisterID* dst, RegisterID* value); // No `dst`: what is sent is not wanted.
    // `yield from` an iterator, or `await` one: the value it finishes with.
    RegisterID* emitDelegate(RegisterID* dst, RegisterID* iterator, int firstMode = 0);
    RegisterID* emitAwait(RegisterID* dst, RegisterID* awaitable, int where);
    void unpack(Seq<Expr*> targets, RegisterID* value, const Range&);

    RegisterID* comprehension(Expr*, ComprehensionKind, Seq<AST::Comprehension*>, Expr* element, Expr* value, RegisterID* dst);
    void comprehensionGenerator(ComprehensionKind, Seq<AST::Comprehension*>, unsigned index, Expr* element, Expr* value, RegisterID* result, RegisterID* firstIterator, bool ownsFirstIterator, const Range&);
    void comprehensionElement(ComprehensionKind, Expr* element, Expr* value, RegisterID* result);

    // ---- Patterns ----

    void pattern(AST::Pattern*, RegisterID* subject, PatternContext&);
    void subpattern(AST::Pattern*, RegisterID* subject, PatternContext&);
    void capture(Name, RegisterID* value, const Range&, PatternContext&);
    Label& whereToFail(PatternContext&);
    void emitFailLabels(const Range&, PatternContext&);
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
    RegisterID* m_classCellOfMethods { nullptr }; // In the body of a class that itself means the __class__ of an enclosing method by the name.

    UncheckedKeyHashMap<Scope*, std::unique_ptr<VariableMap>> m_variables;
    Vector<Scope*, 2> m_overlays; // The inlined comprehensions being compiled, the innermost last.
    RegisterID* m_namespace { nullptr }; // Module, class: where the names live.
    struct CellParameter {
        RegisterID* reg;
        Name name;
    };
    Vector<CellParameter, 2> m_cellParameters; // The parameters that enclosed functions use.
    unsigned m_boundBitCount { 0 };
    unsigned m_usedVariableCount { 0 };
    void markLine(int lineno);
    void place(const Range&);
    int m_markedLine { -1 }; // What line() was last given, and where in the instructions.
    Range m_place; // All of it, if more than the line was given.
    size_t m_markedPosition { 0 };
    // What has been emitted since without a mark is of the line that is marked next.
    size_t m_whereLineBegins { notFound };
    size_t m_start { 0 }; // Where the program's own code begins.
    unsigned m_sameLineDepth { 0 };
    bool m_cannotBeReached { false }; // See deadStatements().
    size_t m_statementStart { std::numeric_limits<size_t>::max() }; // Where in the instructions the last statement begins.
    unsigned m_blockCount { 0 };

    Vector<Block, 8> m_blocks;

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
