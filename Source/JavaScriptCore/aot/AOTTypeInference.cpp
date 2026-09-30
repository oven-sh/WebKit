/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTGraph.h"

#if ENABLE(FTL_JIT)

#include "AOTBuiltins.h"
#include "AOTTypeTable.h"
#include "ImmutableIntrinsics.h"
#include "BytecodeStructs.h"
#include "JSCInlines.h"
#include "UnlinkedCodeBlock.h"
#include "UnlinkedFunctionCodeBlock.h"

namespace JSC { namespace AOT {

namespace {

// Values that ToNumeric turns into a number without running any code and without the chance of a BigInt.
constexpr Type TNumberLike = TNumber | TBoolean | TOther;

class TypeInference {
public:
    TypeInference(Graph& graph)
        : m_graph(graph)
    {
    }

    void run()
    {
        // Every type starts out as None ("not reached yet") and only ever grows, and every rule gives a larger answer for
        // larger inputs, so this ends, at the least fixpoint.
        bool changed = true;
        while (changed) {
            changed = false;
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis)
                    changed |= update(phi);
                for (Node* node : block->nodes)
                    changed |= update(node);
            }
            changed |= std::exchange(m_elementTypesChanged, false);
            // An array that the function is not seen to put anything in has been given what it holds by somebody else. (Not until now:
            // what is put in an array may go by what is taken from it.)
            if (!changed && !std::exchange(m_arraysNothingIsPutInHoldAnything, true))
                changed = true;
        }
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* node : block->nodes) {
                if (node->kind != NodeKind::Bytecode)
                    continue;
                switch (node->opcode) {
                case op_ret:
                    m_returnType |= node->use(node->as<OpRet>().m_value)->type;
                    break;
                case op_tail_call:
                    m_returnType |= resultOfCall(node);
                    break;
                case op_tail_call_varargs:
                    m_returnType |= TTop;
                    break;
                default:
                    break;
                }
                if (calleesGivenMore && isReached()) {
                    noteArgumentsOf(node);
                    noteWhatIsPutInVariablesBy(node);
                }
            }
        }
        if (calleesGivenMore && Options::aotFollowsFunctions() && functionsOfProgram() && isReached()) {
            for (BasicBlock* block : m_graph.m_rpo) {
                for (Node* phi : block->phis)
                    noteWhereValuesGoIn(phi);
                for (Node* node : block->nodes)
                    noteWhereValuesGoIn(node);
            }
        }
        // What is still None is code that no execution reaches with a value. It gets compiled all the same.
        for (BasicBlock* block : m_graph.m_rpo) {
            for (Node* phi : block->phis) {
                if (!phi->type)
                    phi->type = TAll;
            }
            for (Node* node : block->nodes) {
                if (!node->type) {
                    node->type = TAll;
                    node->wasTakenNeverToBeReached = true;
                }
            }
        }
    }

    // Whether anything is seen to get to the code, so far. If not it does nothing, so far: it is looked at again if something turns out to.
    bool isReached() const { return !m_graph.facts() || m_graph.facts()->isReached(); }
    Type returnType() const { return m_returnType; }
    Vector<const KnownFunction*>* calleesConsulted { nullptr };
    Vector<const KnownFunction*>* calleesGivenMore { nullptr };

private:
    // ---- Options::aotFollowsFunctions(). See abi/DESIGN-types.md, for now.

    // The value gets somewhere that is not reckoned with.
    void expose(Type type, uint32_t why)
    {
        const KnownFunction* function = functionsOfProgram()->function(functionThatIs(type));
        if (!function || !function->facts)
            return;
        if (function->facts->expose(why) && !calleesGivenMore->contains(function))
            calleesGivenMore->append(function);
    }

    // It is now part of something of which it can no longer be told that it is there: nobody who gets it from there knows to say where it goes.
    void noteThatItIsPartOf(Type whole, Type part, uint32_t why)
    {
        if (uint32_t function = functionThatIs(part); function && functionThatIs(whole) != function)
            expose(part, why);
    }
    void noteJoin(Type before, Type added, uint32_t why)
    {
        noteThatItIsPartOf(before | added, before, why);
        noteThatItIsPartOf(before | added, added, why);
    }
    static uint32_t usedBy(Node* user) { return ProgramFacts::UsedBy | (user->kind == NodeKind::Bytecode ? static_cast<uint32_t>(user->opcode) : 1000 + static_cast<uint32_t>(user->kind)) << 8; }

    void exposeWhatIsUsedBy(Node* user)
    {
        for (auto& use : user->uses)
            expose(use.node->type, usedBy(user));
    }

    void noteWhereArgumentsGo(Node* node, VirtualRegister calleeRegister, unsigned argc, unsigned argv)
    {
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        bool isProven = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isProven);
        // (What noteArgumentsOf() passes on is passed on. The rest of what is passed, nobody keeps track of.)
        unsigned followed = known && isProven && known->forCall && known->facts && known->facts->isClosed ? std::min<unsigned>(known->forCall->numParameters(), ProgramFacts::mostParameters) : 0;
        for (auto& use : node->uses) {
            // Calling something is not a way of getting hold of it, for anybody but itself.
            if (use.reg == calleeRegister && use.reg.offset() != firstArgument) {
                if (!followed)
                    noteCallOfWhoKnowsWhat(use.node->type);
                continue;
            }
            int index = use.reg.offset() - firstArgument;
            if (index >= 1 && static_cast<unsigned>(index) < followed && static_cast<unsigned>(index) < argc)
                continue;
            expose(use.node->type, !followed ? ProgramFacts::PassedToWhoKnowsWhat : !index ? ProgramFacts::PassedAsThis : ProgramFacts::PassedBeyondParameters);
        }
    }

    // A call that is not made as a call of that function and no other: it passes what it passes.
    void noteCallOfWhoKnowsWhat(Type callee) { expose(callee, ProgramFacts::CalledInSomeOtherWay); }

    void noteWhereValuesGoIn(Node* user)
    {
        switch (user->kind) {
        case NodeKind::Phi:
        case NodeKind::Narrow:
            for (auto& use : user->uses)
                noteThatItIsPartOf(user->type, use.node->type, ProgramFacts::OneOfSeveralInPhi);
            return;
        case NodeKind::SetStack:
            noteThatItIsPartOf(m_graph.homedTypes[m_graph.registerIndex(user->reg)], user->uses[0].node->type, ProgramFacts::OneOfSeveralInHomedRegister);
            return;
        // (It looks. What it stands in front of is a user in its own right.)
        case NodeKind::Guard:
        // (One of the things that something else makes.)
        case NodeKind::Proj:
            return;
        case NodeKind::Bytecode:
            break;
        default:
            exposeWhatIsUsedBy(user);
            return;
        }
        auto exposeAllBut = [&](VirtualRegister harmless) {
            for (auto& use : user->uses) {
                if (use.reg != harmless)
                    expose(use.node->type, usedBy(user));
            }
        };
        switch (user->opcode) {
        // ---- Looking at it.
        case op_get_by_id:
        case op_get_by_id_direct:
        case op_get_length:
        case op_in_by_id:
        case op_del_by_id:
        case op_get_prototype_of:
        case op_typeof:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_empty:
        case op_is_undefined_or_null:
        case op_is_boolean:
        case op_is_number:
        case op_is_big_int:
        case op_is_object:
        case op_is_callable:
        case op_is_constructor:
        case op_is_cell_with_type:
        case op_has_structure_with_flags:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_stricteq:
        case op_nstricteq:
        case op_jstricteq:
        case op_jnstricteq:
        case op_jtrue:
        case op_jfalse:
        case op_jeq_null:
        case op_jneq_null:
        case op_jundefined_or_null:
        case op_jnundefined_or_null:
        case op_jeq_ptr:
        case op_jneq_ptr:
        case op_get_parent_scope:
        case op_set_function_name:
        case op_instanceof:
        case op_check_tdz:
            return;
        case op_get_from_scope: {
            // What it may well read, for all that it cannot be told for certain: then what it gets does not say which function it is.
            bool isProven = false;
            if (const KnownFunction* known = m_graph.knownFunctionReadBy(user, &isProven); known && !isProven)
                expose(typeOfClosureOf(known->executable), ProgramFacts::ReadInAWayThatIsNotProven);
            return;
        }
        case op_get_by_val:
            return exposeAllBut(user->as<OpGetByVal>().m_base);
        case op_in_by_val:
            return exposeAllBut(user->as<OpInByVal>().m_base);
        case op_del_by_val:
            return exposeAllBut(user->as<OpDelByVal>().m_base);
        case op_put_by_id:
            return exposeAllBut(user->as<OpPutById>().m_base);
        case op_put_by_val:
            return exposeAllBut(user->as<OpPutByVal>().m_base);
        case op_put_by_val_direct:
            return exposeAllBut(user->as<OpPutByValDirect>().m_base);
        case op_define_data_property:
            // A method that nothing gets hold of but reads that say which it is (Graph::closedMethodReadBy()).
            if (auto* classes = classesOfProgram(); classes && classes->isClosedMethod(functionThatIs(user->use(user->as<OpDefineDataProperty>().m_value)->type)))
                return exposeAllBut(user->as<OpDefineDataProperty>().m_value);
            exposeWhatIsUsedBy(user);
            return;

        // ---- The same thing by another name, if that is what is made of it.
        case op_check_type:
        case op_type_tag:
        case op_to_this:
        case op_to_object:
        case op_identity_with_profile:
        case op_resolve_scope:
            for (auto& use : user->uses)
                noteThatItIsPartOf(user->type, use.node->type, ProgramFacts::LostByWhatHandsItOn | static_cast<uint32_t>(user->opcode) << 8);
            return;

        // ---- Scopes are not values. (What is put in them: noteWhatIsPutInVariablesBy().)
        case op_put_to_scope:
        case op_create_lexical_environment:
        case op_new_func:
        case op_new_func_exp:
        case op_new_generator_func:
        case op_new_generator_func_exp:
        case op_new_async_func:
        case op_new_async_func_exp:
        case op_new_async_generator_func:
        case op_new_async_generator_func_exp:
            return;

        // ---- Handing it on.
        case op_call:
            return noteWhereArgumentsGo(user, user->as<OpCall>().m_callee, user->as<OpCall>().m_argc, user->as<OpCall>().m_argv);
        case op_call_ignore_result:
            return noteWhereArgumentsGo(user, user->as<OpCallIgnoreResult>().m_callee, user->as<OpCallIgnoreResult>().m_argc, user->as<OpCallIgnoreResult>().m_argv);
        case op_tail_call: {
            noteWhereArgumentsGo(user, user->as<OpTailCall>().m_callee, user->as<OpTailCall>().m_argc, user->as<OpTailCall>().m_argv);
            noteWhatIsReturned(resultOfCall(user));
            return;
        }
        case op_ret:
            return noteWhatIsReturned(user->use(user->as<OpRet>().m_value)->type);
        default:
            exposeWhatIsUsedBy(user);
            return;
        }
    }

    void noteWhatIsReturned(Type type)
    {
        const ProgramFacts* facts = m_graph.facts();
        if (!facts || !facts->isClosed || facts->isExposed.load(std::memory_order_relaxed)) {
            expose(type, ProgramFacts::ReturnedToWhoKnowsWhom);
            return;
        }
        noteJoin(facts->returnType.join(type & TTop), type, ProgramFacts::OneOfSeveralReturned);
    }

    Type typeOfClosureOf(UnlinkedFunctionExecutable* executable)
    {
        if (!Options::aotFollowsFunctions() || !functionsOfProgram())
            return TFunction;
        uint32_t number = functionsOfProgram()->numberOf(executable);
        return number ? typeOfFunction(number) : TFunction;
    }

    void noteWhatIsPutInVariablesBy(Node* node)
    {
        VariableFacts* facts = m_graph.variableFacts();
        if (!facts)
            return;
        auto noteInitialValue = [&](VirtualRegister initialValue) {
            if (const void* scope = m_graph.identityOfScope(node))
                facts->join({ scope, Variable::initialValue }, node->use(initialValue)->type);
        };
        switch (node->opcode) {
        case op_put_to_scope:
            if (Variable variable = m_graph.variableAccessedBy(node)) {
                Type put = node->use(node->as<OpPutToScope>().m_value)->type;
                Type before = facts->join(variable, put);
                if (Options::aotFollowsFunctions() && functionsOfProgram()) {
                    UniquedStringImpl* name = node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl();
                    // A variable of a module can be got at from outside it, in ways that are not reads of the variable, unless whoever put the program together says not.
                    const CalleeHints* hints = m_graph.calleeHints();
                    const KnownFunction* known = hints && hints->scopeOfVariables() == variable.scope ? hints->find(name, variable.offset) : nullptr;
                    bool isOfSomeModule = facts->isScopeOfModule(variable.scope);
                    if (facts->hasGivenUpOn(variable, name))
                        expose(put, ProgramFacts::PutInVariableGivenUpOn);
                    else if (facts->isReadFromWhoKnowsWhere(name))
                        expose(put, ProgramFacts::PutInVariableReadFromWhoKnowsWhere);
                    else if (isOfSomeModule && (!known || known->isVisibleFromOutside))
                        expose(put, ProgramFacts::PutInVariableOfModule);
                    else
                        noteJoin(before, put, ProgramFacts::OneOfSeveralInVariable);
                }
                if (!m_graph.nameForLog().isNull()) [[unlikely]]
                    dataLogLn("FACTLOG put `", node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl(), "` scope ", RawPointer(variable.scope), " offset ", variable.offset, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), ": ", TypeDump(node->use(node->as<OpPutToScope>().m_value)->type));
            } else if (Options::aotFollowsFunctions() && functionsOfProgram())
                expose(node->use(node->as<OpPutToScope>().m_value)->type, ProgramFacts::PutWhoKnowsWhere);
            if (!m_graph.variableAccessedBy(node) && !m_graph.nameForLog().isNull()) [[unlikely]]
                dataLogLn("FACTLOG put `", node->graph->codeBlock()->identifier(node->as<OpPutToScope>().m_var).impl(), "` WHO KNOWS WHERE in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset());
            return;
        case op_create_lexical_environment:
            noteInitialValue(node->as<OpCreateLexicalEnvironment>().m_initialValue);
            return;
        case op_create_generator_frame_environment:
            noteInitialValue(node->as<OpCreateGeneratorFrameEnvironment>().m_initialValue);
            return;
        default:
            return;
        }
    }

    void noteArgumentsOf(Node* node)
    {
        unsigned argc;
        unsigned argv;
        switch (node->opcode) {
        case op_call:
            argc = node->as<OpCall>().m_argc;
            argv = node->as<OpCall>().m_argv;
            break;
        case op_call_ignore_result:
            argc = node->as<OpCallIgnoreResult>().m_argc;
            argv = node->as<OpCallIgnoreResult>().m_argv;
            break;
        case op_tail_call:
            argc = node->as<OpTailCall>().m_argc;
            argv = node->as<OpTailCall>().m_argv;
            break;
        default:
            return;
        }
        bool isProven = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isProven);
        if (!known || !isProven || !known->forCall || !known->facts || !known->facts->isClosed)
            return;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        // A call that nothing has been seen to get to, so far, passes nothing, so far.
        for (unsigned i = 0; i < argc; ++i) {
            if (!node->use(VirtualRegister(firstArgument + i))->type) {
                if (!m_graph.nameForLog().isNull()) [[unlikely]]
                    dataLogLn("FACTLOG call of `", known->executable->name().impl(), "` @", known->key.module, ":", known->key.start, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), " IS NOT REACHED: argument ", i, " is nothing");
                return;
            }
        }
        if (!m_graph.nameForLog().isNull()) [[unlikely]] {
            StringPrintStream out;
            for (unsigned i = 1; i < argc; ++i)
                out.print(" ", TypeDump(node->use(VirtualRegister(firstArgument + i))->type));
            dataLogLn("FACTLOG call of `", known->executable->name().impl(), "` @", known->key.module, ":", known->key.start, " in ", m_graph.nameForLog(), " bc#", node->bytecodeIndex.offset(), " passes", out.toString());
        }
        bool givesMore = false;
        unsigned count = std::min<unsigned>(known->forCall->numParameters(), ProgramFacts::mostParameters);
        for (unsigned i = 1; i < count; ++i) {
            Type type = i < argc ? node->use(VirtualRegister(firstArgument + i))->type & TTop : TUndefined;
            Type before = known->facts->parameterTypes[i].join(type);
            givesMore |= (before | type) != before;
            if (Options::aotFollowsFunctions() && functionsOfProgram())
                noteJoin(before, type, ProgramFacts::OneOfSeveralInParameter);
        }
        {
            Type type = node->use(VirtualRegister(firstArgument))->type & TTop;
            Type before = known->facts->thisType.join(type);
            givesMore |= (before | type) != before;
        }
        // (One with no parameters is reached all the same.)
        Type before = known->facts->parameterTypes[0].join(TTop);
        givesMore |= before != TTop;
        if (givesMore && !calleesGivenMore->contains(known))
            calleesGivenMore->append(known);
    }

    // A call of one of the functions of the language itself, if that is what it is bound to be.
    std::optional<Type> resultOfCallOfBuiltin(Node* node)
    {
        if (!Options::aotKnowsWhatBuiltinsReturn())
            return std::nullopt;
        VirtualRegister calleeRegister;
        unsigned argc;
        unsigned argv;
        if (node->isBytecode(op_call)) {
            auto bytecode = node->as<OpCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
        } else if (node->isBytecode(op_tail_call)) {
            auto bytecode = node->as<OpTailCall>();
            calleeRegister = bytecode.m_callee;
            argc = bytecode.m_argc;
            argv = bytecode.m_argv;
        } else
            return std::nullopt;
        Node* callee = node->use(calleeRegister);
        if (argc == 1 && node->opcode == op_call && Graph::linkTimeConstantOf(callee) == LinkTimeConstant::cloneObject) {
            uint16_t family = Graph::familyOfNewObject(node);
            return family ? typeOfObjectBornAs(family) : TFinalObject;
        }
        if (argc == 2 && Graph::linkTimeConstantOf(callee) == LinkTimeConstant::toLength) {
            Type argument = node->use(VirtualRegister(-static_cast<int>(argv) + CallFrame::thisArgumentOffset() + 1))->type;
            if (!argument)
                return TNone;
            return isSubtype(argument, TInt32) ? TInt32 : TNumber;
        }
        unsigned number = 0;
        if (callee->kind == NodeKind::Intrinsic)
            number = callee->intrinsic;
        else if (callee->isBytecode(op_get_by_id)) {
            auto bytecode = callee->as<OpGetById>();
            Type base = callee->use(bytecode.m_base)->type;
            if (!base)
                return TNone;
            number = intrinsicFoundOnPrimitive(base, *callee->graph->codeBlock()->identifier(bytecode.m_property).impl());
        }
        if (!number)
            return std::nullopt;
        auto signature = signatureOfIntrinsic(number);
        if (!signature)
            return std::nullopt;
        int firstArgument = -static_cast<int>(argv) + CallFrame::thisArgumentOffset();
        switch (signature->condition) {
        case BuiltinSignature::Condition::Always:
            break;
        case BuiltinSignature::Condition::IfFirstArgumentIsNoObject: {
            if (argc < 2)
                break;
            Type first = node->use(VirtualRegister(firstArgument + 1))->type;
            if (!first)
                return TNone;
            if (!isSubtype(first, TPrimitive))
                return std::nullopt;
            break;
        }
        case BuiltinSignature::Condition::IfThisIsHolder: {
            Node* thisValue = node->use(VirtualRegister(firstArgument));
            const ImmutableIntrinsics* intrinsics = ImmutableIntrinsics::shared();
            if (thisValue->kind != NodeKind::Intrinsic || thisValue->intrinsic != intrinsics->at(intrinsics->at(number).holder).canonical)
                return std::nullopt;
            break;
        }
        }
        return signature->result;
    }

    Type resultOfCall(Node* node)
    {
        if (auto result = resultOfCallOfBuiltin(node))
            return *result;
        bool isProven = false;
        const KnownFunction* known = m_graph.knownCallee(node, &isProven);
        if (!known || !isProven || !known->forCall)
            return TTop;
        if (calleesConsulted && !calleesConsulted->contains(known))
            calleesConsulted->append(known);
        return known->returnType.load();
    }

    bool update(Node* node)
    {
        Type type = node->type | compute(node);
        if (type == node->type)
            return false;
        node->type = type;
        return true;
    }

    static Type arithResult(Type left, Type right)
    {
        // sub, mul, div, mod, pow: ToNumeric on both sides.
        if (!left || !right)
            return TNone;
        if (isSubtype(left | right, TNumberLike | TString))
            return TNumber;
        return TNumber | TBigInt;
    }

    static Type bitResult(Type left, Type right)
    {
        if (!left || !right)
            return TNone;
        if (isSubtype(left | right, TNumberLike | TString))
            return TInt32;
        return TInt32 | TBigInt;
    }

    static Type unaryArithResult(Type operand)
    {
        if (!operand)
            return TNone;
        if (isSubtype(operand, TNumberLike | TString))
            return TNumber;
        return TNumber | TBigInt;
    }

    Type compute(Node* node)
    {
        switch (node->kind) {
        case NodeKind::Constant:
        case NodeKind::ConstantCell:
        case NodeKind::Intrinsic:
        case NodeKind::LinkTimeConstant:
        case NodeKind::Argument:
            return node->type;
        case NodeKind::Phi: {
            Type type = TNone;
            for (auto& use : node->uses)
                type |= use.node->type;
            return type;
        }
        case NodeKind::GetStack:
            return m_graph.homedTypes[m_graph.registerIndex(node->reg)];
        case NodeKind::SetStack: {
            Type& homed = m_graph.homedTypes[m_graph.registerIndex(node->reg)];
            homed |= node->uses[0].node->type;
            return homed; // So that a change is seen as a change.
        }
        case NodeKind::Guard:
            return TNone;
        case NodeKind::Narrow:
            if (node->narrowedTo)
                return node->uses[0].node->type & node->narrowedTo;
            return node->target ? node->uses[0].node->type & node->target->type : node->uses[0].node->type;
        case NodeKind::Proj:
            return computeProj(node);
        case NodeKind::Bytecode: {
            Type type = computeBytecode(node);
            if (node->hasFact(FactField, 2))
                type &= typeHeldByFact(node->fact >> 24 & 15);
            else if (node->hasFact(FactElement, 2))
                type &= typeHeldByFact(node->fact & 15) | TUndefined;
            return type;
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
        return TAll;
    }

    Type computeProj(Node* node)
    {
        Node* parent = node->uses[0].node;
        if (unsigned fact = Graph::iteratedFactOf(parent)) {
            if (parent->opcode == op_iterator_open)
                return node->reg == parent->as<OpIteratorOpen>().m_iterator ? TArray : TInt32;
            auto bytecode = parent->as<OpIteratorNext>();
            if (node->reg == bytecode.m_done)
                return TBoolean;
            if (node->reg == bytecode.m_next)
                return TInt32;
            return (Options::aotFacts() & 2) ? typeHeldByFact(fact - 1) : TTop;
        }
        switch (parent->opcode) {
        case op_instanceof:
            if (node->reg == parent->as<OpInstanceof>().m_dst)
                return TBoolean;
            return TAll;
        case op_iterator_next: {
            auto bytecode = parent->as<OpIteratorNext>();
            if (node->reg == bytecode.m_done)
                return TTop; // Whatever the iterator result's done property is.
            if (node->reg == bytecode.m_next) {
                // The next method, which stays what it is, or the index of an array that is iterated without an iterator.
                Type next = parent->use(bytecode.m_next)->type;
                return next ? next | TNumber : TNone;
            }
            return TAll; // Empty once the iteration is done.
        }
        case op_iterator_open:
            if (node->reg == parent->as<OpIteratorOpen>().m_iterator)
                return TAnyObject | TCellOther; // Or the marker that says there is none.
            return TTop;
        case op_async_iterator_open:
            if (node->reg == parent->as<OpAsyncIteratorOpen>().m_iterator)
                return TAnyObject;
            return TTop;
        case op_enumerator_next: {
            auto bytecode = parent->as<OpEnumeratorNext>();
            if (node->reg == bytecode.m_propertyName)
                return TString;
            if (node->reg == bytecode.m_mode)
                return TInt32;
            return TNumber;
        }
        case op_catch:
            if (node->reg == parent->as<OpCatch>().m_exception)
                return TCellOther;
            return TAll;
        default:
            return TAll;
        }
    }

    Type computeBytecode(Node* node)
    {
        auto typeOf = [&](VirtualRegister reg) {
            return node->use(reg)->type;
        };

        if (auto [array, element] = m_graph.arrayAndElementStored(node); array && Graph::isArrayMadeHere(array) && !node->block->isGeneric) {
            Type& elements = m_elementTypes.add(array, TNone).iterator->value;
            if (element->type & ~elements) {
                elements |= element->type;
                m_elementTypesChanged = true;
            }
        }

        switch (node->opcode) {
        case op_add: {
            auto bytecode = node->as<OpAdd>();
            Type left = typeOf(bytecode.m_lhs);
            Type right = typeOf(bytecode.m_rhs);
            if (!left || !right)
                return TNone;
            if (isSubtype(left | right, TNumberLike))
                return TNumber;
            if (isSubtype(left, TString) || isSubtype(right, TString)) {
                // The other side is converted to a string, unless it is a symbol, which throws.
                return TString;
            }
            Type result = TNone;
            if (mayBe(left | right, TString | TAnyObject))
                result |= TString;
            if (mayBe(left, TBigInt | TAnyObject) && mayBe(right, TBigInt | TAnyObject))
                result |= TBigInt;
            if (mayBe(left, TNumberLike | TAnyObject) && mayBe(right, TNumberLike | TAnyObject))
                result |= TNumber;
            return result ? result : TAll; // Nothing but a throw: any type will do.
        }
        case op_sub: {
            auto bytecode = node->as<OpSub>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_mul: {
            auto bytecode = node->as<OpMul>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_div: {
            auto bytecode = node->as<OpDiv>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_mod: {
            auto bytecode = node->as<OpMod>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_pow: {
            auto bytecode = node->as<OpPow>();
            return arithResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitand: {
            auto bytecode = node->as<OpBitand>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitor: {
            auto bytecode = node->as<OpBitor>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_bitxor: {
            auto bytecode = node->as<OpBitxor>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_lshift: {
            auto bytecode = node->as<OpLshift>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_rshift: {
            auto bytecode = node->as<OpRshift>();
            return bitResult(typeOf(bytecode.m_lhs), typeOf(bytecode.m_rhs));
        }
        case op_check_type: {
            auto bytecode = node->as<OpCheckType>();
            return typeOf(bytecode.m_value) & typeAdmittedByMask(bytecode.m_mask);
        }
        case op_type_tag:
            if (Options::aotAuditsTypes()) [[unlikely]]
                return node->uses[0].node->type;
            if (node->narrowedTo)
                return node->uses[0].node->type & node->narrowedTo;
            // What is of a type of a family that is open may be any object that has what the type says, made by anybody. It is what it was.
            if (!node->isTakenAtItsWord)
                return node->uses[0].node->type;
            return node->uses[0].node->type & typeOfObjectBornAs(node->firstLayout);
        case op_urshift:
            return TInt32; // The bits of the result: the op_unsigned that follows makes the number of them. A BigInt throws.
        case op_unsigned:
            return TNumber;
        case op_bitnot: {
            Type operand = typeOf(node->as<OpBitnot>().m_operand);
            return bitResult(operand, operand);
        }
        case op_negate:
            return unaryArithResult(typeOf(node->as<OpNegate>().m_operand));
        case op_inc:
            return unaryArithResult(typeOf(node->as<OpInc>().m_srcDst));
        case op_dec:
            return unaryArithResult(typeOf(node->as<OpDec>().m_srcDst));
        case op_to_number: {
            Type operand = typeOf(node->as<OpToNumber>().m_operand);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TNumber))
                return operand;
            return TNumber;
        }
        case op_to_numeric: {
            Type operand = typeOf(node->as<OpToNumeric>().m_operand);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TNumber | TBigInt))
                return operand;
            return unaryArithResult(operand);
        }
        case op_typeof:
            return TAtomString; // SmallStrings
        case op_to_string: {
            // (A string is its own.)
            Type operand = typeOf(node->as<OpToString>().m_operand);
            if (!operand)
                return TNone;
            return isSubtype(operand, TString) ? operand : TString;
        }
        case op_strcat:
            return TString;
        case op_to_primitive: {
            Type operand = typeOf(node->as<OpToPrimitive>().m_src);
            if (!operand)
                return TNone;
            if (isSubtype(operand, TPrimitive))
                return operand;
            return TPrimitive;
        }
        case op_to_property_key:
            return TString | TSymbol;
        case op_to_property_key_or_number:
            return TString | TSymbol | TNumber;

        case op_eq:
        case op_neq:
        case op_stricteq:
        case op_nstricteq:
        case op_less:
        case op_lesseq:
        case op_greater:
        case op_greatereq:
        case op_below:
        case op_beloweq:
        case op_eq_null:
        case op_neq_null:
        case op_not:
        case op_is_empty:
        case op_typeof_is_undefined:
        case op_typeof_is_object:
        case op_typeof_is_function:
        case op_is_undefined_or_null:
        case op_is_boolean:
        case op_is_number:
        case op_is_big_int:
        case op_is_object:
        case op_is_callable:
        case op_is_constructor:
        case op_is_cell_with_type:
        case op_has_structure_with_flags:
        case op_in_by_id:
        case op_in_by_val:
        case op_has_private_name:
        case op_has_private_brand:
        case op_del_by_id:
        case op_del_by_val:
        case op_enumerator_in_by_val:
        case op_enumerator_has_own_property:
            return TBoolean;

        case op_new_object:
            if (uint16_t family = Graph::familyOfNewObject(node))
                return typeOfObjectBornAs(family);
            if (Options::aotTypesFields() && node->numberOfLiteralProperties) {
                if (auto shape = m_graph.shapeOfLiteral(node); shape && shape->number) {
                    if (TypeTable::areStructs())
                        return shape->family ? typeOfObjectBornAs(shape->family) : TFinalObject;
                    return typeOfObjectBornAs(shape->number);
                }
            }
            return TFinalObject;
        case op_get_by_id:
            if (TypeTable::areStructsToGoBy() && Options::aotTypesFields()) {
                if (uint32_t method = Graph::closedMethodReadBy(node))
                    return typeOf(node->as<OpGetById>().m_base) ? typeOfFunction(method) : TNone;
                // Of a struct: the slot holds that, and nothing else is looked at.
                if (auto field = Graph::fieldOfStructGotAtBy(node); field && field->holds.saysSomething())
                    return typeOf(node->as<OpGetById>().m_base) ? field->holds.type() | (field->isOptional ? TUndefined : TNone) : TNone;
            }
            // What got past the guard is what the slot holds; or there is no such property.
            if (Node* guard = node->guard; guard && guard->guardKind == GuardKind::Field && guard->heldKinds) {
                if (!typeOf(node->as<OpGetById>().m_base))
                    return TNone;
                return TypeTable::Holds { guard->heldKinds, guard->heldFirst, guard->heldLast }.type() | (guard->firstWithout ? TUndefined : TNone);
            }
            // However it is read, it is that or the code does not go on (Lowering::lowerGetById()).
            if (Options::aotTypesFields() && !Options::aotAssertsTypes() && !Options::aotAuditsTypes() && (Options::aotShapes() & 2) && !node->guard && TypeTable::shared()) {
                if (uint32_t tag = Graph::typeTagOf(node)) {
                    if (auto field = TypeTable::shared()->fieldOf(tag, node->graph->codeBlock()->identifier(node->as<OpGetById>().m_property).impl()); field && field->holds.saysSomething()) {
                        if (!typeOf(node->as<OpGetById>().m_base))
                            return TNone;
                        // (Of a struct: the slot holds that, and nothing else is looked at.)
                        if (TypeTable::areStructs())
                            return field->holds.type() | (field->isOptional ? TUndefined : TNone);
                        return field->holds.kindsOnly().type();
                    }
                }
            }
            return TAll;
        case op_new_reg_exp:
        case op_new_reg_exp_shared:
            return TRegExp;
        case op_new_promise:
        case op_create_promise:
            return TPromise;
        case op_create_this:
            if (uint16_t family = node->graph->familyOfThis())
                return typeOfObjectBornAs(family);
            return TObject;
        case op_create_direct_arguments:
        case op_create_scoped_arguments:
        case op_create_cloned_arguments:
        case op_new_generator:
        case op_new_async_function_generator:
        case op_create_generator:
        case op_create_async_generator:
        case op_get_scope:
        case op_get_parent_scope:
        case op_resolve_scope:
        case op_create_lexical_environment:
        case op_create_generator_frame_environment:
        case op_push_with_scope:
            return TObject;
        case op_construct:
            if (Node* callee = node->use(node->as<OpConstruct>().m_callee); callee->kind == NodeKind::Intrinsic && Options::aotKnowsWhatBuiltinsReturn()) {
                if (auto result = resultOfConstructingIntrinsic(callee->intrinsic))
                    return *result;
            }
            return TAnyObject;
        case op_to_object: {
            // An object is the object it is.
            Type operand = typeOf(node->as<OpToObject>().m_operand);
            if (!operand)
                return TNone;
            return isSubtype(operand, TAnyObject) ? operand : TAnyObject;
        }
        case op_super_construct:
        case op_super_construct_varargs:
            if (uint16_t family = node->graph->familyOfThis())
                return typeOfObjectBornAs(family);
            return TAnyObject;
        case op_construct_varargs:
            return TAnyObject;
        case op_new_array:
        case op_new_array_buffer:
        case op_new_array_with_size:
        case op_new_array_with_spread:
        case op_create_rest:
            return TArray;
        case op_new_array_with_species:
            return TAnyObject;
        case op_new_func:
            return typeOfClosureOf(node->graph->codeBlock()->functionDecl(node->as<OpNewFunc>().m_functionDecl));
        case op_new_func_exp:
            return typeOfClosureOf(node->graph->codeBlock()->functionExpr(node->as<OpNewFuncExp>().m_functionDecl));
        case op_new_generator_func:
            return typeOfClosureOf(node->graph->codeBlock()->functionDecl(node->as<OpNewGeneratorFunc>().m_functionDecl));
        case op_new_generator_func_exp:
            return typeOfClosureOf(node->graph->codeBlock()->functionExpr(node->as<OpNewGeneratorFuncExp>().m_functionDecl));
        case op_new_async_func:
            return typeOfClosureOf(node->graph->codeBlock()->functionDecl(node->as<OpNewAsyncFunc>().m_functionDecl));
        case op_new_async_func_exp:
            return typeOfClosureOf(node->graph->codeBlock()->functionExpr(node->as<OpNewAsyncFuncExp>().m_functionDecl));
        case op_new_async_generator_func:
            return typeOfClosureOf(node->graph->codeBlock()->functionDecl(node->as<OpNewAsyncGeneratorFunc>().m_functionDecl));
        case op_new_async_generator_func_exp:
            return typeOfClosureOf(node->graph->codeBlock()->functionExpr(node->as<OpNewAsyncGeneratorFuncExp>().m_functionDecl));
        case op_spread:
        case op_get_property_enumerator:
            return TCellOther;
        case op_argument_count:
            return TInt32;
        case op_get_by_val:
            // In the fast copy of a loop, where all that gets past the guard is an element that is there.
            {
                auto bytecode = node->as<OpGetByVal>();
                if (!typeOf(bytecode.m_base) || !typeOf(bytecode.m_property))
                    return TNone;
                if (auto type = Graph::typedArrayAccessed(node)) {
                    Type element = *type == Float32ArrayType || *type == Float64ArrayType || *type == Uint32ArrayType ? TNumber : TInt32;
                    if (node->guard)
                        return element;
                    // Anywhere else: a typed array has nothing to say about a number but what is there, or that nothing is.
                    if (isSubtype(typeOf(bytecode.m_property), TNumber))
                        return element | TUndefined;
                }
                // In the fast copy of a loop, an array that probably holds numbers is one that only numbers are taken from: for
                // anything else there is the other copy. (See Lowering::guardGetByVal().)
                if (node->guard && isSubtype(typeOf(bytecode.m_base), TArray)) {
                    Node* array = node->use(bytecode.m_base);
                    if (Graph::isArrayMadeHere(array)) {
                        Type elements = m_elementTypes.get(array);
                        if (array->opcode == op_new_array) {
                            for (auto& use : array->uses)
                                elements |= use.node->type;
                        }
                        if (!elements && !m_arraysNothingIsPutInHoldAnything)
                            return TNone;
                        if (elements && isSubtype(elements, TNumber))
                            return TNumber;
                    }
                    if (node->expectedMask && isSubtype(typeAdmittedByMask(node->expectedMask), TNumber | TOther) && (node->expectedMask & MaskNumber))
                        return TNumber;
                }
            }
            return TAll;
        case op_call:
            // In the fast copy of a loop, where all that gets past the guard is the function that it is taken for.
            if (node->guard) {
                switch (m_graph.intrinsicOfCall(node)) {
                case CallIntrinsic::None:
                    break;
                case CallIntrinsic::MathIMul:
                case CallIntrinsic::StringCharCodeAt:
                case CallIntrinsic::ArrayPush:
                    return TInt32;
                default:
                    return TNumber;
                }
            }
            return resultOfCall(node);
        case op_tail_call:
            return resultOfCall(node);
        case op_get_length: {
            Type base = typeOf(node->as<OpGetLength>().m_base);
            if (!base)
                return TNone;
            // In the fast copy of a loop no other kind of length gets past the guard.
            if (isSubtype(base, TString) || node->guard)
                return TInt32;
            if (isSubtype(base, TString | TArray | TTypedArray))
                return TNumber;
            return TTop;
        }
        case op_to_this: {
            auto bytecode = node->as<OpToThis>();
            Type operand = typeOf(bytecode.m_srcDst);
            if (!operand)
                return TNone;
            // Everything is left as it is but for a scope, which strict code sees as undefined.
            if (bytecode.m_ecmaMode.isStrict())
                return operand | (mayBe(operand, TOtherObject) ? TUndefined : TNone);
            return isSubtype(operand, TAnyObject & ~TOtherObject) ? operand : TAnyObject;
        }
        case op_iterator_close_check: {
            Type iterator = typeOf(node->as<OpIteratorCloseCheck>().m_iterator);
            return iterator ? iterator | TObject : TNone;
        }
        case op_resolve_scope_for_hoisting_func_decl_in_eval:
            return TObject | TUndefined;
        case op_get_from_scope: {
            bool isProven = false;
            if (const KnownFunction* known = m_graph.knownFunctionReadBy(node, &isProven); known && isProven)
                return known->isDeclaration ? typeOfClosureOf(known->executable) : typeOfClosureOf(known->executable) | TUndefined | TEmpty;
            if (VariableFacts* facts = m_graph.variableFacts()) {
                if (Variable variable = m_graph.variableAccessedBy(node)) {
                    auto bytecode = node->as<OpGetFromScope>();
                    Type type = facts->read(variable, node->graph->codeBlock()->identifier(bytecode.m_var).impl(), m_graph.readerOfFacts());
                    // (A function that a module declares is made when it is first read, by whatever reads it.)
                    return bytecode.m_getPutInfo.resolveType() == ResolvedLazyClosureVar ? type | TFunction : type;
                }
            }
            return TAll;
        }
        default:
            // Including the empty value: get_from_scope, get_internal_field and others hand out holes.
            return TAll;
        }
    }

    Graph& m_graph;
    Type m_returnType { TNone };
    UncheckedKeyHashMap<Node*, Type> m_elementTypes; // Of the arrays that the function makes: everything it puts in them.
    bool m_elementTypesChanged { false };
    bool m_arraysNothingIsPutInHoldAnything { false };
};

} // anonymous namespace

Type inferTypes(Graph& graph, Vector<const KnownFunction*>* calleesConsulted, Vector<const KnownFunction*>* calleesGivenMore)
{
    TypeInference inference(graph);
    inference.calleesConsulted = calleesConsulted;
    inference.calleesGivenMore = calleesGivenMore;
    inference.run();
    return inference.returnType();
}

} } // namespace JSC::AOT

#endif // ENABLE(FTL_JIT)
