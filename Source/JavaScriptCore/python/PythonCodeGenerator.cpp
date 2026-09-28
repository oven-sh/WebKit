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

#include "config.h"
#include "PythonCodeGenerator.h"

#include "BytecodeGenerator.h"
#include "BytecodeGeneratorBaseInlines.h"
#include "BytecodeStructs.h"
#include "JSBigInt.h"
#include "PythonOperations.h"
#include "PythonSequences.h"
#include "PythonUnparse.h"
#include "JSCInlines.h"
#include "JSCellButterfly.h"
#include "JSGenerator.h"
#include "LinkTimeConstant.h"
#include "PythonCommonNames.h"
#include "TaggedArithmetic.h"
#include "UnlinkedFunctionExecutable.h"
#include <wtf/text/MakeString.h>

namespace JSC { namespace Python {

// A Python syntax tree to JavaScriptCore bytecode. See README.md for how the one language is laid over the other.
//
// As in NodesCodegen.cpp, what emits an expression is given a register that the value would be welcome in, or none, and says which
// register it is in. A local variable's own register is only ever written by the last instruction of what is assigned to it.
class CodeGenerator {
public:
    CodeGenerator(BytecodeGenerator& generator, Arena& arena, SymbolTable& table, Block& block, const FunctionInfo& info, SyntaxError& error)
        : g(generator)
        , m_vm(generator.vm())
        , m_names(m_vm.pythonNames())
        , m_arena(arena)
        , m_table(table)
        , m_block(block)
        , m_info(info)
        , m_error(error)
        , m_private(info.privateName.isNull() ? nullptr : &info.privateName)
    {
    }

    void generate(void* root)
    {
        m_details = makeUnique<CodeDetails>();
        for (auto& name : m_info.parameterNames) {
            // The mapping that the names of a class body are looked up in is a parameter of ours, and not one of Python's.
            if (!m_info.usesNamespace)
                m_details->variableNames.append(name);
        }
        generateKind(root);
        for (Symbol& symbol : m_block.symbols) {
            if (symbol.scope == NameScope::Cell)
                m_details->cellVariables.append(*symbol.name);
        }
        auto byCodePoint = [] (auto& a, auto& b) { return codePointCompareLessThan(a.string(), b.string()); };
        std::ranges::sort(m_details->cellVariables, byCodePoint);
        describeFrame(byCodePoint);
        m_info.details = WTF::move(m_details);
    }

    // What a frame object needs to know.
    void describeFrame(auto& byCodePoint)
    {
        if (!m_info.isGeneratorBody)
            m_details->frameObjectRegister = g.m_pythonFrameObjectRegister->virtualRegister();
        m_details->scopeRegister = g.scopeRegister()->virtualRegister();
        if (!isFunctionLike())
            return;
        HashSet<UniquedStringImpl*> seen;
        auto add = [&] (const Identifier& name, bool isInEnvironment) {
            if (!seen.add(name.impl()).isNewEntry)
                return;
            RegisterID* local = isInEnvironment ? nullptr : m_locals.get(name.impl());
            m_details->frameVariables.append({ name, local ? local->virtualRegister() : VirtualRegister() });
        };
        for (auto& name : m_details->variableNames)
            add(name, m_block.scopeOf(name) == NameScope::Cell);
        for (auto& name : m_details->cellVariables)
            add(name, true);
        Vector<Identifier> freeVariables = m_info.freeVariables;
        std::ranges::sort(freeVariables, byCodePoint);
        for (auto& name : freeVariables)
            add(name, true);

        if (m_info.isGeneratorBody) {
            // Whatever has the frame of a suspended generator can see its variables, whether or not the generator will look at them again.
            g.m_localsToSaveAtEveryYield.append(m_details->scopeRegister);
            for (auto& variable : m_details->frameVariables) {
                if (variable.location.isValid())
                    g.m_localsToSaveAtEveryYield.append(variable.location);
            }
        }
    }

    void generateKind(void* root)
    {
        switch (m_info.kind) {
        case CodeKind::Module:
        case CodeKind::Expression:
        case CodeKind::Interactive:
            generateModule(*static_cast<Module*>(root));
            break;
        case CodeKind::Function: {
            auto& node = *static_cast<FunctionDef*>(root);
            collectDeletedNames(node.body);
            generateFunction([&] {
                emit(node.body);
            });
            break;
        }
        case CodeKind::Lambda: {
            auto& node = *static_cast<Lambda*>(root);
            generateFunction([&] {
                Reg value = emit(node.body);
                g.emitReturn(value.get());
            });
            break;
        }
        case CodeKind::Class:
            generateClassBody(*static_cast<ClassDef*>(root));
            break;
        case CodeKind::GeneratorExpression:
            generateGeneratorExpression(*static_cast<GeneratorExp*>(root));
            break;
        case CodeKind::Comprehension:
            generateComprehension(*static_cast<Expression*>(root));
            break;
        case CodeKind::Annotations:
            generateAnnotations(root);
            break;
        }
    }

private:
    using Reg = RefPtr<RegisterID>;

    // ---- Errors and positions

    void fail(String&& message, const Node& node)
    {
        if (!m_error)
            m_error = { SyntaxError::Kind::SyntaxError, false, WTF::move(message), node.line, node.column, node.endLine, node.endColumn };
    }

    // Says where in the source what is emitted next comes from, for when it raises.
    void mark(const Node& node)
    {
        g.emitExpressionInfo(JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end));
    }

    // ---- Registers

    Reg destination(RegisterID* dst) { return dst ? dst : g.newTemporary(); }
    // For what writes its result before it has read all that it reads.
    Reg temporaryDestination(RegisterID* dst) { return dst && dst->isTemporary() ? dst : g.newTemporary(); }

    RegisterID* finish(RegisterID* dst, RegisterID* result)
    {
        if (!dst || dst == result)
            return result;
        return g.move(dst, result);
    }

    void emitInto(RegisterID* target, Expression* expression)
    {
        RegisterID* result = emit(expression, target);
        if (result != target)
            g.move(target, result);
    }

    // In a register that nothing else will write to while it is wanted.
    Reg emitToTemporary(Expression* expression)
    {
        Reg temporary = g.newTemporary();
        emitInto(temporary.get(), expression);
        return temporary;
    }

    RegisterID* constant(JSValue value) { return g.addConstantValue(value); }
    RegisterID* stringConstant(const Identifier& string) { return g.addConstantValue(g.addStringConstant(string)); }
    RegisterID* none() { return constant(jsUndefined()); }

    // ---- The runtime

    RegisterID* runtime() { return g.moveLinkTimeConstant(nullptr, LinkTimeConstant::pyRuntimeFunctions); }

    // A call to a function of PythonRuntimeFunctions.h.
    RegisterID* emitRuntimeCall(RegisterID* dst, ASCIILiteral name, std::initializer_list<RegisterID*> arguments, const Node& node)
    {
        Reg function = g.newTemporary();
        g.emitGetById(function.get(), runtime(), Identifier::fromString(m_vm, name));
        CallArguments call(g, nullptr, arguments.size());
        g.emitLoad(call.thisRegister(), jsUndefined());
        unsigned i = 0;
        for (RegisterID* argument : arguments)
            g.move(call.argumentRegister(i++), argument);
        Reg result = destination(dst);
        return g.emitCall(result.get(), function.get(), NoExpectedFunction, call, JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end), DebuggableCall::No);
    }

    // The same, of registers that are one after another as the elements of a display are.
    RegisterID* emitRuntimeCall(RegisterID* dst, ASCIILiteral name, const Vector<Reg, 8>& arguments, const Node& node)
    {
        Reg function = g.newTemporary();
        g.emitGetById(function.get(), runtime(), Identifier::fromString(m_vm, name));
        CallArguments call(g, nullptr, arguments.size());
        g.emitLoad(call.thisRegister(), jsUndefined());
        for (unsigned i = 0; i < arguments.size(); ++i)
            g.move(call.argumentRegister(i), arguments[i].get());
        Reg result = destination(dst);
        return g.emitCall(result.get(), function.get(), NoExpectedFunction, call, JSTextPosition(node.start), JSTextPosition(node.start), JSTextPosition(node.end), DebuggableCall::No);
    }

    // ---- Python's opcodes

    RegisterID* emitBinaryOperation(RegisterID* dst, BinaryOperator op, bool inPlace, RegisterID* left, RegisterID* right)
    {
        OpPyBinaryOp::emit(&g, dst, left, right, static_cast<unsigned>(op) | (inPlace ? inPlaceOperatorFlag : 0), g.nextValueProfileIndex());
        return dst;
    }

    RegisterID* emitCompare(RegisterID* dst, ComparisonOperator op, RegisterID* left, RegisterID* right)
    {
        OpPyCompareOp::emit(&g, dst, left, right, static_cast<unsigned>(op), g.nextValueProfileIndex());
        return dst;
    }

    RegisterID* emitGetAttribute(RegisterID* dst, RegisterID* base, const Identifier& name)
    {
        addOnce(m_details->names, name);
        OpPyGetAttr::emit(&g, dst, base, g.addConstant(name), g.nextValueProfileIndex());
        return dst;
    }

    void emitSetAttribute(RegisterID* base, const Identifier& name, RegisterID* value)
    {
        addOnce(m_details->names, name);
        OpPySetAttr::emit(&g, base, g.addConstant(name), value);
    }

    RegisterID* emitGetItem(RegisterID* dst, RegisterID* base, RegisterID* key)
    {
        OpPyGetItem::emit(&g, dst, base, key, g.nextValueProfileIndex());
        return dst;
    }

    void emitJumpIfTrue(RegisterID* value, Label& target)
    {
        Reg truth = g.newTemporary();
        OpPyToBool::emit(&g, truth.get(), value);
        g.emitJumpIfTrue(truth.get(), target);
    }

    void emitJumpIfFalse(RegisterID* value, Label& target)
    {
        Reg truth = g.newTemporary();
        OpPyToBool::emit(&g, truth.get(), value);
        g.emitJumpIfFalse(truth.get(), target);
    }

    // Jumps if the condition is (or is not) true, without making a value of `not`, `and` and `or`.
    void emitBranch(Expression* condition, Label& target, bool jumpIfTrue)
    {
        if (auto* unary = condition->tryAs<UnaryOp>(); unary && unary->op == UnaryOperator::Not)
            return emitBranch(unary->operand, target, !jumpIfTrue);
        if (auto* boolean = condition->tryAs<BoolOp>()) {
            // `a and b` is false as soon as one is, and `a or b` is true as soon as one is.
            bool shortCircuitsOn = boolean->op == BooleanOperator::Or;
            if (shortCircuitsOn == jumpIfTrue) {
                for (Expression* value : boolean->values)
                    emitBranch(value, target, jumpIfTrue);
                return;
            }
            Ref<Label> skip = g.newLabel();
            for (size_t i = 0; i + 1 < boolean->values.size(); ++i)
                emitBranch(boolean->values[i], skip.get(), shortCircuitsOn);
            emitBranch(boolean->values.back(), target, jumpIfTrue);
            g.emitLabel(skip.get());
            return;
        }
        if (auto* comparison = condition->tryAs<Compare>(); comparison && comparison->ops.size() == 1 && isNoneConstant(comparison->comparators[0])
            && (comparison->ops[0] == ComparisonOperator::Is || comparison->ops[0] == ComparisonOperator::IsNot)) {
            // x is None
            Reg value = emit(comparison->left);
            Reg isNone = g.newTemporary();
            g.emitIsUndefinedOrNull(isNone.get(), value.get());
            if ((comparison->ops[0] == ComparisonOperator::Is) == jumpIfTrue)
                g.emitJumpIfTrue(isNone.get(), target);
            else
                g.emitJumpIfFalse(isNone.get(), target);
            return;
        }
        Reg value = emit(condition);
        mark(*condition);
        if (jumpIfTrue)
            emitJumpIfTrue(value.get(), target);
        else
            emitJumpIfFalse(value.get(), target);
    }

    static bool isNoneConstant(Expression* expression)
    {
        auto* constant = expression->tryAs<Constant>();
        return constant && constant->type == Constant::Type::None;
    }

    // ---- Names

    const Identifier& mangle(const Identifier& name) { return SymbolTable::mangle(m_vm, m_arena, m_private, name); }

    bool isFunctionLike() const { return m_block.isFunctionLike(); }

    enum class Where : uint8_t {
        Register, // A local variable of a function, that no other function uses.
        Closure, // A variable of an environment: this function's, or one that it is in.
        Global, // A property of the module's namespace.
        Namespace, // In a class body: an item of the namespace being filled in, and failing that a global.
        NamespaceOrClosure, // In a class body: the same, and failing that a variable of a function that the class is in.
    };

    struct Location {
        Where where;
        RegisterID* local { nullptr };
        bool isAlwaysBound { false };
    };

    static void addOnce(Vector<Identifier>& names, const Identifier& name)
    {
        if (!names.contains(name))
            names.append(name);
    }

    // Where the name is, noting that it has been used, for co_varnames and co_names.
    Location locateAndNote(const Identifier& name)
    {
        Location location = locate(name);
        switch (location.where) {
        case Where::Register:
            if (m_locals.contains(name.impl()))
                addOnce(m_details->variableNames, name);
            break;
        case Where::Closure:
            break;
        case Where::Global:
        case Where::Namespace:
        case Where::NamespaceOrClosure:
            addOnce(m_details->names, name);
            break;
        }
        return location;
    }

    Location locate(const Identifier& name)
    {
        // The variables of a comprehension hide whatever else has their names.
        for (unsigned i = m_comprehensionScopes.size(); i--;) {
            auto iterator = m_comprehensionScopes[i].find(name.impl());
            if (iterator == m_comprehensionScopes[i].end())
                continue;
            if (iterator->value)
                return { Where::Register, iterator->value.get(), false };
            return { Where::Closure };
        }
        bool isClass = m_info.usesNamespace;
        // What is in a class without being part of its body looks there for what is not its own.
        bool looksInClass = isClass || m_info.canSeeClassScope;
        switch (m_block.scopeOf(name)) {
        case NameScope::Local:
            if (isClass)
                return { Where::Namespace };
            if (!isFunctionLike())
                return { Where::Global };
            // In the body of a generator, the parameters are variables of the function that made it.
            if (RegisterID* local = m_locals.get(name.impl()))
                return { Where::Register, local, m_alwaysBound.contains(name.impl()) };
            return { Where::Closure };
        case NameScope::Cell:
            return { Where::Closure };
        case NameScope::Free:
            return { looksInClass ? Where::NamespaceOrClosure : Where::Closure };
        case NameScope::GlobalExplicit:
            return { Where::Global };
        case NameScope::GlobalImplicit:
        case NameScope::Unknown:
            return { looksInClass ? Where::Namespace : Where::Global };
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void emitCheckBound(RegisterID* value, const Identifier& name, const Node& node)
    {
        mark(node);
        OpCheckTdz::emit(&g, value, stringConstant(name));
    }

    RegisterID* emitLoadClosure(RegisterID* dst, const Identifier& name, const Node& node)
    {
        Variable variable = g.variable(name);
        Reg scope = g.emitResolveScope(nullptr, variable);
        Reg result = destination(dst);
        g.emitGetFromScope(result.get(), scope.get(), variable, ThrowIfNotFound);
        emitCheckBound(result.get(), name, node);
        return result.get();
    }

    void emitStoreClosure(const Identifier& name, RegisterID* value)
    {
        Variable variable = g.variable(name);
        Reg scope = g.emitResolveScope(nullptr, variable);
        g.emitPutToScope(scope.get(), variable, value, ThrowIfNotFound, InitializationMode::NotInitialization);
    }

    RegisterID* emitLoadName(RegisterID* dst, const Identifier& rawName, const Node& node)
    {
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            if (!location.isAlwaysBound)
                emitCheckBound(location.local, name, node);
            // What it is now is what is wanted, and it could be changed before that is used: by an assignment expression further on, or by anything that is
            // called and gets hold of the frame, whose f_locals writes through.
            if (!dst)
                return g.move(g.newTemporary(), location.local);
            return finish(dst, location.local);
        case Where::Closure:
            return emitLoadClosure(dst, name, node);
        case Where::Global: {
            Reg result = destination(dst);
            mark(node);
            OpPyLoadGlobal::emit(&g, result.get(), m_globals.get(), m_builtins.get(), g.addConstant(name), g.nextValueProfileIndex());
            return result.get();
        }
        case Where::Namespace:
            return emitRuntimeCall(dst, "loadName"_s, { m_namespace.get(), m_globals.get(), m_builtins.get(), stringConstant(name) }, node);
        case Where::NamespaceOrClosure: {
            Reg result = temporaryDestination(dst);
            emitRuntimeCall(result.get(), "loadFromNamespace"_s, { m_namespace.get(), stringConstant(name) }, node);
            Ref<Label> found = g.newLabel();
            OpJneqPtr::emit(&g, result.get(), marker(), found->bind(&g));
            emitLoadClosure(result.get(), name, node);
            g.emitLabel(found.get());
            return finish(dst, result.get());
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    void emitStoreName(const Identifier& rawName, RegisterID* value, const Node& node)
    {
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            if (location.local != value)
                g.move(location.local, value);
            return;
        case Where::Closure:
            emitStoreClosure(name, value);
            return;
        case Where::Global:
            g.emitDirectPutById(m_globals.get(), name, value);
            return;
        case Where::Namespace:
        case Where::NamespaceOrClosure:
            mark(node);
            OpPySetItem::emit(&g, m_namespace.get(), stringConstant(name), value);
            return;
        }
    }

    void emitDeleteName(const Identifier& rawName, const Node& node)
    {
        const Identifier& name = mangle(rawName);
        Location location = locateAndNote(name);
        switch (location.where) {
        case Where::Register:
            emitCheckBound(location.local, name, node);
            g.moveEmptyValue(location.local);
            return;
        case Where::Closure: {
            emitLoadClosure(nullptr, name, node);
            Reg empty = g.newTemporary();
            g.moveEmptyValue(empty.get());
            emitStoreClosure(name, empty.get());
            return;
        }
        case Where::Global:
            emitRuntimeCall(nullptr, "deleteGlobal"_s, { m_globals.get(), stringConstant(name) }, node);
            return;
        case Where::Namespace:
        case Where::NamespaceOrClosure:
            emitRuntimeCall(nullptr, "deleteName"_s, { m_namespace.get(), stringConstant(name) }, node);
            return;
        }
    }

    // The register to have a value put straight into, if that is all that assigning it to the name takes.
    RegisterID* registerForStore(Expression* target)
    {
        auto* name = target->tryAs<Name>();
        if (!name)
            return nullptr;
        Location location = locate(mangle(*name->id));
        if (location.where != Where::Register)
            return nullptr;
        locateAndNote(mangle(*name->id));
        return location.local;
    }

    // ---- Constants

    RegisterID* emitConstant(RegisterID* dst, Constant& node)
    {
        switch (node.type) {
        case Constant::Type::None:
            return g.emitLoad(dst, jsUndefined());
        case Constant::Type::True:
            return g.emitLoad(dst, jsBoolean(true));
        case Constant::Type::False:
            return g.emitLoad(dst, jsBoolean(false));
        case Constant::Type::Ellipsis: {
            Reg result = destination(dst);
            return g.emitGetById(result.get(), runtime(), Identifier::fromString(m_vm, "Ellipsis"_s));
        }
        case Constant::Type::Integer:
            if (node.integer <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
                return g.emitLoad(dst, jsNumber(static_cast<int32_t>(node.integer)));
            // The generator knows a constant that it has seen before by the address of its digits, so they have to outlive it.
            return g.emitLoad(dst, g.addBigIntConstant(m_arena.identifiers().makeIdentifier(m_vm, String::number(node.integer).span8()), 10, false));
        case Constant::Type::BigInteger:
            return g.emitLoad(dst, g.addBigIntConstant(*node.text, node.radix, false));
        case Constant::Type::Float:
            return g.emitLoad(dst, jsTaggedFloat(node.real));
        case Constant::Type::Imaginary:
            // It is a cell of this realm's, so it cannot be a constant of code that any realm may run.
            return emitRuntimeCall(dst, "newComplex"_s, { constant(jsDoubleNumber(node.real)) }, node);
        case Constant::Type::String:
            return g.emitLoad(dst, *node.text);
        case Constant::Type::Bytes:
            return emitRuntimeCall(dst, "newBytes"_s, { stringConstant(*node.text) }, node);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // ---- Expressions

    RegisterID* emit(Expression* expression, RegisterID* dst = nullptr)
    {
        if (!m_vm.isSafeToRecurse()) [[unlikely]] {
            fail("maximum recursion depth exceeded during compilation"_s, *expression);
            return g.emitLoad(dst, jsUndefined());
        }
        switch (expression->kind) {
        case Expression::Kind::Constant:
            return emitConstant(dst, expression->as<Constant>());
        case Expression::Kind::Name:
            return emitLoadName(dst, *expression->as<Name>().id, *expression);
        case Expression::Kind::BinOp: {
            auto& node = expression->as<BinOp>();
            Reg left = emit(node.left);
            Reg right = emit(node.right);
            Reg result = destination(dst);
            mark(node);
            return emitBinaryOperation(result.get(), node.op, false, left.get(), right.get());
        }
        case Expression::Kind::UnaryOp: {
            auto& node = expression->as<UnaryOp>();
            // -1 is a constant.
            if (auto* operand = node.operand->tryAs<Constant>(); operand && node.op == UnaryOperator::USub) {
                if (operand->type == Constant::Type::Integer && operand->integer <= static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) + 1)
                    return g.emitLoad(dst, jsNumber(static_cast<int32_t>(-static_cast<int64_t>(operand->integer))));
                if (operand->type == Constant::Type::Float)
                    return g.emitLoad(dst, jsTaggedFloat(-operand->real));
            }
            Reg operand = emit(node.operand);
            Reg result = destination(dst);
            mark(node);
            OpPyUnaryOp::emit(&g, result.get(), operand.get(), static_cast<unsigned>(node.op), g.nextValueProfileIndex());
            return result.get();
        }
        case Expression::Kind::BoolOp: {
            auto& node = expression->as<BoolOp>();
            Reg result = temporaryDestination(dst);
            Ref<Label> end = g.newLabel();
            for (size_t i = 0; i < node.values.size(); ++i) {
                emitInto(result.get(), node.values[i]);
                if (i + 1 == node.values.size())
                    break;
                mark(*node.values[i]);
                if (node.op == BooleanOperator::And)
                    emitJumpIfFalse(result.get(), end.get());
                else
                    emitJumpIfTrue(result.get(), end.get());
            }
            g.emitLabel(end.get());
            return finish(dst, result.get());
        }
        case Expression::Kind::IfExp: {
            auto& node = expression->as<IfExp>();
            Reg result = temporaryDestination(dst);
            Ref<Label> otherwise = g.newLabel();
            Ref<Label> end = g.newLabel();
            emitBranch(node.test, otherwise.get(), false);
            emitInto(result.get(), node.body);
            g.emitJump(end.get());
            g.emitLabel(otherwise.get());
            emitInto(result.get(), node.orElse);
            g.emitLabel(end.get());
            return finish(dst, result.get());
        }
        case Expression::Kind::Compare:
            return emitComparison(dst, expression->as<Compare>());
        case Expression::Kind::NamedExpr: {
            auto& node = expression->as<NamedExpr>();
            Reg value = emitToTemporary(node.value);
            emitStoreName(*node.target->as<Name>().id, value.get(), node);
            return finish(dst, value.get());
        }
        case Expression::Kind::Attribute: {
            auto& node = expression->as<Attribute>();
            Reg base = emit(node.value);
            Reg result = destination(dst);
            mark(node);
            return emitGetAttribute(result.get(), base.get(), mangle(*node.attribute));
        }
        case Expression::Kind::Subscript: {
            auto& node = expression->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            Reg result = destination(dst);
            mark(node);
            return emitGetItem(result.get(), base.get(), key.get());
        }
        case Expression::Kind::Slice: {
            auto& node = expression->as<Slice>();
            Reg lower = node.lower ? Reg(emit(node.lower)) : Reg(none());
            Reg upper = node.upper ? Reg(emit(node.upper)) : Reg(none());
            Reg step = node.step ? Reg(emit(node.step)) : Reg(none());
            return emitRuntimeCall(dst, "newSlice"_s, { lower.get(), upper.get(), step.get() }, node);
        }
        case Expression::Kind::Tuple:
            return emitSequenceDisplay(dst, expression->as<Tuple>().elements, Display::Tuple, *expression);
        case Expression::Kind::List:
            return emitSequenceDisplay(dst, expression->as<List>().elements, Display::List, *expression);
        case Expression::Kind::Set:
            return emitSequenceDisplay(dst, expression->as<Set>().elements, Display::Set, *expression);
        case Expression::Kind::Dict:
            return emitDictDisplay(dst, expression->as<Dict>());
        case Expression::Kind::Call:
            return emitCall(dst, expression->as<Call>());
        case Expression::Kind::Lambda: {
            auto& node = expression->as<Lambda>();
            return emitFunction(dst, CodeKind::Lambda, m_arena.identifiers().makeIdentifier(m_vm, "<lambda>"_span8), node.arguments, node, &node, false);
        }
        case Expression::Kind::JoinedStr:
            return emitJoinedString(dst, expression->as<JoinedStr>());
        case Expression::Kind::FormattedValue: {
            // Only ever inside a JoinedStr, but a format specification can be one alone.
            return emitFormattedValue(dst, expression->as<FormattedValue>());
        }
        case Expression::Kind::ListComp: {
            auto& node = expression->as<ListComp>();
            return emitComprehension(dst, node, ComprehensionType::List, node.generators, node.element, nullptr);
        }
        case Expression::Kind::SetComp: {
            auto& node = expression->as<SetComp>();
            return emitComprehension(dst, node, ComprehensionType::Set, node.generators, node.element, nullptr);
        }
        case Expression::Kind::DictComp: {
            auto& node = expression->as<DictComp>();
            return emitComprehension(dst, node, ComprehensionType::Dict, node.generators, node.key, node.value);
        }
        case Expression::Kind::GeneratorExp:
            return emitGeneratorExpression(dst, expression->as<GeneratorExp>());
        case Expression::Kind::Yield:
            return emitYield(dst, expression->as<Yield>());
        case Expression::Kind::YieldFrom:
            return emitYieldFrom(dst, expression->as<YieldFrom>());
        case Expression::Kind::Starred:
            fail("can't use starred expression here"_s, *expression);
            return g.emitLoad(dst, jsUndefined());
        case Expression::Kind::Await:
            return emitAwait(dst, expression->as<Await>());
        case Expression::Kind::TemplateStr:
        case Expression::Kind::Interpolation:
            fail("this is not supported yet"_s, *expression);
            return g.emitLoad(dst, jsUndefined());
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // a < b < c is a < b and b < c, with b evaluated once.
    RegisterID* emitComparison(RegisterID* dst, Compare& node)
    {
        if (node.ops.size() == 1) {
            Reg left = emit(node.left);
            if (isNoneConstant(node.comparators[0]) && (node.ops[0] == ComparisonOperator::Is || node.ops[0] == ComparisonOperator::IsNot)) {
                Reg result = destination(dst);
                g.emitIsUndefinedOrNull(result.get(), left.get());
                if (node.ops[0] == ComparisonOperator::IsNot)
                    g.emitUnaryOp<OpNot>(result.get(), result.get());
                return result.get();
            }
            Reg right = emit(node.comparators[0]);
            Reg result = destination(dst);
            mark(node);
            return emitCompare(result.get(), node.ops[0], left.get(), right.get());
        }
        Reg result = temporaryDestination(dst);
        Ref<Label> end = g.newLabel();
        Reg left = emitToTemporary(node.left);
        for (size_t i = 0; i < node.ops.size(); ++i) {
            Reg right = emitToTemporary(node.comparators[i]);
            mark(node);
            emitCompare(result.get(), node.ops[i], left.get(), right.get());
            if (i + 1 < node.ops.size())
                emitJumpIfFalse(result.get(), end.get());
            left = right;
        }
        g.emitLabel(end.get());
        return finish(dst, result.get());
    }

    // ---- Displays

    enum class Display : uint8_t { Tuple, List, Set };

    static bool hasStarred(Sequence<Expression*> elements)
    {
        for (Expression* element : elements) {
            if (element->is<Starred>())
                return true;
        }
        return false;
    }

    // Each in a register of its own, one after another.
    void emitElements(Sequence<Expression*> elements, Vector<Reg, 8>& registers)
    {
        for (Expression* element : elements) {
            registers.append(g.newTemporary());
            emitInto(registers.last().get(), element);
        }
    }

    RegisterID* emitNewList(RegisterID* dst, const Vector<Reg, 8>& elements)
    {
        OpNewArray::emit(&g, dst, elements.isEmpty() ? VirtualRegister { 0 } : elements[0]->virtualRegister(), elements.size(), ArrayWithUndecided);
        return dst;
    }

    RegisterID* emitNewTuple(RegisterID* dst, const Vector<Reg, 8>& elements)
    {
        OpPyNewTuple::emit(&g, dst, elements.isEmpty() ? VirtualRegister { 0 } : elements[0]->virtualRegister(), elements.size());
        return dst;
    }

    // A list of the elements, some of which are *iterables to be taken apart.
    RegisterID* emitListWithStarred(RegisterID* dst, Sequence<Expression*> elements, const Node& node)
    {
        size_t plain = 0;
        while (plain < elements.size() && !elements[plain]->is<Starred>())
            ++plain;
        {
            Vector<Reg, 8> registers;
            emitElements(elements.first(plain), registers);
            emitNewList(dst, registers);
        }
        for (size_t i = plain; i < elements.size(); ++i) {
            if (auto* starred = elements[i]->tryAs<Starred>()) {
                Reg iterable = emit(starred->value);
                emitRuntimeCall(nullptr, "listExtend"_s, { dst, iterable.get() }, *starred);
            } else {
                Reg value = emit(elements[i]);
                emitRuntimeCall(nullptr, "listAppend"_s, { dst, value.get() }, node);
            }
        }
        return dst;
    }

    RegisterID* emitSequenceDisplay(RegisterID* dst, Sequence<Expression*> elements, Display display, const Node& node)
    {
        if (hasStarred(elements)) {
            Reg list = g.newTemporary();
            emitListWithStarred(list.get(), elements, node);
            switch (display) {
            case Display::List:
                return finish(dst, list.get());
            case Display::Tuple:
                return emitRuntimeCall(dst, "listToTuple"_s, { list.get() }, node);
            case Display::Set:
                return emitRuntimeCall(dst, "listToSet"_s, { list.get() }, node);
            }
        }
        Vector<Reg, 8> registers;
        emitElements(elements, registers);
        switch (display) {
        case Display::List:
            return emitNewList(destination(dst).get(), registers);
        case Display::Tuple:
            return emitNewTuple(destination(dst).get(), registers);
        case Display::Set:
            return emitRuntimeCall(dst, "newSet"_s, registers, node);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    RegisterID* emitDictDisplay(RegisterID* dst, Dict& node)
    {
        bool hasUnpacking = false;
        for (Expression* key : node.keys)
            hasUnpacking |= !key;
        if (!hasUnpacking && node.keys.size() <= 16) {
            // Keys and values by turns.
            Vector<Reg, 8> registers;
            for (size_t i = 0; i < node.keys.size(); ++i) {
                registers.append(g.newTemporary());
                emitInto(registers.last().get(), node.keys[i]);
                registers.append(g.newTemporary());
                emitInto(registers.last().get(), node.values[i]);
            }
            return emitRuntimeCall(dst, "newDict"_s, registers, node);
        }
        Reg dict = g.newTemporary();
        emitRuntimeCall(dict.get(), "newDict"_s, { }, node);
        for (size_t i = 0; i < node.keys.size(); ++i) {
            if (!node.keys[i]) {
                Reg mapping = emit(node.values[i]);
                emitRuntimeCall(nullptr, "dictUpdate"_s, { dict.get(), mapping.get() }, *node.values[i]);
                continue;
            }
            Reg key = emit(node.keys[i]);
            Reg value = emit(node.values[i]);
            mark(*node.keys[i]);
            OpPySetItem::emit(&g, dict.get(), key.get(), value.get());
        }
        return finish(dst, dict.get());
    }

    // ---- f-strings

    RegisterID* emitFormattedValue(RegisterID* dst, FormattedValue& node)
    {
        Reg value = emit(node.value);
        Reg conversion = constant(jsNumber(node.conversion));
        Reg specification = node.formatSpecification ? Reg(emit(node.formatSpecification)) : Reg(none());
        return emitRuntimeCall(dst, "formatValue"_s, { value.get(), conversion.get(), specification.get() }, node);
    }

    RegisterID* emitJoinedString(RegisterID* dst, JoinedStr& node)
    {
        if (node.values.empty())
            return g.emitLoad(dst, m_vm.propertyNames->emptyIdentifier);
        if (node.values.size() == 1)
            return emit(node.values[0], dst);
        // Every piece is a string by now, so this is JavaScript's concatenation.
        Vector<Reg, 8> pieces;
        emitElements(node.values, pieces);
        Reg result = destination(dst);
        return g.emitStrcat(result.get(), pieces[0].get(), pieces.size());
    }

    // ---- Calls

    RegisterID* marker() { return g.moveLinkTimeConstant(nullptr, LinkTimeConstant::pyBoundArgumentsMarker); }

    // With only the first so many of the arguments that there are registers for.
    RegisterID* emitRawCall(RegisterID* dst, RegisterID* function, CallArguments& call, unsigned argumentCount, const Node& node)
    {
        Vector<Reg, CallFrame::headerSizeInRegisters> callFrame;
        for (int i = 0; i < CallFrame::headerSizeInRegisters; ++i)
            callFrame.append(g.newTemporary());
        mark(node);
        OpCall::emit(&g, dst, function, argumentCount + 1, call.stackOffset(), g.nextValueProfileIndex());
        return dst;
    }

    // super() with no arguments means super(__class__, self), which only the compiler can know.
    bool isImplicitSuper(Call& node)
    {
        auto* name = node.function->tryAs<Name>();
        if (!name || *name->id != "super"_s || !node.arguments.empty() || !node.keywords.empty())
            return false;
        if (!isFunctionLike() || m_info.parameterNames.isEmpty() || !m_info.positionalCount)
            return false;
        if (locate(*name->id).where != Where::Global)
            return false;
        NameScope scope = m_block.scopeOf(m_names.dunder_class);
        return scope == NameScope::Free || scope == NameScope::Cell;
    }

    RegisterID* emitCall(RegisterID* dst, Call& node)
    {
        if (!node.keywords.empty() || hasStarred(node.arguments))
            return emitGeneralCall(dst, node);

        unsigned count = node.arguments.size();

        if (isImplicitSuper(node)) {
            Reg function = emitToTemporary(node.function);
            CallArguments call(g, nullptr, 2);
            g.emitLoad(call.thisRegister(), jsUndefined());
            emitLoadClosure(call.argumentRegister(0), m_names.dunder_class, node);
            RegisterID* self = emitLoadName(call.argumentRegister(1), m_info.parameterNames[0], node);
            if (self != call.argumentRegister(1))
                g.move(call.argumentRegister(1), self);
            return emitRawCall(destination(dst).get(), function.get(), call, 2, node);
        }

        auto* attribute = node.function->tryAs<Attribute>();
        if (!attribute) {
            Reg function = emitToTemporary(node.function);
            CallArguments call(g, nullptr, count);
            g.emitLoad(call.thisRegister(), jsUndefined());
            for (unsigned i = 0; i < count; ++i)
                emitInto(call.argumentRegister(i), node.arguments[i]);
            return emitRawCall(destination(dst).get(), function.get(), call, count, node);
        }

        // base.method(...). If it is a method of base's class, base goes in front of the arguments and no bound method is made.
        Reg base = emit(attribute->value);
        Reg function = g.newTemporary();
        CallArguments call(g, nullptr, count + 1);
        RegisterID* self = call.argumentRegister(0);
        mark(*attribute);
        addOnce(m_details->names, mangle(*attribute->attribute));
        OpPyLoadMethod::emit(&g, function.get(), self, base.get(), g.addConstant(mangle(*attribute->attribute)), g.nextValueProfileIndex());
        g.emitLoad(call.thisRegister(), jsUndefined());
        for (unsigned i = 0; i < count; ++i)
            emitInto(call.argumentRegister(i + 1), node.arguments[i]);

        Reg result = temporaryDestination(dst);
        Ref<Label> isNotMethod = g.newLabel();
        Ref<Label> done = g.newLabel();
        Reg isEmpty = g.newTemporary();
        g.emitIsEmpty(isEmpty.get(), self);
        g.emitJumpIfTrue(isEmpty.get(), isNotMethod.get());
        emitRawCall(result.get(), function.get(), call, count + 1, node);
        g.emitJump(done.get());
        g.emitLabel(isNotMethod.get());
        // If it is a function of JavaScript's, it expects what it was got from as `this`. Python's own make nothing of it.
        g.move(call.thisRegister(), base.get());
        for (unsigned i = 0; i < count; ++i)
            g.move(call.argumentRegister(i), call.argumentRegister(i + 1));
        emitRawCall(result.get(), function.get(), call, count, node);
        g.emitLabel(done.get());
        return finish(dst, result.get());
    }

    RegisterID* keywordNamesConstant(Sequence<Keyword*> keywords)
    {
        auto* names = JSCellButterfly::create(m_vm, CopyOnWriteArrayWithContiguous, keywords.size());
        for (unsigned i = 0; i < keywords.size(); ++i)
            names->setIndex(m_vm, i, g.addStringConstant(*keywords[i]->name));
        return constant(names);
    }

    // With keywords, *iterables or **mappings. The runtime works out which parameter each is for.
    RegisterID* emitGeneralCall(RegisterID* dst, Call& node)
    {
        for (size_t i = 0; i < node.keywords.size(); ++i) {
            for (size_t j = 0; node.keywords[i]->name && j < i; ++j) {
                if (node.keywords[j]->name && *node.keywords[j]->name == *node.keywords[i]->name)
                    fail(makeString("keyword argument repeated: "_s, node.keywords[i]->name->string()), *node.keywords[i]);
            }
        }

        // base.function(...): the helpers are given base as their own `this`, to pass on.
        Reg base = g.newTemporary();
        Reg function;
        if (node.function->kind == Expression::Kind::Attribute) {
            auto& attribute = node.function->as<Attribute>();
            emitInto(base.get(), attribute.value);
            function = g.newTemporary();
            mark(attribute);
            emitGetAttribute(function.get(), base.get(), mangle(*attribute.attribute));
        } else {
            g.emitLoad(base.get(), jsUndefined());
            function = emitToTemporary(node.function);
        }
        bool hasMappings = false;
        for (Keyword* keyword : node.keywords)
            hasMappings |= !keyword->name;

        if (!hasMappings && !hasStarred(node.arguments)) {
            // callKeywords(function, names, positional..., values of the keywords...)
            Reg helper = g.newTemporary();
            g.emitGetById(helper.get(), runtime(), Identifier::fromString(m_vm, "callKeywords"_s));
            unsigned count = node.arguments.size() + node.keywords.size();
            CallArguments call(g, nullptr, count + 2);
            g.move(call.thisRegister(), base.get());
            g.move(call.argumentRegister(0), function.get());
            g.move(call.argumentRegister(1), keywordNamesConstant(node.keywords));
            unsigned i = 2;
            for (Expression* argument : node.arguments)
                emitInto(call.argumentRegister(i++), argument);
            for (Keyword* keyword : node.keywords)
                emitInto(call.argumentRegister(i++), keyword->value);
            return emitRawCall(destination(dst).get(), helper.get(), call, count + 2, node);
        }

        // callSpread(function, a list of the positional arguments, a dict of the keywords or None)
        Reg positional = g.newTemporary();
        emitListWithStarred(positional.get(), node.arguments, node);
        Reg keywords = g.newTemporary();
        if (node.keywords.empty())
            g.emitLoad(keywords.get(), jsUndefined());
        else {
            emitRuntimeCall(keywords.get(), "newDict"_s, { }, node);
            for (Keyword* keyword : node.keywords) {
                Reg value = emit(keyword->value);
                if (keyword->name)
                    emitRuntimeCall(nullptr, "addKeyword"_s, { keywords.get(), stringConstant(*keyword->name), value.get(), function.get() }, *keyword);
                else
                    emitRuntimeCall(nullptr, "addKeywords"_s, { keywords.get(), value.get(), function.get() }, *keyword);
            }
        }
        Reg helper = g.newTemporary();
        g.emitGetById(helper.get(), runtime(), Identifier::fromString(m_vm, "callSpread"_s));
        CallArguments call(g, nullptr, 3);
        g.move(call.thisRegister(), base.get());
        g.move(call.argumentRegister(0), function.get());
        g.move(call.argumentRegister(1), positional.get());
        g.move(call.argumentRegister(2), keywords.get());
        return emitRawCall(destination(dst).get(), helper.get(), call, 3, node);
    }

    // ---- Making functions

    String qualifiedNameFor(const Identifier& name)
    {
        switch (m_info.kind) {
        case CodeKind::Module:
            return name.string();
        case CodeKind::Class:
            return makeString(m_info.qualifiedName, '.', name.string());
        default:
            return makeString(m_info.qualifiedName, ".<locals>."_s, name.string());
        }
    }

    Ref<FunctionInfo> makeInfo(CodeKind kind, const Identifier& name, Arguments* arguments, Block& block, const Node& node)
    {
        auto info = adoptRef(*new FunctionInfo);
        info->kind = kind;
        info->isGenerator = block.isGenerator;
        info->isCoroutine = block.isCoroutine;
        info->usesNamespace = kind == CodeKind::Class;
        info->isNested = block.isNested;
        info->isMethod = block.isMethod;
        info->hasDocstring = block.hasDocstring;
        info->futureFeatures = m_info.futureFeatures;
        info->visibility = m_info.visibility;
        info->line = node.line;
        info->name = name;
        info->qualifiedName = qualifiedNameFor(name);
        if (kind == CodeKind::Class)
            info->privateName = name;
        else if (m_private)
            info->privateName = *m_private;
        for (Symbol& symbol : block.symbols) {
            if (symbol.scope == NameScope::Free)
                info->freeVariables.append(*symbol.name);
        }
        if (arguments) {
            auto add = [&] (Argument* argument) { info->parameterNames.append(mangle(*argument->name)); };
            for (Argument* argument : arguments->positionalOnly)
                add(argument);
            for (Argument* argument : arguments->positional)
                add(argument);
            for (Argument* argument : arguments->keywordOnly)
                add(argument);
            info->positionalOnlyCount = arguments->positionalOnly.size();
            info->positionalCount = arguments->positionalOnly.size() + arguments->positional.size();
            info->keywordOnlyCount = arguments->keywordOnly.size();
            if (arguments->variadic) {
                info->hasVariadic = true;
                add(arguments->variadic);
            }
            if (arguments->keywordVariadic) {
                info->hasKeywordVariadic = true;
                add(arguments->keywordVariadic);
            }
        }
        return info;
    }

    // A docstring with the indentation of the source taken out, as CPython's compiler takes it out.
    static String cleanDocstring(const String& original)
    {
        // Tabs are expanded first, to columns of eight.
        StringBuilder expanded;
        unsigned column = 0;
        for (char16_t c : StringView(original).codeUnits()) {
            if (c == '\t') {
                unsigned spaces = 8 - column % 8;
                for (unsigned i = 0; i < spaces; ++i)
                    expanded.append(' ');
                column += spaces;
                continue;
            }
            expanded.append(c);
            column = c == '\n' || c == '\r' ? 0 : column + 1;
        }
        String doc = expanded.toString();
        unsigned length = doc.length();

        // The least indentation of any line after the first that is not blank.
        unsigned p = 0;
        while (p < length && doc[p++] != '\n') { }
        unsigned margin = UINT_MAX;
        while (p < length) {
            unsigned start = p;
            while (p < length && doc[p] == ' ')
                ++p;
            if (p < length && doc[p] != '\n')
                margin = std::min(margin, p - start);
            while (p < length && doc[p++] != '\n') { }
        }
        if (margin == UINT_MAX)
            margin = 0;

        p = 0;
        while (p < length && doc[p] == ' ')
            ++p;
        if (!p && !margin)
            return doc.isNull() ? emptyString() : doc;
        StringBuilder result;
        auto copyLine = [&] {
            while (p < length) {
                char16_t c = doc[p++];
                result.append(c);
                if (c == '\n')
                    break;
            }
        };
        copyLine();
        while (p < length) {
            for (unsigned i = 0; i < margin && p < length && doc[p] == ' '; ++i)
                ++p;
            copyLine();
        }
        String cleaned = result.toString();
        return cleaned.isNull() ? emptyString() : cleaned;
    }

    static String docstringOf(Sequence<Statement*> body)
    {
        return cleanDocstring(body[0]->as<Expr>().value->as<Constant>().text->string());
    }

    // The function object for a piece of code, which is compiled when it is first called.
    RegisterID* emitNewFunction(RegisterID* dst, Ref<FunctionInfo>&& info, const Node& node, SourceParseMode parseMode = SourceParseMode::MethodMode)
    {
        const SourceCode& parentSource = g.m_scopeNode->source();
        unsigned parameterCount = info->isGeneratorBody ? static_cast<unsigned>(JSGenerator::Argument::NumberOfArguments) : info->parameterCount();
        FunctionMetadataNode metadata(JSTokenLocation(), JSTokenLocation(), node.start, node.start, node.start, info->visibility, StrictModeLexicallyScopedFeature, ConstructorKind::None, SuperBinding::NotNeeded, parameterCount, parseMode, false);
        metadata.finishParsing(SourceCode(parentSource.provider(), node.start, node.end), info->name, FunctionMode::FunctionExpression);
        auto* executable = UnlinkedFunctionExecutable::create(m_vm, parentSource, &metadata, UnlinkedNormalFunction, ConstructAbility::CannotConstruct, InlineAttribute::None, JSParserScriptMode::Classic, nullptr, { }, std::nullopt, DerivedContextType::None, EvalContextType::None, NeedsClassFieldInitializer::No, PrivateBrandRequirement::None);
        executable->setPythonInfo(WTF::move(info));
        OpNewFuncExp::emit(&g, dst, g.scopeRegister(), g.m_codeBlock->addFunctionExpr(executable));
        return dst;
    }

    // def and lambda: the defaults are evaluated now, and kept in the function.
    RegisterID* emitFunction(RegisterID* dst, CodeKind kind, const Identifier& name, Arguments* arguments, const Node& node, const void* blockKey, bool isAsync)
    {
        UNUSED_PARAM(isAsync);
        Block* block = m_table.blockFor(blockKey);
        RELEASE_ASSERT(block);

        Reg defaults;
        if (!arguments->defaults.empty()) {
            Vector<Reg, 8> values;
            emitElements(arguments->defaults, values);
            defaults = g.newTemporary();
            emitNewTuple(defaults.get(), values);
        }
        Reg keywordDefaults;
        for (size_t i = 0; i < arguments->keywordOnly.size(); ++i) {
            if (!arguments->keywordDefaults[i])
                continue;
            if (!keywordDefaults) {
                keywordDefaults = g.newTemporary();
                emitRuntimeCall(keywordDefaults.get(), "newDict"_s, { }, node);
            }
            Reg value = emit(arguments->keywordDefaults[i]);
            OpPySetItem::emit(&g, keywordDefaults.get(), stringConstant(mangle(*arguments->keywordOnly[i]->name)), value.get());
        }

        Reg function = temporaryDestination(dst);
        auto info = makeInfo(kind, name, arguments, *block, node);
        if (block->hasDocstring && kind == CodeKind::Function)
            info->docstring = docstringOf(static_cast<const FunctionDef&>(node).body);
        emitNewFunction(function.get(), WTF::move(info), node);
        if (Block* annotations = kind == CodeKind::Function ? m_table.blockFor(arguments) : nullptr; annotations && annotations->usesAnnotations) {
            Reg annotate = g.newTemporary();
            emitNewAnnotateFunction(annotate.get(), *annotations, CodeKind::Function, node);
            g.emitDirectPutById(function.get(), m_names.private_annotate, annotate.get());
        }
        if (defaults)
            g.emitDirectPutById(function.get(), m_names.private_defaults, defaults.get());
        if (keywordDefaults)
            g.emitDirectPutById(function.get(), m_names.private_kwdefaults, keywordDefaults.get());
        return finish(dst, function.get());
    }

    // ---- What a function does before its body

    // Names that can lose their value: `del x`, and `except E as x`, which ends with one.
    void collectDeletedNames(Sequence<Statement*> body)
    {
        for (Statement* statement : body)
            collectDeletedNames(*statement);
    }

    void collectDeletedNames(Statement& statement)
    {
        switch (statement.kind) {
        case Statement::Kind::Delete:
            for (Expression* target : statement.as<Delete>().targets)
                collectDeletedNames(*target);
            return;
        case Statement::Kind::For:
            collectDeletedNames(statement.as<For>().body);
            collectDeletedNames(statement.as<For>().orElse);
            return;
        case Statement::Kind::While:
            collectDeletedNames(statement.as<While>().body);
            collectDeletedNames(statement.as<While>().orElse);
            return;
        case Statement::Kind::If:
            collectDeletedNames(statement.as<If>().body);
            collectDeletedNames(statement.as<If>().orElse);
            return;
        case Statement::Kind::With:
            collectDeletedNames(statement.as<With>().body);
            return;
        case Statement::Kind::Match:
            for (MatchCase* matchCase : statement.as<Match>().cases)
                collectDeletedNames(matchCase->body);
            return;
        case Statement::Kind::Try: {
            auto& node = statement.as<Try>();
            collectDeletedNames(node.body);
            for (ExceptHandler* handler : node.handlers) {
                if (handler->name)
                    m_deletedNames.add(mangle(*handler->name).impl());
                collectDeletedNames(handler->body);
            }
            collectDeletedNames(node.orElse);
            collectDeletedNames(node.finalBody);
            return;
        }
        default:
            return;
        }
    }

    void collectDeletedNames(Expression& target)
    {
        if (auto* name = target.tryAs<Name>())
            m_deletedNames.add(mangle(*name->id).impl());
        else if (auto* tuple = target.tryAs<Tuple>()) {
            for (Expression* element : tuple->elements)
                collectDeletedNames(*element);
        } else if (auto* list = target.tryAs<List>()) {
            for (Expression* element : list->elements)
                collectDeletedNames(*element);
        }
    }

    void emitLoadGlobals()
    {
        for (auto [target, name] : { std::pair { &m_globals, &m_names.globals }, std::pair { &m_builtins, &m_names.builtins } }) {
            *target = g.addVar();
            Variable variable = g.variable(*name);
            Reg scope = g.emitResolveScope(nullptr, variable);
            g.emitGetFromScope(target->get(), scope.get(), variable, ThrowIfNotFound);
        }
    }

    // An environment for the variables of this block that other functions use.
    void emitPushCells(const Vector<const Identifier*, 8>& names)
    {
        if (names.isEmpty())
            return;
        m_environments.append(makeUnique<VariableEnvironment>());
        VariableEnvironment& environment = *m_environments.last();
        for (const Identifier* name : names) {
            auto result = environment.add(*name);
            result.iterator->value.setIsLet();
            result.iterator->value.setIsCaptured();
        }
        g.pushLexicalScopeInternal(environment, BytecodeGenerator::TDZCheckOptimization::DoNotOptimize, BytecodeGenerator::NestedScopeType::IsNested, nullptr, BytecodeGenerator::TDZRequirement::UnderTDZ, BytecodeGenerator::ScopeType::LetConstScope, BytecodeGenerator::ScopeRegisterType::Block);
    }

    void emitPopCells(const Vector<const Identifier*, 8>& names)
    {
        if (names.isEmpty())
            return;
        g.popLexicalScopeInternal(*m_environments.last());
        m_environments.removeLast();
    }

    RegisterID* parameterRegister(unsigned index) { return &g.m_parameters[index + 1]; }

    // Gives each parameter its value, from arguments given by position. Anything else the caller has seen to: see README.md.
    void emitBindArguments(const Node& node)
    {
        unsigned positionalCount = m_info.positionalCount;
        Ref<Label> bound = g.newLabel();
        OpJeqPtr::emit(&g, g.thisRegister(), marker(), bound->bind(&g));

        Reg given = g.newTemporary();
        OpArgumentCount::emit(&g, given.get());
        Reg callee = g.newTemporary();
        g.move(callee.get(), &g.m_calleeRegister);

        // What is left over is where the parameters after the positional ones are, so it is taken before those are given values.
        Reg rest;
        if (m_info.hasVariadic) {
            rest = g.newTemporary();
            OpCreateRest::emit(&g, rest.get(), positionalCount);
        }

        Ref<Label> enough = g.newLabel();
        Reg isEnough = g.newTemporary();
        if (m_info.hasVariadic)
            g.emitBinaryOp<OpGreatereq>(isEnough.get(), given.get(), constant(jsNumber(positionalCount)), OperandTypes());
        else
            g.emitEqualityOp<OpStricteq>(isEnough.get(), given.get(), constant(jsNumber(positionalCount)));
        g.emitJumpIfTrue(isEnough.get(), enough.get());
        // Too few or too many. The runtime raises, or gives a tuple with the default for each parameter in that parameter's place. How
        // many have one is not known here: __defaults__ can be set.
        {
            Reg defaults = g.newTemporary();
            emitRuntimeCall(defaults.get(), "defaultsFor"_s, { callee.get(), given.get() }, node);
            for (unsigned i = 0; i < positionalCount; ++i) {
                Ref<Label> wasGiven = g.newLabel();
                Reg isGiven = g.newTemporary();
                g.emitBinaryOp<OpGreater>(isGiven.get(), given.get(), constant(jsNumber(i)), OperandTypes());
                g.emitJumpIfTrue(isGiven.get(), wasGiven.get());
                emitGetItem(parameterRegister(i), defaults.get(), constant(jsNumber(i)));
                g.emitLabel(wasGiven.get());
            }
        }
        g.emitLabel(enough.get());

        for (unsigned i = positionalCount; i < positionalCount + m_info.keywordOnlyCount; ++i)
            emitRuntimeCall(parameterRegister(i), "keywordDefault"_s, { callee.get(), constant(jsNumber(i)) }, node);
        if (m_info.hasVariadic)
            emitRuntimeCall(parameterRegister(m_info.variadicIndex()), "listToTuple"_s, { rest.get() }, node);
        if (m_info.hasKeywordVariadic)
            emitRuntimeCall(parameterRegister(m_info.keywordVariadicIndex()), "newDict"_s, { }, node);
        g.emitLabel(bound.get());
    }

    bool usesGlobals()
    {
        if (!isFunctionLike() || m_block.hasImport)
            return true;
        for (Symbol& symbol : m_block.symbols) {
            if (symbol.scope == NameScope::GlobalImplicit || symbol.scope == NameScope::GlobalExplicit)
                return true;
        }
        return false;
    }

    // Registers for the local variables, and an environment for those that inner functions use.
    void emitDeclareVariables(bool parametersAreInEnvironment)
    {
        Vector<const Identifier*, 8> cells;
        HashSet<UniquedStringImpl*> parameters;
        for (auto& name : m_info.parameterNames)
            parameters.add(name.impl());

        for (Symbol& symbol : m_block.symbols) {
            bool isParameter = parameters.contains(symbol.name->impl());
            if (symbol.scope == NameScope::Cell) {
                // In the body of a generator, the parameters are the variables of the function that made it.
                if (!(isParameter && parametersAreInEnvironment))
                    cells.append(symbol.name);
                continue;
            }
            if (symbol.scope != NameScope::Local || isParameter)
                continue;
            RegisterID* local = g.addVar();
            m_locals.set(symbol.name->impl(), local);
            g.moveEmptyValue(local);
        }
        emitPushCells(cells);
    }

    template<typename EmitBody>
    void generateFunction(const EmitBody& emitBody)
    {
        Node& node = m_block.location;

        if (m_info.isGeneratorBody)
            return generateGeneratorBody(emitBody);

        emitBindArguments(node);
        m_details->firstTraceableOffset = g.instructions().size();

        if (m_info.isGenerator || m_info.isCoroutine) {
            // Nor is there one at all for this: the frame is the one that the body runs in.
            m_details->firstTraceableOffset = std::numeric_limits<unsigned>::max();
            // The function proper only makes the generator, or the coroutine, which is the same thing under another name. Its parameters are kept where the body, which is another function, finds them.
            Vector<const Identifier*, 8> cells;
            for (auto& name : m_info.parameterNames)
                cells.append(&name);
            emitPushCells(cells);
            for (unsigned i = 0; i < m_info.parameterNames.size(); ++i)
                emitStoreClosure(m_info.parameterNames[i], parameterRegister(i));

            auto info = m_info.copy();
            info->isGeneratorBody = true;
            Reg body = g.newTemporary();
            emitNewFunction(body.get(), WTF::move(info), node, SourceParseMode::GeneratorBodyMode);
            Reg generator = g.newTemporary();
            if (m_info.isCoroutine)
                emitRuntimeCall(generator.get(), "newCoroutine"_s, { body.get(), constant(jsBoolean(m_info.isGenerator)) }, node);
            else {
                g.emitNewGenerator(generator.get());
                g.emitPutInternalField(generator.get(), static_cast<unsigned>(JSGenerator::Field::Next), body.get());
                g.emitPutInternalField(generator.get(), static_cast<unsigned>(JSGenerator::Field::This), none());
            }
            g.emitReturn(generator.get());
            return;
        }

        if (usesGlobals())
            emitLoadGlobals();
        for (unsigned i = 0; i < m_info.parameterNames.size(); ++i) {
            UniquedStringImpl* name = m_info.parameterNames[i].impl();
            m_locals.set(name, parameterRegister(i));
            if (!m_deletedNames.contains(name))
                m_alwaysBound.add(name);
        }
        emitDeclareVariables(false);
        for (unsigned i = 0; i < m_info.parameterNames.size(); ++i) {
            const Identifier& name = m_info.parameterNames[i];
            if (m_block.scopeOf(name) == NameScope::Cell)
                emitStoreClosure(name, parameterRegister(i));
        }
        emitBody();
        g.emitReturn(none());
    }

    // ---- Generators

    // The function that is called each time a generator is resumed. BytecodeGeneratorification makes it pick up where it left off.
    template<typename EmitBody>
    void generateGeneratorBody(const EmitBody& emitBody)
    {
        g.m_generatorRegister = &g.m_parameters[static_cast<unsigned>(JSGenerator::Argument::Generator)];
        g.m_needsGeneratorification = true;

        // Where the registers that are live across a yield are kept.
        JSC::SymbolTable* frameSymbolTable = JSC::SymbolTable::create(m_vm);
        frameSymbolTable->setScopeType(JSC::SymbolTable::ScopeType::VarScope);
        int frameSymbolTableIndex = constant(frameSymbolTable)->index();
        g.m_generatorFrameSymbolTable.set(m_vm, frameSymbolTable);
        g.m_generatorFrameSymbolTableIndex = frameSymbolTableIndex;
        OpCreateGeneratorFrameEnvironment::emit(&g, g.generatorFrameRegister(), g.scopeRegister(), VirtualRegister { frameSymbolTableIndex }, none());
        g.emitPutInternalField(g.generatorRegister(), static_cast<unsigned>(JSGenerator::Field::Frame), g.generatorFrameRegister());

        if (usesGlobals())
            emitLoadGlobals();
        emitDeclareVariables(true);

        // The first time, there is nowhere for a value or an exception to be sent to.
        Ref<Label> start = g.newLabel();
        Ref<Label> thrown = g.newLabel();
        g.emitJumpIfTrue(g.emitEqualityOp<OpStricteq>(g.newTemporary(), g.generatorResumeModeRegister(), g.emitLoad(nullptr, JSGenerator::ResumeMode::NormalMode)), start.get());
        g.emitJumpIfTrue(g.emitEqualityOp<OpStricteq>(g.newTemporary(), g.generatorResumeModeRegister(), g.emitLoad(nullptr, JSGenerator::ResumeMode::ThrowMode)), thrown.get());
        g.emitReturn(g.generatorValueRegister());
        g.emitLabel(thrown.get());
        mark(m_block.location);
        g.emitThrow(g.generatorValueRegister());
        g.emitLabel(start.get());

        emitBody();
        g.emitReturn(none());
    }

    RegisterID* emitYield(RegisterID* dst, Yield& node)
    {
        if (!m_info.isGeneratorBody) {
            fail("'yield' outside function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        Reg value = node.value ? Reg(emitToTemporary(node.value)) : Reg(g.emitLoad(g.newTemporary(), jsUndefined()));
        mark(node);
        return g.move(destination(dst).get(), emitYieldValue(value.get(), node));
    }

    // In an asynchronous generator, what is yielded is marked, to tell it from what an await passes up on its way to the event loop.
    RegisterID* emitYieldValue(RegisterID* value, const Node& node)
    {
        if (!m_info.isCoroutine)
            return g.emitYield(value);
        Reg wrapped = g.newTemporary();
        emitRuntimeCall(wrapped.get(), "wrapAsyncYield"_s, { value }, node);
        return g.emitYield(wrapped.get());
    }

    enum class AwaitContext : uint8_t { Await, AsyncEnter, AsyncExit };

    // await value
    RegisterID* emitAwaitValue(RegisterID* dst, RegisterID* value, const Node& node, AwaitContext context = AwaitContext::Await)
    {
        Reg awaitable = g.newTemporary();
        emitRuntimeCall(awaitable.get(), "getAwaitable"_s, { value, constant(jsNumber(static_cast<unsigned>(context))) }, node);
        return emitDelegate(dst, awaitable.get(), node);
    }

    // False, with an error reported, if this is no place for something asynchronous.
    bool checkIsAsync(ASCIILiteral what, const Node& node)
    {
        if (m_info.isCoroutine && m_info.isGeneratorBody)
            return true;
        if (!isFunctionLike() && what == "'await'"_s)
            fail("'await' outside function"_s, node);
        else if (what == "asynchronous comprehension"_s)
            fail("asynchronous comprehension outside of an asynchronous function"_s, node);
        else
            fail(makeString(what, " outside async function"_s), node);
        return false;
    }

    RegisterID* emitAwait(RegisterID* dst, Await& node)
    {
        if (!checkIsAsync("'await'"_s, node))
            return g.emitLoad(dst, jsUndefined());
        Reg value = emitToTemporary(node.value);
        return emitAwaitValue(dst, value.get(), node);
    }

    // The next item of an asynchronous iterator, or a jump if there are no more.
    void emitAsyncNext(RegisterID* dst, RegisterID* iterator, Label& exhausted, const Node& node)
    {
        emitTryCatch([&] {
            Reg awaitable = g.newTemporary();
            emitRuntimeCall(awaitable.get(), "getAsyncNext"_s, { iterator }, node);
            emitDelegate(dst, awaitable.get(), node);
        }, [&] (RegisterID* exception, RegisterID* thrown) {
            Reg type = g.newTemporary();
            g.emitGetById(type.get(), runtime(), Identifier::fromString(m_vm, "StopAsyncIteration"_s));
            Reg matches = g.newTemporary();
            emitCompare(matches.get(), ComparisonOperator::ExceptionMatch, exception, type.get());
            g.emitJumpIfTrue(matches.get(), exhausted);
            g.emitThrow(thrown);
        }, [] { });
    }

    // yield from iterable: everything that is sent to or thrown into this generator goes to that one, until it is done.
    RegisterID* emitYieldFrom(RegisterID* dst, YieldFrom& node)
    {
        if (!m_info.isGeneratorBody) {
            fail("'yield from' outside function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        if (m_info.isCoroutine) {
            fail("'yield from' inside async function"_s, node);
            return g.emitLoad(dst, jsUndefined());
        }
        Reg iterator = g.newTemporary();
        {
            Reg iterable = emit(node.value);
            emitRuntimeCall(iterator.get(), "getYieldFromIterator"_s, { iterable.get() }, node);
        }
        return emitDelegate(dst, iterator.get(), node);
    }

    // Everything that is sent to or thrown into this generator goes to the iterator, until it is done. The result is what it returned.
    RegisterID* emitDelegate(RegisterID* dst, RegisterID* iteratorRegister, const Node& node)
    {
        Reg iterator = iteratorRegister;
        Reg received = g.emitLoad(g.newTemporary(), jsUndefined());
        Reg wasThrown = g.emitLoad(g.newTemporary(), jsBoolean(false));
        Reg yielded = g.newTemporary();

        Ref<Label> loop = g.newLabel();
        Ref<Label> done = g.newLabel();
        g.emitLabel(loop.get());
        g.emitLoopHint();
        emitRuntimeCall(yielded.get(), "yieldFromStep"_s, { iterator.get(), received.get(), wasThrown.get(), g.generatorRegister() }, node);
        OpJeqPtr::emit(&g, yielded.get(), marker(), done->bind(&g));

        Ref<Label> catchLabel = g.newLabel();
        Ref<Label> tryStart = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryStart.get(), catchLabel.get(), HandlerType::Catch);
        g.move(received.get(), g.emitYield(yielded.get()));
        g.emitLoad(wasThrown.get(), jsBoolean(false));
        Ref<Label> tryEnd = g.newEmittedLabel();
        g.popTry(tryData, tryEnd.get());
        g.emitJump(loop.get());

        g.emitLabel(catchLabel.get());
        g.emitOutOfLineCatchHandler(received.get(), nullptr, tryData);
        g.restoreScopeRegister();
        g.emitLoad(wasThrown.get(), jsBoolean(true));
        g.emitJump(loop.get());

        g.emitLabel(done.get());
        return emitRuntimeCall(dst, "takeReturnValue"_s, { }, node);
    }

    // ---- Comprehensions

    // for target in iterator: [if condition]* ... innermost()
    template<typename Innermost>
    void emitComprehensionLoops(Sequence<Comprehension*> generators, size_t index, RegisterID* firstIterator, const Innermost& innermost)
    {
        if (index == generators.size())
            return innermost();
        Comprehension& generator = *generators[index];
        if (generator.isAsync && !checkIsAsync("asynchronous comprehension"_s, *generator.target))
            return;

        Reg iterator = firstIterator;
        if (!iterator) {
            iterator = g.newTemporary();
            Reg iterable = emit(generator.iterable);
            mark(*generator.iterable);
            if (generator.isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *generator.iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }
        Ref<Label> loop = g.newLabel();
        Ref<Label> end = g.newLabel();
        g.emitLabel(loop.get());
        g.emitLoopHint();
        {
            Reg value = g.newTemporary();
            mark(*generator.iterable);
            if (generator.isAsync)
                emitAsyncNext(value.get(), iterator.get(), end.get(), *generator.iterable);
            else {
                OpPyIterNext::emit(&g, value.get(), iterator.get(), g.nextValueProfileIndex());
                Reg isEmpty = g.newTemporary();
                g.emitIsEmpty(isEmpty.get(), value.get());
                g.emitJumpIfTrue(isEmpty.get(), end.get());
            }
            emitAssign(generator.target, value.get());
        }
        for (Expression* condition : generator.conditions)
            emitBranch(condition, loop.get(), false);
        emitComprehensionLoops(generators, index + 1, nullptr, innermost);
        g.emitJump(loop.get());
        g.emitLabel(end.get());
    }

    // [element for ...], {element for ...} and {key: value for ...} are part of the code they are in, with variables of their own.
    RegisterID* emitComprehension(RegisterID* dst, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Expression* element, Expression* value)
    {
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);
        if (!block->isInlinedComprehension)
            return emitComprehensionFunction(dst, node, type, generators, *block);

        // The outermost iterable is evaluated outside.
        Reg iterator = g.newTemporary();
        {
            Reg iterable = emit(generators[0]->iterable);
            mark(*generators[0]->iterable);
            if (generators[0]->isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }

        ComprehensionScope scope;
        Vector<const Identifier*, 8> cells;
        for (Symbol& symbol : block->symbols) {
            if (symbol.flags & DefParameter)
                continue;
            bool isOwn = (symbol.flags & DefLocal) && !(symbol.flags & DefNonlocal) && !(symbol.flags & DefGlobal);
            if (!isOwn)
                continue;
            if (symbol.scope == NameScope::Cell || (symbol.flags & DefComprehensionCell)) {
                cells.append(symbol.name);
                scope.add(symbol.name->impl(), nullptr);
                continue;
            }
            Reg local = g.newTemporary();
            g.moveEmptyValue(local.get());
            scope.add(symbol.name->impl(), local);
        }
        emitPushCells(cells);
        m_comprehensionScopes.append(WTF::move(scope));

        Reg result = g.newTemporary();
        emitFillComprehension(result.get(), node, type, generators, element, value, iterator.get());

        m_comprehensionScopes.removeLast();
        emitPopCells(cells);
        return finish(dst, result.get());
    }

    // Makes what it is a comprehension of, and goes round putting things in it.
    void emitFillComprehension(RegisterID* result, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Expression* element, Expression* value, RegisterID* iterator)
    {
        switch (type) {
        case ComprehensionType::List:
            emitNewList(result, { });
            break;
        case ComprehensionType::Set:
            emitRuntimeCall(result, "newSet"_s, { }, node);
            break;
        case ComprehensionType::Dict:
            emitRuntimeCall(result, "newDict"_s, { }, node);
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }

        emitComprehensionLoops(generators, 0, iterator, [&] {
            switch (type) {
            case ComprehensionType::List: {
                Reg item = emit(element);
                emitRuntimeCall(nullptr, "listAppend"_s, { result, item.get() }, *element);
                break;
            }
            case ComprehensionType::Set: {
                Reg item = emit(element);
                emitRuntimeCall(nullptr, "setAdd"_s, { result, item.get() }, *element);
                break;
            }
            case ComprehensionType::Dict: {
                Reg key = emit(element);
                Reg item = emit(value);
                mark(*element);
                OpPySetItem::emit(&g, result, key.get(), item.get());
                break;
            }
            default:
                RELEASE_ASSERT_NOT_REACHED();
            }
        });
    }

    // Where the names of a class can be seen without its being the body of the class, as in an annotation, a comprehension is a function that is called at once
    // with the outermost iterator, as a generator expression is everywhere. Its variables would otherwise be taken for the class's.
    RegisterID* emitComprehensionFunction(RegisterID* dst, Expression& node, ComprehensionType type, Sequence<Comprehension*> generators, Block& block)
    {
        auto spelled = type == ComprehensionType::List ? "<listcomp>"_span8 : type == ComprehensionType::Set ? "<setcomp>"_span8 : "<dictcomp>"_span8;
        auto info = makeInfo(CodeKind::Comprehension, m_arena.identifiers().makeIdentifier(m_vm, spelled), nullptr, block, node);
        info->parameterNames.append(Identifier::fromString(m_vm, ".0"_s));
        info->positionalCount = 1;
        info->positionalOnlyCount = 1;

        Reg function = g.newTemporary();
        emitNewFunction(function.get(), WTF::move(info), node);
        CallArguments call(g, nullptr, 1);
        g.emitLoad(call.thisRegister(), jsUndefined());
        {
            Reg iterable = emit(generators[0]->iterable);
            mark(*generators[0]->iterable);
            if (generators[0]->isAsync)
                emitRuntimeCall(call.argumentRegister(0), "getAsyncIterator"_s, { iterable.get() }, *generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, call.argumentRegister(0), iterable.get());
        }
        Reg result = destination(dst);
        emitRawCall(result.get(), function.get(), call, 1, node);
        if (block.isCoroutine)
            emitAwaitValue(result.get(), result.get(), node);
        return result.get();
    }

    void generateComprehension(Expression& node)
    {
        generateFunction([&] {
            // In a coroutine the parameters are variables of the function that made it.
            Reg iterator = m_info.isGeneratorBody ? Reg(emitLoadClosure(nullptr, m_info.parameterNames[0], node)) : Reg(parameterRegister(0));
            Reg result = g.newTemporary();
            if (auto* list = node.tryAs<ListComp>())
                emitFillComprehension(result.get(), node, ComprehensionType::List, list->generators, list->element, nullptr, iterator.get());
            else if (auto* set = node.tryAs<SetComp>())
                emitFillComprehension(result.get(), node, ComprehensionType::Set, set->generators, set->element, nullptr, iterator.get());
            else {
                auto& dict = node.as<DictComp>();
                emitFillComprehension(result.get(), node, ComprehensionType::Dict, dict.generators, dict.key, dict.value, iterator.get());
            }
            g.emitReturn(result.get());
        });
    }

    // (element for ...) is a generator function that is called at once, with the outermost iterator.
    RegisterID* emitGeneratorExpression(RegisterID* dst, GeneratorExp& node)
    {
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);
        const Identifier& name = m_arena.identifiers().makeIdentifier(m_vm, "<genexpr>"_span8);
        auto info = makeInfo(CodeKind::GeneratorExpression, name, nullptr, *block, node);
        info->parameterNames.append(Identifier::fromString(m_vm, ".0"_s));
        info->positionalCount = 1;
        info->positionalOnlyCount = 1;

        Reg function = g.newTemporary();
        emitNewFunction(function.get(), WTF::move(info), node);
        CallArguments call(g, nullptr, 1);
        g.emitLoad(call.thisRegister(), jsUndefined());
        {
            Reg iterable = emit(node.generators[0]->iterable);
            mark(*node.generators[0]->iterable);
            if (node.generators[0]->isAsync)
                emitRuntimeCall(call.argumentRegister(0), "getAsyncIterator"_s, { iterable.get() }, *node.generators[0]->iterable);
            else
                OpPyGetIter::emit(&g, call.argumentRegister(0), iterable.get());
        }
        return emitRawCall(destination(dst).get(), function.get(), call, 1, node);
    }

    void generateGeneratorExpression(GeneratorExp& node)
    {
        generateFunction([&] {
            Reg iterator = emitLoadClosure(nullptr, m_info.parameterNames[0], node);
            emitComprehensionLoops(node.generators, 0, iterator.get(), [&] {
                Reg value = emitToTemporary(node.element);
                emitYieldValue(value.get(), node);
            });
        });
    }

    // ---- Assignment

    void emitAssign(Expression* target, RegisterID* value)
    {
        switch (target->kind) {
        case Expression::Kind::Name:
            emitStoreName(*target->as<Name>().id, value, *target);
            return;
        case Expression::Kind::Attribute: {
            auto& node = target->as<Attribute>();
            Reg base = emit(node.value);
            mark(node);
            emitSetAttribute(base.get(), mangle(*node.attribute), value);
            return;
        }
        case Expression::Kind::Subscript: {
            auto& node = target->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            mark(node);
            OpPySetItem::emit(&g, base.get(), key.get(), value);
            return;
        }
        case Expression::Kind::Tuple:
            return emitUnpack(target->as<Tuple>().elements, value, *target);
        case Expression::Kind::List:
            return emitUnpack(target->as<List>().elements, value, *target);
        case Expression::Kind::Starred:
            fail("starred assignment target must be in a list or tuple"_s, *target);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitUnpack(Sequence<Expression*> targets, RegisterID* value, const Node& node)
    {
        int starIndex = -1;
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!targets[i]->is<Starred>())
                continue;
            if (starIndex >= 0)
                fail("multiple starred expressions in assignment"_s, *targets[i]);
            starIndex = i;
        }
        Vector<Reg, 8> values;
        for (size_t i = 0; i < targets.size(); ++i)
            values.append(g.newTemporary());
        mark(node);
        OpPyUnpackSequence::emit(&g, values.isEmpty() ? VirtualRegister { 0 } : values[0]->virtualRegister(), values.size(), starIndex < 0 ? values.size() : starIndex, value);
        for (size_t i = 0; i < targets.size(); ++i) {
            Expression* target = targets[i];
            if (auto* starred = target->tryAs<Starred>())
                target = starred->value;
            emitAssign(target, values[i].get());
        }
    }

    void emitDelete(Expression* target)
    {
        switch (target->kind) {
        case Expression::Kind::Name:
            emitDeleteName(*target->as<Name>().id, *target);
            return;
        case Expression::Kind::Attribute: {
            auto& node = target->as<Attribute>();
            Reg base = emit(node.value);
            mark(node);
            OpPyDelAttr::emit(&g, base.get(), g.addConstant(mangle(*node.attribute)));
            return;
        }
        case Expression::Kind::Subscript: {
            auto& node = target->as<Subscript>();
            Reg base = emit(node.value);
            Reg key = emit(node.slice);
            mark(node);
            OpPyDelItem::emit(&g, base.get(), key.get());
            return;
        }
        case Expression::Kind::Tuple:
            for (Expression* element : target->as<Tuple>().elements)
                emitDelete(element);
            return;
        case Expression::Kind::List:
            for (Expression* element : target->as<List>().elements)
                emitDelete(element);
            return;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitAugmentedAssignment(AugAssign& node)
    {
        switch (node.target->kind) {
        case Expression::Kind::Name: {
            const Identifier& name = *node.target->as<Name>().id;
            Reg current = emitLoadName(nullptr, name, *node.target);
            Reg value = emit(node.value);
            RegisterID* local = registerForStore(node.target);
            Reg result = local ? Reg(local) : Reg(g.newTemporary());
            mark(node);
            emitBinaryOperation(result.get(), node.op, true, current.get(), value.get());
            emitStoreName(name, result.get(), node);
            return;
        }
        case Expression::Kind::Attribute: {
            auto& target = node.target->as<Attribute>();
            const Identifier& name = mangle(*target.attribute);
            Reg base = emitToTemporary(target.value);
            Reg current = g.newTemporary();
            mark(target);
            emitGetAttribute(current.get(), base.get(), name);
            Reg value = emit(node.value);
            mark(node);
            emitBinaryOperation(current.get(), node.op, true, current.get(), value.get());
            mark(target);
            emitSetAttribute(base.get(), name, current.get());
            return;
        }
        case Expression::Kind::Subscript: {
            auto& target = node.target->as<Subscript>();
            Reg base = emitToTemporary(target.value);
            Reg key = emitToTemporary(target.slice);
            Reg current = g.newTemporary();
            mark(target);
            emitGetItem(current.get(), base.get(), key.get());
            Reg value = emit(node.value);
            mark(node);
            emitBinaryOperation(current.get(), node.op, true, current.get(), value.get());
            mark(target);
            OpPySetItem::emit(&g, base.get(), key.get(), current.get());
            return;
        }
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    // ---- Statements

    void emit(Sequence<Statement*> statements)
    {
        for (Statement* statement : statements)
            emit(*statement);
    }

    void emit(Statement& statement)
    {
        if (m_error)
            return;
        if (!m_vm.isSafeToRecurse()) [[unlikely]]
            return fail("maximum recursion depth exceeded during compilation"_s, statement);

        switch (statement.kind) {
        case Statement::Kind::Expr: {
            Expression* value = statement.as<Expr>().value;
            if (m_info.kind == CodeKind::Interactive) {
                // At a prompt, what an expression comes to is shown.
                Reg result = emit(value);
                emitRuntimeCall(nullptr, "displayHook"_s, { result.get() }, statement);
                return;
            }
            // A docstring, or some other constant that does nothing.
            if (value->is<Constant>())
                return;
            Reg ignored = emit(value);
            return;
        }
        case Statement::Kind::Assign: {
            auto& node = statement.as<Assign>();
            if (node.targets.size() == 1) {
                if (RegisterID* local = registerForStore(node.targets[0])) {
                    emitInto(local, node.value);
                    return;
                }
            }
            Reg value = emitToTemporary(node.value);
            for (Expression* target : node.targets)
                emitAssign(target, value.get());
            return;
        }
        case Statement::Kind::AugAssign:
            return emitAugmentedAssignment(statement.as<AugAssign>());
        case Statement::Kind::AnnAssign: {
            return emitAnnotatedAssignment(statement.as<AnnAssign>());
        }
        case Statement::Kind::Return: {
            auto& node = statement.as<Return>();
            if (!isFunctionLike() || m_info.kind == CodeKind::Class)
                return fail("'return' outside function"_s, node);
            if (m_isInExceptStar)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, node);
            if (node.value && m_info.isCoroutine && m_info.isGenerator)
                return fail("'return' with value in async generator"_s, node);
            Reg value = node.value ? Reg(emitToTemporary(node.value)) : Reg(g.emitLoad(g.newTemporary(), jsUndefined()));
            if (!g.emitReturnViaFinallyIfNeeded(value.get()))
                g.emitReturn(value.get());
            return;
        }
        case Statement::Kind::Delete:
            for (Expression* target : statement.as<Delete>().targets)
                emitDelete(target);
            return;
        case Statement::Kind::Pass:
        case Statement::Kind::Global:
        case Statement::Kind::Nonlocal:
            return;
        case Statement::Kind::If: {
            auto& node = statement.as<If>();
            Ref<Label> otherwise = g.newLabel();
            emitBranch(node.test, otherwise.get(), false);
            emit(node.body);
            if (node.orElse.empty()) {
                g.emitLabel(otherwise.get());
                return;
            }
            Ref<Label> end = g.newLabel();
            g.emitJump(end.get());
            g.emitLabel(otherwise.get());
            emit(node.orElse);
            g.emitLabel(end.get());
            return;
        }
        case Statement::Kind::While: {
            auto& node = statement.as<While>();
            Ref<LabelScope> scope = g.newLabelScope(LabelScope::Loop);
            SetForScope mayBreak(m_isInExceptStarOutsideLoop, false);
            Ref<Label> otherwise = g.newLabel();
            g.emitLabel(*scope->continueTarget());
            g.emitLoopHint();
            emitBranch(node.test, otherwise.get(), false);
            emit(node.body);
            g.emitJump(*scope->continueTarget());
            g.emitLabel(otherwise.get());
            emit(node.orElse);
            g.emitLabel(scope->breakTarget());
            return;
        }
        case Statement::Kind::For:
            return emitFor(statement.as<For>());
        case Statement::Kind::Break: {
            LabelScope* scope = g.breakTarget(Identifier());
            if (!scope)
                return fail("'break' outside loop"_s, statement);
            if (m_isInExceptStarOutsideLoop)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, statement);
            if (!g.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), scope->breakTarget())) {
                g.restoreScopeRegister(g.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth()));
                g.emitJump(scope->breakTarget());
            }
            return;
        }
        case Statement::Kind::Continue: {
            LabelScope* scope = g.continueTarget(Identifier());
            if (!scope)
                return fail("'continue' not properly in loop"_s, statement);
            if (m_isInExceptStarOutsideLoop)
                return fail("'break', 'continue' and 'return' cannot appear in an except* block"_s, statement);
            if (!g.emitJumpViaFinallyIfNeeded(scope->scopeDepth(), *scope->continueTarget())) {
                g.restoreScopeRegister(g.labelScopeDepthToLexicalScopeIndex(scope->scopeDepth()));
                g.emitJump(*scope->continueTarget());
            }
            return;
        }
        case Statement::Kind::FunctionDef: {
            auto& node = statement.as<FunctionDef>();
            if (!node.typeParameters.empty())
                return fail("type parameters are not supported yet"_s, node);
            emitDecorated(node.decorators, *node.name, node, [&] (RegisterID* dst) {
                emitFunction(dst, CodeKind::Function, *node.name, node.arguments, node, &node, node.isAsync);
            });
            return;
        }
        case Statement::Kind::ClassDef:
            return emitClass(statement.as<ClassDef>());
        case Statement::Kind::Raise:
            return emitRaise(statement.as<Raise>());
        case Statement::Kind::Try:
            return emitTry(statement.as<Try>());
        case Statement::Kind::With:
            return emitWith(statement.as<With>(), 0);
        case Statement::Kind::Assert: {
            auto& node = statement.as<Assert>();
            Ref<Label> holds = g.newLabel();
            emitBranch(node.test, holds.get(), true);
            Reg message = node.message ? Reg(emit(node.message)) : Reg(marker());
            emitRuntimeCall(nullptr, "raiseAssertionError"_s, { message.get() }, node);
            g.emitLabel(holds.get());
            return;
        }
        case Statement::Kind::Import:
            return emitImport(statement.as<Import>());
        case Statement::Kind::ImportFrom:
            return emitImportFrom(statement.as<ImportFrom>());
        case Statement::Kind::Match:
            return emitMatch(statement.as<Match>());
        case Statement::Kind::TypeAlias:
            return fail("this statement is not supported yet"_s, statement);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // ---- match

    // What a pattern has captured is kept aside, and given its name only once the whole pattern has matched.
    struct Capture {
        const Identifier* name;
        Reg value;
    };

    struct PatternContext {
        Vector<Capture, 4> captures;
        // Whether a pattern that always matches may stand here. It may not where there are others after it to try.
        bool allowIrrefutable { true };
    };

    void addCapture(PatternContext& context, const Identifier& name, RegisterID* value, const Node& node)
    {
        for (auto& capture : context.captures) {
            if (*capture.name == name)
                return fail(makeString("multiple assignments to name '"_s, name.string(), "' in pattern"_s), node);
        }
        Reg copy = g.newTemporary();
        g.move(copy.get(), value);
        context.captures.append({ &name, WTF::move(copy) });
    }

    static bool isWildcard(Pattern& pattern)
    {
        return pattern.is<MatchAs>() && !pattern.as<MatchAs>().name;
    }

    static bool isStarWildcard(Pattern& pattern)
    {
        return pattern.is<MatchStar>() && !pattern.as<MatchStar>().name;
    }

    // The number that a constant is, if it is one: True is 1.
    static std::optional<double> numberOf(Constant& constant)
    {
        switch (constant.type) {
        case Constant::Type::True:
            return 1;
        case Constant::Type::False:
            return 0;
        case Constant::Type::Integer:
            return static_cast<double>(constant.integer);
        case Constant::Type::Float:
            return constant.real;
        default:
            return std::nullopt;
        }
    }

    // Whether they would be the same key of a dict.
    static bool isSameConstant(Constant& a, Constant& b)
    {
        if (auto number = numberOf(a))
            return number == numberOf(b);
        if (a.type != b.type)
            return false;
        return a.type == Constant::Type::None || (a.text && b.text && *a.text == *b.text);
    }

    static String reprOfConstant(Constant& constant)
    {
        switch (constant.type) {
        case Constant::Type::None:
            return "None"_s;
        case Constant::Type::True:
            return "True"_s;
        case Constant::Type::False:
            return "False"_s;
        case Constant::Type::Integer:
            return String::number(constant.integer);
        case Constant::Type::Float:
            return reprOfDouble(constant.real);
        case Constant::Type::String:
            return reprOfString(constant.text->string());
        case Constant::Type::Bytes:
            return makeString('b', reprOfString(constant.text->string()));
        default:
            return constant.text ? constant.text->string() : String();
        }
    }

    // A pattern inside another. There, one that always matches hides nothing.
    void emitSubpattern(Pattern& pattern, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        SetForScope allow(context.allowIrrefutable, true);
        emitPattern(pattern, subject, mismatch, context);
    }

    void emitJumpIfMarker(RegisterID* value, Label& target)
    {
        OpJeqPtr::emit(&g, value, marker(), target.bind(&g));
    }

    // The elements of a tuple that the runtime made, each in a register.
    Vector<Reg, 8> emitUnpackExactly(RegisterID* tuple, unsigned count)
    {
        Vector<Reg, 8> values;
        for (unsigned i = 0; i < count; ++i)
            values.append(g.newTemporary());
        if (count)
            OpPyUnpackSequence::emit(&g, values[0]->virtualRegister(), count, count, tuple);
        return values;
    }

    // Goes on if the subject matches, and jumps if it does not.
    void emitPattern(Pattern& pattern, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        if (m_error)
            return;
        switch (pattern.kind) {
        case Pattern::Kind::MatchValue: {
            Reg value = emit(pattern.as<MatchValue>().value);
            Reg same = g.newTemporary();
            mark(pattern);
            emitCompare(same.get(), ComparisonOperator::Eq, subject, value.get());
            emitJumpIfFalse(same.get(), mismatch);
            return;
        }
        case Pattern::Kind::MatchSingleton: {
            Reg same = g.newTemporary();
            Constant::Type value = pattern.as<MatchSingleton>().value;
            if (value == Constant::Type::None)
                g.emitIsUndefinedOrNull(same.get(), subject);
            else
                emitCompare(same.get(), ComparisonOperator::Is, subject, constant(jsBoolean(value == Constant::Type::True)));
            g.emitJumpIfFalse(same.get(), mismatch);
            return;
        }
        case Pattern::Kind::MatchAs: {
            auto& node = pattern.as<MatchAs>();
            if (!node.pattern) {
                if (!context.allowIrrefutable) {
                    if (node.name)
                        return fail(makeString("name capture '"_s, node.name->string(), "' makes remaining patterns unreachable"_s), node);
                    return fail("wildcard makes remaining patterns unreachable"_s, node);
                }
            } else
                emitPattern(*node.pattern, subject, mismatch, context);
            if (node.name)
                addCapture(context, *node.name, subject, node);
            return;
        }
        case Pattern::Kind::MatchOr:
            return emitOrPattern(pattern.as<MatchOr>(), subject, mismatch, context);
        case Pattern::Kind::MatchSequence:
            return emitSequencePattern(pattern.as<MatchSequence>(), subject, mismatch, context);
        case Pattern::Kind::MatchMapping:
            return emitMappingPattern(pattern.as<MatchMapping>(), subject, mismatch, context);
        case Pattern::Kind::MatchClass:
            return emitClassPattern(pattern.as<MatchClass>(), subject, mismatch, context);
        case Pattern::Kind::MatchStar:
            // The parser lets it be only in a sequence, which deals with it.
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    void emitOrPattern(MatchOr& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        Ref<Label> matched = g.newLabel();
        // Whichever alternative matches, what it captured ends up here.
        Vector<Capture, 4> common;
        for (size_t i = 0; i < node.patterns.size(); ++i) {
            PatternContext alternative;
            alternative.allowIrrefutable = i + 1 == node.patterns.size() && context.allowIrrefutable;
            Ref<Label> next = g.newLabel();
            emitPattern(*node.patterns[i], subject, next.get(), alternative);
            if (m_error)
                return;
            if (!i) {
                for (auto& capture : alternative.captures)
                    common.append({ capture.name, g.newTemporary() });
            } else if (alternative.captures.size() != common.size())
                return fail("alternative patterns bind different names"_s, node);
            for (auto& capture : alternative.captures) {
                auto* target = common.findIf([&] (auto& candidate) { return *candidate.name == *capture.name; }) == notFound ? nullptr : &common[common.findIf([&] (auto& candidate) { return *candidate.name == *capture.name; })];
                if (!target)
                    return fail("alternative patterns bind different names"_s, node);
                g.move(target->value.get(), capture.value.get());
            }
            g.emitJump(matched.get());
            g.emitLabel(next.get());
        }
        g.emitJump(mismatch);
        g.emitLabel(matched.get());
        for (auto& capture : common)
            addCapture(context, *capture.name, capture.value.get(), node);
    }

    void emitSequencePattern(MatchSequence& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned size = node.patterns.size();
        int star = -1;
        bool onlyWildcards = true;
        bool starIsWildcard = false;
        for (unsigned i = 0; i < size; ++i) {
            Pattern& element = *node.patterns[i];
            if (element.is<MatchStar>()) {
                if (star >= 0)
                    return fail("multiple starred names in sequence pattern"_s, node);
                starIsWildcard = isStarWildcard(element);
                onlyWildcards &= starIsWildcard;
                star = i;
                continue;
            }
            onlyWildcards &= isWildcard(element);
        }

        // That it is a sequence, and how long: exactly so, or at least so if a star takes up the slack.
        Reg fits = g.newTemporary();
        emitRuntimeCall(fits.get(), "matchSequence"_s, { subject, constant(jsNumber(star < 0 ? size : size - 1)), constant(jsBoolean(star >= 0)) }, node);
        g.emitJumpIfFalse(fits.get(), mismatch);
        if (onlyWildcards)
            return;

        if (starIsWildcard) {
            // Only what is wanted is got, by its index.
            Reg length;
            for (unsigned i = 0; i < size; ++i) {
                Pattern& element = *node.patterns[i];
                if (static_cast<int>(i) == star || isWildcard(element))
                    continue;
                Reg index = g.newTemporary();
                if (static_cast<int>(i) < star)
                    g.emitLoad(index.get(), jsNumber(i));
                else {
                    if (!length) {
                        length = g.newTemporary();
                        emitRuntimeCall(length.get(), "length"_s, { subject }, node);
                    }
                    emitBinaryOperation(index.get(), BinaryOperator::Sub, false, length.get(), constant(jsNumber(size - i)));
                }
                Reg value = g.newTemporary();
                mark(element);
                emitGetItem(value.get(), subject, index.get());
                emitSubpattern(element, value.get(), mismatch, context);
            }
            return;
        }

        Vector<Reg, 8> values;
        for (unsigned i = 0; i < size; ++i)
            values.append(g.newTemporary());
        mark(node);
        OpPyUnpackSequence::emit(&g, values[0]->virtualRegister(), size, star < 0 ? size : star, subject);
        for (unsigned i = 0; i < size; ++i) {
            Pattern& element = *node.patterns[i];
            if (auto* starred = element.is<MatchStar>() ? &element.as<MatchStar>() : nullptr)
                addCapture(context, *starred->name, values[i].get(), element);
            else
                emitSubpattern(element, values[i].get(), mismatch, context);
        }
    }

    void emitMappingPattern(MatchMapping& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned size = node.keys.size();
        Reg fits = g.newTemporary();
        emitRuntimeCall(fits.get(), "matchMapping"_s, { subject, constant(jsNumber(size)) }, node);
        g.emitJumpIfFalse(fits.get(), mismatch);
        if (!size && !node.rest)
            return;

        // A key that is written out twice can be seen now. One that is looked up has to wait until it is.
        for (unsigned i = 0; i < size; ++i) {
            auto* key = node.keys[i]->tryAs<Constant>();
            if (!key) {
                if (!node.keys[i]->is<Attribute>() && !node.keys[i]->is<UnaryOp>() && !node.keys[i]->is<BinOp>())
                    return fail("mapping pattern keys may only match literals and attribute lookups"_s, *node.keys[i]);
                continue;
            }
            for (unsigned j = 0; j < i; ++j) {
                auto* other = node.keys[j]->tryAs<Constant>();
                if (other && isSameConstant(*key, *other))
                    return fail(makeString("mapping pattern checks duplicate key ("_s, reprOfConstant(*key), ')'), node);
            }
        }

        Vector<Reg, 8> keyRegisters;
        emitElements(node.keys, keyRegisters);
        Reg keys = g.newTemporary();
        emitNewTuple(keys.get(), keyRegisters);
        Reg found = g.newTemporary();
        emitRuntimeCall(found.get(), "matchKeys"_s, { subject, keys.get() }, node);
        emitJumpIfMarker(found.get(), mismatch);
        auto values = emitUnpackExactly(found.get(), size);
        for (unsigned i = 0; i < size; ++i)
            emitSubpattern(*node.patterns[i], values[i].get(), mismatch, context);
        if (node.rest) {
            Reg rest = g.newTemporary();
            emitRuntimeCall(rest.get(), "matchRest"_s, { subject, keys.get() }, node);
            addCapture(context, *node.rest, rest.get(), node);
        }
    }

    void emitClassPattern(MatchClass& node, RegisterID* subject, Label& mismatch, PatternContext& context)
    {
        unsigned positional = node.patterns.size();
        unsigned keywords = node.keywordAttributes.size();
        for (unsigned i = 0; i < keywords; ++i) {
            for (unsigned j = i + 1; j < keywords; ++j) {
                if (*node.keywordAttributes[i] == *node.keywordAttributes[j])
                    return fail(makeString("attribute name repeated in class pattern: "_s, node.keywordAttributes[i]->string()), *node.keywordPatterns[j]);
            }
        }
        auto* names = JSCellButterfly::create(m_vm, CopyOnWriteArrayWithContiguous, keywords);
        for (unsigned i = 0; i < keywords; ++i)
            names->setIndex(m_vm, i, g.addStringConstant(*node.keywordAttributes[i]));

        Reg cls = emitToTemporary(node.cls);
        Reg found = g.newTemporary();
        emitRuntimeCall(found.get(), "matchClass"_s, { subject, cls.get(), constant(jsNumber(positional)), constant(names) }, node);
        emitJumpIfMarker(found.get(), mismatch);
        auto values = emitUnpackExactly(found.get(), positional + keywords);
        for (unsigned i = 0; i < positional + keywords; ++i) {
            Pattern& element = i < positional ? *node.patterns[i] : *node.keywordPatterns[i - positional];
            if (!isWildcard(element))
                emitSubpattern(element, values[i].get(), mismatch, context);
        }
    }

    void emitMatch(Match& node)
    {
        Reg subject = emitToTemporary(node.subject);
        Ref<Label> end = g.newLabel();
        for (size_t i = 0; i < node.cases.size(); ++i) {
            MatchCase& matchCase = *node.cases[i];
            Ref<Label> next = g.newLabel();
            PatternContext context;
            context.allowIrrefutable = matchCase.guard || i + 1 == node.cases.size();
            emitPattern(*matchCase.pattern, subject.get(), next.get(), context);
            if (m_error)
                return;
            for (auto& capture : context.captures)
                emitStoreName(*capture.name, capture.value.get(), *matchCase.pattern);
            context.captures.clear();
            if (matchCase.guard)
                emitBranch(matchCase.guard, next.get(), false);
            emit(matchCase.body);
            g.emitJump(end.get());
            g.emitLabel(next.get());
        }
        g.emitLabel(end.get());
    }

    void emitFor(For& node)
    {
        if (node.isAsync && !checkIsAsync("'async for'"_s, node))
            return;
        Reg iterator = g.newTemporary();
        {
            Reg iterable = emit(node.iterable);
            mark(*node.iterable);
            if (node.isAsync)
                emitRuntimeCall(iterator.get(), "getAsyncIterator"_s, { iterable.get() }, *node.iterable);
            else
                OpPyGetIter::emit(&g, iterator.get(), iterable.get());
        }
        Ref<LabelScope> scope = g.newLabelScope(LabelScope::Loop);
        SetForScope mayBreak(m_isInExceptStarOutsideLoop, false);
        Ref<Label> exhausted = g.newLabel();
        g.emitLabel(*scope->continueTarget());
        g.emitLoopHint();
        {
            RegisterID* local = registerForStore(node.target);
            // Not straight into the variable: it keeps its last value when there is no more.
            Reg value = g.newTemporary();
            mark(*node.iterable);
            if (node.isAsync)
                emitAsyncNext(value.get(), iterator.get(), exhausted.get(), *node.iterable);
            else {
                OpPyIterNext::emit(&g, value.get(), iterator.get(), g.nextValueProfileIndex());
                Reg isEmpty = g.newTemporary();
                g.emitIsEmpty(isEmpty.get(), value.get());
                g.emitJumpIfTrue(isEmpty.get(), exhausted.get());
            }
            if (local)
                g.move(local, value.get());
            else
                emitAssign(node.target, value.get());
        }
        emit(node.body);
        g.emitJump(*scope->continueTarget());
        g.emitLabel(exhausted.get());
        emit(node.orElse);
        g.emitLabel(scope->breakTarget());
    }

    // @a @b def f: ... is f = a(b(f)). The decorators are evaluated first, from the top.
    template<typename EmitDefinition>
    void emitDecorated(Sequence<Expression*> decorators, const Identifier& name, const Node& node, const EmitDefinition& emitDefinition)
    {
        Vector<Reg, 4> functions;
        for (Expression* decorator : decorators)
            functions.append(emitToTemporary(decorator));
        Reg value = g.newTemporary();
        emitDefinition(value.get());
        for (unsigned i = functions.size(); i--;) {
            CallArguments call(g, nullptr, 1);
            g.emitLoad(call.thisRegister(), jsUndefined());
            g.move(call.argumentRegister(0), value.get());
            emitRawCall(value.get(), functions[i].get(), call, 1, *decorators[i]);
        }
        emitStoreName(name, value.get(), node);
    }

    // ---- Exceptions

    void emitRaise(Raise& node)
    {
        if (!node.exception) {
            // The exception being handled, again.
            if (!m_handledExceptions.isEmpty()) {
                mark(node);
                g.emitThrow(m_handledExceptions.last());
                return;
            }
            emitRuntimeCall(nullptr, "reraise"_s, { }, node);
            return;
        }
        Reg exception = emit(node.exception);
        Reg cause = node.cause ? Reg(emit(node.cause)) : Reg(marker());
        emitRuntimeCall(nullptr, "raise"_s, { exception.get(), cause.get() }, node);
    }

    // try: body() finally: finalizer(). However the body is left, by falling off its end, an exception, return, break or continue.
    template<typename Body, typename Finalizer>
    void emitTryFinally(const Body& body, const Finalizer& finalizer)
    {
        Ref<Label> finallyLabel = g.newLabel();
        Ref<Label> finallyEndLabel = g.newLabel();
        FinallyContext context(g, finallyLabel.get());
        g.pushFinallyControlFlowScope(context);

        Ref<Label> tryLabel = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryLabel.get(), finallyLabel.get(), HandlerType::Finally);
        body();
        Ref<Label> tryEndLabel = g.newEmittedLabel();
        g.popTry(tryData, tryEndLabel.get());

        g.popFinallyControlFlowScope();
        g.emitOutOfLineFinallyHandler(context.completionValueRegister(), context.completionTypeRegister(), tryData);
        g.emitLabel(finallyLabel.get());
        g.restoreScopeRegister();
        if constexpr (std::is_invocable_v<Finalizer, RegisterID*, RegisterID*>)
            finalizer(context.completionTypeRegister(), context.completionValueRegister());
        else
            finalizer();
        g.emitFinallyCompletion(context, finallyEndLabel.get());
        g.emitLabel(finallyEndLabel.get());
    }

    // try: body() except: handler(the exception, the same as it was thrown) else: otherwise()
    // To let it go on its way, the handler throws the second, which is what says that it is the same throw and not a new one.
    template<typename Body, typename Handler, typename Otherwise>
    void emitTryCatch(const Body& body, const Handler& handler, const Otherwise& otherwise)
    {
        Ref<Label> catchLabel = g.newLabel();
        Ref<Label> endLabel = g.newLabel();
        Ref<Label> tryLabel = g.newEmittedLabel();
        TryData* tryData = g.pushTry(tryLabel.get(), catchLabel.get(), HandlerType::Catch);
        body();
        Ref<Label> tryEndLabel = g.newEmittedLabel();
        g.popTry(tryData, tryEndLabel.get());
        otherwise();
        g.emitJump(endLabel.get());

        g.emitLabel(catchLabel.get());
        Reg exception = g.newTemporary();
        Reg thrown = g.newTemporary();
        g.emitOutOfLineExceptionHandler(thrown.get(), exception.get(), nullptr, tryData);
        g.restoreScopeRegister();
        handler(exception.get(), thrown.get());
        g.emitLabel(endLabel.get());
    }

    // While it runs, `thrown` is the exception being handled, and whichever way it is left, what was being handled before is again.
    template<typename Body>
    void emitWhileHandling(RegisterID* thrown, const Node& node, const Body& body)
    {
        Reg previous = g.newTemporary();
        emitRuntimeCall(previous.get(), "pushHandledException"_s, { thrown }, node);
        m_handledExceptions.append(thrown);
        emitTryFinally(body, [&] {
            emitRuntimeCall(nullptr, "popHandledException"_s, { previous.get() }, node);
        });
        m_handledExceptions.removeLast();
    }

    void emitTry(Try& node)
    {
        if (!node.finalBody.empty()) {
            emitTryFinally([&] {
                emitTryExcept(node);
            }, [&] (RegisterID* completionType, RegisterID* completionValue) {
                // If an exception is on its way through, it is what is being handled meanwhile.
                Reg previous = g.newTemporary();
                Reg wasThrown = g.newTemporary();
                g.emitEqualityOp<OpStricteq>(wasThrown.get(), completionType, constant(jsNumber(static_cast<int>(CompletionType::Throw))));
                emitRuntimeCall(previous.get(), "pushIfThrown"_s, { wasThrown.get(), completionValue }, node);
                emitTryFinally([&] {
                    emit(node.finalBody);
                }, [&] {
                    emitRuntimeCall(nullptr, "popHandledException"_s, { previous.get() }, node);
                });
            });
            return;
        }
        emitTryExcept(node);
    }

    void emitTryExcept(Try& node)
    {
        if (node.handlers.empty())
            return emit(node.body);
        if (node.isStar)
            return emitTryExceptStar(node);

        for (size_t i = 0; i + 1 < node.handlers.size(); ++i) {
            if (!node.handlers[i]->type)
                return fail("default 'except:' must be last"_s, *node.handlers[i]);
        }

        emitTryCatch([&] {
            emit(node.body);
        }, [&] (RegisterID* exception, RegisterID* thrown) {
            emitWhileHandling(thrown, node, [&] {
                Ref<Label> handled = g.newLabel();
                for (ExceptHandler* handler : node.handlers) {
                    Ref<Label> next = g.newLabel();
                    if (handler->type) {
                        Reg type = emit(handler->type);
                        Reg matches = g.newTemporary();
                        mark(*handler->type);
                        emitCompare(matches.get(), ComparisonOperator::ExceptionMatch, exception, type.get());
                        g.emitJumpIfFalse(matches.get(), next.get());
                    }
                    if (handler->name) {
                        // except E as name: the name is gone afterwards, so that the exception, which refers to the frame, can go too.
                        emitStoreName(*handler->name, exception, *handler);
                        emitTryFinally([&] {
                            emit(handler->body);
                        }, [&] {
                            emitStoreName(*handler->name, none(), *handler);
                            emitDeleteName(*handler->name, *handler);
                        });
                    } else
                        emit(handler->body);
                    g.emitJump(handled.get());
                    g.emitLabel(next.get());
                }
                // Nothing wanted it.
                g.emitThrow(thrown);
                g.emitLabel(handled.get());
            });
        }, [&] {
            emit(node.orElse);
        });
    }

    // try: ... except* E: ... Each clause is given the part of the exception that is for it, and they can all run. What they raise, and what none
    // of them took, is put together again at the end.
    void emitTryExceptStar(Try& node)
    {
        emitTryCatch([&] {
            emit(node.body);
        }, [&] (RegisterID* original, RegisterID* thrown) {
            emitWhileHandling(thrown, node, [&] {
                Reg results = g.newTemporary();
                emitNewList(results.get(), { });
                Reg rest = g.newTemporary();
                g.move(rest.get(), original);
                for (ExceptHandler* handler : node.handlers) {
                    Ref<Label> next = g.newLabel();
                    Reg match = g.newTemporary();
                    {
                        Reg type = emit(handler->type);
                        Reg pair = g.newTemporary();
                        emitRuntimeCall(pair.get(), "matchExceptionGroup"_s, { rest.get(), type.get() }, *handler->type);
                        auto parts = emitUnpackExactly(pair.get(), 2);
                        g.move(match.get(), parts[0].get());
                        g.move(rest.get(), parts[1].get());
                    }
                    Reg isNone = g.newTemporary();
                    g.emitIsUndefinedOrNull(isNone.get(), match.get());
                    g.emitJumpIfTrue(isNone.get(), next.get());

                    emitTryCatch([&] {
                        Reg matchAsThrown = g.newTemporary();
                        emitRuntimeCall(matchAsThrown.get(), "asThrown"_s, { match.get() }, *handler);
                        emitWhileHandling(matchAsThrown.get(), *handler, [&] {
                            SetForScope noReturn(m_isInExceptStar, true);
                            SetForScope noBreak(m_isInExceptStarOutsideLoop, true);
                            if (handler->name) {
                                emitStoreName(*handler->name, match.get(), *handler);
                                emitTryFinally([&] {
                                    emit(handler->body);
                                }, [&] {
                                    emitStoreName(*handler->name, none(), *handler);
                                    emitDeleteName(*handler->name, *handler);
                                });
                            } else
                                emit(handler->body);
                        });
                    }, [&] (RegisterID* raised, RegisterID*) {
                        emitRuntimeCall(nullptr, "listAppend"_s, { results.get(), raised }, *handler);
                    }, [] { });
                    g.emitLabel(next.get());
                }
                emitRuntimeCall(nullptr, "listAppend"_s, { results.get(), rest.get() }, node);
                Reg toRaise = g.newTemporary();
                emitRuntimeCall(toRaise.get(), "prepareReraiseStar"_s, { original, results.get() }, node);
                Ref<Label> done = g.newLabel();
                Reg isNone = g.newTemporary();
                g.emitIsUndefinedOrNull(isNone.get(), toRaise.get());
                g.emitJumpIfTrue(isNone.get(), done.get());
                // It goes on from here as what was caught would have, whether or not it is the same object.
                emitRuntimeCall(toRaise.get(), "asThrown"_s, { toRaise.get() }, node);
                g.emitThrow(toRaise.get());
                g.emitLabel(done.get());
            });
        }, [&] {
            emit(node.orElse);
        });
    }

    // with a as x, b as y: body is with a as x: with b as y: body
    void emitWith(With& node, size_t index)
    {
        if (node.isAsync && !checkIsAsync("'async with'"_s, node))
            return;
        if (index == node.items.size())
            return emit(node.body);

        WithItem& item = *node.items[index];
        Node& position = *item.contextExpression;
        Reg manager = emitToTemporary(item.contextExpression);
        // __exit__ is looked up before __enter__ is called.
        Reg exit = g.newTemporary();
        RegisterID* isAsync = constant(jsBoolean(node.isAsync));
        emitRuntimeCall(exit.get(), "loadExit"_s, { manager.get(), isAsync }, position);
        Reg entered = g.newTemporary();
        emitRuntimeCall(entered.get(), "callEnter"_s, { manager.get(), isAsync }, position);
        if (node.isAsync)
            emitAwaitValue(entered.get(), entered.get(), position, AwaitContext::AsyncEnter);

        Reg finishedNormally = g.emitLoad(g.newTemporary(), jsBoolean(true));
        emitTryFinally([&] {
            emitTryCatch([&] {
                if (item.optionalVariables)
                    emitAssign(item.optionalVariables, entered.get());
                emitWith(node, index + 1);
            }, [&] (RegisterID* exception, RegisterID* thrown) {
                // __exit__ is told of the exception, and may say that it has been dealt with.
                g.emitLoad(finishedNormally.get(), jsBoolean(false));
                emitWhileHandling(thrown, position, [&] {
                    Reg suppress = g.newTemporary();
                    emitRuntimeCall(suppress.get(), "callExit"_s, { exit.get(), exception }, position);
                    if (node.isAsync)
                        emitAwaitValue(suppress.get(), suppress.get(), position, AwaitContext::AsyncExit);
                    Ref<Label> suppressed = g.newLabel();
                    emitJumpIfTrue(suppress.get(), suppressed.get());
                    g.emitThrow(thrown);
                    g.emitLabel(suppressed.get());
                });
            }, [] { });
        }, [&] {
            Ref<Label> done = g.newLabel();
            g.emitJumpIfFalse(finishedNormally.get(), done.get());
            Reg result = g.newTemporary();
            emitRuntimeCall(result.get(), "callExit"_s, { exit.get(), none() }, position);
            if (node.isAsync)
                emitAwaitValue(result.get(), result.get(), position, AwaitContext::AsyncExit);
            g.emitLabel(done.get());
        });
    }

    // ---- Classes

    // class C(bases, keywords): body is C = __build_class__(a function that runs the body, "C", bases, keywords)
    void emitClass(ClassDef& node)
    {
        if (!node.typeParameters.empty())
            return fail("type parameters are not supported yet"_s, node);
        Block* block = m_table.blockFor(&node);
        RELEASE_ASSERT(block);

        emitDecorated(node.decorators, *node.name, node, [&] (RegisterID* dst) {
            auto info = makeInfo(CodeKind::Class, *node.name, nullptr, *block, node);
            info->parameterNames.append(Identifier::fromString(m_vm, ".namespace"_s));
            info->positionalCount = 1;
            info->positionalOnlyCount = 1;
            Reg body = g.newTemporary();
            emitNewFunction(body.get(), WTF::move(info), node);

            Reg bases = g.newTemporary();
            emitListWithStarred(bases.get(), node.bases, node);
            Reg keywords = g.newTemporary();
            if (node.keywords.empty())
                g.emitLoad(keywords.get(), jsUndefined());
            else {
                emitRuntimeCall(keywords.get(), "newDict"_s, { }, node);
                for (Keyword* keyword : node.keywords) {
                    Reg value = emit(keyword->value);
                    if (keyword->name)
                        OpPySetItem::emit(&g, keywords.get(), stringConstant(*keyword->name), value.get());
                    else
                        emitRuntimeCall(nullptr, "dictUpdate"_s, { keywords.get(), value.get() }, *keyword);
                }
            }
            emitRuntimeCall(dst, "buildClass"_s, { body.get(), stringConstant(*node.name), bases.get(), keywords.get() }, node);
        });
    }

    // The function that runs the body of a class statement. It is given the namespace to fill in. It returns the environment that
    // __class__ is a variable of, for the class to be put in once there is one, or None.
    void generateClassBody(ClassDef& node)
    {
        m_namespace = parameterRegister(0);
        emitLoadGlobals();

        Vector<const Identifier*, 8> cells;
        if (m_block.needsClassClosure)
            cells.append(&m_names.dunder_class);
        if (m_block.needsClassDict)
            cells.append(&m_names.dunder_classdict);
        if (m_block.hasConditionalAnnotations)
            cells.append(&m_names.dunder_conditional_annotations);
        emitPushCells(cells);
        if (m_block.needsClassDict)
            emitStoreClosure(m_names.dunder_classdict, m_namespace.get());
        if (m_block.hasConditionalAnnotations) {
            Reg set = g.newTemporary();
            emitRuntimeCall(set.get(), "newSet"_s, { }, node);
            emitStoreClosure(m_names.dunder_conditional_annotations, set.get());
        }
        Reg environment = g.newTemporary();
        if (m_block.needsClassClosure)
            g.move(environment.get(), g.scopeRegister());
        else
            g.emitLoad(environment.get(), jsUndefined());

        auto store = [&] (const Identifier& name, RegisterID* value) {
            OpPySetItem::emit(&g, m_namespace.get(), stringConstant(name), value);
        };
        {
            Reg moduleName = g.newTemporary();
            mark(node);
            OpPyLoadGlobal::emit(&g, moduleName.get(), m_globals.get(), m_builtins.get(), g.addConstant(m_names.dunder_name), g.nextValueProfileIndex());
            store(m_names.dunder_module, moduleName.get());
        }
        store(m_names.dunder_qualname, constant(jsString(m_vm, m_info.qualifiedName)));
        store(m_names.dunder_firstlineno, constant(jsNumber(node.line)));
        if (m_block.hasDocstring)
            store(m_names.dunder_doc, constant(jsString(m_vm, docstringOf(node.body))));

        emitSetUpAnnotations(node);
        collectDeferredAnnotations(node.body, CodeKind::Class);
        emit(node.body);
        if (Reg annotate = emitAnnotateFunctionForBody(CodeKind::Class, node))
            store(m_names.dunder_annotate_func, annotate.get());
        {
            Vector<String> attributes;
            for (const Identifier* name : m_block.staticAttributes)
                attributes.append(name->string());
            std::ranges::sort(attributes, [] (const String& a, const String& b) { return codePointCompareLessThan(a, b); });
            Vector<Reg, 8> items;
            for (auto& attribute : attributes) {
                items.append(g.newTemporary());
                g.move(items.last().get(), constant(jsString(m_vm, attribute)));
            }
            Reg tuple = g.newTemporary();
            emitNewTuple(tuple.get(), items);
            store(m_names.dunder_static_attributes, tuple.get());
        }
        g.emitReturn(environment.get());
    }

    // ---- Annotations
    //
    // They are evaluated when they are asked for, by a function that is made for it: PEP 649. This is codegen_function_annotations(), codegen_annassign() and
    // codegen_process_deferred_annotations() of CPython's Python/codegen.c, and what those call.

    bool hasFutureAnnotations() const { return m_info.futureFeatures & FutureAnnotations; }

    // An annotation of a variable of a class or a module, and if the statement is one that may not be come to, which of those it is.
    struct DeferredAnnotation {
        AnnAssign* statement;
        int conditionalIndex; // -1 if it is always come to.
    };

    // Those of a body, in order. What has them and what evaluates them are compiled at different times, and both go by this.
    void collectDeferredAnnotations(Sequence<Statement*> body, bool isConditional, Vector<DeferredAnnotation>& result, int& nextIndex)
    {
        for (Statement* statement : body) {
            switch (statement->kind) {
            case Statement::Kind::AnnAssign: {
                auto& node = statement->as<AnnAssign>();
                if (node.isSimple && node.target->is<Name>())
                    result.append({ &node, isConditional ? nextIndex++ : -1 });
                break;
            }
            case Statement::Kind::For:
                collectDeferredAnnotations(statement->as<For>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<For>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::While:
                collectDeferredAnnotations(statement->as<While>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<While>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::If:
                collectDeferredAnnotations(statement->as<If>().body, true, result, nextIndex);
                collectDeferredAnnotations(statement->as<If>().orElse, true, result, nextIndex);
                break;
            case Statement::Kind::With:
                collectDeferredAnnotations(statement->as<With>().body, true, result, nextIndex);
                break;
            case Statement::Kind::Match:
                for (MatchCase* matchCase : statement->as<Match>().cases)
                    collectDeferredAnnotations(matchCase->body, true, result, nextIndex);
                break;
            case Statement::Kind::Try: {
                auto& node = statement->as<Try>();
                collectDeferredAnnotations(node.body, true, result, nextIndex);
                for (ExceptHandler* handler : node.handlers)
                    collectDeferredAnnotations(handler->body, true, result, nextIndex);
                collectDeferredAnnotations(node.orElse, true, result, nextIndex);
                collectDeferredAnnotations(node.finalBody, true, result, nextIndex);
                break;
            }
            default:
                break;
            }
        }
    }

    Vector<DeferredAnnotation> deferredAnnotationsOf(Sequence<Statement*> body, bool isModule)
    {
        Vector<DeferredAnnotation> result;
        int nextIndex = 0;
        // A module may have run only in part, so all of its count as ones that may not be come to.
        collectDeferredAnnotations(body, isModule, result, nextIndex);
        return result;
    }

    void emitNewAnnotateFunction(RegisterID* dst, Block& block, CodeKind owner, const Node& node)
    {
        auto info = makeInfo(CodeKind::Annotations, m_names.dunder_annotate, nullptr, block, node);
        info->annotationsOf = owner;
        info->canSeeClassScope = block.canSeeClassScope;
        // That of a function goes by the function's name, though it is beside the function and not in it.
        if (owner == CodeKind::Function)
            info->qualifiedName = makeString(qualifiedNameFor(*static_cast<const FunctionDef&>(node).name), ".__annotate__"_s);
        // To the symbol table it is `.format`, so that an annotation can name the built-in function. It is `format` to whoever asks.
        info->parameterNames.append(Identifier::fromString(m_vm, "format"_s));
        info->positionalOnlyCount = 1;
        info->positionalCount = 1;
        emitNewFunction(dst, WTF::move(info), node);
    }

    // To be done before the body of a class or a module is gone through.
    void collectDeferredAnnotations(Sequence<Statement*> body, CodeKind owner)
    {
        if (!hasFutureAnnotations() && m_block.annotationBlock)
            m_deferredAnnotations = deferredAnnotationsOf(body, owner != CodeKind::Class);
    }

    // The __annotate__ of a class or a module. Null if there is nothing for it to evaluate.
    Reg emitAnnotateFunctionForBody(CodeKind owner, const Node& node)
    {
        if (m_deferredAnnotations.isEmpty())
            return nullptr;
        Reg annotate = g.newTemporary();
        emitNewAnnotateFunction(annotate.get(), *m_block.annotationBlock, owner, node);
        return annotate;
    }

    // With `from __future__ import annotations` they are strings, and are put in __annotations__ as they are come to.
    void emitSetUpAnnotations(const Node& node)
    {
        if (hasFutureAnnotations() && m_block.usesAnnotations)
            emitRuntimeCall(nullptr, "setUpAnnotations"_s, { m_info.usesNamespace ? m_namespace.get() : m_globals.get(), constant(jsBoolean(m_info.usesNamespace)) }, node);
    }

    RegisterID* emitAnnotation(RegisterID* dst, Expression& annotation)
    {
        if (hasFutureAnnotations())
            return g.move(dst, constant(jsString(m_vm, unparse(annotation))));
        // *args: *Ts, which is [value] = [*Ts].
        if (auto* starred = annotation.tryAs<Starred>()) {
            Reg iterable = emitToTemporary(starred->value);
            mark(annotation);
            OpPyUnpackSequence::emit(&g, dst->virtualRegister(), 1, 1, iterable.get());
            return dst;
        }
        emitInto(dst, &annotation);
        return dst;
    }

    void emitEvaluateAndDiscard(Expression* expression)
    {
        if (expression)
            emitToTemporary(expression);
    }

    // x: T = value
    void emitAnnotatedAssignment(AnnAssign& node)
    {
        bool isInClassOrModule = !isFunctionLike();
        if (node.value) {
            Reg value = emitToTemporary(node.value);
            emitAssign(node.target, value.get());
        }
        switch (node.target->kind) {
        case Expression::Kind::Name: {
            if (!node.isSimple || !isInClassOrModule)
                break;
            const Identifier& name = mangle(*node.target->as<Name>().id);
            if (hasFutureAnnotations()) {
                Reg annotation = g.newTemporary();
                emitAnnotation(annotation.get(), *node.annotation);
                Reg annotations = emitLoadName(nullptr, m_names.dunder_annotations, node);
                mark(node);
                OpPySetItem::emit(&g, annotations.get(), stringConstant(name), annotation.get());
                break;
            }
            // That it has been come to is noted, for what evaluates the annotations.
            for (auto& deferred : m_deferredAnnotations) {
                if (deferred.statement != &node || deferred.conditionalIndex < 0)
                    continue;
                Reg set = m_info.kind == CodeKind::Class ? Reg(emitLoadClosure(nullptr, m_names.dunder_conditional_annotations, node)) : Reg(emitLoadName(nullptr, m_names.dunder_conditional_annotations, node));
                emitRuntimeCall(nullptr, "setAdd"_s, { set.get(), constant(jsNumber(deferred.conditionalIndex)) }, node);
            }
            break;
        }
        case Expression::Kind::Attribute:
            if (!node.value)
                emitEvaluateAndDiscard(node.target->as<Attribute>().value);
            break;
        case Expression::Kind::Subscript:
            if (!node.value) {
                emitEvaluateAndDiscard(node.target->as<Subscript>().value);
                emitEvaluateAndDiscard(node.target->as<Subscript>().slice);
            }
            break;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    // def __annotate__(format, /)
    void generateAnnotations(void* root)
    {
        generateFunction([&] {
            const Node& node = m_block.location;
            emitRuntimeCall(nullptr, "checkAnnotationFormat"_s, { parameterRegister(0) }, node);
            // If they are strings there is nothing to look up, and the class has not kept its namespace for it.
            if (m_info.canSeeClassScope && !hasFutureAnnotations()) {
                m_namespace = g.newTemporary();
                emitLoadClosure(m_namespace.get(), m_names.dunder_classdict, node);
            }
            Reg annotations = g.newTemporary();
            emitRuntimeCall(annotations.get(), "newDict"_s, { }, node);
            auto add = [&] (const Identifier& name, Expression& annotation) {
                Reg value = g.newTemporary();
                emitAnnotation(value.get(), annotation);
                OpPySetItem::emit(&g, annotations.get(), stringConstant(name), value.get());
            };

            if (m_info.annotationsOf == CodeKind::Function) {
                auto& function = *static_cast<FunctionDef*>(root);
                Arguments& arguments = *function.arguments;
                auto addArgument = [&] (Argument* argument) {
                    if (argument && argument->annotation)
                        add(mangle(*argument->name), *argument->annotation);
                };
                for (Argument* argument : arguments.positional)
                    addArgument(argument);
                for (Argument* argument : arguments.positionalOnly)
                    addArgument(argument);
                addArgument(arguments.variadic);
                for (Argument* argument : arguments.keywordOnly)
                    addArgument(argument);
                addArgument(arguments.keywordVariadic);
                if (function.returns)
                    add(Identifier::fromString(m_vm, "return"_s), *function.returns);
            } else {
                bool isClass = m_info.annotationsOf == CodeKind::Class;
                Sequence<Statement*> body = isClass ? static_cast<ClassDef*>(root)->body : static_cast<Module*>(root)->body;
                for (auto& deferred : deferredAnnotationsOf(body, !isClass)) {
                    Ref<Label> notComeTo = g.newLabel();
                    if (deferred.conditionalIndex >= 0) {
                        Reg set = isClass ? Reg(emitLoadClosure(nullptr, m_names.dunder_conditional_annotations, *deferred.statement)) : Reg(emitLoadName(nullptr, m_names.dunder_conditional_annotations, *deferred.statement));
                        Reg isIn = g.newTemporary();
                        mark(*deferred.statement);
                        emitCompare(isIn.get(), ComparisonOperator::In, constant(jsNumber(deferred.conditionalIndex)), set.get());
                        emitJumpIfFalse(isIn.get(), notComeTo.get());
                    }
                    add(mangle(*deferred.statement->target->as<Name>().id), *deferred.statement->annotation);
                    g.emitLabel(notComeTo.get());
                }
            }
            g.emitReturn(annotations.get());
        });
    }

    // ---- Imports

    void emitImport(Import& node)
    {
        for (Alias* alias : node.names) {
            Reg module = g.newTemporary();
            // import a.b.c gives a, and import a.b.c as d gives c.
            emitRuntimeCall(module.get(), "importName"_s, { m_globals.get(), stringConstant(*alias->name), none(), constant(jsNumber(0)), constant(jsBoolean(!!alias->asName)) }, *alias);
            if (alias->asName) {
                emitStoreName(*alias->asName, module.get(), *alias);
                continue;
            }
            StringView name = alias->name->string();
            size_t dot = name.find('.');
            if (dot == notFound)
                emitStoreName(*alias->name, module.get(), *alias);
            else
                emitStoreName(Identifier::fromString(m_vm, name.left(dot).toString()), module.get(), *alias);
        }
    }

    void emitImportFrom(ImportFrom& node)
    {
        if (!node.level && node.module && *node.module == "__future__"_s)
            return;
        Vector<Reg, 8> names;
        for (Alias* alias : node.names) {
            names.append(g.newTemporary());
            g.emitLoad(names.last().get(), *alias->name);
        }
        Reg fromList = g.newTemporary();
        emitNewTuple(fromList.get(), names);
        names.clear();

        Reg module = g.newTemporary();
        Reg moduleName = node.module ? Reg(stringConstant(*node.module)) : Reg(stringConstant(m_vm.propertyNames->emptyIdentifier));
        emitRuntimeCall(module.get(), "importName"_s, { m_globals.get(), moduleName.get(), fromList.get(), constant(jsNumber(node.level)), constant(jsBoolean(false)) }, node);
        for (Alias* alias : node.names) {
            if (*alias->name == "*"_s) {
                emitRuntimeCall(nullptr, "importStar"_s, { module.get(), m_globals.get() }, *alias);
                continue;
            }
            Reg value = g.newTemporary();
            emitRuntimeCall(value.get(), "importFrom"_s, { module.get(), stringConstant(*alias->name) }, *alias);
            emitStoreName(alias->asName ? *alias->asName : *alias->name, value.get(), *alias);
        }
    }

    // ---- Modules

    void generateModule(Module& module)
    {
        if (m_info.usesNamespace)
            m_namespace = parameterRegister(0);
        emitLoadGlobals();
        if (module.kind == Module::Kind::Expression) {
            Reg value = emit(module.expression);
            g.emitReturn(value.get());
            return;
        }
        // All of the source, which is what the annotations of a module are compiled from.
        Node whole = m_block.location;
        whole.start = 0;
        whole.end = g.m_scopeNode->source().provider()->source().length();
        if (m_block.hasConditionalAnnotations) {
            Reg set = g.newTemporary();
            emitRuntimeCall(set.get(), "newSet"_s, { }, whole);
            emitStoreName(m_names.dunder_conditional_annotations, set.get(), whole);
        }
        emitSetUpAnnotations(whole);
        // It is there from the start, and says of each annotation whether the statement that it is in has been come to.
        CodeKind kind = module.kind == Module::Kind::Interactive ? CodeKind::Interactive : CodeKind::Module;
        collectDeferredAnnotations(module.body, kind);
        if (Reg annotate = emitAnnotateFunctionForBody(kind, whole))
            emitStoreName(m_names.dunder_annotate, annotate.get(), whole);
        if (m_block.hasDocstring)
            emitStoreName(m_names.dunder_doc, constant(jsString(m_vm, docstringOf(module.body))), *module.body[0]);
        emit(module.body);
        g.emitReturn(none());
    }

    using ComprehensionScope = HashMap<UniquedStringImpl*, Reg>; // Null for a variable of an environment.

    BytecodeGenerator& g;
    VM& m_vm;
    std::unique_ptr<CodeDetails> m_details;
    bool m_isInExceptStar { false };
    bool m_isInExceptStarOutsideLoop { false }; // And not in a loop that is itself in the block.
    CommonNames& m_names;
    Arena& m_arena;
    SymbolTable& m_table;
    Block& m_block;
    const FunctionInfo& m_info;
    SyntaxError& m_error;
    const Identifier* m_private;
    Vector<DeferredAnnotation> m_deferredAnnotations; // Those of the class or the module that this is the code of.

    HashMap<UniquedStringImpl*, RegisterID*> m_locals;
    HashSet<UniquedStringImpl*> m_alwaysBound;
    HashSet<UniquedStringImpl*> m_deletedNames;
    Vector<ComprehensionScope, 2> m_comprehensionScopes;
    Vector<std::unique_ptr<VariableEnvironment>> m_environments;
    Vector<RegisterID*, 4> m_handledExceptions;
    Reg m_globals;
    Reg m_builtins;
    Reg m_namespace;
};

// ---- ScopeNode

ScopeNode::ScopeNode(ParserArena& parserArena, const SourceCode& source, Arena& arena, SymbolTable& symbolTable, Block& block, const FunctionInfo& info, void* root)
    : JSC::ScopeNode(parserArena, JSTokenLocation(), JSTokenLocation(), source, nullptr, VariableEnvironment(), FunctionStack(), VariableEnvironment(), NoFeatures, StrictModeLexicallyScopedFeature, NoInnerArrowFunctionFeatures, 0)
    , m_arena(arena)
    , m_symbolTable(symbolTable)
    , m_block(block)
    , m_info(info)
    , m_root(root)
{
}

unsigned ScopeNode::parameterCount() const
{
    if (m_info.isGeneratorBody)
        return static_cast<unsigned>(JSGenerator::Argument::NumberOfArguments);
    return m_info.parameterCount();
}

void ScopeNode::emitBytecode(BytecodeGenerator& generator, RegisterID*)
{
    CodeGenerator(generator, m_arena, m_symbolTable, m_block, m_info, m_error).generate(m_root);
}

} } // namespace JSC::Python
