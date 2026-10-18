/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "YarrErrorCode.h"
#include "YarrFlags.h"
#include "YarrSyntaxChecker.h"
#include <wtf/StringPrintStream.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class Verifier {
public:
    Verifier(const Program& program, FunctionID id, const Function& function, Vector<String>& findings, VerifierScope scope)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_findings(findings)
        , m_scope(scope)
    {
    }

    void run()
    {
        if (m_function.blocks.isEmpty())
            return;
        if (m_scope != VerifierScope::DefinitionsOnly)
            verifyEntry();
        BitVector isReached;
        Vector<unsigned> worklist { 0u };
        isReached.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        bool hasInvokeWithoutTwoTargets = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            if (m_scope != VerifierScope::DefinitionsOnly) {
                for (ValueID id : block.values)
                    verify(id, m_function.at(id));
                verify(block.terminator);
            }
            if (block.terminator.kind == TerminatorKind::Invoke && block.values.isEmpty())
                failAtEndOf(b, "ends in an Invoke but has no value for it to guard"_s);
            if (block.terminator.kind == TerminatorKind::Invoke && block.terminator.targets.size() != 2) {
                failAtEndOf(b, "ends in an Invoke that does not have two targets"_s);
                hasInvokeWithoutTwoTargets = true;
            }
            for (auto& target : block.terminator.targets) {
                if (target.arguments.size() != m_function.at(target.block).parameters.size())
                    failAtEndOf(b, "passes a block a different number of arguments than it has parameters"_s);
            }
        }
        if (hasInvokeWithoutTwoTargets)
            return;
        if (m_scope != VerifierScope::DefinitionsOnly)
            verifyInitialStores(isReached);
        if (m_scope != VerifierScope::EverythingButDefinitions)
            verifyDefinitions(isReached);
        if (m_scope == VerifierScope::DefinitionsOnly)
            return;
        for (unsigned v = 0; v < m_function.values.size(); ++v) {
            auto& value = m_function.values[v];
            if (!value.type)
                fail(ValueID(v), "has no type"_s);
            else if (value.rep != repOf(m_program.at(value.type)))
                fail(ValueID(v), "does not have the representation that its type requires"_s);
        }
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.at(m_function.at(id).type); }

    String place() const
    {
        if (!m_function.module || m_function.module.index() >= m_program.modules.size())
            return { };
        auto& module = m_program.modules[m_function.module.index()];
        String path = String::fromUTF8(m_program.string(module.displayPath ? module.displayPath : module.path));
        if (module.lineStarts.isEmpty())
            return makeString(" ("_s, path, ')');
        LineColumn start = m_program.lineColumnFor(m_function.module, m_function.position);
        return makeString(" ("_s, path, ':', start.line, ':', start.column, ')');
    }

    void failAtEndOf(unsigned block, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`", place(), ": b", block, " ", what);
        m_findings.append(makeString(out.toString(), "; please report it"_s));
    }

    void fail(ValueID id, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`", place(), ": v", id.index(), " ", nameOf(m_function.at(id).opcode), " ", what);
        m_findings.append(makeString(out.toString(), "; please report it"_s));
    }

    void fail(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`", place(), ": v", operand.index(), " of type ");
        m_program.dump(out, typeOf(operand));
        out.print(" is used as ", as, " of type ");
        m_program.dump(out, needed);
        if (user)
            out.print(" by v", user.index(), " ", nameOf(m_function.at(user).opcode));
        m_findings.append(makeString(out.toString(), "; please report it"_s));
    }

    void need(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        auto& type = typeOf(operand);
        if (&type != &needed && !type.isSubsetOf(needed, m_program))
            fail(user, operand, needed, as);
    }

    void need(ValueID user, ValueID operand, TypeID needed, ASCIILiteral as)
    {
        if (needed)
            need(user, operand, m_program.at(needed), as);
    }

    void requireSingleClassObject(ValueID id, LayoutID layout)
    {
        if (m_program.at(layout).kind == LayoutKind::Instance && !(m_program.at(layout).constructor && m_program.at(layout).hasFixedLayout && m_program.at(layout).isEvaluatedOnce))
            fail(id, "names a class that has no layout or is not evaluated exactly once"_s);
    }

    void requireObjectWithFieldsOf(ValueID id, const Value& value)
    {
        need(id, value.child(0), Type::forLayout(value.layout()), "an object"_s);
        auto& layout = m_program.at(value.layout());
        if (layout.fields[value.field()].flags & fieldIsNeverStored)
            fail(id, "names a field that is never stored, so it has no slot"_s);
        if (!layout.acceptsUntypedObjects)
            return;
        ValueID created = value.child(0);
        while (m_function.at(created).opcode == Opcode::Cast)
            created = m_function.at(created).child(0);
        if (m_function.at(created).opcode != Opcode::NewObjectWithShape || (m_function.at(created).layout() != value.layout() && !std::ranges::binary_search(layout.subtypes, m_function.at(created).layout())))
            fail(id, "names a field of a value that might not have the layout"_s);
    }

    void verifyInitialStores(const BitVector& isReached)
    {
        auto isInitialStore = [&](ValueID id, const Value& like, uint32_t index) {
            auto& value = m_function.at(id);
            return value.opcode == Opcode::PutById && value.initialStoreIndex() == index && value.inlineFrame == like.inlineFrame && value.child(0) == like.child(0);
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (size_t i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                auto& store = m_function.at(id);
                if (store.opcode != Opcode::PutById || !store.isInitialStore())
                    continue;
                if (store.inlineFrame >= std::max<size_t>(m_function.inlineFrames.size(), 1))
                    continue;
                uint32_t index = store.initialStoreIndex();
                auto list = initialStoresOf(store.inlineFrame ? m_program.at(m_function.inlineFrames[store.inlineFrame].callee) : m_function);
                if (index >= list.size() || (!store.inlineFrame && list[index] != id)) {
                    fail(id, "is not an initial store of the function it comes from"_s);
                    continue;
                }
                if (index && (!i || !isInitialStore(block.values[i - 1], store, index - 1)))
                    fail(id, "is an initial store that does not directly follow the previous one"_s);
                if (index + 1 < list.size() && (i + 1 == block.values.size() || !isInitialStore(block.values[i + 1], store, index + 1)))
                    fail(id, "is an initial store that is not directly followed by the next one"_s);
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke)
                    fail(id, "is an initial store inside the range of an exception handler"_s);
                for (uint32_t earlier = 0; earlier < index && earlier < i; ++earlier) {
                    if (m_function.at(block.values[i - 1 - earlier]).string() == store.string())
                        fail(id, "is an initial store to a name that was already stored"_s);
                }
                if (store.inlineFrame)
                    continue;
                LayoutID supertype = typeOf(store.child(0)).singleLayout();
                if (!supertype || !fieldOf(m_program, typeOf(store.child(0)), store.string()) || inheritsThroughLayoutPrototype(m_program, supertype, store.string()))
                    fail(id, "is an initial store to a name that is not a field or may be inherited"_s);
                bool hasThis = m_function.kind == FunctionKind::Plain && m_function.has(Format::FunctionHasThis) && !m_function.has(FunctionUsesArgumentBuffer) && !m_function.isGenericEntrypoint();
                auto& entry = m_function.blocks[0].parameters;
                if (!hasThis || !m_function.thisType || m_function.captureTypes.size() >= entry.size() || store.child(0) != entry[m_function.captureTypes.size()])
                    fail(id, "is an initial store to a value other than `this` of a plain function"_s);
            }
        }
    }

    void fail(ValueID operand, ValueID user, unsigned block, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`", place(), ": v", operand.index(), " ", nameOf(m_function.at(operand).opcode), " ", what, ", and is used by ");
        if (user)
            out.print("v", user.index(), " ", nameOf(m_function.at(user).opcode), " in b", block);
        else
            out.print("the end of b", block);
        m_findings.append(makeString(out.toString(), "; please report it"_s));
    }

    void verifyDefinitions(const BitVector& isReached)
    {
        static constexpr unsigned none = UINT_MAX;
        unsigned numberOfBlocks = m_function.blocks.size();
        unsigned numberOfNodes = 2 * numberOfBlocks;
        auto hasThrowingValue = [&](unsigned block) {
            return m_function.blocks[block].terminator.kind == TerminatorKind::Invoke && !m_function.blocks[block].values.isEmpty();
        };
        auto throwingValueNode = [&](unsigned block) {
            return numberOfBlocks + block;
        };
        auto numberOfSuccessors = [&](unsigned node) -> unsigned {
            if (node >= numberOfBlocks)
                return 1;
            return hasThrowingValue(node) ? 2 : m_function.blocks[node].terminator.targets.size();
        };
        auto successorAt = [&](unsigned node, unsigned index) -> unsigned {
            if (node >= numberOfBlocks)
                return m_function.blocks[node - numberOfBlocks].terminator.targets[0].block.index();
            if (hasThrowingValue(node))
                return index ? m_function.blocks[node].terminator.targets[1].block.index() : throwingValueNode(node);
            return m_function.blocks[node].terminator.targets[index].block.index();
        };

        Vector<unsigned> order;
        BitVector isVisited;
        isVisited.set(0);
        Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
        while (!stack.isEmpty()) {
            auto [node, next] = stack.last();
            if (next == numberOfSuccessors(node)) {
                order.append(node);
                stack.removeLast();
                continue;
            }
            ++stack.last().second;
            unsigned successor = successorAt(node, next);
            if (!isVisited.set(successor))
                stack.append({ successor, 0u });
        }
        order.reverse();
        Vector<unsigned> orderIndex;
        orderIndex.fill(none, numberOfNodes);
        struct Predecessors {
            std::span<const unsigned> operator[](unsigned node) const { return all.span().subspan(first[node], first[node + 1] - first[node]); }
            Vector<unsigned> first;
            Vector<unsigned> all;
        } predecessors;
        predecessors.first.fill(0, numberOfNodes + 2);
        for (unsigned i = 0; i < order.size(); ++i) {
            orderIndex[order[i]] = i;
            for (unsigned k = 0; k < numberOfSuccessors(order[i]); ++k)
                ++predecessors.first[successorAt(order[i], k) + 2];
        }
        for (unsigned node = 0; node < numberOfNodes; ++node)
            predecessors.first[node + 2] += predecessors.first[node + 1];
        predecessors.all.grow(predecessors.first[numberOfNodes + 1]);
        for (unsigned node : order) {
            for (unsigned k = 0; k < numberOfSuccessors(node); ++k)
                predecessors.all[predecessors.first[successorAt(node, k) + 1]++] = node;
        }

        Vector<unsigned> dominator;
        dominator.fill(none, numberOfNodes);
        dominator[0] = 0;
        for (bool changed = !tryFindImmediateDominators(order.size(), [&](unsigned i) { return order[i]; }, predecessors, dominator); changed;) {
            changed = false;
            for (unsigned i = 1; i < order.size(); ++i) {
                unsigned found = none;
                for (unsigned other : predecessors[order[i]]) {
                    if (dominator[other] == none)
                        continue;
                    if (found == none) {
                        found = other;
                        continue;
                    }
                    while (found != other) {
                        while (orderIndex[other] > orderIndex[found])
                            other = dominator[other];
                        while (orderIndex[found] > orderIndex[other])
                            found = dominator[found];
                    }
                }
                if (dominator[order[i]] != found) {
                    dominator[order[i]] = found;
                    changed = true;
                }
            }
        }
        Vector<unsigned> firstDominated;
        Vector<unsigned> nextDominated;
        firstDominated.fill(none, numberOfNodes);
        nextDominated.fill(none, numberOfNodes);
        for (unsigned i = order.size(); i-- > 1;)
            nextDominated[order[i]] = std::exchange(firstDominated[dominator[order[i]]], order[i]);
        Vector<unsigned> enter;
        Vector<unsigned> leave;
        enter.fill(0, numberOfNodes);
        leave.fill(0, numberOfNodes);
        unsigned clock = 0;
        enter[0] = clock++;
        stack.append({ 0u, firstDominated[0] });
        while (!stack.isEmpty()) {
            auto [node, next] = stack.last();
            if (next == none) {
                leave[node] = clock++;
                stack.removeLast();
                continue;
            }
            stack.last().second = nextDominated[next];
            enter[next] = clock++;
            stack.append({ next, firstDominated[next] });
        }

        Vector<unsigned> nodeOf;
        nodeOf.fill(none, m_function.values.size());
        Vector<unsigned> indexInNode;
        indexInNode.fill(0, m_function.values.size());
        auto define = [&](ValueID id, unsigned node, unsigned at) {
            if (nodeOf[id.index()] != none)
                fail(id, "is defined twice"_s);
            nodeOf[id.index()] = node;
            indexInNode[id.index()] = at;
        };
        auto valueNodeAt = [&](unsigned block, size_t index) {
            return hasThrowingValue(block) && index + 1 == m_function.blocks[block].values.size() ? throwingValueNode(block) : block;
        };
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID parameter : block.parameters)
                define(parameter, b, 0);
            for (size_t i = 0; i < block.values.size(); ++i)
                define(block.values[i], valueNodeAt(b, i), i + 1);
        }
        auto use = [&](ValueID operand, ValueID user, unsigned block, unsigned node, unsigned at) {
            unsigned definedIn = nodeOf[operand.index()];
            if (definedIn == none) {
                fail(operand, user, block, "has no reachable definition"_s);
                return;
            }
            bool isDefined = definedIn == node ? indexInNode[operand.index()] < at : enter[definedIn] <= enter[node] && leave[node] <= leave[definedIn];
            if (!isDefined)
                fail(operand, user, block, "is not defined on every path to its use"_s);
        };
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (size_t i = 0; i < block.values.size(); ++i) {
                m_function.at(block.values[i]).forEachChild([&](ValueID child) {
                    use(child, block.values[i], b, valueNodeAt(b, i), i + 1);
                });
            }
            unsigned end = block.values.size() + 1;
            if (block.terminator.value)
                use(block.terminator.value, ValueID(), b, b, end);
            for (size_t t = 0; t < block.terminator.targets.size(); ++t) {
                bool isAfterThrowingValue = hasThrowingValue(b) && !t;
                bool isBeforeThrowingValue = hasThrowingValue(b) && t;
                for (ValueID argument : block.terminator.targets[t].arguments)
                    use(argument, ValueID(), b, isAfterThrowingValue ? throwingValueNode(b) : b, isBeforeThrowingValue ? end - 1 : end);
            }
        }
    }

    void verifyEntry()
    {
        auto& entry = m_function.blocks[0].parameters;
        unsigned at = 0;
        auto is = [&](const Type& type, ASCIILiteral what) {
            if (at >= entry.size())
                return;
            if (!type.isSubsetOf(typeOf(entry[at]), m_program))
                fail(ValueID(), entry[at], type, what);
            ++at;
        };
        for (TypeID capture : m_function.captureTypes)
            is(m_program.at(capture), "a capture"_s);
        if (m_function.has(FunctionUsesArgumentBuffer) || m_function.isGenericEntrypoint())
            return;
        if (m_function.has(Format::FunctionHasThis))
            is(m_function.thisType ? m_program.at(m_function.thisType) : Type::anything(), "`this`"_s);
        for (TypeID parameter : m_function.parameterTypes)
            is(parameter ? m_program.at(parameter) : Type::anything(), "a parameter"_s);
    }

    void verify(ValueID id, const Value& value)
    {
        if (value.inlineFrame && (value.inlineFrame >= m_function.inlineFrames.size() || m_function.inlineFrames[value.inlineFrame].parent >= value.inlineFrame))
            fail(id, "has no valid inline frame"_s);
        switch (value.opcode) {
        case Opcode::PutModuleVariable:
            need(id, value.child(0), m_program.variables[value.variable().index()].type, "the variable's value"_s);
            break;
        case Opcode::PutStaticField:
            need(id, value.child(0), m_program.at(value.layout()).staticFields[value.immediates[1]].type, "the static field's value"_s);
            break;
        case Opcode::LogicalNot:
            need(id, value.child(0), m_program.types.boolean, "the negated operand"_s);
            break;
        case Opcode::NewObjectWithShape:
            if (m_program.at(value.type) != Type::forLayout(value.layout()))
                fail(id, "does not have the layout it is created with"_s);
            if (!!value.child(0) != (m_function.allocations[value.allocation()].discriminant != UINT16_MAX))
                fail(id, "has a child and no discriminant, or a discriminant and no child"_s);
            requireSingleClassObject(id, value.layout());
            break;
        case Opcode::IsInstanceOfClass:
            if (m_program.at(value.layout()).kind != LayoutKind::Instance)
                fail(id, "names a layout that is not a class"_s);
            requireSingleClassObject(id, value.layout());
            break;
        case Opcode::FunctionReference:
            if (auto& function = m_program.at(value.function()); function.kind == FunctionKind::Constructor && !(function.constructedClass && m_program.at(function.constructedClass).isEvaluatedOnce))
                fail(id, "is the unique class object of a class that is not evaluated exactly once"_s);
            break;
        case Opcode::GetField:
            requireObjectWithFieldsOf(id, value);
            if (m_program.at(value.layout()).fields[value.field()].flags & fieldHoldsCachedClosure)
                fail(id, "reads the slot of a closure that is created once"_s);
            break;
        case Opcode::PutField:
            requireObjectWithFieldsOf(id, value);
            need(id, value.child(1), m_program.at(value.layout()).fields[value.field()].type, "the field's value"_s);
            if (m_program.at(value.layout()).fields[value.field()].flags & fieldHoldsCachedClosure)
                need(id, value.child(1), m_program.at(m_program.types.undefined), "the initial value of a variable that holds a closure created once"_s);
            break;
        case Opcode::GetById:
            if (value.isAtKnownSlot() && value.knownSlot() >= Value::numberOfKnownSlots)
                fail(id, "names an inline slot that no object has"_s);
            break;
        case Opcode::PutById:
            if ((value.isAtKnownSlot() || value.isFirstStoreToThis()) && value.knownSlot() >= Value::numberOfKnownSlots)
                fail(id, "names an inline slot that no object has"_s);
            if (auto access = fieldOf(m_program, typeOf(value.child(0)), value.string()))
                need(id, value.child(1), m_program.at(access->layout).fields[access->field].type, "the field's value"_s);
            break;
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce: {
            auto& callee = m_program.at(value.function());
            if (callee.kind == FunctionKind::Constructor) {
                LayoutID environment = callee.constructedClass ? m_program.at(callee.constructedClass).environment : LayoutID();
                if (!environment)
                    fail(id, "is a second class object of a class that is evaluated once"_s);
                else if (value.opcode != Opcode::NewClosure || value.arguments.size() != 1)
                    fail(id, "is a class object that does not capture exactly the environment of its class"_s);
                else if (typeOf(value.arguments[0]) != Type::forLayout(environment))
                    fail(id, value.arguments[0], Type::forLayout(environment), "the environment of a class"_s);
                break;
            }
            if (value.arguments.size() != callee.captureTypes.size()) {
                fail(id, "captures a different number of values than its function expects"_s);
                break;
            }
            for (unsigned i = 0; i < value.arguments.size(); ++i)
                need(id, value.arguments[i], callee.captureTypes[i], "a capture"_s);
            if (value.opcode == Opcode::NewClosure)
                break;
            LayoutID environment = typeOf(value.child(0)).singleLayout();
            if (!environment || typeOf(value.child(0)) != Type::forLayout(environment) || m_program.at(environment).kind != LayoutKind::Environment || value.field() >= m_program.at(environment).fields.size()) {
                fail(id, "is stored in something other than an environment variable"_s);
                break;
            }
            auto& field = m_program.at(environment).fields[value.field()];
            if (!(field.flags & fieldHoldsCachedClosure) || field.singleFunction != value.function())
                fail(id, "is stored in a variable that is not declared to hold only closures of its function"_s);
            break;
        }
        case Opcode::GetCapture:
            if (value.captureIndex() >= m_program.at(value.function()).captureTypes.size() || value.type != m_program.at(value.function()).captureTypes[value.captureIndex()])
                fail(id, "does not have the type that its function captures at that position"_s);
            break;
        case Opcode::GetArgument:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::GetMyArgumentByVal:
        case Opcode::CallForwardVarargs:
        case Opcode::ConstructSuperForwardVarargs:
            if (!m_function.has(FunctionUsesArgumentBuffer))
                fail(id, "is in a function that does not receive its arguments in memory"_s);
            break;
        case Opcode::CallDirect: {
            auto& callee = m_program.at(value.function());
            if (callee.has(FunctionUsesArgumentBuffer) || callee.isGenericEntrypoint())
                break;
            unsigned at = 0;
            auto pass = [&](const Type& type, ASCIILiteral as) {
                if (at < value.arguments.size())
                    need(id, value.arguments[at], type, as);
                ++at;
            };
            for (TypeID capture : callee.captureTypes)
                pass(m_program.at(capture), "a capture"_s);
            if (callee.has(Format::FunctionHasThis))
                pass(callee.thisType ? m_program.at(callee.thisType) : Type::anything(), "`this`"_s);
            for (TypeID parameter : callee.parameterTypes)
                pass(parameter ? m_program.at(parameter) : Type::anything(), "an argument"_s);
            break;
        }
        case Opcode::Cast:
            if (value.castReason() == CastReason::Upcast)
                need(id, value.child(0), value.type, "a value"_s);
            if (value.castReason() == CastReason::Coerce) {
                auto& type = m_program.at(value.type);
                auto& coercedTo = m_program.at(value.coercedType());
                if (!coercedTo.isSubsetOf(type, m_program))
                    fail(id, "coerces to a type that its own type does not include"_s);
                if (coercedTo.hasObjects() && !typeOf(value.child(0)).withoutPrimitives(Primitive::All).isSubsetOf(type, m_program))
                    fail(id, value.child(0), type, "an object that a coercion may leave unchanged"_s);
            }
            break;
        case Opcode::Operation: {
            Vector<Type, 4> arguments;
            for (ValueID argument : value.arguments)
                arguments.append(typeOf(argument));
            if (!resolveBuiltinOverload(m_program.operationOf(value), arguments.span())) {
                StringPrintStream out;
                out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`", place(), ": v", id.index(), " Operation ", Builtins::singleton().at(value.builtin()).name, ": the built-ins table has no entry for its ", value.arguments.size(), " arguments");
                m_findings.append(makeString(out.toString(), "; please report it"_s));
            }
            if (m_program.operationOf(value) == Operations::NewObjectOfClass) {
                LayoutID created = m_program.at(value.assertedType() ? value.assertedType() : value.type).singleLayout();
                if (!created || m_program.at(created).kind != LayoutKind::Instance || !m_program.at(created).isAllocated)
                    fail(id, "creates an instance of a layout that is not an instantiable class"_s);
            }
            break;
        }
        default:
            break;
        }
    }

    void verify(const Terminator& terminator)
    {
        for (auto& target : terminator.targets) {
            auto& parameters = m_function.at(target.block).parameters;
            for (unsigned i = 0; i < target.arguments.size() && i < parameters.size(); ++i)
                need(parameters[i], target.arguments[i], m_function.at(parameters[i]).type, "a block parameter"_s);
        }
        switch (terminator.kind) {
        case TerminatorKind::Branch:
            need(ValueID(), terminator.value, m_program.types.boolean, "a condition"_s);
            break;
        case TerminatorKind::Switch:
            need(ValueID(), terminator.value, Type::integer(Range::full()), "the subject of a switch"_s);
            break;
        case TerminatorKind::Return:
            if (terminator.value && !m_function.has(Format::FunctionIsAsync | Format::FunctionIsGenerator))
                need(ValueID(), terminator.value, m_function.resultType, "the result"_s);
            break;
        default:
            break;
        }
    }

    const Program& m_program;
    FunctionID m_id;
    const Function& m_function;
    Vector<String>& m_findings;
    VerifierScope m_scope;
};

} // anonymous namespace

void verify(const Program& program, FunctionID id, const Function& function, Vector<String>& findings, VerifierScope scope)
{
    Verifier(program, id, function, findings, scope).run();
}

void verifyRegExpLiterals(const Program& program, Vector<String>& findings)
{
    for (auto [patternID, flagsID] : program.regExpLiterals) {
        StringView flags { byteCast<Latin1Character>(program.string(flagsID)) };
        String pattern = String::fromUTF8(program.string(patternID));
        auto error = Yarr::parseFlags(flags) ? Yarr::ErrorCode::NoError : Yarr::ErrorCode::InvalidRegularExpressionFlags;
        if (!Yarr::hasError(error) && !pattern.isNull())
            error = Yarr::checkSyntax(pattern, flags);
        if (!Yarr::hasError(error))
            continue;
        StringPrintStream out;
        out.print("/", pattern, "/", flags, ": SyntaxError: ", Yarr::errorMessage(error));
        findings.append(out.toString());
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
