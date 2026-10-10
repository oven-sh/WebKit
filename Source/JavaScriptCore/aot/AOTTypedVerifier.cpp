/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include <wtf/StringPrintStream.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// Compiled code tests nothing that a type says. So a value that is used as something its type does not say would be read as what it is
// not. This makes sure there is none: it goes once over a function and compares types that are given. It finds out no type.
class Verifier {
public:
    Verifier(const Program& program, FunctionID id, const Function& function, Vector<String>& findings)
        : m_program(program)
        , m_id(id)
        , m_function(function)
        , m_findings(findings)
        , m_newObjectOfClass(program.find("NewObjectOfClass"_s.span8()).value_or(StringID()))
    {
    }

    void run()
    {
        if (m_function.blocks.isEmpty())
            return;
        verifyEntry();
        // What nothing gets to is never run, and not lowered.
        BitVector isReached;
        Vector<unsigned> worklist { 0u };
        isReached.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReached.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values)
                verify(id, m_function.at(id));
            verify(block.terminator);
        }
        verifyInitialStores(isReached);
        verifyDefinitions(isReached);
        for (unsigned v = 0; v < m_function.values.size(); ++v) {
            auto& value = m_function.values[v];
            if (!value.type)
                fail(ValueID(v), "has no type"_s);
            else if (value.rep != repOf(m_program.at(value.type)))
                fail(ValueID(v), "is not kept the way its type says"_s);
        }
    }

private:
    const Type& typeOf(ValueID id) const { return m_program.at(m_function.at(id).type); }

    void fail(ValueID id, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", id.index(), " ", nameOf(m_function.at(id).opcode), " ", what);
        m_findings.append(out.toString());
    }

    void fail(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", operand.index(), " of type ");
        m_program.dump(out, typeOf(operand));
        out.print(" is used as ", as, " of type ");
        m_program.dump(out, needed);
        if (user)
            out.print(" by v", user.index(), " ", nameOf(m_function.at(user).opcode));
        m_findings.append(out.toString());
    }

    void need(ValueID user, ValueID operand, const Type& needed, ASCIILiteral as)
    {
        if (!typeOf(operand).isWithin(needed, m_program))
            fail(user, operand, needed, as);
    }

    // None: anything will do.
    void need(ValueID user, ValueID operand, TypeID needed, ASCIILiteral as)
    {
        if (needed)
            need(user, operand, m_program.at(needed), as);
    }

    // The prototype of an object goes with its allocation, and whether an object is of a class goes by its shape.
    void needOneClassObject(ValueID id, LayoutID layout)
    {
        if (m_program.at(layout).kind == LayoutKind::Instance && !(m_program.at(layout).constructor && m_program.at(layout).isLaidOut && m_program.at(layout).isEvaluatedOnce))
            fail(id, "names a class that is not laid out or not evaluated once"_s);
    }

    // GetField and PutField go by where the field is, untested. The type of the operand vouches for that, but for a layout that admits
    // foreign objects, of which any object is. There the operand has to be an allocation of this function.
    void needObjectWithFieldsOf(ValueID id, const Value& value)
    {
        need(id, value.child(0), Type::view(value.layout()), "an object"_s);
        auto& layout = m_program.at(value.layout());
        if (layout.fields[value.field()].flags & fieldIsNeverStored)
            fail(id, "names a field that nothing stores, which has no place"_s);
        if (!layout.admitsForeignObjects)
            return;
        ValueID made = value.child(0);
        while (m_function.at(made).opcode == Opcode::Cast)
            made = m_function.at(made).child(0);
        if (m_function.at(made).opcode != Opcode::NewObjectWithShape || (m_function.at(made).layout() != value.layout() && !std::ranges::binary_search(layout.members, m_function.at(made).layout())))
            fail(id, "names a field of what may be no object of the layout"_s);
    }

    // The lowering carries them out together, and only so are the fields counted among the keys of the object: see Lowering::lowerInitialStores(). So the
    // stores of a list stay together, in order, all of them, wherever they were copied to.
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
                    fail(id, "is no initial store of the function it is from"_s);
                    continue;
                }
                if (index && (!i || !isInitialStore(block.values[i - 1], store, index - 1)))
                    fail(id, "is an initial store that does not follow the one before it"_s);
                if (index + 1 < list.size() && (i + 1 == block.values.size() || !isInitialStore(block.values[i + 1], store, index + 1)))
                    fail(id, "is an initial store that the next one does not follow"_s);
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke)
                    fail(id, "is an initial store that a handler watches"_s);
                for (uint32_t earlier = 0; earlier < index && earlier < i; ++earlier) {
                    if (m_function.at(block.values[i - 1 - earlier]).string() == store.string())
                        fail(id, "is an initial store to a name that was stored to"_s);
                }
                // In a copy the receiver may be known to be of a layout that lacks a field of the list.
                if (store.inlineFrame)
                    continue;
                LayoutID view = typeOf(store.child(0)).onlyView();
                if (!view || !fieldOf(m_program, typeOf(store.child(0)), store.string()) || inheritsByItsLayout(m_program, view, store.string()))
                    fail(id, "is an initial store to what is no field, or to what may be inherited"_s);
                bool hasThis = m_function.kind == FunctionKind::Plain && m_function.has(Format::FunctionHasThis) && !m_function.has(FunctionUsesArgumentBuffer) && !m_function.isGenericEntrypoint();
                auto& entry = m_function.blocks[0].parameters;
                if (!hasThis || !m_function.thisType || m_function.captureTypes.size() >= entry.size() || store.child(0) != entry[m_function.captureTypes.size()])
                    fail(id, "is an initial store to what is not `this` of a plain function"_s);
            }
        }
    }

    // Null: by the end of the block.
    void fail(ValueID operand, ValueID user, unsigned block, ASCIILiteral what)
    {
        StringPrintStream out;
        out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", operand.index(), " ", nameOf(m_function.at(operand).opcode), " ", what, ", and is used by ");
        if (user)
            out.print("v", user.index(), " ", nameOf(m_function.at(user).opcode), " in b", block);
        else
            out.print("the end of b", block);
        m_findings.append(out.toString());
    }

    // The definition of an operand dominates its use. The edge of an Invoke to the handler is taken from within the last value of the
    // block, which is not defined then: see Lowering::lowerBlock(). So such a block counts as two: all but that value, which leads to
    // the handler, and that value, which leads on.
    void verifyDefinitions(const BitVector& isReached)
    {
        static constexpr unsigned none = UINT_MAX;
        unsigned numberOfBlocks = m_function.blocks.size();
        unsigned numberOfNodes = 2 * numberOfBlocks;
        auto hasWatchedValue = [&](unsigned block) {
            return m_function.blocks[block].terminator.kind == TerminatorKind::Invoke && !m_function.blocks[block].values.isEmpty();
        };
        auto nodeOfWatchedValue = [&](unsigned block) {
            return numberOfBlocks + block;
        };
        Vector<Vector<unsigned, 2>> successors(numberOfNodes);
        Vector<Vector<unsigned, 2>> predecessors(numberOfNodes);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isReached.get(b))
                continue;
            auto& targets = m_function.blocks[b].terminator.targets;
            if (hasWatchedValue(b)) {
                successors[b] = { nodeOfWatchedValue(b), targets[1].block.index() };
                successors[nodeOfWatchedValue(b)] = { targets[0].block.index() };
                continue;
            }
            for (auto& target : targets)
                successors[b].append(target.block.index());
        }

        Vector<unsigned> order;
        BitVector isVisited;
        isVisited.set(0);
        Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
        while (!stack.isEmpty()) {
            auto [node, next] = stack.last();
            if (next == successors[node].size()) {
                order.append(node);
                stack.removeLast();
                continue;
            }
            ++stack.last().second;
            unsigned successor = successors[node][next];
            if (!isVisited.set(successor))
                stack.append({ successor, 0u });
        }
        order.reverse();
        Vector<unsigned> place;
        place.fill(none, numberOfNodes);
        for (unsigned i = 0; i < order.size(); ++i) {
            place[order[i]] = i;
            for (unsigned successor : successors[order[i]])
                predecessors[successor].append(order[i]);
        }

        Vector<unsigned> dominator;
        dominator.fill(none, numberOfNodes);
        dominator[0] = 0;
        for (bool changed = true; changed;) {
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
                        while (place[other] > place[found])
                            other = dominator[other];
                        while (place[found] > place[other])
                            found = dominator[found];
                    }
                }
                if (dominator[order[i]] != found) {
                    dominator[order[i]] = found;
                    changed = true;
                }
            }
        }
        Vector<Vector<unsigned, 2>> dominated(numberOfNodes);
        for (unsigned i = 1; i < order.size(); ++i)
            dominated[dominator[order[i]]].append(order[i]);
        Vector<unsigned> enter;
        Vector<unsigned> leave;
        enter.fill(0, numberOfNodes);
        leave.fill(0, numberOfNodes);
        unsigned clock = 0;
        enter[0] = clock++;
        stack.append({ 0u, 0u });
        while (!stack.isEmpty()) {
            auto [node, next] = stack.last();
            if (next == dominated[node].size()) {
                leave[node] = clock++;
                stack.removeLast();
                continue;
            }
            ++stack.last().second;
            enter[dominated[node][next]] = clock++;
            stack.append({ dominated[node][next], 0u });
        }

        // Where a value is defined: the node, and its place there. A parameter is at 0, the first value of the block at 1.
        Vector<unsigned> nodeOf;
        nodeOf.fill(none, m_function.values.size());
        Vector<unsigned> placeInNode;
        placeInNode.fill(0, m_function.values.size());
        auto define = [&](ValueID id, unsigned node, unsigned at) {
            if (nodeOf[id.index()] != none)
                fail(id, "is defined twice"_s);
            nodeOf[id.index()] = node;
            placeInNode[id.index()] = at;
        };
        auto nodeOfValueAt = [&](unsigned block, size_t index) {
            return hasWatchedValue(block) && index + 1 == m_function.blocks[block].values.size() ? nodeOfWatchedValue(block) : block;
        };
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (ValueID parameter : block.parameters)
                define(parameter, b, 0);
            for (size_t i = 0; i < block.values.size(); ++i)
                define(block.values[i], nodeOfValueAt(b, i), i + 1);
        }
        auto use = [&](ValueID operand, ValueID user, unsigned block, unsigned node, unsigned at) {
            unsigned definedIn = nodeOf[operand.index()];
            if (definedIn == none) {
                fail(operand, user, block, "is defined nowhere that is reached"_s);
                return;
            }
            bool isDefined = definedIn == node ? placeInNode[operand.index()] < at : enter[definedIn] <= enter[node] && leave[node] <= leave[definedIn];
            if (!isDefined)
                fail(operand, user, block, "is not defined on every way there"_s);
        };
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!isReached.get(b))
                continue;
            auto& block = m_function.blocks[b];
            for (size_t i = 0; i < block.values.size(); ++i) {
                m_function.at(block.values[i]).forEachChild([&](ValueID child) {
                    use(child, block.values[i], b, nodeOfValueAt(b, i), i + 1);
                });
            }
            unsigned end = block.values.size() + 1;
            if (block.terminator.value)
                use(block.terminator.value, ValueID(), b, b, end);
            for (size_t t = 0; t < block.terminator.targets.size(); ++t) {
                // The handler is passed what there is before the watched value.
                bool isAfterWatchedValue = hasWatchedValue(b) && !t;
                bool isBeforeWatchedValue = hasWatchedValue(b) && t;
                for (ValueID argument : block.terminator.targets[t].arguments)
                    use(argument, ValueID(), b, isAfterWatchedValue ? nodeOfWatchedValue(b) : b, isBeforeWatchedValue ? end - 1 : end);
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
            // Whoever calls passes what the declaration says. The body may take it for no less.
            if (!type.isWithin(typeOf(entry[at]), m_program))
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
            need(id, value.child(0), m_program.variables[value.variable().index()].type, "what the variable holds"_s);
            break;
        case Opcode::LogicalNot:
            need(id, value.child(0), m_program.types.boolean, "what is negated"_s);
            break;
        case Opcode::NewObjectWithShape:
            if (m_program.at(value.type) != Type::view(value.layout()))
                fail(id, "is not of the layout it is made with"_s);
            // The child says how large the object is made, and nothing of any type.
            if (!!value.child(0) != (m_function.allocations[value.allocation()].discriminant != UINT16_MAX))
                fail(id, "has a child and no discriminant, or a discriminant and no child"_s);
            needOneClassObject(id, value.layout());
            break;
        case Opcode::IsInstanceOfClass:
            if (m_program.at(value.layout()).kind != LayoutKind::Instance)
                fail(id, "asks for the objects of what is no class"_s);
            needOneClassObject(id, value.layout());
            break;
        // The closure of a constructor is the class object.
        case Opcode::FunctionReference:
            if (auto& function = m_program.at(value.function()); function.kind == FunctionKind::Constructor && !(function.constructedClass && m_program.at(function.constructedClass).isEvaluatedOnce))
                fail(id, "is the one class object of a class that is not evaluated once"_s);
            break;
        case Opcode::GetField:
            needObjectWithFieldsOf(id, value);
            if (m_program.at(value.layout()).fields[value.field()].flags & fieldHoldsClosureMadeOnce)
                fail(id, "reads where a closure that is made once is kept"_s);
            break;
        case Opcode::PutField:
            needObjectWithFieldsOf(id, value);
            need(id, value.child(1), m_program.at(value.layout()).fields[value.field()].type, "what the field holds"_s);
            if (m_program.at(value.layout()).fields[value.field()].flags & fieldHoldsClosureMadeOnce)
                need(id, value.child(1), m_program.at(m_program.types.undefined), "what a variable starts with that a closure that is made once is kept in"_s);
            break;
        case Opcode::PutById:
            if (auto access = fieldOf(m_program, typeOf(value.child(0)), value.string()))
                need(id, value.child(1), m_program.at(access->layout).fields[access->field].type, "what the field holds"_s);
            break;
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce: {
            auto& callee = m_program.at(value.function());
            // A class object captures the environment of its class, whether its constructor takes that or not: the run time makes the closures
            // of the members with it.
            if (callee.kind == FunctionKind::Constructor) {
                LayoutID environment = callee.constructedClass ? m_program.at(callee.constructedClass).environment : LayoutID();
                if (!environment)
                    fail(id, "is another class object of a class that is evaluated once"_s);
                else if (value.opcode != Opcode::NewClosure || value.arguments.size() != 1)
                    fail(id, "is a class object that does not capture the environment of its class and nothing else"_s);
                // That very type: the lowering goes by it for what the cell holds, and the run time hands the word on to the members.
                else if (typeOf(value.arguments[0]) != Type::view(environment))
                    fail(id, value.arguments[0], Type::view(environment), "the environment of a class"_s);
                break;
            }
            if (value.arguments.size() != callee.captureTypes.size()) {
                fail(id, "captures more or less than its function takes"_s);
                break;
            }
            for (unsigned i = 0; i < value.arguments.size(); ++i)
                need(id, value.arguments[i], callee.captureTypes[i], "a capture"_s);
            if (value.opcode == Opcode::NewClosure)
                break;
            LayoutID environment = typeOf(value.child(0)).onlyView();
            if (!environment || typeOf(value.child(0)) != Type::view(environment) || m_program.at(environment).kind != LayoutKind::Environment || value.field() >= m_program.at(environment).fields.size()) {
                fail(id, "is kept in what is no variable of an environment"_s);
                break;
            }
            auto& field = m_program.at(environment).fields[value.field()];
            if (!(field.flags & fieldHoldsClosureMadeOnce) || field.onlyFunction != value.function())
                fail(id, "is kept in a variable that is not said to hold closures of its function only"_s);
            break;
        }
        case Opcode::GetCapture:
            if (value.captureIndex() >= m_program.at(value.function()).captureTypes.size() || value.type != m_program.at(value.function()).captureTypes[value.captureIndex()])
                fail(id, "is not of the type of what its function captures there"_s);
            break;
        case Opcode::GetArgument:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::GetMyArgumentByVal:
        case Opcode::CallForwardVarargs:
            if (!m_function.has(FunctionUsesArgumentBuffer))
                fail(id, "is in a function that does not take its arguments in memory"_s);
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
            // Whatever comes of it is of the type of the cast untested: a value of the type after `as`, or the object that went in.
            if (value.castReason() == CastReason::Coerce) {
                auto& type = m_program.at(value.type);
                auto& coercedTo = m_program.at(value.coercedType());
                if (!coercedTo.isWithin(type, m_program))
                    fail(id, "coerces to a type that its own does not take in"_s);
                if (coercedTo.hasObjects() && !typeOf(value.child(0)).withoutPrimitives(Primitive::All).isWithin(type, m_program))
                    fail(id, value.child(0), type, "an object that a coercion may leave as it is"_s);
            }
            break;
        // The run time makes the object, so no allocation says that the layout has objects.
        case Opcode::Operation: {
            Vector<Type, 4> arguments;
            for (ValueID argument : value.arguments)
                arguments.append(typeOf(argument));
            if (!applicableEntry(Builtins::singleton().find(BuiltinKind::Operation, { }, m_program.builtinName(value.string())), arguments.span())) {
                StringPrintStream out;
                out.print("f", m_id.index(), " `", String::fromUTF8(m_program.string(m_function.name)), "`: v", id.index(), " Operation ", String::fromUTF8(m_program.string(value.string())), ": the built-ins table has no entry for its ", value.arguments.size(), " arguments");
                m_findings.append(out.toString());
            }
            if (value.string() == m_newObjectOfClass) {
                LayoutID made = m_program.at(value.assertedType ? value.assertedType : value.type).onlyView();
                if (!made || m_program.at(made).kind != LayoutKind::Instance || !m_program.at(made).isAllocated)
                    fail(id, "makes an object of what is no class that has objects"_s);
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
            need(ValueID(), terminator.value, Type::integer(Range::full()), "what a switch goes by"_s);
            break;
        case TerminatorKind::Return:
            // What an async function or a generator is declared to yield is not what `return` carries in its body.
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
    StringID m_newObjectOfClass;
};

} // anonymous namespace

void verify(const Program& program, FunctionID id, const Function& function, Vector<String>& findings)
{
    Verifier(program, id, function, findings).run();
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
