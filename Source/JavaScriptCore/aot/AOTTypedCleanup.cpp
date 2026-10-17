/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include "Interpreter.h"
#include "Options.h"
#include <wtf/BitSet.h>
#include <wtf/BitVector.h>
#include <wtf/LayeredHashMap.h>
#include <wtf/text/ASCIIFastPath.h>

namespace JSC { namespace AOT { namespace Typed {

std::optional<LanguageOperation> languageOperationOf(const Program& program, const Value& value)
{
    if (value.opcode != Opcode::Operation && value.opcode != Opcode::CallBuiltin)
        return std::nullopt;
    auto& builtins = Builtins::singleton();
    BuiltinID entry = program.operationOf(value);
    if (!entry || builtins.at(entry).implementation.kind != BuiltinImplementation::Operation)
        return std::nullopt;
    return builtins.at(entry).implementation.operation;
}

using NameSet = UncheckedKeyHashSet<uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

static void collectPropertiesAssignedToObject(const Program& program, const Function& function, ValueID object, unsigned depth, Vector<StringID, 16>& names, NameSet& nameSet, unsigned& numberOfKeys, const UsersInBlocks* users, bool followsPrototypeChain, bool followsCallsOnObject = true)
{
    Vector<ValueID, 4> objects { object };
    auto isObject = [&](ValueID id) {
        while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
            id = function.at(id).child(0);
        return objects.contains(id);
    };
    for (bool hasGrown = followsPrototypeChain; hasGrown;) {
        hasGrown = false;
        for (auto& block : function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = function.at(target.block).parameters;
                for (unsigned i = 0; i < std::min<size_t>(target.arguments.size(), parameters.size()); ++i) {
                    if (!isObject(target.arguments[i]) || objects.contains(parameters[i]))
                        continue;
                    objects.append(parameters[i]);
                    hasGrown = true;
                }
            }
        }
    }
    auto isPredictedSymbol = [&](ValueID id) {
        TypeID prediction = function.at(id).prediction();
        auto& type = prediction ? program.at(prediction) : program.typeOf(function, id);
        return type.primitives & Primitive::Symbol && type.hasOnlyPrimitives(Primitive::Symbol | Primitive::UndefinedOrNull);
    };
    auto look = [&](ValueID userID) {
        auto& user = function.at(userID);
        std::optional<StringID> name;
        if (user.opcode == Opcode::PutById && isObject(user.child(0)))
            name = user.string();
        else if (user.opcode == Opcode::PutByVal && isObject(user.child(0)))
            numberOfKeys += isPredictedSymbol(user.child(1));
        else if (user.opcode == Opcode::CallBuiltin && languageOperationOf(program, user) == LanguageOperation::DefineField && isObject(user.arguments[0]) && function.at(user.arguments[1]).opcode == Opcode::String)
            name = function.at(user.arguments[1]).string();
        else if (user.opcode == Opcode::CallBuiltin && languageOperationOf(program, user) == LanguageOperation::PrivateFieldAdd && isObject(user.arguments[0]))
            ++numberOfKeys;
        else if (auto operation = user.opcode == Opcode::CallBuiltin ? languageOperationOf(program, user) : std::nullopt; (operation == LanguageOperation::DefineMethod || operation == LanguageOperation::DefineGetter || operation == LanguageOperation::DefineSetter) && isObject(user.arguments[0]) && function.at(user.arguments[1]).opcode != Opcode::String)
            ++numberOfKeys;
        else if (user.opcode == Opcode::CallBuiltin && languageOperationOf(program, user) == LanguageOperation::PrivateMethodsAdd && isObject(user.arguments[0])) {
            Vector<StringID, 4> privateMethods;
            if (function.constructedClass) {
                for (auto& method : program.at(function.constructedClass).methods) {
                    if (method.name && isPrivateName(program.string(method.name)) && !privateMethods.contains(method.name))
                        privateMethods.append(method.name);
                }
            }
            numberOfKeys += std::max<size_t>(privateMethods.size(), 1);
        } else if (user.opcode == Opcode::CallDirect && depth) {
            auto& callee = program.at(user.function());
            if (callee.blocks.isEmpty())
                return;
            auto& parameters = callee.blocks[0].parameters;
            for (unsigned i = callee.captureTypes.size() + (!followsCallsOnObject && callee.has(Format::FunctionHasThis)); i < std::min<size_t>(user.arguments.size(), parameters.size()); ++i) {
                if (isObject(user.arguments[i]))
                    collectPropertiesAssignedToObject(program, callee, parameters[i], depth - 1, names, nameSet, numberOfKeys, nullptr, followsPrototypeChain, followsCallsOnObject);
            }
        } else if (followsPrototypeChain && followsCallsOnObject) {
            ValueID linkCallee;
            bool isLink = false;
            if (user.opcode == Opcode::Call || user.opcode == Opcode::CallForwardVarargs) {
                isLink = isObject(user.child(1));
                linkCallee = user.child(0);
            } else if (user.opcode == Opcode::CallBuiltin && user.arguments.size() >= 2 && (languageOperationOf(program, user) == LanguageOperation::Apply || languageOperationOf(program, user) == LanguageOperation::CallWithSpread)) {
                isLink = isObject(user.arguments[1]);
                linkCallee = user.arguments[0];
            }
            if (!isLink)
                return;
            FunctionID expected = depth && linkCallee ? function.at(linkCallee).predictedClosure() : FunctionID();
            bool isFollowed = expected && !program.at(expected).blocks.isEmpty() && program.at(expected).blocks[0].parameters.size() > program.at(expected).captureTypes.size();
            if (!isFollowed)
                return;
            auto& callee = program.at(expected);
            collectPropertiesAssignedToObject(program, callee, callee.blocks[0].parameters[callee.captureTypes.size()], depth - 1, names, nameSet, numberOfKeys, nullptr, true);
            return;
        }
        if (!name)
            return;
        static constexpr size_t maxNamesToCompare = 16;
        if (names.size() <= maxNamesToCompare) {
            if (!names.contains(*name))
                names.append(*name);
            return;
        }
        if (nameSet.isEmpty()) {
            for (StringID listed : names)
                nameSet.add(listed.index());
        }
        if (nameSet.add(name->index()).isNewEntry)
            names.append(*name);
    };
    if (users) {
        for (auto& user : *users)
            look(user.value);
        return;
    }
    for (auto& block : function.blocks) {
        for (ValueID userID : block.values)
            look(userID);
    }
}

void collectAssignedProperties(const Program& program, const Function& function, ValueID object, unsigned depth, Vector<StringID, 16>& names, unsigned& numberOfKeys, const UsersInBlocks* users, bool followsPrototypeChain)
{
    NameSet nameSet;
    collectPropertiesAssignedToObject(program, function, object, depth, names, nameSet, numberOfKeys, users, followsPrototypeChain);
}

bool collectPropertiesAssignedToThis(const Program& program, FunctionID id, Vector<StringID, 16>& names, unsigned& numberOfKeys, bool followsCallsOnThis)
{
    static constexpr unsigned maxLevels = 16;
    for (unsigned level = 0; level < maxLevels; ++level) {
        auto& function = program.at(id);
        if (function.blocks.isEmpty())
            return false;
        Vector<ValueID, 8> objects;
        if (function.blocks[0].parameters.size() > function.captureTypes.size())
            objects.append(function.blocks[0].parameters[function.captureTypes.size()]);
        bool callsSuper = false;
        for (auto& block : function.blocks) {
            for (ValueID value : block.values) {
                auto& created = function.at(value);
                if (created.opcode != Opcode::ConstructSuper && (created.opcode != Opcode::CallBuiltin || languageOperationOf(program, created) != LanguageOperation::SuperConstruct))
                    continue;
                objects.append(value);
                callsSuper = true;
            }
        }
        for (bool hasGrown = callsSuper; hasGrown;) {
            hasGrown = false;
            for (auto& block : function.blocks) {
                for (auto& target : block.terminator.targets) {
                    auto& parameters = function.at(target.block).parameters;
                    for (unsigned i = 0; i < std::min<size_t>(target.arguments.size(), parameters.size()); ++i) {
                        if (!objects.contains(target.arguments[i]) || objects.contains(parameters[i]))
                            continue;
                        objects.append(parameters[i]);
                        hasGrown = true;
                    }
                }
            }
        }
        for (ValueID object : objects) {
            NameSet nameSet;
            collectPropertiesAssignedToObject(program, function, object, 3, names, nameSet, numberOfKeys, nullptr, true, followsCallsOnThis);
        }
        if (!callsSuper)
            return true;
        if (!followsCallsOnThis)
            return false;
        if (!function.constructedClass)
            return false;
        auto& ofClass = program.at(function.constructedClass);
        id = ofClass.declaredSuperclassConstructor ? ofClass.declaredSuperclassConstructor : ofClass.base ? program.at(ofClass.base).constructor : FunctionID();
        if (!id)
            return false;
    }
    return false;
}

namespace {

class ArrayEscapeAnalysis {
public:
    ArrayEscapeAnalysis(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
        , m_length(program.find(asBytes("length"_s.span8())).value_or(StringID()))
        , m_slice(program.find(asBytes("slice"_s.span8())).value_or(StringID()))
        , m_from(program.find(asBytes("from"_s.span8())))
    {
    }

    bool run()
    {
        if (!findAllocations())
            return false;
        findUses();
        m_isStale.ensureSize(m_function.values.size());
        bool changed = false;
        for (unsigned i = 0; i < m_allocations.size(); ++i) {
            m_current = i + 1;
            changed |= tryReplace(m_allocations[i]);
            if (m_didCutBlocks)
                break;
        }
        return changed;
    }

private:
    struct Use {
        BlockID block;
        ValueID user;
    };

    enum class Kind : uint8_t { ArrayLiteral, ArgumentsObject, Rest, Closure };

    struct Allocation {
        Kind kind;
        Vector<Use, 2> aliases;
        Vector<Use, 2> uses;
        bool escapesAnalysis { false };
    };

    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }
    unsigned allocationOf(ValueID value) const { return value.index() < m_allocationOf.size() ? m_allocationOf[value.index()] : 0; }
    bool isAlias(ValueID value) const { return allocationOf(value) == m_current; }

    bool findAllocations()
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                Kind kind;
                if (value.opcode == Opcode::NewArray && !mayThrow(m_program, m_function, value))
                    kind = Kind::ArrayLiteral;
                else if (value.opcode == Opcode::CreateRest)
                    kind = Kind::Rest;
                else if (value.opcode == Opcode::NewClosure)
                    kind = Kind::Closure;
                else if (languageOperationOf(m_program, value) == LanguageOperation::CreateArguments && value.arguments.size() == 1)
                    kind = Kind::ArgumentsObject;
                else
                    continue;
                if (m_allocations.isEmpty())
                    m_allocationOf.fill(0, m_function.values.size());
                m_allocations.append(Allocation { kind, { Use { BlockID(b), id } }, { }, false });
                m_allocationOf[id.index()] = m_allocations.size();
            }
        }
        if (m_allocations.isEmpty())
            return false;
        Type array = Type::engineClass(Builtins::singleton().arrayClass());
        Type object = Type::object();
        for (bool grew = true; grew;) {
            grew = false;
            for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
                for (ValueID id : m_function.blocks[b].values) {
                    auto& value = m_function.at(id);
                    if ((value.opcode != Opcode::Cast && value.opcode != Opcode::IteratorOpen) || allocationOf(id))
                        continue;
                    unsigned of = allocationOf(value.child(0));
                    if (!of)
                        continue;
                    if (m_allocations[of - 1].kind == Kind::Closure && (value.opcode != Opcode::Cast || value.castReason() != CastReason::Upcast))
                        continue;
                    if (value.opcode == Opcode::IteratorOpen ? m_allocations[of - 1].kind != Kind::ArrayLiteral : !isAliasingCast(value, m_allocations[of - 1].kind == Kind::ArgumentsObject ? object : array))
                        continue;
                    m_allocationOf[id.index()] = of;
                    m_allocations[of - 1].aliases.append(Use { BlockID(b), id });
                    grew = true;
                }
            }
        }
        return true;
    }

    void findUses()
    {
        m_indexInBlock.fill(0, m_function.values.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            uint32_t location = 0;
            for (ValueID id : block.values) {
                m_indexInBlock[id.index()] = location++;
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    unsigned of = allocationOf(child);
                    if (!of || of == allocationOf(id))
                        return;
                    auto& uses = m_allocations[of - 1].uses;
                    if (uses.isEmpty() || uses.last().user != id)
                        uses.append(Use { BlockID(b), id });
                });
            }
            block.terminator.forEachChild([&](ValueID& value) {
                if (unsigned of = allocationOf(value))
                    m_allocations[of - 1].escapesAnalysis = true;
            });
        }
    }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    bool isAliasingCast(const Value& value, const Type& created) const
    {
        switch (value.castReason()) {
        case CastReason::Upcast:
            return true;
        case CastReason::Store:
        case CastReason::Claim:
        case CastReason::Flow:
        case CastReason::NonNull:
            return !Options::auditAOTTypedAssertions() && m_program.castIsRedundant(created, m_program.at(value.type), value);
        default:
            return false;
        }
    }

    bool readsLengthOfArray(const Value& value) const
    {
        if (value.opcode == Opcode::GetById)
            return value.string() == m_length;
        if (value.opcode != Opcode::CallBuiltin || !value.arguments.isEmpty())
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ArrayLength;
    }

    std::optional<int64_t> evaluateIntegerWithLength(ValueID id, unsigned depth) const
    {
        if (ValueID constant = m_program.constantOf(m_function, id)) {
            if (m_function.at(constant).opcode != Opcode::Integer)
                return std::nullopt;
            return m_function.at(constant).integer();
        }
        if (!m_currentLength || !depth)
            return std::nullopt;
        while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
            id = m_function.at(id).child(0);
        auto& value = m_function.at(id);
        if (readsLengthOfArray(value) && isAlias(value.child(0)))
            return m_currentLength;
        if (value.opcode != Opcode::Add && value.opcode != Opcode::Sub)
            return std::nullopt;
        auto left = evaluateIntegerWithLength(value.child(0), depth - 1);
        auto right = evaluateIntegerWithLength(value.child(1), depth - 1);
        if (!left || !right || std::abs(*left) > std::numeric_limits<int32_t>::max() || std::abs(*right) > std::numeric_limits<int32_t>::max())
            return std::nullopt;
        return value.opcode == Opcode::Add ? *left + *right : *left - *right;
    }

    std::optional<uint64_t> constantIndex(ValueID key) const
    {
        auto index = evaluateIntegerWithLength(key, 4);
        if (!index || *index < 0)
            return std::nullopt;
        return *index;
    }

    bool isApplyOfList(const Value& user) const
    {
        auto operation = languageOperationOf(m_program, user);
        return (operation == LanguageOperation::Apply || operation == LanguageOperation::CallWithSpread) && user.arguments.size() == 3
            && isAlias(user.arguments[2]) && !isAlias(user.arguments[0]) && !isAlias(user.arguments[1]);
    }

    bool isSuperConstructOfList(const Value& user) const
    {
        return languageOperationOf(m_program, user) == LanguageOperation::SuperConstruct && user.arguments.size() == 3
            && isAlias(user.arguments[2]) && !isAlias(user.arguments[0]) && !isAlias(user.arguments[1]);
    }

    bool isSuperclassReadJustBefore(BlockID block, ValueID superConstruct, const Allocation& literal, ValueID spread) const
    {
        auto& construct = m_function.at(superConstruct);
        auto& parent = m_function.at(construct.arguments[3]);
        if (m_program.operationOf(parent) != Operations::GetSuperConstructor || parent.arguments.size() != 1 || parent.arguments[0] != construct.arguments[0])
            return false;
        auto& values = m_function.at(block).values;
        size_t from = values.find(construct.arguments[3]);
        size_t to = values.find(superConstruct);
        if (from == notFound || to == notFound || from > to)
            return false;
        for (size_t i = from + 1; i < to; ++i) {
            if (values[i] != spread && !isAlias(values[i]) && !literal.aliases.containsIf([&](auto& alias) { return alias.user == values[i]; }))
                return false;
        }
        return true;
    }

    bool isSpreadOfList(const Value& user) const
    {
        return languageOperationOf(m_program, user) == LanguageOperation::AppendSpread && user.arguments.size() == 2 && isAlias(user.arguments[1]) && !isAlias(user.arguments[0]);
    }

    const Allocation* spreadTarget(const Value& user) const
    {
        if (languageOperationOf(m_program, user) != LanguageOperation::AppendSpread || user.arguments.size() != 2 || !isAlias(user.arguments[1]))
            return nullptr;
        unsigned of = allocationOf(user.arguments[0]);
        if (!of || of == m_current || !Type::engineClass(Builtins::singleton().arrayClass()).isSubsetOf(m_program.at(user.type), m_program))
            return nullptr;
        auto& target = m_allocations[of - 1];
        if (target.kind != Kind::ArrayLiteral || target.escapesAnalysis || target.uses.size() != 1 || m_isStale.get(target.aliases[0].user.index()))
            return nullptr;
        return &target;
    }

    bool isArrayPrototypeSlice(ValueID value) const
    {
        auto& builtins = Builtins::singleton();
        auto& read = m_function.at(originOf(value));
        if (read.opcode != Opcode::GetById || read.string() != m_slice)
            return false;
        auto& holder = m_function.at(originOf(read.child(0)));
        if (holder.opcode == Opcode::NewArray) {
            unsigned of = allocationOf(read.child(0));
            if (!of)
                return false;
            auto& literal = m_allocations[of - 1];
            return literal.kind == Kind::ArrayLiteral && !literal.escapesAnalysis && literal.uses.size() == 1 && !m_isStale.get(literal.aliases[0].user.index());
        }
        if (holder.opcode != Opcode::GetById || m_program.builtinName(holder.string()) != builtins.prototypeName())
            return false;
        auto& global = m_function.at(originOf(holder.child(0)));
        return global.opcode == Opcode::GetGlobal && m_program.builtinName(global.string()) == builtins.arrayClass().name && !m_program.isExtended(builtins.arrayClass().name);
    }

    ValueID* sliceReceiver(Value& user) const
    {
        ValueID* receiver = nullptr;
        ValueID slice = user.child(0);
        bool isCallOfCall = false;
        if (user.opcode == Opcode::CallMethod)
            isCallOfCall = m_program.builtinName(user.string()) == Builtins::singleton().callName();
        else if (user.opcode != Opcode::Call)
            return nullptr;
        else if (auto& read = m_function.at(user.child(0)); read.opcode == Opcode::GetById && m_program.builtinName(read.string()) == Builtins::singleton().callName() && originOf(read.child(0)) == originOf(user.child(1))) {
            isCallOfCall = true;
            slice = user.child(1);
        } else
            receiver = &user.children[1];
        if (isCallOfCall && !user.arguments.isEmpty())
            receiver = &user.arguments[0];
        if (!receiver || !isAlias(*receiver) || !isArrayPrototypeSlice(slice))
            return nullptr;
        unsigned count = 0;
        std::as_const(user).forEachChild([&](ValueID child) {
            count += isAlias(child);
        });
        return count == 1 ? receiver : nullptr;
    }

    ValueID* arrayFromArgument(Value& user) const
    {
        auto& builtins = Builtins::singleton();
        if (user.arguments.size() != 1 || !isAlias(user.arguments[0]) || m_program.isExtended(builtins.arrayClass().name))
            return nullptr;
        if (user.opcode == Opcode::CallBuiltin) {
            auto& builtin = builtins.at(user.builtin());
            bool isArrayFrom = builtin.kind == BuiltinKind::StaticFunction && builtin.holderName == builtins.arrayClass().name && builtin.nameName == builtins.nameOf(asBytes("from"_s.span8()));
            return isArrayFrom ? &user.arguments[0] : nullptr;
        }
        if (!m_from)
            return nullptr;
        ValueID holder = user.child(0);
        if (user.opcode == Opcode::Call) {
            auto& read = m_function.at(originOf(user.child(0)));
            if (read.opcode != Opcode::GetById || read.string() != *m_from || !user.child(1) || originOf(read.child(0)) != originOf(user.child(1)))
                return nullptr;
            holder = read.child(0);
        } else if (user.opcode != Opcode::CallMethod || user.string() != *m_from)
            return nullptr;
        auto& global = m_function.at(originOf(holder));
        bool isOnArray = global.opcode == Opcode::GetGlobal && m_program.builtinName(global.string()) == builtins.arrayClass().name;
        return isOnArray ? &user.arguments[0] : nullptr;
    }

    bool isGetFromArguments(const Value& user) const
    {
        return languageOperationOf(m_program, user) == LanguageOperation::GetFromArguments && user.arguments.size() == 2 && isAlias(user.arguments[0]) && !isAlias(user.arguments[1]);
    }

    bool isRestRead(const Value& user) const
    {
        auto& type = m_program.at(user.type);
        if (user.opcode == Opcode::GetById && user.string() == m_length)
            return type.primitives & Primitive::Int && type.integers.contains(0) && type.integers.contains(std::numeric_limits<int32_t>::max());
        bool isAnything = m_program.at(m_program.types.anything).isSubsetOf(type, m_program);
        if (user.opcode == Opcode::GetByVal && !isAlias(user.child(1)))
            return isAnything || (!Options::auditAOTTypedAssertions() && writesOfElementRead(m_program, type) == Writes::Nothing);
        return isAnything && isGetFromArguments(user);
    }

    bool onlyBorrows(const Value& user) const
    {
        if (isApplyOfList(user) || isSuperConstructOfList(user))
            return true;
        auto operation = languageOperationOf(m_program, user);
        if (operation == LanguageOperation::CreateArguments)
            return user.arguments.size() == 1;
        if (operation == LanguageOperation::CreateMappedArguments)
            return user.arguments.size() == 3 && !isAlias(user.arguments[1]) && !isAlias(user.arguments[2]);
        return isSpreadOfList(user);
    }

    void replaceRestRead(const Use& use, uint32_t numberOfArgumentsToSkip)
    {
        auto& user = m_function.at(use.user);
        bool readsArgumentsObject = isGetFromArguments(user);
        bool isLength = user.opcode == Opcode::GetById;
        ValueID key = readsArgumentsObject ? user.arguments[1] : user.child(1);
        user.children = { };
        user.arguments.clear();
        user.immediates = { numberOfArgumentsToSkip, Format::none };
        if (isLength) {
            user.opcode = Opcode::GetRestLength;
            user.setPrediction({ });
            user.setPredictedClosure({ });
            if (m_program.at(m_program.types.int32).isSubsetOf(m_program.at(user.type), m_program)) {
                user.type = m_program.types.int32;
                user.rep = repOf(m_program.at(user.type));
            }
        } else if (auto index = constantIndex(key); index && *index < maxArguments) {
            user.opcode = Opcode::GetArgument;
            user.immediates[0] += static_cast<uint32_t>(*index);
        } else {
            user.opcode = Opcode::GetMyArgumentByVal;
            user.children[0] = key;
            user.immediates[1] = readsArgumentsObject;
        }
        if (!mayThrow(m_program, m_function, user))
            stopWatching(use.block, use.user);
    }

    bool isReplaceableRead(Value& user, const Arguments* elements) const
    {
        if (isApplyOfList(user))
            return true;
        if (!elements && sliceReceiver(user))
            return true;
        if (!elements && arrayFromArgument(user))
            return true;
        if (!elements && isSpreadOfList(user))
            return true;
        if (elements && spreadTarget(user))
            return true;
        auto& type = m_program.at(user.type);
        if (elements ? readsLengthOfArray(user) : user.opcode == Opcode::GetById && user.string() == m_length)
            return !elements || (type.primitives & Primitive::Int && type.integers.contains(elements->size()));
        if (elements && user.opcode == Opcode::IteratorClose)
            return true;
        if (elements && user.opcode == Opcode::IteratorHasNext)
            return !!constantIndex(user.child(1));
        if ((user.opcode != Opcode::GetByVal && (!elements || user.opcode != Opcode::IteratorValue)) || isAlias(user.child(1)))
            return false;
        if (!elements)
            return true;
        auto index = constantIndex(user.child(1));
        if (!index)
            return false;
        if (*index >= elements->size())
            return type.primitives & Primitive::Undefined;
        return typeOf(elements->at(*index)).isSubsetOf(type, m_program) || (!Options::auditAOTTypedAssertions() && writesOfElementRead(m_program, type) == Writes::Nothing);
    }

    bool isReadAtDynamicIndex(const Value& user, const Arguments& elements) const
    {
        static constexpr unsigned maxElementsToChooseAmong = 4;
        if (user.opcode != Opcode::GetByVal || !isAlias(user.child(0)) || isAlias(user.child(1)) || elements.size() > maxElementsToChooseAmong)
            return false;
        auto& type = m_program.at(user.type);
        if (!typeOf(user.child(1)).isInteger() || !(type.primitives & Primitive::Undefined))
            return false;
        return std::ranges::all_of(elements, [&](ValueID element) {
            return typeOf(element).isSubsetOf(type, m_program);
        });
    }

    void selectElementByIndex(const Use& read, const Arguments& elements)
    {
        BlockID block = read.block;
        while (!m_function.at(block).values.contains(read.user)) {
            size_t next = m_cuts.findIf([&](auto& cut) {
                return cut.first == block;
            });
            RELEASE_ASSERT(next != notFound);
            block = m_cuts[next].second;
        }
        stopWatching(block, read.user);
        Value none;
        none.opcode = Opcode::Undefined;
        none.type = m_program.types.undefined;
        none.rep = repOf(m_program.at(none.type));
        none.position = m_function.at(read.user).position;
        none.inlineFrame = m_function.at(read.user).inlineFrame;
        m_function.values.append(WTF::move(none));
        ValueID nothing(m_function.values.size() - 1);
        m_function.blocks.append(Block { });
        BlockID after(m_function.blocks.size() - 1);
        for (auto& cut : m_cuts) {
            if (cut.first == block)
                cut.first = after;
        }
        m_cuts.append({ block, after });
        auto& from = m_function.at(block);
        auto& to = m_function.at(after);
        size_t location = from.values.find(read.user);
        to.values.append(from.values.subspan(location + 1));
        from.values.shrink(location);
        from.values.append(nothing);
        to.terminator = std::exchange(from.terminator, Terminator { });
        to.parameters.append(read.user);
        auto& user = m_function.at(read.user);
        from.terminator.kind = TerminatorKind::Switch;
        from.terminator.value = user.child(1);
        for (unsigned i = 0; i < elements.size(); ++i) {
            from.terminator.cases.append(i);
            from.terminator.targets.append(Target { after, Arguments { elements[i] } });
        }
        from.terminator.targets.append(Target { after, Arguments { nothing } });
        user.opcode = Opcode::Parameter;
        user.children = { };
        user.arguments.clear();
        user.immediates = { after.index(), 0 };
        m_didCutBlocks = true;
    }

    bool isLastUse(const Use& escape, BlockID allocatedIn) const
    {
        static constexpr size_t maxUnrankedFullWalks = 8;
        if (m_componentRank.isEmpty() && m_lastUseWalkCount > maxUnrankedFullWalks * m_function.blocks.size())
            computeBlockCycles(m_function, &m_componentRank);
        bool knowsRanks = !m_componentRank.isEmpty();
        unsigned minUseRank = knowsRanks ? m_componentRank[escape.block.index()] : 0;
        BitVector containsUse(m_function.blocks.size());
        containsUse.quickSet(escape.block.index());
        auto isLaterInBlock = [&](const Use& other) {
            if (other.user == escape.user)
                return false;
            if (other.block != escape.block) {
                containsUse.quickSet(other.block.index());
                if (knowsRanks)
                    minUseRank = std::min(minUseRank, m_componentRank[other.block.index()]);
                return false;
            }
            return m_indexInBlock[other.user.index()] > m_indexInBlock[escape.user.index()];
        };
        if (m_reads.containsIf(isLaterInBlock) || m_borrows.containsIf(isLaterInBlock) || m_escapes.containsIf(isLaterInBlock))
            return false;
        BitVector seen(m_function.blocks.size());
        Vector<BlockID> worklist { escape.block };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.at(worklist.takeLast()).terminator.targets) {
                if (target.block == allocatedIn || (knowsRanks && m_componentRank[target.block.index()] < minUseRank) || seen.set(target.block.index()))
                    continue;
                if (containsUse.quickGet(target.block.index()))
                    return false;
                ++m_lastUseWalkCount;
                worklist.append(target.block);
            }
        }
        return true;
    }

    void stopWatching(BlockID block, ValueID value)
    {
        auto& terminator = m_function.at(block).terminator;
        if (terminator.kind != TerminatorKind::Invoke || m_function.at(block).values.isEmpty() || m_function.at(block).values.last() != value)
            return;
        terminator.kind = TerminatorKind::Jump;
        terminator.targets.shrink(1);
    }

    void replaceRead(const Use& use, const Arguments* elements, ValueID list)
    {
        auto& user = m_function.at(use.user);
        if (isApplyOfList(user)) {
            if (!elements) {
                user.arguments[2] = list;
                return;
            }
            user.opcode = Opcode::Call;
            user.children = { user.arguments[0], user.arguments[1], ValueID() };
            user.immediates = { Format::none, Format::none };
            user.arguments = *elements;
            return;
        }
        if (ValueID* receiver = elements ? nullptr : sliceReceiver(user)) {
            *receiver = list;
            return;
        }
        if (ValueID* argument = elements ? nullptr : arrayFromArgument(user)) {
            *argument = list;
            return;
        }
        if (!elements && isSpreadOfList(user)) {
            user.arguments[1] = list;
            return;
        }
        if (auto* target = elements ? spreadTarget(user) : nullptr) {
            Arguments all = m_function.at(target->aliases[0].user).arguments;
            all.appendVector(*elements);
            user.opcode = Opcode::NewArray;
            user.immediates = { Format::none, Format::none };
            user.arguments = WTF::move(all);
            stopWatching(use.block, use.user);
            for (auto& alias : target->aliases)
                m_isStale.set(alias.user.index());
            return;
        }
        if (!elements && user.opcode == Opcode::GetByVal && !typeOf(user.child(1)).isNumber()) {
            user.opcode = Opcode::CallBuiltin;
            user.arguments = Arguments { list, user.child(1) };
            user.children = { };
            user.immediates = { Operations::GetFromArguments.index(), Format::none };
            return;
        }
        if (!elements) {
            user.children[0] = list;
            return;
        }
        ValueID key = user.child(1);
        Opcode read = user.opcode;
        user.children = { };
        user.immediates = { Format::none, Format::none };
        if (read == Opcode::GetById || read == Opcode::CallBuiltin) {
            user.opcode = Opcode::Integer;
            user.setBits(elements->size());
        } else if (read == Opcode::IteratorClose)
            user.opcode = Opcode::Undefined;
        else if (read == Opcode::IteratorHasNext) {
            user.opcode = Opcode::Boolean;
            user.immediates[0] = *constantIndex(key) < elements->size();
        } else if (uint64_t index = *constantIndex(key); index < elements->size()) {
            user.opcode = Opcode::Cast;
            user.children[0] = elements->at(index);
            user.immediates[1] = static_cast<uint32_t>(typeOf(elements->at(index)).isSubsetOf(m_program.at(user.type), m_program) ? CastReason::Upcast : CastReason::Claim);
        } else
            user.opcode = Opcode::Undefined;
        if (!mayThrow(m_program, m_function, user))
            stopWatching(use.block, use.user);
    }

    void sinkTo(const Allocation& allocation, const Use& escape)
    {
        Vector<ValueID, 4> needed;
        std::as_const(m_function.at(escape.user)).forEachChild([&](ValueID child) {
            needed.append(child);
        });
        for (unsigned i = allocation.aliases.size(); i-- > 1;) {
            if (needed.contains(allocation.aliases[i].user))
                needed.append(m_function.at(allocation.aliases[i].user).child(0));
        }
        Vector<std::pair<ValueID, ValueID>, 4> copies;
        for (auto& alias : allocation.aliases) {
            if (alias.user != allocation.aliases[0].user && !needed.contains(alias.user))
                continue;
            Value copy = m_function.at(alias.user);
            copy.forEachChild([&](ValueID& child) {
                for (auto& [original, ownCopy] : copies) {
                    if (child == original)
                        child = ownCopy;
                }
            });
            m_function.values.append(WTF::move(copy));
            copies.append({ alias.user, ValueID(m_function.values.size() - 1) });
        }
        auto& values = m_function.at(escape.block).values;
        size_t location = values.find(escape.user);
        for (auto& [original, copy] : copies)
            values.insert(location++, copy);
        m_function.at(escape.user).forEachChild([&](ValueID& child) {
            for (auto& [original, copy] : copies) {
                if (child == original)
                    child = copy;
            }
        });
    }

    ValueID withoutUpcasts(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast)
            value = m_function.at(value).child(0);
        return value;
    }

    bool isAllocationLength(ValueID value) const
    {
        auto& read = m_function.at(withoutUpcasts(value));
        return (m_currentLength ? readsLengthOfArray(read) : read.opcode == Opcode::GetById && read.string() == m_length) && isAlias(read.child(0));
    }

    std::optional<bool> evaluateWithLength(ValueID condition, int64_t length) const
    {
        auto& value = m_function.at(withoutUpcasts(condition));
        switch (value.opcode) {
        case Opcode::LogicalNot:
            if (auto operand = evaluateWithLength(value.child(0), length))
                return !*operand;
            return std::nullopt;
        case Opcode::ToBoolean:
            if (isAllocationLength(value.child(0)))
                return !!length;
            return evaluateWithLength(value.child(0), length);
        case Opcode::StrictEq:
        case Opcode::Eq:
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            break;
        default:
            return std::nullopt;
        }
        bool isLeft = isAllocationLength(value.child(0));
        if (!isLeft && !isAllocationLength(value.child(1)))
            return std::nullopt;
        auto& other = m_function.at(withoutUpcasts(value.child(isLeft)));
        if (other.opcode != Opcode::Integer)
            return std::nullopt;
        int64_t left = isLeft ? length : other.integer();
        int64_t right = isLeft ? other.integer() : length;
        switch (value.opcode) {
        case Opcode::Less:
            return left < right;
        case Opcode::LessEq:
            return left <= right;
        case Opcode::Greater:
            return left > right;
        case Opcode::GreaterEq:
            return left >= right;
        default:
            return left == right;
        }
    }

    struct DecidedBranch {
        BlockID block;
        bool isTaken;
    };

    BitVector blocksReachedWhileUnmodified(BlockID allocatedIn, int64_t length, Vector<DecidedBranch, 4>& decided) const
    {
        BitVector seen(m_function.blocks.size());
        seen.set(allocatedIn.index());
        Vector<BlockID> worklist { allocatedIn };
        while (!worklist.isEmpty()) {
            BlockID block = worklist.takeLast();
            auto& terminator = m_function.at(block).terminator;
            auto follow = [&](const Target& target) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block);
            };
            if (terminator.kind == TerminatorKind::Branch) {
                if (auto isTaken = evaluateWithLength(terminator.value, length)) {
                    decided.append(DecidedBranch { block, *isTaken });
                    follow(terminator.targets[*isTaken ? 0 : 1]);
                    continue;
                }
            }
            for (auto& target : terminator.targets)
                follow(target);
        }
        return seen;
    }

    bool tryReplace(const Allocation& allocation)
    {
        if (allocation.escapesAnalysis)
            return false;
        auto isStale = [&](const Use& use) {
            return m_isStale.get(use.user.index());
        };
        if (allocation.aliases.containsIf(isStale) || allocation.uses.containsIf(isStale))
            return false;

        if (allocation.kind == Kind::Rest && allocation.uses.size() == 1) {
            auto& use = allocation.uses[0];
            auto& spread = m_function.at(use.user);
            auto* target = spreadTarget(spread);
            bool isOnlySuperConstructList = target && m_function.at(target->aliases[0].user).arguments.isEmpty() && typeOf(spread.arguments[1]).isSubsetOf(m_program.at(spread.type), m_program);
            ValueID explicitParent;
            for (unsigned b = 0; isOnlySuperConstructList && b < m_function.blocks.size(); ++b) {
                for (ValueID id : m_function.blocks[b].values) {
                    auto& value = std::as_const(m_function).at(id);
                    bool isUser = false;
                    value.forEachChild([&](ValueID child) {
                        isUser |= child == use.user;
                    });
                    if (!isUser)
                        continue;
                    isOnlySuperConstructList &= languageOperationOf(m_program, value) == LanguageOperation::SuperConstruct && (value.arguments.size() == 3 || value.arguments.size() == 4) && value.arguments[2] == use.user && value.arguments[0] != use.user && value.arguments[1] != use.user;
                    if (isOnlySuperConstructList && value.arguments.size() == 4) {
                        isOnlySuperConstructList = !explicitParent && isSuperclassReadJustBefore(BlockID(b), id, *target, use.user);
                        explicitParent = id;
                    }
                }
                m_function.blocks[b].terminator.forEachChild([&](ValueID& child) {
                    isOnlySuperConstructList &= child != use.user;
                });
            }
            if (isOnlySuperConstructList) {
                for (auto& alias : target->aliases) {
                    m_function.at(alias.block).values.removeFirst(alias.user);
                    m_isStale.set(alias.user.index());
                }
                for (auto& alias : allocation.aliases)
                    m_isStale.set(alias.user.index());
                m_isStale.set(use.user.index());
                stopWatching(use.block, use.user);
                spread.opcode = Opcode::Cast;
                spread.children = { spread.arguments[1], ValueID(), ValueID() };
                spread.arguments.clear();
                spread.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                if (explicitParent)
                    m_function.at(explicitParent).arguments.shrink(3);
                return true;
            }
        }

        BlockID block = allocation.aliases[0].block;
        Arguments operands = m_function.at(allocation.aliases[0].user).arguments;
        const Arguments* elements = allocation.kind == Kind::ArrayLiteral ? &operands : nullptr;
        m_currentLength = elements ? std::optional<int64_t>(elements->size()) : std::nullopt;
        ValueID list = allocation.kind == Kind::ArgumentsObject ? operands[0] : ValueID();
        uint32_t numberOfArgumentsToSkip = m_function.at(allocation.aliases[0].user).numberOfArgumentsToSkip();
        m_reads.clear();
        m_dynamicIndexReads.clear();
        m_borrows.clear();
        m_escapes.clear();
        Vector<DecidedBranch, 4> decided;
        BitVector isReached;
        auto readsLength = [&](const Use& use) {
            return isAllocationLength(use.user);
        };
        if (elements && allocation.uses.containsIf(readsLength))
            isReached = blocksReachedWhileUnmodified(block, elements->size(), decided);
        for (auto& use : allocation.uses) {
            if (!decided.isEmpty() && !isReached.get(use.block.index()))
                continue;
            auto& user = m_function.at(use.user);
            if (user.opcode == Opcode::AssertNotAllocated)
                m_reads.append(use);
            else if (allocation.kind == Kind::Closure)
                m_escapes.append(use);
            else if (allocation.kind == Kind::Rest)
                (isRestRead(user) ? m_reads : onlyBorrows(user) ? m_borrows : m_escapes).append(use);
            else if (isReplaceableRead(user, elements))
                m_reads.append(use);
            else
                (elements && isReadAtDynamicIndex(user, *elements) ? m_dynamicIndexReads : m_escapes).append(use);
        }
        if (!m_escapes.isEmpty() || !m_borrows.isEmpty()) {
            m_escapes.appendVector(m_dynamicIndexReads);
            m_dynamicIndexReads.clear();
        }
        auto isForwarded = [&](const Use& borrow) {
            auto& user = m_function.at(borrow.user);
            return isSuperConstructOfList(user) || (isApplyOfList(user) && m_program.at(m_program.types.anything).isSubsetOf(m_program.at(user.type), m_program));
        };
        if (m_reads.isEmpty() && m_borrows.size() + m_escapes.size() == 1 && (m_borrows.isEmpty() ? m_escapes : m_borrows)[0].block == block && (m_borrows.isEmpty() || !isForwarded(m_borrows[0])))
            return false;
        for (auto& escape : m_escapes) {
            if (!isLastUse(escape, block))
                return false;
        }

        for (auto& read : m_reads) {
            if (m_function.at(read.user).opcode == Opcode::AssertNotAllocated)
                m_function.at(read.block).values.removeFirst(read.user);
            else if (allocation.kind == Kind::Rest)
                replaceRestRead(read, numberOfArgumentsToSkip);
            else
                replaceRead(read, elements, list);
        }
        for (auto& borrow : m_borrows) {
            if (isForwarded(borrow)) {
                auto& user = m_function.at(borrow.user);
                user.opcode = isSuperConstructOfList(user) ? Opcode::ConstructSuperForwardVarargs : Opcode::CallForwardVarargs;
                user.children = { user.arguments[0], user.arguments[1], ValueID() };
                user.arguments.clear();
                user.immediates = { numberOfArgumentsToSkip, Format::none };
                continue;
            }
            sinkTo(allocation, borrow);
        }
        for (auto& escape : m_escapes)
            sinkTo(allocation, escape);
        if (!m_escapes.isEmpty() || !m_borrows.isEmpty())
            recordAllocation(m_program, m_function, m_function.at(allocation.aliases[0].user), AllocationOutcome::BuiltLate);
        for (auto& alias : allocation.aliases) {
            stopWatching(alias.block, alias.user);
            m_function.at(alias.block).values.removeFirst(alias.user);
            m_isStale.set(alias.user.index());
        }
        for (auto& use : allocation.uses)
            m_isStale.set(use.user.index());
        for (auto& branch : decided) {
            auto& terminator = m_function.at(branch.block).terminator;
            Target target = terminator.targets[branch.isTaken ? 0 : 1];
            terminator.kind = TerminatorKind::Jump;
            terminator.value = { };
            terminator.targets = { WTF::move(target) };
        }
        for (auto& read : m_dynamicIndexReads)
            selectElementByIndex(read, *elements);
        return true;
    }

    const Program& m_program;
    Function& m_function;
    StringID m_length;
    StringID m_slice;
    std::optional<StringID> m_from;
    Vector<Allocation> m_allocations;
    Vector<unsigned> m_allocationOf;
    Vector<uint32_t> m_indexInBlock;
    mutable size_t m_lastUseWalkCount { 0 };
    mutable Vector<unsigned> m_componentRank;
    BitVector m_isStale;
    unsigned m_current { 0 };
    std::optional<int64_t> m_currentLength;
    Vector<Use, 4> m_reads;
    Vector<Use, 4> m_dynamicIndexReads;
    Vector<Use, 4> m_borrows;
    Vector<Use, 4> m_escapes;
    Vector<std::pair<BlockID, BlockID>, 4> m_cuts;
    bool m_didCutBlocks { false };
};

std::optional<TypeofType> typeofTypeNamed(std::span<const uint8_t> text)
{
    static constexpr std::pair<ASCIILiteral, TypeofType> names[] = {
        { "undefined"_s, TypeofType::Undefined },
        { "bigint"_s, TypeofType::BigInt },
        { "boolean"_s, TypeofType::Boolean },
        { "number"_s, TypeofType::Number },
        { "string"_s, TypeofType::String },
        { "symbol"_s, TypeofType::Symbol },
        { "object"_s, TypeofType::Object },
        { "function"_s, TypeofType::Function },
    };
    for (auto& [name, type] : names) {
        if (equalSpans(name.span8(), text))
            return type;
    }
    return std::nullopt;
}

class PathFacts {
public:
    struct State {
        unsigned layerCount;
        uint32_t validFrom;
        unsigned size;
    };

    void reset()
    {
        m_facts.clear();
        m_facts.startLayer();
        clear();
    }

    bool isEmpty() const { return !m_size; }

    ValueID get(uint64_t key) const
    {
        auto fact = m_facts.find(key);
        return fact && fact->stamp >= m_validFrom ? fact->value : ValueID();
    }

    bool contains(uint64_t key) const { return !!get(key); }

    void set(uint64_t key, ValueID value)
    {
        m_size += !contains(key);
        m_facts.overlay(key, Fact { value, m_clock });
    }

    void add(uint64_t key) { set(key, ValueID(0u)); }

    void remove(uint64_t key)
    {
        if (!contains(key))
            return;
        --m_size;
        m_facts.overlay(key, Fact { ValueID(), m_clock });
    }

    void clear()
    {
        m_validFrom = ++m_clock;
        m_size = 0;
    }

    State save() const { return { m_facts.layerCount(), m_validFrom, m_size }; }
    void startLayer() { m_facts.startLayer(); }

    void restore(const State& state)
    {
        while (m_facts.layerCount() > state.layerCount)
            m_facts.dropLastLayer();
        m_facts.startLayer();
        m_validFrom = state.validFrom;
        m_size = state.size;
    }

private:
    struct Fact {
        ValueID value;
        uint32_t stamp { 0 };
    };

    mutable LayeredHashMap<uint64_t, Fact, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_facts;
    uint32_t m_clock { 0 };
    uint32_t m_validFrom { 0 };
    unsigned m_size { 0 };
};

class RedundantReadElimination {
public:
    RedundantReadElimination(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool run()
    {
        if (!findReads())
            return false;
        static constexpr size_t minReadsForWordSolver = 4096;
        m_usesWordSolver = m_reads.size() >= minReadsForWordSolver;
        findAllocations();
        findActions();
        solve();
        if (m_usesWordSolver)
            return replace(m_wordsInUse);
        BitVector bits;
        return replace(bits);
    }

private:
    static constexpr uint32_t none = UINT32_MAX;
    static uint64_t property(StringID name) { return name.index(); }
    static uint64_t environmentVariable(LayoutID layout, uint32_t field) { return 1ull << 62 | static_cast<uint64_t>(layout.index()) << 32 | field; }
    static uint64_t moduleVariable(VariableID variable) { return 2ull << 62 | variable.index(); }

    struct Read {
        uint64_t location;
        ValueID receiver;
        ValueID id;
        bool isFixed { false };
        ValueID stored { };

        ValueID returnedValue() const { return stored ? stored : id; }
    };

    struct Action {
        enum Kind : uint8_t { Reads, Clears, ClearsAll, Allocates, Escapes };
        Kind kind;
        uint32_t first { 0 };
        uint32_t second { 0 };
        uint32_t allocation { none };
    };

    struct Words {
        bool isAll { true };
        Vector<uint64_t> words;
    };

    class WordsInUse {
    public:
        void initialize(size_t numberOfBits)
        {
            m_numberOfBits = numberOfBits;
            m_words.fill(0, (numberOfBits + 63) / 64);
            m_summary.fill(0, (m_words.size() + 63) / 64);
            m_callClobberedSummary.fill(0, m_summary.size());
        }

        bool quickGet(size_t bit) const { return m_words[bit / 64] >> (bit % 64) & 1; }
        void quickClear(size_t bit) { m_words[bit / 64] &= ~(1ull << (bit % 64)); }
        void quickSet(size_t bit)
        {
            m_words[bit / 64] |= 1ull << (bit % 64);
            m_summary[bit / 4096] |= 1ull << (bit / 64 % 64);
            m_callClobberedSummary[bit / 4096] |= 1ull << (bit / 64 % 64);
        }
        void markCallClobbered(size_t bit) { m_callClobberedSummary[bit / 4096] |= 1ull << (bit / 64 % 64); }

        size_t findSetBit(size_t from, size_t end) const
        {
            if (from >= end)
                return end;
            size_t last = (end - 1) / 64;
            uint64_t word = m_words[from / 64] & ~0ull << (from % 64);
            for (size_t index = from / 64;; word = m_words[++index]) {
                if (word)
                    return index * 64 + WTF::ctz(word);
                if (index == last)
                    return end;
            }
        }

        template<typename Functor>
        void forEachWordInUse(const Functor& functor)
        {
            for (size_t i = 0; i < m_summary.size(); ++i) {
                for (uint64_t rest = m_summary[i]; rest; rest &= rest - 1)
                    functor(i * 64 + WTF::ctz(rest), m_words[i * 64 + WTF::ctz(rest)]);
            }
        }

        template<typename Functor>
        void forEachCallClobberedWord(size_t from, size_t end, const Functor& functor)
        {
            if (from >= end)
                return;
            size_t first = from / 64;
            size_t last = (end - 1) / 64;
            for (size_t i = first / 64; i <= last / 64; ++i) {
                uint64_t rest = m_callClobberedSummary[i] & m_summary[i];
                if (i == first / 64)
                    rest &= ~0ull << (first % 64);
                if (i == last / 64 && last % 64 != 63)
                    rest &= (2ull << (last % 64)) - 1;
                for (; rest; rest &= rest - 1)
                    functor(i * 64 + WTF::ctz(rest), m_words[i * 64 + WTF::ctz(rest)]);
            }
        }

        void didClearCallClobberedBits(size_t firstFullyClobberedWord)
        {
            for (size_t i = firstFullyClobberedWord / 64; i < m_callClobberedSummary.size(); ++i)
                m_callClobberedSummary[i] &= i == firstFullyClobberedWord / 64 ? (1ull << (firstFullyClobberedWord % 64)) - 1 : 0;
        }

        size_t numberOfBits() const { return m_numberOfBits; }

        void reset()
        {
            forEachWordInUse([](size_t, uint64_t& word) {
                word = 0;
            });
            m_summary.fill(0);
            m_callClobberedSummary.fill(0);
        }

        void load(const Words& set)
        {
            size_t next = m_summary.size();
            for (size_t i = 0; i < m_summary.size(); ++i) {
                m_summary[i] = set.words[i];
                m_callClobberedSummary[i] = set.words[i];
                for (uint64_t rest = set.words[i]; rest; rest &= rest - 1)
                    m_words[i * 64 + WTF::ctz(rest)] = set.words[next++];
            }
        }

        void filter(const Words& set)
        {
            size_t base = m_summary.size();
            for (size_t i = 0; i < m_summary.size(); ++i) {
                uint64_t mine = m_summary[i];
                uint64_t theirs = set.words[i];
                for (uint64_t rest = mine & ~theirs; rest; rest &= rest - 1)
                    m_words[i * 64 + WTF::ctz(rest)] = 0;
                for (uint64_t rest = mine & theirs; rest; rest &= rest - 1)
                    m_words[i * 64 + WTF::ctz(rest)] &= set.words[base + std::popcount(theirs & ((rest & -rest) - 1))];
                base += std::popcount(theirs);
                m_summary[i] = mine & theirs;
                m_callClobberedSummary[i] = mine & theirs;
            }
        }

        void setAll()
        {
            for (size_t index = 0; index < m_words.size(); ++index) {
                m_words[index] = allOf(index);
                m_summary[index / 64] |= 1ull << (index % 64);
                m_callClobberedSummary[index / 64] |= 1ull << (index % 64);
            }
        }

        bool store(Vector<uint64_t>& words)
        {
            words.fill(0, m_summary.size());
            bool isAll = true;
            for (size_t i = 0; i < m_summary.size(); ++i) {
                uint64_t kept = 0;
                for (uint64_t rest = m_summary[i]; rest; rest &= rest - 1) {
                    size_t index = i * 64 + WTF::ctz(rest);
                    uint64_t word = m_words[index];
                    if (!word)
                        continue;
                    kept |= rest & -rest;
                    isAll &= word == allOf(index);
                    words.append(word);
                }
                words[i] = kept;
                m_summary[i] = kept;
            }
            if (!isAll || words.size() != m_summary.size() + m_words.size())
                return false;
            words.shrink(0);
            return true;
        }

    private:
        uint64_t allOf(size_t index) const { return index + 1 == m_words.size() && m_numberOfBits % 64 ? (1ull << (m_numberOfBits % 64)) - 1 : ~0ull; }

        size_t m_numberOfBits { 0 };
        Vector<uint64_t> m_words;
        Vector<uint64_t> m_summary;
        Vector<uint64_t> m_callClobberedSummary;
    };

    static size_t findSetBit(const BitVector& bits, size_t from, size_t) { return bits.findBit(from, true); }
    static size_t findSetBit(const WordsInUse& bits, size_t from, size_t end) { return bits.findSetBit(from, end); }

    const Type& typeOf(ValueID id) const { return m_program.typeOf(m_function, id); }

    ValueID withoutCheckCasts(ValueID id) const
    {
        for (;;) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::Cast || value.castReason() == CastReason::As || value.castReason() == CastReason::Coerce)
                return id;
            id = value.child(0);
        }
    }

    bool isAllocatedWithField(const Value& access) const
    {
        auto& receiver = m_function.at(withoutCheckCasts(access.child(0)));
        return receiver.opcode == Opcode::NewObjectWithShape && isObjectLayout(m_program.at(receiver.layout()).kind) && !m_program.at(receiver.layout()).mayDespecialize && m_program.at(receiver.layout()).fieldNamed(access.string());
    }

    std::optional<FieldAccess> accessedField(const Value& access) const
    {
        if (isAllocatedWithField(access)) {
            LayoutID created = m_function.at(withoutCheckCasts(access.child(0))).layout();
            return FieldAccess { created, *m_program.at(created).fieldNamed(access.string()) };
        }
        return fieldOf(m_program, typeOf(access.child(0)), access.string());
    }

    bool isAccessToField(const Value& access)
    {
        auto found = accessedField(access);
        if (!found)
            return false;
        bool isCreated = isAllocatedWithField(access);
        return m_isOnlyInFields.ensure(static_cast<uint64_t>(isCreated) << 63 | static_cast<uint64_t>(found->layout.index()) << 32 | found->field, [&] {
            return isCreated ? !mayInherit(m_program, found->layout, access.string()) : prototypeChainLacksProperty(m_program, found->layout, access.string());
        }).iterator->value;
    }

    std::optional<uint64_t> fieldLocation(const Value& access) const
    {
        auto& layout = m_program.at(access.layout());
        if (!isObjectLayout(layout.kind))
            return environmentVariable(access.layout(), access.field());
        StringID name = layout.fields[access.field()].name;
        return name ? std::optional<uint64_t>(property(name)) : std::nullopt;
    }

    bool isFixed(const Value& read) const
    {
        if (read.opcode == Opcode::GetModuleVariable)
            return m_program.variables[read.variable().index()].isStoredOnce && !m_storedLocations.contains(moduleVariable(read.variable()));
        auto& layout = m_program.at(read.layout());
        if (isObjectLayout(layout.kind))
            return false;
        uint32_t flags = layout.fields[read.field()].flags;
        if (flags & fieldIsImmutable)
            return true;
        return flags & fieldIsStoredOnce && flags & fieldIsNeverReadUninitialized && !m_storedLocations.contains(environmentVariable(read.layout(), read.field()));
    }

    std::optional<Read> readBy(ValueID id)
    {
        auto& value = m_function.at(id);
        switch (value.opcode) {
        case Opcode::GetModuleVariable:
            return Read { moduleVariable(value.variable()), ValueID(), id };
        case Opcode::GetField:
            if (auto location = fieldLocation(value))
                return Read { *location, withoutCheckCasts(value.child(0)), id };
            return std::nullopt;
        case Opcode::GetById:
            if (value.immediates[1] != Format::none)
                return std::nullopt;
            return Read { property(value.string()), withoutCheckCasts(value.child(0)), id };
        case Opcode::PutModuleVariable:
            return Read { moduleVariable(value.variable()), ValueID(), id, false, value.child(0) };
        case Opcode::PutField:
            if (auto location = fieldLocation(value))
                return Read { *location, withoutCheckCasts(value.child(0)), id, false, value.child(1) };
            return std::nullopt;
        case Opcode::PutById:
            return Read { property(value.string()), withoutCheckCasts(value.child(0)), id, false, value.child(1) };
        default:
            return std::nullopt;
        }
    }

    bool resolvesToField(const Read& read)
    {
        auto& value = m_function.at(read.id);
        if (value.opcode != Opcode::GetById && value.opcode != Opcode::PutById)
            return true;
        if (!isAccessToField(value))
            return false;
        if (value.opcode == Opcode::GetById)
            return true;
        auto found = accessedField(value);
        TypeID type = m_program.at(found->layout).fields[found->field].type;
        return !type || typeOf(value.child(1)).isSubsetOf(m_program.at(type), m_program);
    }

    static bool isWatched(const Block& block, size_t index) { return block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size(); }

    bool findReads()
    {
        Vector<Read> reads;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::PutModuleVariable)
                    m_storedLocations.add(moduleVariable(value.variable()));
                else if (value.opcode == Opcode::PutField && !isObjectLayout(m_program.at(value.layout()).kind))
                    m_storedLocations.add(environmentVariable(value.layout(), value.field()));
                if (auto read = readBy(id))
                    reads.append(*read);
            }
        }
        if (reads.size() < 2)
            return false;
        for (auto& read : reads) {
            if (auto& value = m_function.at(read.id); value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::GetField)
                read.isFixed = isFixed(value);
        }
        std::ranges::sort(reads, [](auto& a, auto& b) {
            if (a.isFixed != b.isFixed)
                return a.isFixed;
            if (a.location != b.location)
                return a.location < b.location;
            if (a.receiver != b.receiver)
                return a.receiver.index() < b.receiver.index();
            return a.id.index() < b.id.index();
        });
        m_bitOf.fill(none, m_function.values.size());
        for (size_t begin = 0; begin < reads.size();) {
            size_t end = begin + 1;
            while (end < reads.size() && reads[end].isFixed == reads[begin].isFixed && reads[end].location == reads[begin].location && reads[end].receiver == reads[begin].receiver)
                ++end;
            size_t next = end;
            bool hasRead = false;
            if (end - begin > 1) {
                end = begin;
                for (size_t i = begin; i < next; ++i) {
                    if (!resolvesToField(reads[i]))
                        continue;
                    hasRead |= !reads[i].stored;
                    reads[end++] = reads[i];
                }
            }
            if (end - begin > 1 && hasRead) {
                uint32_t first = m_reads.size();
                for (size_t i = begin; i < end; ++i) {
                    m_bitOf[reads[i].id.index()] = m_reads.size();
                    m_reads.append(reads[i]);
                    m_numberOfFixedBits += reads[i].isFixed;
                }
                for (size_t i = begin; i < end; ++i)
                    m_sameAs.append({ first, static_cast<uint32_t>(m_reads.size()) });
                auto& range = m_locationBits.add(reads[begin].location, std::pair<uint32_t, uint32_t> { first, 0 }).iterator->value;
                range.second = m_reads.size();
            }
            begin = next;
        }
        return !m_reads.isEmpty();
    }

    void findAllocations()
    {
        m_allocationIndex.fill(none, m_function.values.size());
        m_readAllocation.fill(none, m_reads.size());
        for (uint32_t bit = 0; bit < m_reads.size(); ++bit) {
            ValueID receiver = m_reads[bit].receiver;
            if (!receiver || m_function.at(receiver).opcode != Opcode::NewObjectWithShape || !isObjectLayout(m_program.at(m_function.at(receiver).layout()).kind))
                continue;
            uint32_t& index = m_allocationIndex[receiver.index()];
            if (index == none) {
                index = m_allocationReads.size();
                m_allocationReads.append(m_usesWordSolver ? BitVector() : BitVector(m_reads.size()));
            }
            if (!m_usesWordSolver)
                m_allocationReads[index].quickSet(bit);
            m_readAllocation[bit] = index;
        }
        if (!m_usesWordSolver)
            return;
        m_firstAllocationRead.fill(0, m_allocationReads.size() + 1);
        for (uint32_t allocation : m_readAllocation) {
            if (allocation != none)
                ++m_firstAllocationRead[allocation + 1];
        }
        for (size_t allocation = 0; allocation < m_allocationReads.size(); ++allocation)
            m_firstAllocationRead[allocation + 1] += m_firstAllocationRead[allocation];
        m_readsByAllocation.grow(m_firstAllocationRead.last());
        Vector<uint32_t> next = m_firstAllocationRead;
        for (uint32_t bit = 0; bit < m_reads.size(); ++bit) {
            if (uint32_t allocation = m_readAllocation[bit]; allocation != none)
                m_readsByAllocation[next[allocation]++] = bit;
        }
    }

    std::span<const uint32_t> readsOf(uint32_t allocation) const
    {
        return m_readsByAllocation.span().subspan(m_firstAllocationRead[allocation], m_firstAllocationRead[allocation + 1] - m_firstAllocationRead[allocation]);
    }

    unsigned numberOfBits() const { return m_reads.size() + m_allocationReads.size(); }

    template<typename Functor>
    void forEachAllocationEscapingThrough(const Value& value, const Functor& functor)
    {
        auto visitEscapingChild = [&](ValueID child) {
            if (!child)
                return;
            if (uint32_t allocation = m_allocationIndex[withoutCheckCasts(child).index()]; allocation != none)
                functor(allocation);
        };
        switch (value.opcode) {
        case Opcode::AssertNotAllocated:
            return;
        case Opcode::Cast:
            if (value.castReason() != CastReason::As && value.castReason() != CastReason::Coerce)
                return;
            break;
        case Opcode::StrictEq:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
            return;
        case Opcode::GetField:
            return;
        case Opcode::PutField:
            visitEscapingChild(value.child(1));
            return;
        case Opcode::GetById:
        case Opcode::InById:
            if (isAccessToField(value))
                return;
            break;
        case Opcode::PutById:
            if (isAccessToField(value)) {
                visitEscapingChild(value.child(1));
                return;
            }
            break;
        default:
            break;
        }
        value.forEachChild(visitEscapingChild);
    }

    Action clears(std::optional<uint64_t> location) const
    {
        if (!location)
            return { Action::ClearsAll };
        auto found = m_locationBits.find(*location);
        if (found == m_locationBits.end())
            return { Action::Clears };
        return { Action::Clears, found->value.first, found->value.second };
    }

    void findActions()
    {
        m_actions.grow(m_function.blocks.size());
        m_bitSetOnNormalExit.fill(none, m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            auto add = [&](Action action) {
                if (action.kind != Action::Clears || action.first != action.second)
                    m_actions[b].append(action);
            };
            auto through = [&](const Value& store, Action action) {
                if (!m_allocationReads.isEmpty())
                    action.allocation = m_allocationIndex[withoutCheckCasts(store.child(0)).index()];
                return action;
            };
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& value = m_function.at(block.values[i]);
                if (!m_allocationReads.isEmpty()) {
                    if (uint32_t allocation = m_allocationIndex[block.values[i].index()]; allocation != none)
                        add({ Action::Allocates, allocation });
                    forEachAllocationEscapingThrough(std::as_const(value), [&](uint32_t allocation) {
                        add({ Action::Escapes, allocation });
                    });
                }
                switch (value.opcode) {
                case Opcode::GetModuleVariable:
                case Opcode::GetField:
                    break;
                case Opcode::GetById:
                case Opcode::InById:
                    if (!isAccessToField(value))
                        add({ Action::ClearsAll });
                    break;
                case Opcode::PutById:
                    add(through(value, clears(isAccessToField(value) ? std::optional<uint64_t>(property(value.string())) : std::nullopt)));
                    break;
                case Opcode::PutField:
                    add(through(value, clears(fieldLocation(value))));
                    break;
                case Opcode::PutModuleVariable:
                    add(clears(moduleVariable(value.variable())));
                    break;
                case Opcode::CallDirect:
                case Opcode::Await:
                case Opcode::Yield:
                case Opcode::GetResumeMode:
                    add({ Action::ClearsAll });
                    continue;
                default:
                    if (writesOf(m_program, m_function, block, i) >= Writes::Fields)
                        add({ Action::ClearsAll });
                    continue;
                }
                if (uint32_t bit = m_bitOf[block.values[i].index()]; bit != none) {
                    if (isWatched(block, i))
                        m_bitSetOnNormalExit[b] = bit;
                    else
                        add({ Action::Reads, bit, static_cast<uint32_t>(i) });
                }
            }
            if (!m_allocationReads.isEmpty()) {
                block.terminator.forEachChild([&](ValueID& child) {
                    if (uint32_t allocation = m_allocationIndex[withoutCheckCasts(child).index()]; allocation != none)
                        add({ Action::Escapes, allocation });
                });
            }
        }
    }

    template<typename Bits>
    void carryOut(const Action& action, Bits& bits) const
    {
        switch (action.kind) {
        case Action::Reads:
            bits.quickSet(action.first);
            return;
        case Action::Clears:
            if constexpr (std::is_same_v<Bits, WordsInUse>) {
                if (action.allocation != none) {
                    auto own = readsOf(action.allocation);
                    for (auto bit = std::ranges::lower_bound(own, action.first); bit != own.end() && *bit < action.second; ++bit)
                        bits.quickClear(*bit);
                }
                bits.forEachCallClobberedWord(action.first, action.second, [&](size_t index, uint64_t& word) {
                    uint64_t wordBits = word;
                    if (index == action.first / 64)
                        wordBits &= ~0ull << (action.first % 64);
                    if (index == (action.second - 1) / 64 && action.second % 64)
                        wordBits &= (1ull << (action.second % 64)) - 1;
                    for (; wordBits; wordBits &= wordBits - 1) {
                        uint32_t allocation = m_allocationReads.isEmpty() ? none : m_readAllocation[index * 64 + WTF::ctz(wordBits)];
                        if (!(allocation != none && allocation != action.allocation && bits.quickGet(m_reads.size() + allocation)))
                            word &= ~(wordBits & -wordBits);
                    }
                });
                return;
            }
            for (size_t bit = findSetBit(bits, action.first, action.second); bit < action.second; bit = findSetBit(bits, bit + 1, action.second)) {
                if (uint32_t allocation = m_allocationReads.isEmpty() ? none : m_readAllocation[bit]; allocation != none && allocation != action.allocation && bits.quickGet(m_reads.size() + allocation))
                    continue;
                bits.quickClear(bit);
            }
            return;
        case Action::ClearsAll:
            if constexpr (std::is_same_v<Bits, WordsInUse>) {
                bits.forEachCallClobberedWord(0, bits.numberOfBits(), [&](size_t index, uint64_t& word) {
                    uint64_t loose = word & ~m_fixedWords[index];
                    if (m_allocationReads.isEmpty()) {
                        word &= ~loose;
                        return;
                    }
                    for (; loose; loose &= loose - 1) {
                        uint32_t allocation = m_readAllocation[index * 64 + WTF::ctz(loose)];
                        if (!(allocation != none && bits.quickGet(m_reads.size() + allocation)))
                            word &= ~(loose & -loose);
                    }
                });
                bits.didClearCallClobberedBits((m_numberOfFixedBits + 63) / 64);
            } else if (!m_allocationReads.isEmpty()) {
                BitVector kept = m_fixed;
                for (size_t bit = bits.findBit(m_reads.size(), true); bit < numberOfBits(); bit = bits.findBit(bit + 1, true))
                    kept.merge(m_allocationReads[bit - m_reads.size()]);
                bits.filter(kept);
            } else if (!m_numberOfFixedBits)
                bits.clearAll();
            else
                bits.filter(m_fixed);
            return;
        case Action::Allocates:
            bits.quickSet(m_reads.size() + action.first);
            return;
        case Action::Escapes:
            if constexpr (std::is_same_v<Bits, WordsInUse>) {
                if (bits.quickGet(m_reads.size() + action.first)) {
                    for (uint32_t bit : readsOf(action.first))
                        bits.markCallClobbered(bit);
                }
            }
            bits.quickClear(m_reads.size() + action.first);
            return;
        }
    }

    BitVector stateAtStartOf(unsigned block) const
    {
        if (!block)
            return BitVector(numberOfBits());
        BitVector bits = m_all;
        for (unsigned predecessor : m_predecessors[block]) {
            uint32_t bit = m_bitSetOnNormalExit[predecessor];
            auto& terminator = m_function.blocks[predecessor].terminator;
            bool isExecuted = bit != none && bits.quickGet(bit) && (terminator.kind != TerminatorKind::Invoke || terminator.targets[1].block.index() != block);
            bits.filter(m_atEnd[predecessor]);
            if (isExecuted)
                bits.quickSet(bit);
        }
        return bits;
    }

    void startAt(unsigned block, BitVector& bits) const { bits = stateAtStartOf(block); }

    void startAt(unsigned block, WordsInUse& bits) const
    {
        bits.reset();
        if (!block)
            return;
        bool isAll = true;
        for (unsigned predecessor : m_predecessors[block]) {
            uint32_t bit = m_bitSetOnNormalExit[predecessor];
            auto& terminator = m_function.blocks[predecessor].terminator;
            bool isExecuted = bit != none && (isAll || bits.quickGet(bit)) && (terminator.kind != TerminatorKind::Invoke || terminator.targets[1].block.index() != block);
            auto& atEnd = m_wordsAtEnd[predecessor];
            if (atEnd.isAll)
                continue;
            if (isAll) {
                bits.load(atEnd);
                isAll = false;
            } else
                bits.filter(atEnd);
            if (isExecuted)
                bits.quickSet(bit);
        }
        if (isAll)
            bits.setAll();
    }

    bool solveWithWords(BitVector& isStale)
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_fixedWords.fill(0, (numberOfBits() + 63) / 64);
        for (unsigned bit = 0; bit < m_numberOfFixedBits; ++bit)
            m_fixedWords[bit / 64] |= 1ull << (bit % 64);
        for (unsigned bit = m_reads.size(); bit < numberOfBits(); ++bit)
            m_fixedWords[bit / 64] |= 1ull << (bit % 64);
        m_wordsAtEnd.grow(numberOfBlocks);
        m_wordsInUse.initialize(numberOfBits());
        auto& bits = m_wordsInUse;
        Vector<uint64_t> atEnd;
        size_t maxWordsPerBlock = (m_fixedWords.size() + 63) / 64 + m_fixedWords.size() / 12;
        size_t numberOfWordsStored = 0;
        size_t visitedBlockCount = 256;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned block : m_order) {
                if (!isStale.quickClear(block))
                    continue;
                startAt(block, bits);
                for (auto& action : m_actions[block])
                    carryOut(action, bits);
                bool isAll = bits.store(atEnd);
                auto& was = m_wordsAtEnd[block];
                if (isAll != was.isAll || atEnd != was.words) {
                    was.isAll = isAll;
                    was.words = atEnd;
                    for (auto& target : m_function.blocks[block].terminator.targets)
                        isStale.quickSet(target.block.index());
                    changed = true;
                }
                numberOfWordsStored += atEnd.size();
                if (numberOfWordsStored > ++visitedBlockCount * maxWordsPerBlock)
                    return false;
            }
        }
        return true;
    }

    void fallBackToBitVectors()
    {
        m_usesWordSolver = false;
        for (auto& reads : m_allocationReads)
            reads.ensureSize(m_reads.size());
        for (uint32_t bit = 0; bit < m_reads.size(); ++bit) {
            if (uint32_t allocation = m_readAllocation[bit]; allocation != none)
                m_allocationReads[allocation].quickSet(bit);
        }
        for (unsigned block = 0; block < m_wordsAtEnd.size(); ++block) {
            if (m_wordsAtEnd[block].isAll)
                continue;
            BitVector bits(numberOfBits());
            m_wordsInUse.reset();
            m_wordsInUse.load(m_wordsAtEnd[block]);
            m_wordsInUse.forEachWordInUse([&](size_t index, uint64_t& word) {
                for (uint64_t rest = word; rest; rest &= rest - 1)
                    bits.quickSet(index * 64 + WTF::ctz(rest));
            });
            m_atEnd[block] = WTF::move(bits);
        }
        m_wordsAtEnd.clear();
    }

    void solve()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        m_predecessors.grow(numberOfBlocks);
        auto& predecessors = m_predecessors;
        auto& order = m_order;
        {
            BitVector seen(numberOfBlocks);
            seen.set(0);
            Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next == targets.size()) {
                    order.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned target = targets[next++].block.index();
                predecessors[target].append(block);
                if (!seen.set(target))
                    stack.append({ target, 0u });
            }
            order.reverse();
        }
        BitVector isStale(numberOfBlocks);
        for (unsigned block : order)
            isStale.quickSet(block);
        if (m_usesWordSolver && solveWithWords(isStale))
            return;
        m_all.ensureSize(numberOfBits());
        for (unsigned bit = 0; bit < numberOfBits(); ++bit)
            m_all.quickSet(bit);
        m_fixed.ensureSize(numberOfBits());
        for (unsigned bit = 0; bit < m_numberOfFixedBits; ++bit)
            m_fixed.quickSet(bit);
        for (unsigned bit = m_reads.size(); bit < numberOfBits(); ++bit)
            m_fixed.quickSet(bit);
        m_atEnd.fill(m_all, numberOfBlocks);
        if (m_usesWordSolver)
            fallBackToBitVectors();
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned block : order) {
                if (!isStale.quickClear(block))
                    continue;
                BitVector bits = stateAtStartOf(block);
                for (auto& action : m_actions[block])
                    carryOut(action, bits);
                if (bits != m_atEnd[block]) {
                    m_atEnd[block] = WTF::move(bits);
                    for (auto& target : m_function.blocks[block].terminator.targets)
                        isStale.quickSet(target.block.index());
                    changed = true;
                }
            }
        }
    }

    bool mergePredecessorFacts(unsigned b, size_t actionIndex)
    {
        static constexpr unsigned maxPredecessors = 4;
        uint32_t bit = m_actions[b][actionIndex].first;
        auto& incomingEdges = m_predecessors[b];
        if (m_usesWordSolver || m_reads[bit].stored || incomingEdges.size() < 2 || incomingEdges.size() > maxPredecessors)
            return false;
        if (m_incomingEdgeCounts.isEmpty()) {
            m_incomingEdgeCounts.fill(0, m_function.blocks.size());
            for (auto& block : m_function.blocks) {
                for (auto& target : block.terminator.targets)
                    ++m_incomingEdgeCounts[target.block.index()];
            }
        }
        if (m_incomingEdgeCounts[b] != incomingEdges.size())
            return false;
        ValueID id = m_function.blocks[b].values[m_actions[b][actionIndex].second];
        TypeID type = m_function.at(id).type;
        if (!m_program.at(type).hasOnlyPrimitives(Primitive::Number | Primitive::UndefinedOrNull))
            return false;
        auto [first, end] = m_sameAs[bit];
        Vector<ValueID, maxPredecessors> known;
        for (unsigned from : incomingEdges) {
            TerminatorKind kind = m_function.blocks[from].terminator.kind;
            if (kind != TerminatorKind::Jump && kind != TerminatorKind::Branch)
                return false;
            BitVector bits = m_atEnd[from];
            for (size_t i = 0; i < actionIndex; ++i)
                carryOut(m_actions[b][i], bits);
            ValueID found;
            for (size_t other = bits.findBit(first, true); other < end && !found; other = bits.findBit(other + 1, true)) {
                if (other != bit && typeOf(m_reads[other].returnedValue()).isSubsetOf(m_program.at(type), m_program))
                    found = m_reads[other].returnedValue();
            }
            if (!found)
                return false;
            known.append(found);
        }
        Value parameter;
        parameter.opcode = Opcode::Parameter;
        parameter.position = m_function.at(id).position;
        parameter.inlineFrame = m_function.at(id).inlineFrame;
        parameter.type = type;
        parameter.rep = m_function.at(id).rep;
        parameter.copyPredictionFrom(m_function.at(id));
        parameter.immediates = { b, static_cast<uint32_t>(m_function.blocks[b].parameters.size()) };
        ValueID joined(m_function.values.size());
        m_function.values.append(WTF::move(parameter));
        m_function.blocks[b].parameters.append(joined);
        for (unsigned i = 0; i < incomingEdges.size(); ++i) {
            bool isAlreadyPassed = false;
            for (unsigned earlier = 0; earlier < i; ++earlier)
                isAlreadyPassed |= incomingEdges[earlier] == incomingEdges[i];
            if (isAlreadyPassed)
                continue;
            for (auto& target : m_function.blocks[incomingEdges[i]].terminator.targets) {
                if (target.block.index() == b)
                    target.arguments.append(known[i]);
            }
        }
        auto& value = m_function.at(id);
        value.opcode = Opcode::Cast;
        value.children = { joined, ValueID(), ValueID() };
        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
        return true;
    }

    template<typename Bits>
    bool replace(Bits& bits)
    {
        bool changed = false;
        for (unsigned b : m_order) {
            if (m_actions[b].isEmpty() && m_bitSetOnNormalExit[b] == none)
                continue;
            auto& block = m_function.blocks[b];
            startAt(b, bits);
            auto tryReplace = [&](uint32_t bit, ValueID id) {
                if (m_reads[bit].stored)
                    return false;
                auto& value = m_function.at(id);
                auto [first, end] = m_sameAs[bit];
                for (size_t other = findSetBit(bits, first, end); other < end; other = findSetBit(bits, other + 1, end)) {
                    if (other == bit)
                        continue;
                    ValueID earlier = m_reads[other].returnedValue();
                    if (!typeOf(earlier).isSubsetOf(m_program.at(value.type), m_program))
                        continue;
                    value.opcode = Opcode::Cast;
                    value.children = { earlier, ValueID(), ValueID() };
                    value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                    return true;
                }
                return false;
            };
            for (size_t i = 0; i < m_actions[b].size(); ++i) {
                auto& action = m_actions[b][i];
                if (action.kind == Action::Reads)
                    changed |= tryReplace(action.first, block.values[action.second]) || mergePredecessorFacts(b, i);
                carryOut(action, bits);
            }
            if (uint32_t bit = m_bitSetOnNormalExit[b]; bit != none && tryReplace(bit, block.values.last())) {
                block.terminator.kind = TerminatorKind::Jump;
                block.terminator.targets.shrink(1);
                changed = true;
            }
        }
        return changed;
    }

    const Program& m_program;
    Function& m_function;
    Vector<Read> m_reads;
    Vector<std::pair<uint32_t, uint32_t>> m_sameAs;
    Vector<uint32_t> m_bitOf;
    Vector<uint32_t> m_allocationIndex;
    Vector<BitVector> m_allocationReads;
    Vector<uint32_t> m_readAllocation;
    Vector<uint32_t> m_firstAllocationRead;
    Vector<uint32_t> m_readsByAllocation;
    UncheckedKeyHashMap<uint64_t, std::pair<uint32_t, uint32_t>, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_locationBits;
    UncheckedKeyHashMap<uint64_t, bool, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_isOnlyInFields;
    Vector<Vector<Action>> m_actions;
    Vector<uint32_t> m_bitSetOnNormalExit;
    Vector<Vector<unsigned, 2>> m_predecessors;
    Vector<unsigned> m_incomingEdgeCounts;
    Vector<BitVector> m_atEnd;
    Vector<unsigned> m_order;
    BitVector m_all;
    BitVector m_fixed;
    unsigned m_numberOfFixedBits { 0 };
    bool m_usesWordSolver { false };
    Vector<Words> m_wordsAtEnd;
    Vector<uint64_t> m_fixedWords;
    WordsInUse m_wordsInUse;
    UncheckedKeyHashSet<uint64_t, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_storedLocations;
};

class ReadHoisting {
public:
    ReadHoisting(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool run()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        Vector<Vector<unsigned, 2>> predecessors(numberOfBlocks);
        Vector<unsigned> order;
        Vector<unsigned> orderIndex;
        orderIndex.fill(unknown, numberOfBlocks);
        {
            BitVector seen(numberOfBlocks);
            seen.set(0);
            Vector<std::pair<unsigned, unsigned>> stack { { 0u, 0u } };
            while (!stack.isEmpty()) {
                auto& [block, next] = stack.last();
                auto& targets = m_function.blocks[block].terminator.targets;
                if (next == targets.size()) {
                    order.append(block);
                    stack.removeLast();
                    continue;
                }
                unsigned target = targets[next++].block.index();
                predecessors[target].append(block);
                if (!seen.set(target))
                    stack.append({ target, 0u });
            }
            order.reverse();
            for (unsigned i = 0; i < order.size(); ++i)
                orderIndex[order[i]] = i;
        }
        Vector<unsigned> dominator;
        dominator.fill(unknown, numberOfBlocks);
        dominator[0] = 0;
        for (bool changed = !tryFindImmediateDominators(order.size(), [&](unsigned i) { return order[i]; }, predecessors, dominator); changed;) {
            changed = false;
            for (unsigned i = 1; i < order.size(); ++i) {
                unsigned found = unknown;
                for (unsigned other : predecessors[order[i]]) {
                    if (dominator[other] == unknown)
                        continue;
                    if (found == unknown) {
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
        auto dominates = [&](unsigned a, unsigned b) {
            while (orderIndex[b] > orderIndex[a])
                b = dominator[b];
            return a == b;
        };
        m_definingBlock.fill(unknown, m_function.values.size());
        for (unsigned b : order) {
            for (ValueID id : m_function.blocks[b].parameters)
                m_definingBlock[id.index()] = b;
            for (ValueID id : m_function.blocks[b].values)
                m_definingBlock[id.index()] = b;
        }

        bool changed = false;
        for (unsigned i = order.size(); i--;) {
            unsigned header = order[i];
            BitVector body(numberOfBlocks);
            body.set(header);
            Vector<unsigned, 16> worklist;
            unsigned entering = unknown;
            bool isEnteredFromOneBlock = true;
            for (unsigned predecessor : predecessors[header]) {
                if (dominates(header, predecessor)) {
                    if (!body.set(predecessor))
                        worklist.append(predecessor);
                    continue;
                }
                isEnteredFromOneBlock &= entering == unknown;
                entering = predecessor;
            }
            if ((worklist.isEmpty() && !predecessors[header].contains(header)) || entering == unknown || !isEnteredFromOneBlock || m_function.blocks[entering].terminator.kind != TerminatorKind::Jump)
                continue;
            while (!worklist.isEmpty()) {
                for (unsigned predecessor : predecessors[worklist.takeLast()]) {
                    if (!body.set(predecessor))
                        worklist.append(predecessor);
                }
            }
            changed |= hoistOutOf(body, header, entering);
        }
        return changed;
    }

private:
    static constexpr unsigned unknown = UINT_MAX;
    static uint64_t environmentVariable(LayoutID layout, uint32_t field) { return 1ull << 62 | static_cast<uint64_t>(layout.index()) << 32 | field; }
    static uint64_t moduleVariable(VariableID variable) { return 2ull << 62 | variable.index(); }

    std::optional<uint64_t> placeOf(const Value& access) const
    {
        if (access.opcode == Opcode::GetModuleVariable || access.opcode == Opcode::PutModuleVariable)
            return moduleVariable(access.variable());
        if (isObjectLayout(m_program.at(access.layout()).kind))
            return std::nullopt;
        return environmentVariable(access.layout(), access.field());
    }

    bool hoistOutOf(const BitVector& body, unsigned header, unsigned entering)
    {
        bool hasRead = m_function.blocks[header].values.containsIf([&](ValueID id) {
            auto& value = m_function.at(id);
            return (value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::GetField) && placeOf(value);
        });
        if (!hasRead)
            return false;
        UncheckedKeyHashSet<uint64_t> untouchedLocations;
        for (size_t b : body) {
            auto& block = m_function.blocks[b];
            for (size_t index = 0; index < block.values.size(); ++index) {
                auto& value = m_function.at(block.values[index]);
                switch (value.opcode) {
                case Opcode::GetModuleVariable:
                case Opcode::GetField:
                    if (auto location = placeOf(value)) {
                        if (mayThrow(m_program, m_function, value) || (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size()))
                            untouchedLocations.add(*location);
                    }
                    break;
                case Opcode::PutModuleVariable:
                    untouchedLocations.add(*placeOf(value));
                    break;
                case Opcode::PutField:
                    if (auto location = placeOf(value))
                        untouchedLocations.add(*location);
                    else if (writesOf(m_program, m_function, block, index) >= Writes::Heap)
                        return false;
                    break;
                case Opcode::PutById:
                    if (writesOf(m_program, m_function, block, index) >= Writes::Heap)
                        return false;
                    break;
                case Opcode::CallDirect:
                case Opcode::Await:
                case Opcode::Yield:
                case Opcode::GetResumeMode:
                    return false;
                default:
                    if (writesOf(m_program, m_function, block, index) >= Writes::Fields)
                        return false;
                    break;
                }
            }
        }
        bool changed = false;
        m_function.blocks[header].values.removeAllMatching([&](ValueID id) {
            auto& value = m_function.at(id);
            if (value.opcode != Opcode::GetModuleVariable && value.opcode != Opcode::GetField)
                return false;
            auto location = placeOf(value);
            if (!location || untouchedLocations.contains(*location))
                return false;
            if (auto* engineClass = m_program.at(value.type).singleEngineClass(); engineClass && engineClass->type == ArrayType)
                return false;
            if (value.opcode == Opcode::GetField && (m_definingBlock[value.child(0).index()] == unknown || body.get(m_definingBlock[value.child(0).index()])))
                return false;
            m_function.blocks[entering].values.append(id);
            m_definingBlock[id.index()] = entering;
            changed = true;
            return true;
        });
        return changed;
    }

    const Program& m_program;
    Function& m_function;
    Vector<unsigned> m_definingBlock;
};

static bool isSpreadOfNonObjectConstant(const Program& program, const Function& function, const Value& value)
{
    if ((value.opcode != Opcode::Operation && value.opcode != Opcode::CallBuiltin) || value.arguments.size() != 2 || languageOperationOf(program, value) != LanguageOperation::CopyDataProperties)
        return false;
    ValueID id = value.arguments[1];
    while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
        id = function.at(id).child(0);
    Opcode source = function.at(id).opcode;
    return source == Opcode::Undefined || source == Opcode::Null || source == Opcode::Boolean || source == Opcode::Integer || source == Opcode::Double;
}

bool isIteratorResultCheckOfObject(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::Operation || value.arguments.size() != 1 || languageOperationOf(program, value) != LanguageOperation::RequireIteratorResultObject)
        return false;
    ValueID id = value.arguments[0];
    while (function.at(id).opcode == Opcode::Cast && function.at(id).castReason() == CastReason::Upcast)
        id = function.at(id).child(0);
    switch (function.at(id).opcode) {
    case Opcode::NewObjectWithShape:
    case Opcode::NewObject:
    case Opcode::NewArray:
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
    case Opcode::FunctionReference:
        return true;
    default:
        return program.typeOf(function, id).isObject();
    }
}

class Cleanup {
public:
    Cleanup(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    void hoistAllocationsIntoPredecessors()
    {
        bool hasWrapperAllocation = m_function.worker || m_function.inlineFrames.containsIf([&](auto& frame) {
            return frame.callee && m_program.at(frame.callee).worker;
        });
        if (!hasWrapperAllocation)
            return;
        m_hoistsAllocations = true;
        hoistStoresIntoPredecessors();
        gatherInitializingStores();
    }

    void runTrivialParameterRemoval()
    {
        if (!m_function.blocks.isEmpty())
            removeTrivialParameters();
    }

    bool runUnusedValueRemoval()
    {
        return !m_function.blocks.isEmpty() && removeUnusedValues();
    }

    bool runDeadLoopRemoval()
    {
        return !m_function.blocks.isEmpty() && removeDeadCountedLoops();
    }

    bool runSinkToBooleanIntoPredecessors()
    {
        bool changed = sinkToBooleanIntoPredecessors();
        changed |= sinkComparisonIntoPredecessors();
        return changed;
    }

    void run(bool onlyIfFoldable = false)
    {
        if (m_function.blocks.isEmpty())
            return;
        for (auto& value : m_function.values)
            m_opcodes.set(static_cast<unsigned>(value.opcode));
        bool hasOperations = has(Opcode::Operation) || has(Opcode::CallBuiltin);
        bool mayAllocateArrays = hasOperations || has(Opcode::NewArray) || has(Opcode::CreateRest) || has(Opcode::NewClosure);
        m_allocatesObjects = has(Opcode::NewObject) || has(Opcode::NewObjectWithShape);
        if (has(Opcode::GetByVal) || has(Opcode::PutByVal) || has(Opcode::InByVal) || has(Opcode::DeleteByVal))
            convertConstantKeysToNames();
        if (has(Opcode::GetModuleVariable) || has(Opcode::GetField))
            foldReads();
        if (has(Opcode::TypeOf))
            foldTypeOfComparisons();
        if (hasOperations) {
            foldSpreadCalls();
            foldSuperConstructs();
        }
        if (onlyIfFoldable && !m_hasFolded)
            return;
        do {
            bool changed;
            do {
                foldInvokes();
                foldConstantConditions();
                foldBranches();
                removeUnreachableBlocks();
                changed = removeTrivialParameters(UnreachableBlockState::AreRemoved);
                changed |= propagateBranchConditions();
                changed |= has(Opcode::ToBoolean) && sinkToBooleanIntoPredecessors();
                changed |= (has(Opcode::Less) || has(Opcode::LessEq) || has(Opcode::Greater) || has(Opcode::GreaterEq)) && sinkComparisonIntoPredecessors();
                changed |= threadBranchesOnParameters();
                changed |= threadEdgesThatDecideTest();
                changed |= mergeChains();
                changed |= m_allocatesObjects && forwardStores();
                changed |= removeUnusedValues();
            } while (changed);
        } while (!Options::auditAOTTypedAssertions() && (RedundantReadElimination(m_program, m_function).run() || (mayAllocateArrays && ArrayEscapeAnalysis(m_program, m_function).run()) || (has(Opcode::NewObjectWithShape) && (promoteEnvironments() || sinkObjectAllocations(m_program, m_function, AllocationSinkingPolicy::EliminateOnly)))));
        if (mergeBooleanForwardingBlocks())
            removeUnreachableBlocks();
        compactBlocks(m_function);
        if (has(Opcode::NewObject) && hasOperations && !Options::auditAOTTypedAssertions())
            gatherLiteralFields();
        if (!has(Opcode::NewObjectWithShape))
            return;
        hoistStoresIntoPredecessors();
        gatherInitializingStores();
    }

private:
    void convertConstantKeysToNames()
    {
        for (auto& value : m_function.values) {
            Opcode byName;
            switch (value.opcode) {
            case Opcode::GetByVal:
                byName = Opcode::GetById;
                break;
            case Opcode::PutByVal:
                byName = Opcode::PutById;
                break;
            case Opcode::InByVal:
                byName = Opcode::InById;
                break;
            case Opcode::DeleteByVal:
                byName = Opcode::DeleteById;
                break;
            default:
                continue;
            }
            ValueID key = value.child(1);
            while (m_function.at(key).opcode == Opcode::Cast && m_function.at(key).castReason() == CastReason::Upcast)
                key = m_function.at(key).child(0);
            if (m_function.at(key).opcode != Opcode::String)
                continue;
            StringID name = m_function.at(key).string();
            auto text = m_program.string(name);
            if (text.empty() || isASCIIDigit(text[0]) || text[0] == '-' || equalSpans(text, asBytes("Infinity"_s.span8())) || equalSpans(text, asBytes("NaN"_s.span8())) || isPrivateName(text))
                continue;
            ValueID stored = value.opcode == Opcode::PutByVal ? value.child(2) : ValueID();
            if (stored) {
                auto access = fieldOf(m_program, m_program.typeOf(m_function, value.child(0)), name);
                TypeID held = access ? m_program.at(access->layout).fields[access->field].type : TypeID();
                if (held && !m_program.typeOf(m_function, stored).isSubsetOf(m_program.at(held), m_program))
                    continue;
            }
            value.opcode = byName;
            m_hasFolded = true;
            value.children = { value.child(0), stored, ValueID() };
            value.immediates = { name.index(), Format::none };
        }
    }

    struct LocalObjectField {
        ValueID object;
        StringID name;
    };

    std::optional<LocalObjectField> localObjectField(const Value& access) const
    {
        ValueID object = access.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !isObjectLayout(m_program.at(m_function.at(object).layout()).kind))
            return std::nullopt;
        bool isByName = access.opcode == Opcode::GetById || access.opcode == Opcode::PutById;
        if (isByName && m_program.at(m_function.at(object).layout()).mayDespecialize && !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return std::nullopt;
        StringID name = isByName ? access.string() : m_program.at(access.layout()).fields[access.field()].name;
        if (!name || !m_program.at(m_function.at(object).layout()).fieldNamed(name))
            return std::nullopt;
        if (isByName && mayInherit(m_program, m_function.at(object).layout(), name))
            return std::nullopt;
        return LocalObjectField { object, name };
    }

    bool readsLocalAllocations() const
    {
        auto isAllocatedHere = [&](ValueID object) {
            while (m_function.at(object).opcode == Opcode::Cast)
                object = m_function.at(object).child(0);
            auto& created = m_function.at(object);
            return created.opcode == Opcode::NewObject || (created.opcode == Opcode::NewObjectWithShape && isObjectLayout(m_program.at(created.layout()).kind));
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                switch (value.opcode) {
                case Opcode::GetById:
                case Opcode::GetField:
                case Opcode::InById:
                    if (isAllocatedHere(value.child(0)))
                        return true;
                    break;
                case Opcode::Operation:
                case Opcode::CallBuiltin:
                    if (value.arguments.size() == 2 && m_function.at(value.arguments[0]).opcode == Opcode::NewObject && m_function.at(value.arguments[1]).opcode == Opcode::NewObject)
                        return true;
                    break;
                default:
                    break;
                }
            }
        }
        return false;
    }

    bool forwardStores()
    {
        if (!readsLocalAllocations())
            return false;
        bool changed = false;
        PathFacts held;
        Vector<unsigned> incomingEdges;
        incomingEdges.fill(0, m_function.blocks.size());
        incomingEdges[0] = 1;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++incomingEdges[target.block.index()];
        }
        BitVector isContinuation(m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (size_t i = 0; i < successorCount(block); ++i) {
                if (incomingEdges[block.terminator.targets[i].block.index()] == 1)
                    isContinuation.set(block.terminator.targets[i].block.index());
            }
        }
        for (unsigned head = 0; head < m_function.blocks.size(); ++head) {
            if (isContinuation.get(head))
                continue;
            held.reset();
            m_absent.reset();
            m_literalFacts.shrink(0);
            m_firstRelevantKnown = 0;
            forwardStoresFrom(head, isContinuation, held, changed);
        }
        return changed;
    }

    static size_t successorCount(const Block& block) { return block.terminator.kind == TerminatorKind::Invoke ? 1 : block.terminator.targets.size(); }

    void forwardStoresFrom(unsigned head, const BitVector& isContinuation, PathFacts& held, bool& changed)
    {
        struct PendingSuccessor {
            unsigned block;
            PathFacts::State held;
            PathFacts::State absent;
            size_t literalFactCount;
            size_t firstRelevantKnown;
        };
        Vector<PendingSuccessor> later;
        for (unsigned b = head;;) {
            auto& block = m_function.blocks[b];
            Vector<unsigned, 2> pendingSuccessors;
            for (size_t i = 0; i < successorCount(block); ++i) {
                if (isContinuation.get(block.terminator.targets[i].block.index()))
                    pendingSuccessors.append(block.terminator.targets[i].block.index());
            }
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& access = m_function.at(block.values[i]);
                bool isRead = access.opcode == Opcode::GetById || access.opcode == Opcode::GetField;
                bool isStore = access.opcode == Opcode::PutById || access.opcode == Opcode::PutField;
                if (!isStore && held.isEmpty() && m_absent.isEmpty() && access.opcode != Opcode::NewObject && access.opcode != Opcode::NewObjectWithShape)
                    continue;
                if (expandLiteralSpreadIntoFields(block, i)) {
                    changed = true;
                    --i;
                    continue;
                }
                auto accessed = isRead || isStore ? localObjectField(access) : std::nullopt;
                ValueID literal = literalBeingDefined(access);
                invalidateLiteralsUsedBy(access, literal);
                if (!accessed) {
                    if (literal) {
                        held.set(static_cast<uint64_t>(literal.index()) << 32 | m_function.at(access.arguments[1]).string().index(), access.arguments[2]);
                        m_literalFacts.append({ static_cast<uint64_t>(literal.index()) << 32 | m_function.at(access.arguments[1]).string().index(), access.arguments[2] });
                    } else if (access.opcode == Opcode::NewObject) {
                        m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | UINT32_MAX);
                        m_literalFacts.append({ static_cast<uint64_t>(block.values[i].index()) << 32 | UINT32_MAX, ValueID() });
                    }
                    else if (access.opcode == Opcode::NewObjectWithShape && !access.child(0)) {
                        m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | UINT32_MAX);
                        for (auto& field : m_program.at(access.layout()).fields) {
                            if (field.name)
                                m_absent.add(static_cast<uint64_t>(block.values[i].index()) << 32 | field.name.index());
                        }
                    } else if (ValueID defined = literalPropertyValue(access, held)) {
                        if (access.opcode == Opcode::InById) {
                            access.opcode = Opcode::Boolean;
                            access.children = { };
                            access.immediates = { 1, Format::none };
                        } else {
                            access.opcode = Opcode::Cast;
                            access.children = { defined, ValueID(), ValueID() };
                            access.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        }
                        changed = true;
                        if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                            block.terminator.kind = TerminatorKind::Jump;
                            block.terminator.targets.shrink(1);
                        }
                    } else if (readsMissingProperty(access) || readsMissingLiteralProperty(access, held)) {
                        bool isQuery = access.opcode == Opcode::InById;
                        access.opcode = isQuery ? Opcode::Boolean : Opcode::Undefined;
                        access.children = { };
                        access.immediates = { isQuery ? 0 : Format::none, Format::none };
                        changed = true;
                        if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                            block.terminator.kind = TerminatorKind::Jump;
                            block.terminator.targets.shrink(1);
                        }
                    } else if (bool writesFields = !isNoOp(access) && mayWriteFields(block, i); isStore || writesFields) {
                        Vector<std::pair<uint64_t, ValueID>, 16> stillKnown;
                        while (m_firstRelevantKnown < m_literalFacts.size() && !m_absent.contains(m_literalFacts[m_firstRelevantKnown].first | UINT32_MAX))
                            ++m_firstRelevantKnown;
                        for (size_t k = m_firstRelevantKnown; k < m_literalFacts.size(); ++k) {
                            if (m_absent.contains(m_literalFacts[k].first | UINT32_MAX))
                                stillKnown.append(m_literalFacts[k]);
                        }
                        m_absent.clear();
                        if (writesFields)
                            held.clear();
                        for (auto& [key, defined] : stillKnown) {
                            if (defined)
                                held.set(key, defined);
                            else
                                m_absent.add(key);
                        }
                    }
                    continue;
                }
                uint64_t key = static_cast<uint64_t>(accessed->object.index()) << 32 | accessed->name.index();
                if (isStore) {
                    m_absent.remove(key);
                    auto& layout = m_program.at(m_function.at(accessed->object).layout());
                    TypeID fieldType = layout.fields[*layout.fieldNamed(accessed->name)].type;
                    if (fieldType && !m_program.typeOf(m_function, access.child(1)).isSubsetOf(m_program.at(fieldType), m_program))
                        held.remove(key);
                    else
                        held.set(key, access.child(1));
                    continue;
                }
                ValueID found = held.get(key);
                if (!found) {
                    if (!m_absent.contains(key) || !(m_program.at(access.type).primitives & Primitive::Undefined))
                        continue;
                    access.opcode = Opcode::Undefined;
                    access.children = { };
                    access.immediates = { Format::none, Format::none };
                    changed = true;
                    if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                        block.terminator.kind = TerminatorKind::Jump;
                        block.terminator.targets.shrink(1);
                    }
                    continue;
                }
                if (!m_program.typeOf(m_function, found).isSubsetOf(m_program.at(access.type), m_program)) {
                    held.remove(key);
                    continue;
                }
                ValueID value = found;
                access.opcode = Opcode::Cast;
                access.children = { value, ValueID(), ValueID() };
                access.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                changed = true;
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            if (!m_absent.isEmpty()) {
                block.terminator.forEachChild([&](ValueID& child) {
                    invalidateLiteral(child);
                });
            }
            if (pendingSuccessors.isEmpty()) {
                if (later.isEmpty())
                    return;
                auto last = later.takeLast();
                b = last.block;
                held.restore(last.held);
                m_absent.restore(last.absent);
                m_literalFacts.shrink(last.literalFactCount);
                m_firstRelevantKnown = last.firstRelevantKnown;
                continue;
            }
            for (size_t i = 0; i + 1 < pendingSuccessors.size(); ++i)
                later.append(PendingSuccessor { pendingSuccessors[i], held.save(), m_absent.save(), m_literalFacts.size(), m_firstRelevantKnown });
            if (pendingSuccessors.size() > 1) {
                held.startLayer();
                m_absent.startLayer();
            }
            b = pendingSuccessors.last();
        }
    }

    PathFacts m_absent;
    Vector<std::pair<uint64_t, ValueID>, 16> m_literalFacts;
    size_t m_firstRelevantKnown { 0 };

    bool readsMissingProperty(const Value& read) const
    {
        if (read.opcode != Opcode::InById && (read.opcode != Opcode::GetById || !(m_program.at(read.type).primitives & Primitive::Undefined)))
            return false;
        ValueID object = read.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return false;
        LayoutID layout = m_function.at(object).layout();
        StringID name = read.string();
        if (m_program.at(layout).kind != LayoutKind::Record || m_program.at(layout).fieldNamed(name) || isPrivateName(m_program.string(name)) || mayInherit(m_program, layout, name))
            return false;
        for (LayoutID ancestor = layout; ancestor; ancestor = m_program.at(ancestor).base) {
            if (m_program.at(ancestor).hasComputedMethodName)
                return false;
            for (auto& method : m_program.at(ancestor).methods) {
                if (method.name == name)
                    return false;
            }
        }
        return true;
    }

    static bool isCheckOnlyCast(const Value& cast) { return cast.castReason() != CastReason::As && cast.castReason() != CastReason::Coerce; }

    void invalidateLiteral(ValueID object)
    {
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return;
        m_absent.remove(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX);
        for (size_t i = 0; i < m_literalFacts.size(); ++i) {
            auto [key, defined] = m_literalFacts[i];
            if (defined && key >> 32 == object.index())
                invalidateLiteral(defined);
        }
    }

    ValueID literalBeingDefined(const Value& value) const
    {
        if (languageOperationOf(m_program, value) != LanguageOperation::DefineField || value.arguments.size() != 3 || m_function.at(value.arguments[1]).opcode != Opcode::String)
            return { };
        ValueID object = value.arguments[0];
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return { };
        return object;
    }

    void invalidateLiteralsUsedBy(const Value& user, ValueID definedOn)
    {
        bool asks = user.opcode == Opcode::GetById || user.opcode == Opcode::InById || (user.opcode == Opcode::Cast && isCheckOnlyCast(user));
        if (asks)
            return;
        for (ValueID child : user.children) {
            if (child)
                invalidateLiteral(child);
        }
        for (size_t i = 0; i < user.arguments.size(); ++i) {
            if (!(definedOn && i != 1))
                invalidateLiteral(user.arguments[i]);
        }
    }

    bool readsMissingLiteralProperty(const Value& read, const PathFacts& held) const
    {
        if (read.opcode != Opcode::InById && (read.opcode != Opcode::GetById || !(m_program.at(read.type).primitives & Primitive::Undefined)))
            return false;
        ValueID object = read.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return false;
        StringID name = read.string();
        return !held.contains(static_cast<uint64_t>(object.index()) << 32 | name.index()) && !isPrivateName(m_program.string(name)) && !isOnObjectPrototype(m_program, name);
    }

    bool expandLiteralSpreadIntoFields(Block& block, size_t index)
    {
        ValueID spread = block.values[index];
        if (languageOperationOf(m_program, m_function.at(spread)) != LanguageOperation::CopyDataProperties || m_function.at(spread).arguments.size() != 2)
            return false;
        ValueID target = m_function.at(spread).arguments[0];
        ValueID source = m_function.at(spread).arguments[1];
        auto isUnescapedLiteral = [&](ValueID id) {
            return m_function.at(id).opcode == Opcode::NewObject && m_absent.contains(static_cast<uint64_t>(id.index()) << 32 | UINT32_MAX);
        };
        if (target == source || !isUnescapedLiteral(target) || !isUnescapedLiteral(source))
            return false;
        struct Field {
            uint32_t name;
            ValueID defined;
        };
        Vector<Field, 8> fields;
        for (auto& [key, defined] : m_literalFacts) {
            if (key >> 32 != source.index() || !defined)
                continue;
            uint32_t name = static_cast<uint32_t>(key);
            size_t location = fields.findIf([&](auto& field) {
                return field.name == name;
            });
            if (location != notFound) {
                fields[location].defined = defined;
                continue;
            }
            auto text = m_program.string(StringID(name));
            if (text.empty() || isASCIIDigit(text[0]))
                return false;
            fields.append({ name, defined });
        }
        Vector<ValueID, 8> definitions;
        for (auto& field : fields) {
            ValueID written;
            for (unsigned i = 0; i < m_function.values.size() && !written; ++i) {
                auto& value = m_function.values[i];
                if (languageOperationOf(m_program, value) == LanguageOperation::DefineField && value.arguments.size() == 3 && value.arguments[0] == source && m_function.at(value.arguments[1]).opcode == Opcode::String && m_function.at(value.arguments[1]).string().index() == field.name)
                    written = ValueID(i);
            }
            if (!written)
                return false;
            definitions.append(written);
        }
        Vector<ValueID, 8> added;
        for (size_t i = 0; i < fields.size(); ++i) {
            Value definition = m_function.at(definitions[i]);
            definition.arguments[0] = target;
            definition.arguments[2] = fields[i].defined;
            definition.position = m_function.at(spread).position;
            definition.inlineFrame = m_function.at(spread).inlineFrame;
            added.append(ValueID(m_function.values.size()));
            m_function.values.append(WTF::move(definition));
        }
        auto& replaced = m_function.at(spread);
        replaced.opcode = Opcode::Cast;
        replaced.children = { target, ValueID(), ValueID() };
        replaced.arguments.clear();
        replaced.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
        replaced.type = m_function.at(target).type;
        replaced.rep = m_function.at(target).rep;
        if (index + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
        for (size_t i = added.size(); i--;)
            block.values.insert(index, added[i]);
        return true;
    }

    ValueID literalPropertyValue(const Value& read, const PathFacts& held) const
    {
        if ((read.opcode != Opcode::GetById && read.opcode != Opcode::InById) || read.immediates[1] != Format::none)
            return { };
        ValueID object = read.child(0);
        while (m_function.at(object).opcode == Opcode::Cast)
            object = m_function.at(object).child(0);
        if (m_function.at(object).opcode != Opcode::NewObject || !m_absent.contains(static_cast<uint64_t>(object.index()) << 32 | UINT32_MAX))
            return { };
        ValueID defined = held.get(static_cast<uint64_t>(object.index()) << 32 | read.string().index());
        if (!defined || (read.opcode == Opcode::GetById && !m_program.typeOf(m_function, defined).isSubsetOf(m_program.at(read.type), m_program)))
            return { };
        return defined;
    }

    bool mayWriteFields(const Block& block, size_t index) const
    {
        switch (m_function.at(block.values[index]).opcode) {
        case Opcode::CallDirect:
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
            return true;
        default:
            return writesOf(m_program, m_function, block, index) >= Writes::Fields;
        }
    }

    bool promoteEnvironments()
    {
        Vector<unsigned> allocationBlockOf;
        allocationBlockOf.fill(0, m_function.values.size());
        bool any = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::NewObjectWithShape || value.child(0))
                    continue;
                auto& layout = m_program.at(value.layout());
                if (layout.kind != LayoutKind::Environment || layout.isCoroutineFrame)
                    continue;
                allocationBlockOf[id.index()] = b + 1;
                any = true;
            }
        }
        if (!any)
            return false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& user = m_function.at(id);
                bool isAccess = user.opcode == Opcode::GetField || user.opcode == Opcode::PutField;
                std::as_const(user).forEachChild([&](ValueID child) {
                    if (!allocationBlockOf[child.index()] || (isAccess && user.child(0) == child && user.child(1) != child && user.layout() == m_function.at(child).layout()))
                        return;
                    allocationBlockOf[child.index()] = 0;
                });
            }
            block.terminator.forEachChild([&](ValueID& value) {
                allocationBlockOf[value.index()] = 0;
            });
        }
        for (unsigned v = 0; v < allocationBlockOf.size(); ++v) {
            if (allocationBlockOf[v] && promote(ValueID(v), allocationBlockOf[v] - 1))
                return true;
        }
        return false;
    }

    bool promote(ValueID environment, unsigned allocationBlockOf)
    {
        auto& fields = m_program.at(m_function.at(environment).layout()).fields;
        unsigned numberOfBlocks = m_function.blocks.size();
        static constexpr unsigned maxBlocks = 4096;
        if (fields.size() > 64 || numberOfBlocks > maxBlocks)
            return false;
        auto fieldType = [&](unsigned field) {
            return fields[field].type ? fields[field].type : m_program.types.anything;
        };
        auto isAccess = [&](const Value& value) {
            return (value.opcode == Opcode::GetField || value.opcode == Opcode::PutField) && value.child(0) == environment;
        };

        Vector<uint64_t> readFirst;
        Vector<uint64_t> stored;
        readFirst.fill(0, numberOfBlocks);
        stored.fill(0, numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& access = m_function.at(id);
                if (!isAccess(access))
                    continue;
                auto& type = m_program.at(fieldType(access.field()));
                uint64_t bit = 1ull << access.field();
                if (access.opcode == Opcode::PutField) {
                    if (!m_program.typeOf(m_function, access.child(1)).isSubsetOf(type, m_program))
                        return false;
                    stored[b] |= bit;
                    continue;
                }
                if (!type.isSubsetOf(m_program.at(access.type), m_program))
                    return false;
                if (!(stored[b] & bit))
                    readFirst[b] |= bit;
            }
        }

        Vector<uint64_t> live = readFirst;
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = numberOfBlocks; b--;) {
                uint64_t after = 0;
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (target.block.index() != allocationBlockOf)
                        after |= live[target.block.index()];
                }
                uint64_t before = readFirst[b] | (after & ~stored[b]);
                if (before != live[b]) {
                    live[b] = before;
                    changed = true;
                }
            }
        }
        if (live[allocationBlockOf])
            return false;

        Vector<Vector<ValueID>> handedOn(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            if (!live[b])
                continue;
            handedOn[b].fill(ValueID(), fields.size());
            auto& block = m_function.blocks[b];
            for (unsigned field = 0; field < fields.size(); ++field) {
                if (!(live[b] & 1ull << field))
                    continue;
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = m_function.at(environment).position;
                parameter.inlineFrame = m_function.at(environment).inlineFrame;
                parameter.type = fieldType(field);
                parameter.rep = repOf(m_program.at(parameter.type));
                parameter.immediates = { b, static_cast<uint32_t>(block.parameters.size()) };
                m_function.values.append(WTF::move(parameter));
                handedOn[b][field] = ValueID(m_function.values.size() - 1);
                block.parameters.append(handedOn[b][field]);
            }
        }
        Vector<ValueID> held;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_function.blocks[b];
            if (live[b])
                held = handedOn[b];
            else
                held.fill(ValueID(), fields.size());
            for (size_t i = 0; i < block.values.size(); ++i) {
                auto& access = m_function.at(block.values[i]);
                if (!isAccess(access))
                    continue;
                unsigned field = access.field();
                if (access.opcode == Opcode::PutField) {
                    held[field] = access.child(1);
                    access.opcode = Opcode::Undefined;
                    access.children = { };
                    access.immediates = { Format::none, Format::none };
                } else {
                    RELEASE_ASSERT(held[field]);
                    access.opcode = Opcode::Cast;
                    access.children = { held[field], ValueID(), ValueID() };
                    access.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                }
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            for (auto& target : block.terminator.targets) {
                unsigned to = target.block.index();
                if (to == allocationBlockOf || !live[to])
                    continue;
                for (unsigned field = 0; field < fields.size(); ++field) {
                    if (!(live[to] & 1ull << field))
                        continue;
                    RELEASE_ASSERT(held[field]);
                    target.arguments.append(held[field]);
                }
            }
        }
        return true;
    }

    static bool isConstant(const Value& value)
    {
        switch (value.opcode) {
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
            return true;
        default:
            return false;
        }
    }

    void hoistStoresIntoPredecessors()
    {
        static constexpr size_t maxValuesCopiedIntoPredecessors = 40;
        struct IncomingEdge {
            unsigned block;
            bool isJump;
        };
        Vector<Vector<IncomingEdge, 2>> incomingEdges(m_function.blocks.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            for (auto& target : terminator.targets)
                incomingEdges[target.block.index()].append(IncomingEdge { b, terminator.kind == TerminatorKind::Jump && target.block.index() != b });
        }
        auto isAllocatedIn = [&](ValueID value, unsigned block) {
            while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast)
                value = m_function.at(value).child(0);
            return m_function.at(value).opcode == Opcode::NewObjectWithShape && m_function.blocks[block].values.contains(value);
        };
        auto appendTo = [&](unsigned block, Value&& value) {
            ValueID id(m_function.values.size());
            m_function.values.append(WTF::move(value));
            m_function.blocks[block].values.append(id);
            return id;
        };
        auto copyIntoPredecessors = [&](unsigned j, size_t end, const Vector<IncomingEdge, 2>& arms) {
            Vector<ValueID, 16> head(m_function.blocks[j].values.span().first(end));
            Vector<ValueID, 4> parameters(m_function.blocks[j].parameters.span());
            BitVector isInHead(m_function.values.size());
            for (ValueID id : head)
                isInHead.set(id.index());
            BitVector isUsedLater(m_function.values.size());
            auto use = [&](ValueID value) {
                if (isInHead.get(value.index()))
                    isUsedLater.set(value.index());
            };
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (!isInHead.get(id.index()))
                        std::as_const(m_function.at(id)).forEachChild(use);
                }
                block.terminator.forEachChild([&](ValueID& value) {
                    use(value);
                });
            }
            for (ValueID id : head) {
                if (isUsedLater.get(id.index()) && m_function.at(id).rep == Rep::Void)
                    return;
            }
            m_function.cleanUpMovedStores = true;
            for (auto& arm : arms) {
                Arguments given = m_function.blocks[arm.block].terminator.targets[0].arguments;
                Vector<std::pair<ValueID, ValueID>, 16> copies;
                auto copyOf = [&](ValueID value) {
                    for (auto& [original, copy] : copies) {
                        if (original == value)
                            return copy;
                    }
                    size_t position = parameters.find(value);
                    if (position == notFound)
                        return value;
                    ValueID passed = given[position];
                    if (m_function.at(passed).type != m_function.at(value).type || m_function.at(value).hasDifferentPredictionThan(m_function.at(passed))) {
                        Value upcast = m_function.at(value);
                        upcast.opcode = Opcode::Cast;
                        upcast.children = { passed };
                        upcast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                        passed = appendTo(arm.block, WTF::move(upcast));
                    }
                    copies.append({ value, passed });
                    return passed;
                };
                for (ValueID id : head) {
                    Value copy = m_function.at(id);
                    copy.forEachChild([&](ValueID& child) {
                        child = copyOf(child);
                    });
                    ValueID created = appendTo(arm.block, WTF::move(copy));
                    copies.append({ id, created });
                    if (isUsedLater.get(id.index()))
                        m_function.blocks[arm.block].terminator.targets[0].arguments.append(created);
                }
            }
            auto& join = m_function.blocks[j];
            join.values.removeAt(0, end);
            for (ValueID id : head) {
                if (!isUsedLater.get(id.index()))
                    continue;
                auto& original = m_function.at(id);
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = original.position;
                parameter.inlineFrame = original.inlineFrame;
                parameter.type = original.type;
                parameter.rep = original.rep;
                parameter.copyPredictionFrom(original);
                parameter.immediates = { j, static_cast<uint32_t>(join.parameters.size()) };
                original = WTF::move(parameter);
                join.parameters.append(id);
            }
        };
        for (unsigned j = 0; j < m_function.blocks.size(); ++j) {
            auto& arms = incomingEdges[j];
            bool areJumps = arms.size() >= 2;
            for (auto& arm : arms)
                areJumps &= arm.isJump;
            if (!areJumps)
                continue;
            if (m_hoistsAllocations) {
                auto& join = m_function.blocks[j];
                Vector<ValueID, 8> given(join.parameters.span());
                BitVector isAllocatedHere(m_function.values.size());
                for (ValueID id : join.values)
                    isAllocatedHere.set(id.index());
                auto isAvailable = [&](ValueID operand) {
                    return given.contains(operand) || !isAllocatedHere.get(operand.index());
                };
                ValueID allocatedObject;
                size_t end = 0;
                for (size_t at = 0; at < std::min<size_t>(join.values.size(), maxValuesCopiedIntoPredecessors); ++at) {
                    ValueID id = join.values[at];
                    auto& value = m_function.at(id);
                    bool areAvailable = true;
                    value.forEachChild([&](ValueID child) {
                        areAvailable &= child == allocatedObject || isAvailable(child);
                    });
                    if (!areAvailable)
                        break;
                    if (isConstant(value) || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && (!allocatedObject || value.child(0) != allocatedObject))) {
                        given.append(id);
                        continue;
                    }
                    bool isOfWrapper = value.inlineFrame ? !!m_program.at(m_function.inlineFrames[value.inlineFrame].callee).worker : !!m_function.worker;
                    if (!allocatedObject && isOfWrapper && (value.opcode == Opcode::NewArray || (value.opcode == Opcode::NewObjectWithShape && !value.child(0)))) {
                        allocatedObject = id;
                        end = at + 1;
                        continue;
                    }
                    if (!allocatedObject || value.opcode != Opcode::PutField || value.child(0) != allocatedObject || value.child(1) == allocatedObject)
                        break;
                    end = at + 1;
                }
                if (allocatedObject) {
                    auto head = join.values.span().first(end);
                    bool hasStoreOutsideHead = false;
                    for (auto& block : m_function.blocks) {
                        for (ValueID id : block.values) {
                            auto& store = m_function.at(id);
                            if (store.opcode != Opcode::PutField || std::ranges::find(head, id) != head.end())
                                continue;
                            ValueID base = store.child(0);
                            while (m_function.at(base).opcode == Opcode::Cast)
                                base = m_function.at(base).child(0);
                            hasStoreOutsideHead |= base == allocatedObject;
                        }
                    }
                    if (hasStoreOutsideHead)
                        allocatedObject = ValueID();
                }
                if (allocatedObject) {
                    copyIntoPredecessors(j, end, arms);
                    auto& left = m_function.blocks[j];
                    size_t position = left.terminator.kind == TerminatorKind::Return && left.values.isEmpty() ? left.parameters.find(left.terminator.value) : notFound;
                    if (position != notFound) {
                        for (auto& arm : arms) {
                            auto& terminator = m_function.blocks[arm.block].terminator;
                            ValueID returned = terminator.targets[0].arguments[position];
                            terminator = Terminator { };
                            terminator.kind = TerminatorKind::Return;
                            terminator.value = returned;
                        }
                        m_function.blocks[j] = Block { };
                    }
                }
                continue;
            }
            for (size_t position = 0; position < m_function.blocks[j].parameters.size(); ++position) {
                bool isAllocatedInPredecessor = false;
                for (auto& arm : arms)
                    isAllocatedInPredecessor |= isAllocatedIn(m_function.blocks[arm.block].terminator.targets[0].arguments[position], arm.block);
                if (!isAllocatedInPredecessor)
                    continue;
                auto& join = m_function.blocks[j];
                Vector<ValueID, 2> upcastAliases { join.parameters[position] };
                size_t limit = std::min<size_t>(join.values.size() - (join.terminator.kind == TerminatorKind::Invoke), maxValuesCopiedIntoPredecessors);
                size_t end = 0;
                for (size_t at = 0; at < limit; ++at) {
                    ValueID id = join.values[at];
                    auto& value = m_function.at(id);
                    if (value.opcode == Opcode::LoopHint || value.opcode == Opcode::CatchValue || value.opcode == Opcode::Await || value.opcode == Opcode::Yield || value.opcode == Opcode::GetResumeMode)
                        break;
                    if (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && upcastAliases.contains(value.child(0))) {
                        upcastAliases.append(id);
                        continue;
                    }
                    bool usesObject = false;
                    value.forEachChild([&](ValueID child) {
                        usesObject |= upcastAliases.contains(child);
                    });
                    if (!usesObject)
                        continue;
                    if (value.opcode != Opcode::PutById || value.isInitialStore() || !upcastAliases.contains(value.child(0)) || upcastAliases.contains(value.child(1)))
                        break;
                    end = at + 1;
                }
                if (end)
                    copyIntoPredecessors(j, end, arms);
            }
        }
    }

    bool canMoveAboveAllocation(const Block& block, size_t index, bool hasNullPrototypeAtCreation) const
    {
        switch (m_function.at(block.values[index]).opcode) {
        case Opcode::Await:
        case Opcode::Yield:
        case Opcode::GetResumeMode:
        case Opcode::LoopHint:
        case Opcode::CatchValue:
            return false;
        case Opcode::CallDirect:
            return hasNullPrototypeAtCreation;
        default:
            return hasNullPrototypeAtCreation || writesOf(m_program, m_function, block, index) <= Writes::Fields;
        }
    }

    bool mayStoreDirectlyToObject(ValueID object, const Value& store) const
    {
        LayoutID created = m_function.at(object).layout();
        auto& layout = m_program.at(created);
        auto field = layout.fieldNamed(store.string());
        if (!isObjectLayout(layout.kind) || !field || mayInheritAtCreation(m_program, m_function, object, store.string()))
            return false;
        TypeID type = layout.fields[*field].type;
        return !type || m_program.typeOf(m_function, store.child(1)).isSubsetOf(m_program.at(type), m_program);
    }

    void gatherInitializingStores()
    {
        Vector<uint32_t> numberOfStores;
        Vector<uint32_t> numberOfInitialStoresThroughUpcasts;
        for (auto& block : m_function.blocks) {
            bool endsInInvoke = block.terminator.kind == TerminatorKind::Invoke;
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::PutField && value.opcode != Opcode::PutById)
                    continue;
                ValueID into = value.child(0);
                bool isThroughUpcast = m_function.at(into).opcode == Opcode::Cast && m_function.at(into).castReason() == CastReason::Upcast;
                if (isThroughUpcast)
                    into = m_function.at(into).child(0);
                if (m_function.at(into).opcode != Opcode::NewObjectWithShape)
                    continue;
                if (numberOfStores.isEmpty()) {
                    numberOfStores.fill(0, m_function.values.size());
                    numberOfInitialStoresThroughUpcasts.fill(0, m_function.values.size());
                }
                ++numberOfStores[into.index()];
                if (isThroughUpcast && value.opcode == Opcode::PutById && value.isInitialStore())
                    ++numberOfInitialStoresThroughUpcasts[into.index()];
            }
            if (numberOfStores.isEmpty())
                continue;
            for (size_t i = 0; i < block.values.size(); ++i) {
                ValueID object = block.values[i];
                if (m_function.at(object).opcode != Opcode::NewObjectWithShape || !numberOfStores[object.index()])
                    continue;
                auto isUpcastOfObject = [&](ValueID id) {
                    auto& value = m_function.at(id);
                    return value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast && value.child(0) == object;
                };
                bool initialStoresMayGoOver = true;
                uint32_t left = numberOfInitialStoresThroughUpcasts[object.index()];
                for (size_t next = i + 1; left && next < block.values.size(); ++next) {
                    auto& value = m_function.at(block.values[next]);
                    if (value.opcode == Opcode::PutById && value.isInitialStore() && isUpcastOfObject(value.child(0))) {
                        initialStoresMayGoOver &= mayStoreDirectlyToObject(object, value);
                        --left;
                    }
                }
                left = numberOfInitialStoresThroughUpcasts[object.index()];
                for (size_t next = i + 1; initialStoresMayGoOver && left && next < block.values.size(); ++next) {
                    auto& value = m_function.at(block.values[next]);
                    if (value.opcode == Opcode::PutById && value.isInitialStore() && isUpcastOfObject(value.child(0))) {
                        value.children[0] = object;
                        m_function.cleanUpMovedStores = true;
                        --left;
                    }
                }
                auto& objectLayout = m_program.at(m_function.at(object).layout());
                bool hasNullPrototypeAtCreation = objectLayout.kind == LayoutKind::Environment && !objectLayout.mayDespecialize;
                Vector<ValueID, 2> upcasts;
                size_t lastStore = i;
                size_t limit = block.values.size() - endsInInvoke;
                left = numberOfStores[object.index()];
                for (size_t next = i + 1; left && next < limit; ++next) {
                    ValueID id = block.values[next];
                    auto& value = m_function.at(id);
                    if (isUpcastOfObject(id)) {
                        upcasts.append(id);
                        continue;
                    }
                    if ((value.opcode == Opcode::PutField || value.opcode == Opcode::PutById) && (value.child(0) == object || upcasts.contains(value.child(0))))
                        --left;
                    if (value.opcode == Opcode::PutById && !value.isInitialStore() && upcasts.contains(value.child(0)) && mayStoreDirectlyToObject(object, value)) {
                        value.children[0] = object;
                        m_function.cleanUpMovedStores = true;
                    }
                    bool usesObject = false;
                    value.forEachChild([&](ValueID child) {
                        usesObject |= child == object || upcasts.contains(child);
                    });
                    if (!usesObject) {
                        if (!canMoveAboveAllocation(block, next, hasNullPrototypeAtCreation))
                            break;
                        continue;
                    }
                    if ((value.opcode != Opcode::PutField && value.opcode != Opcode::PutById) || value.child(0) != object || value.child(1) == object || upcasts.contains(value.child(1)))
                        break;
                    if (value.opcode == Opcode::PutById && !mayStoreDirectlyToObject(object, value))
                        break;
                    lastStore = next;
                }
                if (lastStore == i)
                    continue;
                Vector<ValueID, 16> above;
                Vector<ValueID, 16> stores;
                Vector<ValueID, 2> behind;
                unsigned unknownStoreCount = 0;
                for (size_t next = i + 1; next <= lastStore; ++next) {
                    ValueID id = block.values[next];
                    if (isUpcastOfObject(id))
                        behind.append(id);
                    else if (m_function.at(id).child(0) == object && (m_function.at(id).opcode == Opcode::PutField || m_function.at(id).opcode == Opcode::PutById))
                        stores.append(id);
                    else {
                        unknownStoreCount += hasNullPrototypeAtCreation && !canMoveAboveAllocation(block, next, false);
                        above.append(id);
                    }
                }
                if (unknownStoreCount && Options::verboseAOTCompilation()) [[unlikely]]
                    dataLogLn("AOT: ", m_function.name ? String::fromUTF8(m_program.string(m_function.name)) : String(), ": the allocation of an environment sinks below ", unknownStoreCount, " values that may store anywhere");
                size_t at = i;
                auto put = [&](ValueID id) {
                    m_function.cleanUpMovedStores |= block.values[at] != id;
                    block.values[at++] = id;
                };
                for (ValueID id : above)
                    put(id);
                i = at;
                put(object);
                for (ValueID id : stores)
                    put(id);
                for (ValueID id : behind)
                    put(id);
            }
        }
    }

    void foldReads()
    {
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if ((value.opcode != Opcode::GetModuleVariable && value.opcode != Opcode::GetField) || mayThrow(m_program, m_function, value))
                    continue;
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                auto& type = m_program.at(value.type);
                if (type.hasObjects())
                    continue;
                Value constant = value;
                constant.children = { };
                constant.immediates = { Format::none, Format::none };
                switch (type.primitives) {
                case Primitive::Undefined:
                    constant.opcode = Opcode::Undefined;
                    break;
                case Primitive::Null:
                    constant.opcode = Opcode::Null;
                    break;
                case Primitive::False:
                case Primitive::True:
                    constant.opcode = Opcode::Boolean;
                    constant.immediates[0] = type.primitives == Primitive::True;
                    break;
                case Primitive::Int:
                    if (type.integers.minimum != type.integers.maximum)
                        continue;
                    constant.opcode = Opcode::Integer;
                    constant.setBits(static_cast<uint64_t>(type.integers.minimum));
                    break;
                default:
                    continue;
                }
                value = WTF::move(constant);
                m_hasFolded = true;
            }
        }
    }

    void foldTypeOfComparisons()
    {
        auto definitionOf = [&](ValueID id) -> const Value& {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return m_function.at(id);
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::StrictEq && value.opcode != Opcode::Eq)
                    continue;
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                for (unsigned i = 0; i < 2; ++i) {
                    auto& typeOf = definitionOf(value.child(i));
                    auto& string = definitionOf(value.child(1 - i));
                    if (typeOf.opcode != Opcode::TypeOf || string.opcode != Opcode::String)
                        continue;
                    ValueID operand = typeOf.child(0);
                    auto wanted = typeofTypeNamed(m_program.string(string.string()));
                    value.opcode = wanted ? Opcode::TypeOfIs : Opcode::Boolean;
                    value.children = { };
                    if (wanted)
                        value.children[0] = operand;
                    value.immediates = { wanted ? static_cast<uint32_t>(*wanted) : 0, Format::none };
                    m_hasFolded = true;
                    break;
                }
            }
        }
    }

    void foldSuperConstructs()
    {
        Vector<unsigned> uses;
        for (auto& value : m_function.values) {
            if (languageOperationOf(m_program, value) != LanguageOperation::SuperConstruct || value.arguments.size() < 3 || value.arguments.size() > 4)
                continue;
            auto& list = m_function.at(value.arguments[2]);
            if (list.opcode != Opcode::NewArray || mayThrow(m_program, m_function, list))
                continue;
            if (uses.isEmpty()) {
                uses.fill(0, m_function.values.size());
                for (auto& block : m_function.blocks) {
                    for (ValueID id : block.values) {
                        std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                            ++uses[child.index()];
                        });
                    }
                    block.terminator.forEachChild([&](ValueID& child) {
                        ++uses[child.index()];
                    });
                }
            }
            if (uses[value.arguments[2].index()] != 1)
                continue;
            Arguments elements = list.arguments;
            value.opcode = Opcode::ConstructSuper;
            if (m_program.at(value.type).isAnything()) {
                value.type = m_program.types.object;
                value.rep = repOf(m_program.at(value.type));
            }
            m_hasFolded = true;
            value.children = { value.arguments[0], value.arguments[1], value.arguments.size() == 4 ? value.arguments[3] : ValueID() };
            value.immediates = { Format::none, Format::none };
            value.arguments = WTF::move(elements);
        }
    }

    void foldSpreadCalls()
    {
        Vector<unsigned> uses;
        Vector<unsigned> incomingEdges;
        Vector<BlockID> cameFrom;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (unsigned i = 0; i < block.values.size(); ++i) {
                auto& apply = m_function.at(block.values[i]);
                if (languageOperationOf(m_program, apply) != LanguageOperation::Apply || apply.arguments.size() != 3)
                    continue;
                ValueID appended = apply.arguments[2];
                auto& append = m_function.at(appended);
                if (languageOperationOf(m_program, append) != LanguageOperation::AppendSpread || append.arguments.size() != 2)
                    continue;
                auto& target = m_function.at(append.arguments[0]);
                if (target.opcode != Opcode::NewArray || !target.arguments.isEmpty())
                    continue;
                if (uses.isEmpty()) {
                    uses.fill(0, m_function.values.size());
                    incomingEdges.fill(0, m_function.blocks.size());
                    cameFrom.fill(BlockID(), m_function.blocks.size());
                    for (unsigned from = 0; from < m_function.blocks.size(); ++from) {
                        auto& other = m_function.blocks[from];
                        for (ValueID id : other.values) {
                            std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                                ++uses[child.index()];
                            });
                        }
                        other.terminator.forEachChild([&](ValueID& value) {
                            ++uses[value.index()];
                        });
                        for (auto& to : other.terminator.targets) {
                            ++incomingEdges[to.block.index()];
                            cameFrom[to.block.index()] = BlockID(from);
                        }
                    }
                }
                if (uses[appended.index()] != 1 || uses[append.arguments[0].index()] != 1)
                    continue;
                auto& before = i ? block : m_function.at(incomingEdges[b] == 1 ? cameFrom[b] : BlockID(b));
                bool isWatched = !i && before.terminator.kind == TerminatorKind::Invoke && before.terminator.targets[0].block == BlockID(b) && !before.values.isEmpty();
                if ((i ? before.values[i - 1] : isWatched ? before.values.last() : ValueID()) != appended)
                    continue;
                apply.opcode = Opcode::CallBuiltin;
                apply.immediates = { Operations::CallWithSpread.index(), Format::none };
                m_hasFolded = true;
                apply.arguments[2] = append.arguments[1];
                if (isWatched) {
                    before.terminator.kind = TerminatorKind::Jump;
                    before.terminator.targets.shrink(1);
                    before.values.removeLast();
                } else
                    before.values.removeAt(--i);
            }
        }
    }

    void gatherLiteralFields()
    {
        Vector<unsigned> literalBlock;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                if (m_function.at(id).opcode != Opcode::NewObject || m_function.at(id).hasInitialStructure())
                    continue;
                if (literalBlock.isEmpty())
                    literalBlock.fill(noBlock, m_function.values.size());
                literalBlock[id.index()] = b;
            }
        }
        if (literalBlock.isEmpty())
            return;
        auto isFieldOf = [&](const Value& value, ValueID object) {
            return languageOperationOf(m_program, value) == LanguageOperation::DefineField && value.arguments.size() == 3 && value.arguments[0] == object && value.arguments[1] != object && value.arguments[2] != object && m_function.at(value.arguments[1]).opcode == Opcode::String;
        };
        Vector<ValueID, 4> candidates;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (ValueID id : m_function.blocks[b].values) {
                auto& value = m_function.at(id);
                if ((value.opcode != Opcode::Operation && value.opcode != Opcode::CallBuiltin) || value.arguments.isEmpty())
                    continue;
                ValueID object = value.arguments[0];
                if (literalBlock[object.index()] == noBlock || literalBlock[object.index()] == b || candidates.contains(object))
                    continue;
                auto operation = languageOperationOf(m_program, value);
                if (operation == LanguageOperation::CopyDataProperties)
                    candidates.append(object);
            }
        }
        if (candidates.isEmpty())
            return;
        Vector<unsigned> dominators = immediateDominatorsOf(m_function);
        auto dominates = [&](unsigned a, unsigned b) {
            for (unsigned at = b;; at = dominators[at]) {
                if (at == a)
                    return true;
                if (dominators[at] == at)
                    return false;
            }
        };
        struct Use {
            unsigned block;
            size_t index;
            ValueID user;
        };
        auto precedes = [&](const Use& a, const Use& b) {
            return a.block == b.block ? a.index < b.index : dominates(a.block, b.block);
        };
        for (ValueID object : candidates) {
            unsigned home = literalBlock[object.index()];
            Vector<Use, 8> fields;
            Vector<Use, 8> others;
            for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
                auto& block = m_function.blocks[b];
                for (size_t i = 0; i < block.values.size(); ++i) {
                    auto& value = m_function.at(block.values[i]);
                    bool uses = false;
                    std::as_const(value).forEachChild([&](ValueID child) {
                        uses |= child == object;
                    });
                    if (uses)
                        (isFieldOf(value, object) ? fields : others).append(Use { b, i, block.values[i] });
                }
                bool escapesAnalysis = false;
                block.terminator.forEachChild([&](ValueID& child) {
                    escapesAnalysis |= child == object;
                });
                if (escapesAnalysis)
                    others.append(Use { b, block.values.size(), ValueID() });
            }
            if (others.isEmpty())
                continue;
            const Use* first = &others[0];
            for (auto& use : others) {
                if (precedes(use, *first))
                    first = &use;
            }
            bool isSettled = std::ranges::all_of(others, [&](const Use& use) {
                return &use == first || precedes(*first, use);
            });
            if (!isSettled || !dominates(home, first->block) || !first->user)
                continue;
            if (auto& spread = m_function.at(first->user); languageOperationOf(m_program, spread) != LanguageOperation::CopyDataProperties || spread.arguments.size() != 2 || spread.arguments[0] != object || spread.arguments[1] == object)
                continue;
            Vector<Use, 8> moving;
            for (auto& field : fields) {
                if (precedes(field, *first))
                    moving.append(field);
                else
                    isSettled &= precedes(*first, field);
                auto& block = m_function.blocks[field.block];
                isSettled &= block.terminator.kind != TerminatorKind::Invoke || field.index + 1 != block.values.size() || !precedes(field, *first);
            }
            bool isContiguous = home == first->block && std::ranges::all_of(moving, [&](const Use& field) {
                return field.block == home;
            });
            if (!isSettled || isContiguous)
                continue;
            BitVector isSeen(m_function.blocks.size());
            Vector<unsigned, 16> worklist { first->block };
            bool loopsBack = false;
            while (!worklist.isEmpty() && !loopsBack) {
                for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                    unsigned next = target.block.index();
                    loopsBack |= next == first->block;
                    if (next != home && !isSeen.set(next))
                        worklist.append(next);
                }
            }
            if (loopsBack)
                continue;
            std::ranges::sort(moving, precedes);
            Vector<ValueID, 8> moved { object };
            for (auto& field : moving)
                moved.append(field.user);
            unsigned target = first->block;
            ValueID user = first->user;
            for (auto& block : m_function.blocks) {
                block.values.removeAllMatching([&](ValueID id) {
                    return moved.contains(id);
                });
            }
            auto& values = m_function.blocks[target].values;
            size_t at = values.find(user);
            RELEASE_ASSERT(at != notFound);
            values.insertVector(at, moved);
            literalBlock[object.index()] = target;
        }
    }

    void foldInvokes()
    {
        Vector<unsigned> singlePredecessor;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Invoke)
                continue;
            if (!block.values.isEmpty() && mayThrow(m_program, m_function, m_function.at(block.values.last()))) {
                bool isField = languageOperationOf(m_program, m_function.at(block.values.last())) == LanguageOperation::DefineField;
                if (m_function.at(block.values.last()).opcode != Opcode::PutById && !isField)
                    continue;
                if (singlePredecessor.isEmpty())
                    singlePredecessor = findSinglePredecessors();
                if (!(isField ? endsWithFieldOfUnescapedLiteral(b, singlePredecessor) : endsWithStoreToUnescapedObject(b, singlePredecessor)))
                    continue;
            }
            block.terminator.kind = TerminatorKind::Jump;
            block.terminator.targets.shrink(1);
        }
    }

    static constexpr unsigned noBlock = UINT_MAX;

    Vector<unsigned> findSinglePredecessors() const
    {
        static constexpr unsigned several = noBlock - 1;
        Vector<unsigned> result;
        result.fill(noBlock, m_function.blocks.size());
        result[0] = several;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (auto& target : m_function.blocks[b].terminator.targets) {
                unsigned& from = result[target.block.index()];
                from = from == noBlock ? b : several;
            }
        }
        for (unsigned& from : result) {
            if (from == several)
                from = noBlock;
        }
        return result;
    }

    bool endsWithStoreToUnescapedObject(unsigned block, const Vector<unsigned>& singlePredecessor) const
    {
        auto withoutUpcasts = [&](ValueID id) {
            while (id && m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return id;
        };
        ValueID object = withoutUpcasts(m_function.at(m_function.blocks[block].values.last()).child(0));
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        auto keepsObjectUnescaped = [&](const Value& value) {
            bool isOperand = false;
            value.forEachChild([&](ValueID child) {
                isOperand |= withoutUpcasts(child) == object;
            });
            if (!isOperand || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast))
                return true;
            if (withoutUpcasts(value.child(0)) != object || withoutUpcasts(value.child(1)) == object)
                return false;
            return value.opcode == Opcode::PutField || (value.opcode == Opcode::PutById && mayStoreDirectlyToObject(object, value));
        };
        unsigned at = block;
        for (unsigned steps = 0; steps < m_function.blocks.size(); ++steps) {
            auto& values = m_function.blocks[at].values;
            for (size_t i = values.size(); i--;) {
                if (values[i] == object)
                    return true;
                if (!keepsObjectUnescaped(m_function.at(values[i])))
                    return false;
            }
            at = singlePredecessor[at];
            if (at == noBlock)
                return false;
            bool escapesAnalysis = false;
            Terminator terminator = m_function.blocks[at].terminator;
            terminator.forEachChild([&](ValueID& child) {
                escapesAnalysis |= withoutUpcasts(child) == object;
            });
            if (escapesAnalysis)
                return false;
        }
        return false;
    }

    bool endsWithFieldOfUnescapedLiteral(unsigned block, const Vector<unsigned>& singlePredecessor) const
    {
        auto withoutUpcasts = [&](ValueID id) {
            while (id && m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return id;
        };
        auto& field = m_function.at(m_function.blocks[block].values.last());
        if (field.arguments.size() != 3 || m_function.at(field.arguments[1]).opcode != Opcode::String)
            return false;
        ValueID object = withoutUpcasts(field.arguments[0]);
        if (m_function.at(object).opcode != Opcode::NewObject)
            return false;
        auto keepsObjectUnescaped = [&](const Value& value) {
            unsigned uses = 0;
            value.forEachChild([&](ValueID child) {
                uses += withoutUpcasts(child) == object;
            });
            if (!uses || (value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast))
                return true;
            auto operation = languageOperationOf(m_program, value);
            bool addsPlainProperties = (operation == LanguageOperation::DefineField && value.arguments.size() == 3 && m_function.at(value.arguments[1]).opcode == Opcode::String) || (operation == LanguageOperation::CopyDataProperties && value.arguments.size() == 2);
            return addsPlainProperties && uses == 1 && withoutUpcasts(value.arguments[0]) == object;
        };
        unsigned at = block;
        for (unsigned steps = 0; steps < m_function.blocks.size(); ++steps) {
            auto& values = m_function.blocks[at].values;
            for (size_t i = values.size(); i--;) {
                if (values[i] == object)
                    return true;
                if (!keepsObjectUnescaped(m_function.at(values[i])))
                    return false;
            }
            at = singlePredecessor[at];
            if (at == noBlock)
                return false;
            bool escapesAnalysis = false;
            Terminator terminator = m_function.blocks[at].terminator;
            terminator.forEachChild([&](ValueID& child) {
                escapesAnalysis |= withoutUpcasts(child) == object;
            });
            if (escapesAnalysis)
                return false;
        }
        return false;
    }

    void foldConstantConditions()
    {
        auto definitionOf = [&](ValueID id) -> const Value& {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return m_function.at(id);
        };
        auto isAllocation = [](const Value& value) {
            return value.opcode == Opcode::NewObject || value.opcode == Opcode::NewObjectWithShape || value.opcode == Opcode::NewArray || value.opcode == Opcode::NewClosure || value.opcode == Opcode::NewClosureOnce;
        };
        auto isNever = [&](ValueID id, uint16_t primitives) {
            while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
                id = m_function.at(id).child(0);
            return isAllocation(m_function.at(id)) || !(m_program.typeOf(m_function, id).primitives & primitives);
        };
        auto isDoubleArrayCheck = [](const Value& value) {
            auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
            return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::IsDoubleArray && value.arguments.size() == 1;
        };
        auto mayContainNumbers = [&](ValueID array) {
            for (;; array = m_function.at(array).child(0)) {
                TypeID elements = m_program.typeOf(m_function, array).elements;
                if (elements && !(m_program.at(elements).primitives & Primitive::Number))
                    return false;
                if (m_function.at(array).opcode != Opcode::Cast)
                    return true;
            }
        };
        static constexpr OpcodeSet foldableOpcodes { Opcode::Operation, Opcode::CallBuiltin, Opcode::StrictEq, Opcode::Eq, Opcode::LogicalNot, Opcode::ToBoolean, Opcode::IsUndefinedOrNull, Opcode::HasShapeOf, Opcode::TypeOfIs, Opcode::IsClosureOf };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (!foldableOpcodes.contains(value.opcode))
                    continue;
                if (block.terminator.kind == TerminatorKind::Invoke && id == block.values.last())
                    continue;
                if (isIteratorResultCheckOfObject(m_program, m_function, value) || isSpreadOfNonObjectConstant(m_program, m_function, value)) {
                    value.opcode = Opcode::Undefined;
                    value.arguments.clear();
                    value.immediates = { Format::none, Format::none };
                    value.type = m_program.types.undefined;
                    value.rep = repOf(m_program.at(value.type));
                    continue;
                }
                std::optional<bool> result;
                switch (value.opcode) {
                case Opcode::StrictEq:
                case Opcode::Eq:
                    result = areEqualConstants(m_program, definitionOf(value.child(0)), definitionOf(value.child(1)), value.opcode == Opcode::StrictEq);
                    for (unsigned side = 0; side < 2 && !result && value.opcode == Opcode::StrictEq; ++side) {
                        Opcode constant = definitionOf(value.child(side)).opcode;
                        if ((constant == Opcode::Undefined && isNever(value.child(1 - side), Primitive::Undefined)) || (constant == Opcode::Null && isNever(value.child(1 - side), Primitive::Null)))
                            result = false;
                    }
                    break;
                case Opcode::LogicalNot:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Boolean)
                        result = !operand.boolean();
                    break;
                case Opcode::ToBoolean:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Boolean)
                        result = operand.boolean();
                    else if (operand.opcode == Opcode::Integer)
                        result = !!operand.integer();
                    else if (operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null)
                        result = false;
                    else if (isAllocation(operand))
                        result = true;
                    break;
                case Opcode::IsUndefinedOrNull:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null)
                        result = true;
                    else if (isNever(value.child(0), Primitive::UndefinedOrNull))
                        result = false;
                    break;
                case Opcode::HasShapeOf:
                    if (auto& type = m_program.at(definitionOf(value.child(0)).type); !value.immediates[1] && type.singleLayout() == value.layout() && !type.mayBeUndefinedOrNull() && !type.acceptsUntypedObjects(m_program) && type.isSubsetOf(m_program.at(m_program.at(value.layout()).type), m_program, false))
                        result = true;
                    break;
                case Opcode::TypeOfIs:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::NewClosure || operand.opcode == Opcode::NewClosureOnce || operand.opcode == Opcode::FunctionReference)
                        result = value.typeofType() == TypeofType::Function;
                    else if (operand.opcode == Opcode::NewObject || operand.opcode == Opcode::NewArray)
                        result = value.typeofType() == TypeofType::Object;
                    break;
                case Opcode::IsClosureOf:
                    if (auto& operand = definitionOf(value.child(0)); operand.opcode == Opcode::NewClosure || operand.opcode == Opcode::NewClosureOnce || operand.opcode == Opcode::FunctionReference) {
                        if (operand.function() != value.function())
                            result = false;
                        else if (value.immediates[1] != closureIsUnmodified && value.immediates[1] != closureIsConstructingCallee)
                            result = true;
                    }
                    break;
                case Opcode::CallBuiltin:
                    if (isDoubleArrayCheck(value) && !mayContainNumbers(value.arguments[0]))
                        result = false;
                    break;
                default:
                    break;
                }
                if (!result)
                    continue;
                value.opcode = Opcode::Boolean;
                value.arguments.clear();
                value.children = { };
                value.immediates = { *result, Format::none };
            }
        }
    }

    void foldBranches()
    {
        for (auto& block : m_function.blocks) {
            auto& terminator = block.terminator;
            if (terminator.kind != TerminatorKind::Branch || m_function.at(terminator.value).opcode != Opcode::Boolean)
                continue;
            Target taken = WTF::move(terminator.targets[m_function.at(terminator.value).boolean() ? 0 : 1]);
            terminator = Terminator { };
            terminator.kind = TerminatorKind::Jump;
            terminator.targets.append(WTF::move(taken));
        }
    }

    bool isOneComparison(ValueID id) const
    {
        auto& value = m_function.at(id);
        auto isNumber = [&](unsigned child) {
            Rep rep = m_function.at(value.child(child)).rep;
            return rep == Rep::Int32 || rep == Rep::Int64 || rep == Rep::Double || rep == Rep::OptionalDouble;
        };
        switch (value.opcode) {
        case Opcode::LogicalNot:
            return isOneComparison(value.child(0));
        case Opcode::IsUndefinedOrNull:
            return true;
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
            return isNumber(0) && isNumber(1);
        case Opcode::StrictEq: {
            if (isNumber(0) && isNumber(1))
                return true;
            bool areCellsComparedByAddress = true;
            for (unsigned i = 0; i < 2; ++i) {
                auto& operand = m_function.at(value.child(i));
                if (operand.opcode == Opcode::Undefined || operand.opcode == Opcode::Null)
                    return true;
                if (operand.opcode == Opcode::String) {
                    auto text = m_program.string(operand.string());
                    return !text.empty() && text.size() <= maxInlineStringCompareLength && charactersAreAllASCII(byteCast<Latin1Character>(text));
                }
                areCellsComparedByAddress &= operand.rep == Rep::Cell && !(m_program.at(operand.type).primitives & (Primitive::String | Primitive::BigInt));
            }
            return areCellsComparedByAddress;
        }
        default:
            return false;
        }
    }

    bool propagateBranchConditions()
    {
        Vector<unsigned> predecessorCount;
        predecessorCount.fill(0, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++predecessorCount[target.block.index()];
        }
        bool changed = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (m_function.blocks[b].terminator.kind != TerminatorKind::Branch)
                continue;
            ValueID condition = m_function.blocks[b].terminator.value;
            if (m_function.at(condition).opcode == Opcode::Boolean || !m_program.at(m_function.at(condition).type).hasOnlyPrimitives(Primitive::True | Primitive::False))
                continue;
            bool isOnlyCreatedForOtherUsers = isOneComparison(condition);
            std::array<BlockID, 2> targets { m_function.blocks[b].terminator.targets[0].block, m_function.blocks[b].terminator.targets[1].block };
            if (targets[0] == targets[1])
                continue;
            for (unsigned i = 0; i < 2; ++i) {
                unsigned target = targets[i].index();
                if (!target || predecessorCount[target] != 1)
                    continue;
                bool isBranchedOnAgain = m_function.blocks[target].terminator.kind == TerminatorKind::Branch && m_function.blocks[target].terminator.value == condition;
                bool isUsed = isBranchedOnAgain;
                if (isOnlyCreatedForOtherUsers) {
                    for (ValueID id : m_function.blocks[target].values) {
                        std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                            isUsed |= child == condition;
                        });
                    }
                    m_function.blocks[target].terminator.forEachChild([&](ValueID& child) {
                        isUsed |= child == condition;
                    });
                }
                if (!isUsed)
                    continue;
                Value constant;
                constant.opcode = Opcode::Boolean;
                constant.rep = m_function.at(condition).rep;
                constant.position = m_function.at(condition).position;
                constant.inlineFrame = m_function.at(condition).inlineFrame;
                constant.type = m_function.at(condition).type;
                constant.immediates = { !i, Format::none };
                ValueID found(m_function.values.size());
                m_function.values.append(WTF::move(constant));
                auto replace = [&](ValueID& child) {
                    if (child == condition)
                        child = found;
                };
                if (isOnlyCreatedForOtherUsers) {
                    for (ValueID id : m_function.blocks[target].values)
                        m_function.at(id).forEachChild(replace);
                    m_function.blocks[target].terminator.forEachChild(replace);
                } else
                    m_function.blocks[target].terminator.value = found;
                m_function.blocks[target].values.insert(0, found);
                changed |= isBranchedOnAgain;
            }
        }
        return changed;
    }

    bool mergeBooleanForwardingBlocks()
    {
        struct Candidate {
            uint32_t target;
            uint32_t booleans;
            uint32_t block;
        };
        Vector<Candidate> candidates;
        for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Jump || !block.parameters.isEmpty() || block.values.isEmpty())
                continue;
            auto& arguments = block.terminator.targets[0].arguments;
            if (arguments.isEmpty() || arguments.size() > 32 || block.terminator.targets[0].block.index() == b)
                continue;
            bool containsOnlyBooleans = std::ranges::all_of(block.values, [&](ValueID id) {
                return m_function.at(id).opcode == Opcode::Boolean;
            });
            uint32_t booleans = 0;
            for (unsigned k = 0; k < arguments.size(); ++k) {
                containsOnlyBooleans &= block.values.contains(arguments[k]);
                booleans |= static_cast<uint32_t>(containsOnlyBooleans && m_function.at(arguments[k]).boolean()) << k;
            }
            if (containsOnlyBooleans)
                candidates.append({ block.terminator.targets[0].block.index(), booleans, b });
        }
        if (candidates.size() < 2)
            return false;
        std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
            return std::tie(a.target, a.booleans, a.block) < std::tie(b.target, b.booleans, b.block);
        });
        Vector<uint32_t> retainedFor(m_function.blocks.size(), [](size_t b) {
            return static_cast<uint32_t>(b);
        });
        bool changed = false;
        for (size_t i = 1; i < candidates.size(); ++i) {
            if (candidates[i].target != candidates[i - 1].target || candidates[i].booleans != candidates[i - 1].booleans)
                continue;
            retainedFor[candidates[i].block] = retainedFor[candidates[i - 1].block];
            changed = true;
        }
        if (!changed)
            return false;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                target.block = BlockID(retainedFor[target.block.index()]);
        }
        return true;
    }

    bool sinkToBooleanIntoPredecessors()
    {
        auto testedParameter = [&](const Block& join) -> size_t {
            if (join.terminator.kind != TerminatorKind::Branch || join.values.size() != 1 || join.terminator.value != join.values[0])
                return notFound;
            auto& test = m_function.at(join.values[0]);
            return test.opcode == Opcode::ToBoolean ? join.parameters.find(test.child(0)) : notFound;
        };
        auto isBoolean = [&](ValueID id) {
            return m_program.at(m_function.at(id).type).isBoolean();
        };
        unsigned numberOfBlocks = m_function.blocks.size();
        BitVector testsParameter(numberOfBlocks);
        for (unsigned j = 1; j < numberOfBlocks; ++j) {
            if (testedParameter(m_function.blocks[j]) != notFound)
                testsParameter.set(j);
        }
        if (testsParameter.isEmpty())
            return false;
        BitVector isIncomingBoolean(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            for (auto& target : terminator.targets) {
                unsigned j = target.block.index();
                if (!testsParameter.get(j))
                    continue;
                if (terminator.kind != TerminatorKind::Jump || b == j)
                    testsParameter.clear(j);
                else if (isBoolean(target.arguments[testedParameter(m_function.blocks[j])]))
                    isIncomingBoolean.set(j);
            }
        }
        testsParameter.filter(isIncomingBoolean);
        if (testsParameter.isEmpty())
            return false;
        Vector<unsigned> numberOfUses;
        numberOfUses.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    ++numberOfUses[child.index()];
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                ++numberOfUses[child.index()];
            });
        }
        bool changed = false;
        for (size_t j : testsParameter) {
            size_t index = testedParameter(m_function.blocks[j]);
            ValueID parameter = m_function.blocks[j].parameters[index];
            ValueID test = m_function.blocks[j].values[0];
            if (numberOfUses[parameter.index()] != 1 || numberOfUses[test.index()] != 1)
                continue;
            for (unsigned b = 0; b < numberOfBlocks; ++b) {
                if (m_function.blocks[b].terminator.kind != TerminatorKind::Jump || m_function.blocks[b].terminator.targets[0].block.index() != j)
                    continue;
                ValueID brought = m_function.blocks[b].terminator.targets[0].arguments[index];
                if (isBoolean(brought))
                    continue;
                Value asked = m_function.at(test);
                asked.children[0] = brought;
                ValueID answer(m_function.values.size());
                m_function.values.append(WTF::move(asked));
                m_function.blocks[b].values.append(answer);
                m_function.blocks[b].terminator.targets[0].arguments[index] = answer;
            }
            auto& joined = m_function.at(parameter);
            joined.type = m_function.at(test).type;
            joined.rep = m_function.at(test).rep;
            joined.setPrediction({ });
            joined.setPredictedClosure({ });
            m_function.blocks[j].values.clear();
            m_function.blocks[j].terminator.value = parameter;
            changed = true;
        }
        return changed;
    }

    bool sinkComparisonIntoPredecessors()
    {
        auto isComparison = [&](const Block& join) {
            if (join.values.isEmpty() || join.parameters.isEmpty() || (join.values.size() == 1 && join.terminator.kind == TerminatorKind::Invoke))
                return false;
            switch (m_function.at(join.values[0]).opcode) {
            case Opcode::Less:
            case Opcode::LessEq:
            case Opcode::Greater:
            case Opcode::GreaterEq:
                return true;
            default:
                return false;
            }
        };
        auto isNumber = [&](ValueID id) {
            return m_program.at(m_function.at(id).type).isNumber();
        };
        auto incomingOperand = [&](const Block& join) -> std::optional<unsigned> {
            auto& comparison = m_function.at(join.values[0]);
            bool isFirst = join.parameters.contains(comparison.child(0));
            if (isFirst == join.parameters.contains(comparison.child(1)))
                return std::nullopt;
            unsigned operand = isFirst ? 0 : 1;
            return isNumber(comparison.child(operand)) ? std::nullopt : std::optional(operand);
        };
        unsigned numberOfBlocks = m_function.blocks.size();
        BitVector comparesParameter(numberOfBlocks);
        for (unsigned j = 1; j < numberOfBlocks; ++j) {
            if (isComparison(m_function.blocks[j]) && incomingOperand(m_function.blocks[j]))
                comparesParameter.set(j);
        }
        if (comparesParameter.isEmpty())
            return false;
        BitVector isIncomingNumber(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            for (auto& target : terminator.targets) {
                unsigned j = target.block.index();
                if (!comparesParameter.get(j))
                    continue;
                if (terminator.kind != TerminatorKind::Jump || b == j) {
                    comparesParameter.clear(j);
                    continue;
                }
                auto& join = m_function.blocks[j];
                if (isNumber(target.arguments[join.parameters.find(m_function.at(join.values[0]).child(*incomingOperand(join)))]))
                    isIncomingNumber.set(j);
            }
        }
        comparesParameter.filter(isIncomingNumber);
        if (comparesParameter.isEmpty())
            return false;
        Vector<unsigned> numberOfUses;
        numberOfUses.fill(0, m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    ++numberOfUses[child.index()];
                });
            }
            block.terminator.forEachChild([&](ValueID& child) {
                ++numberOfUses[child.index()];
            });
        }
        bool changed = false;
        for (size_t j : comparesParameter) {
            ValueID comparison = m_function.blocks[j].values[0];
            unsigned operand = *incomingOperand(m_function.blocks[j]);
            size_t index = m_function.blocks[j].parameters.find(m_function.at(comparison).child(operand));
            if (numberOfUses[m_function.blocks[j].parameters[index].index()] != 1)
                continue;
            for (unsigned b = 0; b < numberOfBlocks; ++b) {
                if (m_function.blocks[b].terminator.kind != TerminatorKind::Jump || m_function.blocks[b].terminator.targets[0].block.index() != j)
                    continue;
                Value onThisPath = m_function.at(comparison);
                onThisPath.children[operand] = m_function.blocks[b].terminator.targets[0].arguments[index];
                ValueID answer(m_function.values.size());
                m_function.values.append(WTF::move(onThisPath));
                m_function.blocks[b].values.append(answer);
                m_function.blocks[b].terminator.targets[0].arguments[index] = answer;
            }
            auto& answered = m_function.at(comparison);
            Value parameter;
            parameter.opcode = Opcode::Parameter;
            parameter.position = answered.position;
            parameter.inlineFrame = answered.inlineFrame;
            parameter.type = answered.type;
            parameter.rep = answered.rep;
            parameter.immediates = { static_cast<uint32_t>(j), static_cast<uint32_t>(index) };
            answered = WTF::move(parameter);
            m_function.blocks[j].parameters[index] = comparison;
            m_function.blocks[j].values.removeAt(0);
            changed = true;
        }
        return changed;
    }

    bool threadBranchesOnParameters()
    {
        auto matchesPattern = [&](const Block& block) {
            return block.terminator.kind == TerminatorKind::Branch && block.values.isEmpty() && block.parameters.contains(block.terminator.value);
        };
        bool mayThread = false;
        for (unsigned j = 1; j < m_function.blocks.size() && !mayThread; ++j)
            mayThread = matchesPattern(m_function.blocks[j]);
        if (!mayThread)
            return false;
        Vector<unsigned> numberOfUses;
        numberOfUses.fill(0, m_function.values.size());
        Vector<Vector<unsigned, 2>> incomingEdges(m_function.blocks.size());
        auto adjustUses = [&](Terminator& terminator, int difference) {
            terminator.forEachChild([&](ValueID& child) {
                numberOfUses[child.index()] += difference;
            });
        };
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values) {
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    ++numberOfUses[child.index()];
                });
            }
            adjustUses(block.terminator, 1);
            if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch)
                continue;
            for (auto& target : block.terminator.targets)
                incomingEdges[target.block.index()].append(b);
        }
        bool changed = false;
        for (unsigned j = 1; j < m_function.blocks.size(); ++j) {
            auto& join = m_function.blocks[j];
            if (!matchesPattern(join))
                continue;
            size_t tested = join.parameters.find(join.terminator.value);
            if (tested == notFound || numberOfUses[join.terminator.value.index()] != 1)
                continue;
            if (join.terminator.targets[0].block.index() == j || join.terminator.targets[1].block.index() == j)
                continue;
            bool isUsedOutside = false;
            for (ValueID parameter : join.parameters) {
                unsigned here = 0;
                join.terminator.forEachChild([&](ValueID& child) {
                    here += child == parameter;
                });
                isUsedOutside |= numberOfUses[parameter.index()] != here;
            }
            if (isUsedOutside)
                continue;
            auto through = [&](const Target& to, const Arguments& brought) {
                Target result;
                result.block = to.block;
                for (ValueID argument : to.arguments) {
                    size_t index = join.parameters.find(argument);
                    result.arguments.append(index == notFound ? argument : brought[index]);
                }
                return result;
            };
            for (unsigned way = 0; way < incomingEdges[j].size(); ++way) {
                unsigned b = incomingEdges[j][way];
                auto& terminator = m_function.blocks[b].terminator;
                if (b == j)
                    continue;
                for (auto& target : terminator.targets) {
                    if (target.block.index() != j)
                        continue;
                    Arguments brought = target.arguments;
                    auto& given = m_function.at(brought[tested]);
                    if (given.opcode == Opcode::Boolean) {
                        adjustUses(terminator, -1);
                        target = through(join.terminator.targets[given.boolean() ? 0 : 1], brought);
                        adjustUses(terminator, 1);
                        incomingEdges[target.block.index()].append(b);
                        changed = true;
                        continue;
                    }
                    if (terminator.kind != TerminatorKind::Jump)
                        continue;
                    Terminator threaded;
                    threaded.kind = TerminatorKind::Branch;
                    threaded.value = brought[tested];
                    threaded.targets.append(through(join.terminator.targets[0], brought));
                    threaded.targets.append(through(join.terminator.targets[1], brought));
                    adjustUses(terminator, -1);
                    terminator = WTF::move(threaded);
                    adjustUses(terminator, 1);
                    incomingEdges[terminator.targets[0].block.index()].append(b);
                    incomingEdges[terminator.targets[1].block.index()].append(b);
                    changed = true;
                    break;
                }
            }
        }
        return changed;
    }

    struct KnownValue {
        enum Kind : uint8_t { Unknown, Undefined, Null, Boolean, Integer, String };
        Kind kind { Unknown };
        int64_t number { 0 };
    };

    KnownValue constantKnownValue(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::Undefined:
            return { KnownValue::Undefined, 0 };
        case Opcode::Null:
            return { KnownValue::Null, 0 };
        case Opcode::Boolean:
            return { KnownValue::Boolean, value.boolean() };
        case Opcode::Integer:
            return { KnownValue::Integer, value.integer() };
        case Opcode::String:
            return { KnownValue::String, value.string().index() };
        default:
            return { };
        }
    }

    bool mayContainTest(const Block& block) const
    {
        if (block.terminator.kind != TerminatorKind::Branch || block.parameters.isEmpty() || block.values.isEmpty() || block.values.size() > 8)
            return false;
        for (ValueID id : block.values) {
            switch (m_function.at(id).opcode) {
            case Opcode::Undefined:
            case Opcode::Null:
            case Opcode::Boolean:
            case Opcode::Integer:
            case Opcode::String:
            case Opcode::ToBoolean:
            case Opcode::LogicalNot:
            case Opcode::IsUndefinedOrNull:
            case Opcode::StrictEq:
            case Opcode::BitAnd:
            case Opcode::Cast:
                break;
            default:
                return false;
            }
        }
        return true;
    }

    KnownValue evaluateTest(const Block& join, const Arguments& brought) const
    {
        Vector<std::pair<ValueID, KnownValue>, 12> known;
        for (unsigned i = 0; i < join.parameters.size(); ++i)
            known.append({ join.parameters[i], constantKnownValue(m_function.at(brought[i])) });
        auto lookUp = [&](ValueID id) -> KnownValue {
            for (auto& [value, what] : known) {
                if (value == id)
                    return what;
            }
            return constantKnownValue(m_function.at(id));
        };
        auto boolean = [](bool value) {
            return KnownValue { KnownValue::Boolean, value };
        };
        for (ValueID id : join.values) {
            auto& value = m_function.at(id);
            KnownValue result = constantKnownValue(value);
            switch (value.opcode) {
            case Opcode::ToBoolean: {
                KnownValue operand = lookUp(value.child(0));
                switch (operand.kind) {
                case KnownValue::Unknown:
                    break;
                case KnownValue::Undefined:
                case KnownValue::Null:
                    result = boolean(false);
                    break;
                case KnownValue::Boolean:
                case KnownValue::Integer:
                    result = boolean(!!operand.number);
                    break;
                case KnownValue::String:
                    result = boolean(!m_program.string(StringID(static_cast<uint32_t>(operand.number))).empty());
                    break;
                }
                break;
            }
            case Opcode::LogicalNot: {
                KnownValue operand = lookUp(value.child(0));
                if (operand.kind == KnownValue::Boolean)
                    result = boolean(!operand.number);
                break;
            }
            case Opcode::IsUndefinedOrNull: {
                KnownValue operand = lookUp(value.child(0));
                if (operand.kind != KnownValue::Unknown)
                    result = boolean(operand.kind == KnownValue::Undefined || operand.kind == KnownValue::Null);
                break;
            }
            case Opcode::StrictEq: {
                KnownValue left = lookUp(value.child(0));
                KnownValue right = lookUp(value.child(1));
                if (left.kind == KnownValue::String && right.kind == KnownValue::String)
                    result = boolean(std::ranges::equal(m_program.string(StringID(static_cast<uint32_t>(left.number))), m_program.string(StringID(static_cast<uint32_t>(right.number)))));
                else if (left.kind != KnownValue::Unknown && right.kind != KnownValue::Unknown)
                    result = boolean(left.kind == right.kind && left.number == right.number);
                break;
            }
            case Opcode::BitAnd: {
                KnownValue left = lookUp(value.child(0));
                KnownValue right = lookUp(value.child(1));
                if (left.kind == KnownValue::Integer && right.kind == KnownValue::Integer)
                    result = { KnownValue::Integer, static_cast<int32_t>(left.number) & static_cast<int32_t>(right.number) };
                else if ((left.kind == KnownValue::Integer && !static_cast<int32_t>(left.number)) || (right.kind == KnownValue::Integer && !static_cast<int32_t>(right.number)))
                    result = { KnownValue::Integer, 0 };
                break;
            }
            case Opcode::Cast: {
                KnownValue operand = lookUp(value.child(0));
                uint16_t admitted = m_program.at(value.type).primitives;
                if (value.castReason() != CastReason::As && ((operand.kind == KnownValue::Undefined && admitted & Primitive::Undefined) || (operand.kind == KnownValue::Null && admitted & Primitive::Null)))
                    result = operand;
                break;
            }
            default:
                break;
            }
            known.append({ id, result });
        }
        return lookUp(join.terminator.value);
    }

    BitVector m_rejectedSides;

    bool threadEdgesThatDecideTest()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        BitVector containsTest(numberOfBlocks);
        for (unsigned j = 1; j < numberOfBlocks; ++j) {
            if (mayContainTest(m_function.blocks[j]))
                containsTest.set(j);
        }
        if (containsTest.isEmpty())
            return false;
        bool mayThread = false;
        for (unsigned b = 0; b < numberOfBlocks && !mayThread; ++b) {
            auto& terminator = m_function.blocks[b].terminator;
            if (terminator.kind != TerminatorKind::Jump && terminator.kind != TerminatorKind::Branch)
                continue;
            for (auto& target : terminator.targets) {
                unsigned j = target.block.index();
                if (j == b || !containsTest.get(j))
                    continue;
                KnownValue outcome = evaluateTest(m_function.blocks[j], target.arguments);
                mayThread |= outcome.kind == KnownValue::Boolean && !m_rejectedSides.get(j * 2 + (outcome.number ? 0 : 1));
            }
        }
        if (!mayThread)
            return false;
        Vector<unsigned> definingJoin;
        definingJoin.fill(0, m_function.values.size());
        for (unsigned j = 1; j < numberOfBlocks; ++j) {
            auto& join = m_function.blocks[j];
            if (!containsTest.get(j))
                continue;
            for (ValueID id : join.parameters)
                definingJoin[id.index()] = j;
            for (ValueID id : join.values)
                definingJoin[id.index()] = j;
        }
        Vector<Vector<unsigned, 2>> usersOutsideBlock(numberOfBlocks);
        Vector<Vector<unsigned, 2>> incomingEdges(numberOfBlocks);
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            auto& block = m_function.blocks[b];
            auto note = [&](ValueID child) {
                unsigned j = definingJoin[child.index()];
                if (j && j != b && !usersOutsideBlock[j].contains(b))
                    usersOutsideBlock[j].append(b);
            };
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(note);
            block.terminator.forEachChild([&](ValueID& child) {
                note(child);
            });
            if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch)
                continue;
            for (auto& target : block.terminator.targets)
                incomingEdges[target.block.index()].append(b);
        }
        BitVector isLoopHead(numberOfBlocks);
        {
            Vector<uint8_t> state;
            state.fill(0, numberOfBlocks);
            Vector<std::pair<unsigned, unsigned>, 16> stack { { 0, 0 } };
            state[0] = 1;
            while (!stack.isEmpty()) {
                auto& [b, next] = stack.last();
                auto& targets = m_function.blocks[b].terminator.targets;
                if (next == targets.size()) {
                    state[b] = 2;
                    stack.removeLast();
                    continue;
                }
                unsigned successor = targets[next++].block.index();
                if (state[successor] == 1)
                    isLoopHead.set(successor);
                else if (!state[successor]) {
                    state[successor] = 1;
                    stack.append({ successor, 0 });
                }
            }
        }
        auto reachesUser = [&](unsigned j, unsigned from) {
            if (usersOutsideBlock[j].isEmpty())
                return false;
            BitVector seen(numberOfBlocks);
            seen.set(j);
            seen.set(from);
            Vector<unsigned, 16> worklist { from };
            unsigned budget = 128;
            while (!worklist.isEmpty()) {
                unsigned b = worklist.takeLast();
                if (usersOutsideBlock[j].contains(b) || !budget--)
                    return true;
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (!seen.set(target.block.index()))
                        worklist.append(target.block.index());
                }
            }
            return false;
        };
        bool changed = false;
        for (unsigned j = 1; j < numberOfBlocks; ++j) {
            auto& join = m_function.blocks[j];
            if (!containsTest.get(j))
                continue;
            if (isLoopHead.get(j) || join.terminator.targets[0].block.index() == j || join.terminator.targets[1].block.index() == j) {
                m_rejectedSides.set(j * 2);
                m_rejectedSides.set(j * 2 + 1);
                continue;
            }
            std::array<std::optional<bool>, 2> canJumpDirectlyTo;
            for (unsigned way = 0; way < incomingEdges[j].size(); ++way) {
                unsigned b = incomingEdges[j][way];
                if (b == j)
                    continue;
                auto& terminator = m_function.blocks[b].terminator;
                for (auto& target : terminator.targets) {
                    if (target.block.index() != j)
                        continue;
                    Arguments brought = target.arguments;
                    KnownValue outcome = evaluateTest(join, brought);
                    if (outcome.kind != KnownValue::Boolean)
                        continue;
                    unsigned side = outcome.number ? 0 : 1;
                    auto& chosen = join.terminator.targets[side];
                    if (!canJumpDirectlyTo[side]) {
                        bool passesOwnArguments = false;
                        for (ValueID argument : chosen.arguments)
                            passesOwnArguments |= join.values.contains(argument);
                        canJumpDirectlyTo[side] = !passesOwnArguments && !reachesUser(j, chosen.block.index());
                        if (!*canJumpDirectlyTo[side])
                            m_rejectedSides.set(j * 2 + side);
                    }
                    if (!*canJumpDirectlyTo[side])
                        continue;
                    Target result;
                    result.block = chosen.block;
                    for (ValueID argument : chosen.arguments) {
                        size_t index = join.parameters.find(argument);
                        result.arguments.append(index == notFound ? argument : brought[index]);
                        unsigned other = definingJoin[result.arguments.last().index()];
                        if (other && other != b && !usersOutsideBlock[other].contains(b))
                            usersOutsideBlock[other].append(b);
                    }
                    target = WTF::move(result);
                    incomingEdges[target.block.index()].append(b);
                    changed = true;
                }
            }
        }
        return changed;
    }

    void removeUnreachableBlocks()
    {
        BitVector seen(m_function.blocks.size());
        seen.set(0);
        Vector<BlockID> worklist { BlockID(0u) };
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.at(worklist.takeLast()).terminator.targets) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block);
            }
        }
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (seen.get(b))
                continue;
            auto& block = m_function.blocks[b];
            if (block.terminator.kind != TerminatorKind::Unreachable || !block.values.isEmpty() || !block.parameters.isEmpty() || block.terminator.value || !block.terminator.targets.isEmpty() || !block.terminator.cases.isEmpty())
                block = Block { };
        }
    }

    void resetReplacements()
    {
        m_replaced.resize(m_function.values.size());
        for (unsigned i = 0; i < m_replaced.size(); ++i)
            m_replaced[i] = ValueID(i);
    }

    ValueID resolve(ValueID value) const
    {
        if (!value)
            return value;
        while (m_replaced[value.index()] != value)
            value = m_replaced[value.index()];
        return value;
    }

    void rewrite()
    {
        auto replace = [&](ValueID& value) {
            value = resolve(value);
        };
        for (auto& value : m_function.values)
            value.forEachChild(replace);
        for (auto& block : m_function.blocks)
            block.terminator.forEachChild(replace);
    }

    bool canReplaceParameter(ValueID value, ValueID parameter) const
    {
        return m_function.at(value).type == m_function.at(parameter).type && !m_function.at(parameter).hasDifferentPredictionThan(m_function.at(value));
    }

    void makeUpcast(ValueID parameter, ValueID of)
    {
        auto& value = m_function.at(parameter);
        value.opcode = Opcode::Cast;
        value.children = { of };
        value.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
    }

    bool isCastOf(ValueID value, ValueID of)
    {
        while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() != CastReason::Coerce && m_function.at(value).castReason() != CastReason::As) {
            value = resolve(m_function.at(value).child(0));
            if (value == of)
                return true;
        }
        return false;
    }

    enum class UnreachableBlockState : bool { MayHaveSuccessors, AreRemoved };

    bool hasTrivialParameter(const Vector<unsigned>& firstParameter, unsigned numberOfParameters, UnreachableBlockState unreachableBlockState) const
    {
        auto isCastOf = [&](ValueID value, ValueID of) {
            while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() != CastReason::Coerce && m_function.at(value).castReason() != CastReason::As) {
                value = m_function.at(value).child(0);
                if (value == of)
                    return true;
            }
            return false;
        };
        Vector<ValueID, 64> only(numberOfParameters);
        BitVector hasMultipleIncomingValues(numberOfParameters);
        auto recordArguments = [&](const Target& target) {
            unsigned to = target.block.index();
            for (unsigned i = 0; i < target.arguments.size(); ++i) {
                unsigned index = firstParameter[to] + i;
                if (hasMultipleIncomingValues.quickGet(index))
                    continue;
                ValueID argument = target.arguments[i];
                ValueID parameter = m_function.blocks[to].parameters[i];
                if (argument == parameter || argument == only[index] || isCastOf(argument, parameter))
                    continue;
                if (only[index])
                    hasMultipleIncomingValues.quickSet(index);
                else
                    only[index] = argument;
            }
        };
        if (unreachableBlockState == UnreachableBlockState::AreRemoved) {
            for (auto& block : m_function.blocks) {
                for (auto& target : block.terminator.targets)
                    recordArguments(target);
            }
        } else {
            BitVector isReachable(m_function.blocks.size());
            Vector<unsigned, 16> worklist { 0u };
            isReachable.set(0);
            while (!worklist.isEmpty()) {
                for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                    if (!isReachable.set(target.block.index()))
                        worklist.append(target.block.index());
                    recordArguments(target);
                }
            }
        }
        for (unsigned index = m_function.blocks[0].parameters.size(); index < numberOfParameters; ++index) {
            if (only[index] && !hasMultipleIncomingValues.quickGet(index))
                return true;
        }
        return false;
    }

    bool removeTrivialParameters(UnreachableBlockState unreachableBlockState = UnreachableBlockState::MayHaveSuccessors)
    {
        Vector<unsigned> firstParameter;
        firstParameter.reserveInitialCapacity(m_function.blocks.size());
        unsigned numberOfParameters = 0;
        for (auto& block : m_function.blocks) {
            firstParameter.append(numberOfParameters);
            numberOfParameters += block.parameters.size();
        }
        if (numberOfParameters == m_function.blocks[0].parameters.size())
            return false;
        if (!hasTrivialParameter(firstParameter, numberOfParameters, unreachableBlockState))
            return false;
        resetReplacements();
        Vector<Vector<ValueID, 2>> given(numberOfParameters);
        BitVector isReachable(m_function.blocks.size());
        Vector<unsigned, 16> worklist { 0u };
        isReachable.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                if (!isReachable.set(target.block.index()))
                    worklist.append(target.block.index());
                for (unsigned i = 0; i < target.arguments.size(); ++i)
                    given[firstParameter[target.block.index()] + i].append(target.arguments[i]);
            }
        }
        bool any = false;
        bool changed;
        do {
            changed = false;
            for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
                auto& parameters = m_function.blocks[b].parameters;
                for (unsigned i = 0; i < parameters.size(); ++i) {
                    ValueID parameter = parameters[i];
                    if (m_replaced[parameter.index()] != parameter)
                        continue;
                    ValueID only;
                    bool isTrivial = true;
                    for (ValueID argument : given[firstParameter[b] + i]) {
                        argument = resolve(argument);
                        if (argument == parameter || argument == only || isCastOf(argument, parameter))
                            continue;
                        if (only) {
                            isTrivial = false;
                            break;
                        }
                        only = argument;
                    }
                    if (isTrivial && only) {
                        m_replaced[parameter.index()] = only;
                        changed = true;
                    }
                }
            }
            any |= changed;
        } while (changed);
        if (!any)
            return false;

        BitVector isRemoved(m_function.values.size());
        for (unsigned v = 0; v < m_replaced.size(); ++v) {
            if (m_replaced[v] != ValueID(v))
                isRemoved.set(v);
        }
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                Arguments kept;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (!isRemoved.get(parameters[i].index()))
                        kept.append(target.arguments[i]);
                }
                target.arguments = WTF::move(kept);
            }
        }
        Vector<ValueID> values(m_replaced.size());
        for (unsigned v : isRemoved)
            values[v] = resolve(ValueID(v));
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            unsigned at = !block.values.isEmpty() && m_function.at(block.values[0]).opcode == Opcode::CatchValue ? 1 : 0;
            for (ValueID parameter : block.parameters) {
                if (!isRemoved.get(parameter.index()))
                    continue;
                ValueID value = values[parameter.index()];
                m_replaced[parameter.index()] = value;
                if (canReplaceParameter(value, parameter))
                    continue;
                m_replaced[parameter.index()] = parameter;
                makeUpcast(parameter, value);
                block.values.insert(at++, parameter);
            }
            block.parameters.removeAllMatching([&](ValueID parameter) {
                return isRemoved.get(parameter.index());
            });
            for (unsigned i = 0; i < block.parameters.size(); ++i)
                m_function.at(block.parameters[i]).immediates = { b, i };
        }
        rewrite();
        return true;
    }

    bool mergeChains()
    {
        Vector<unsigned> incomingEdges;
        incomingEdges.fill(0, m_function.blocks.size());
        incomingEdges[0] = 1;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++incomingEdges[target.block.index()];
        }
        bool any = false;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (;;) {
                auto& terminator = m_function.blocks[b].terminator;
                if (terminator.kind != TerminatorKind::Jump)
                    break;
                unsigned next = terminator.targets[0].block.index();
                if (next == b || incomingEdges[next] != 1)
                    break;
                if (!any)
                    resetReplacements();
                Arguments arguments = terminator.targets[0].arguments;
                Block taken = std::exchange(m_function.blocks[next], Block { });
                for (unsigned i = 0; i < taken.parameters.size(); ++i) {
                    ValueID parameter = taken.parameters[i];
                    if (canReplaceParameter(arguments[i], parameter)) {
                        m_replaced[parameter.index()] = arguments[i];
                        continue;
                    }
                    makeUpcast(parameter, arguments[i]);
                    m_function.blocks[b].values.append(parameter);
                }
                m_function.blocks[b].values.appendVector(taken.values);
                m_function.blocks[b].terminator = WTF::move(taken.terminator);
                incomingEdges[next] = 0;
                any = true;
            }
        }
        if (any)
            rewrite();
        return any;
    }

    bool convertsOnlyPrimitivesToString(const Value& value) const
    {
        constexpr uint16_t convertible = Primitive::String | Primitive::LiteralString | Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull | Primitive::BigInt;
        bool result = true;
        value.forEachChild([&](ValueID child) {
            auto& type = m_program.typeOf(m_function, child);
            result &= !type.isNever() && type.hasOnlyPrimitives(convertible);
        });
        return result;
    }

    bool isNoOp(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::Add: {
            if (!convertsOnlyPrimitivesToString(value))
                return false;
            constexpr uint16_t number = Primitive::Number | Primitive::Boolean | Primitive::UndefinedOrNull;
            uint16_t left = m_program.typeOf(m_function, value.child(0)).primitives;
            uint16_t right = m_program.typeOf(m_function, value.child(1)).primitives;
            return !(left & Primitive::BigInt && right & number) && !(right & Primitive::BigInt && left & number);
        }
        case Opcode::Sub:
        case Opcode::Mul:
        case Opcode::Div:
        case Opcode::Mod:
        case Opcode::Pow:
        case Opcode::BitAnd:
        case Opcode::BitOr:
        case Opcode::BitXor:
        case Opcode::LShift:
        case Opcode::RShift:
        case Opcode::URShift:
        case Opcode::Negate:
        case Opcode::BitNot:
            return !mayThrow(m_program, m_function, value);
        case Opcode::Strcat:
        case Opcode::ToString:
            return convertsOnlyPrimitivesToString(value);
        case Opcode::GetModuleVariable:
        case Opcode::GetField:
        case Opcode::GetArgument:
        case Opcode::NewArray:
            return !mayThrow(m_program, m_function, value);
        case Opcode::Operation:
        case Opcode::CallBuiltin:
            return languageOperationOf(m_program, value) == LanguageOperation::CreateArguments;
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::Boolean:
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::BigInt:
        case Opcode::ConstantSymbol:
        case Opcode::FunctionReference:
        case Opcode::NewClosure:
        case Opcode::NewClosureOnce:
        case Opcode::NewObjectWithShape:
        case Opcode::NewObject:
        case Opcode::GetCapture:
        case Opcode::CreateRest:
        case Opcode::GetRestLength:
        case Opcode::CreateThis:
        case Opcode::LogicalNot:
        case Opcode::IsUndefinedOrNull:
        case Opcode::HasShapeOf:
        case Opcode::IsClassObjectUnmodified:
        case Opcode::IsClosureOf:
        case Opcode::IsBuiltinFunction:
        case Opcode::BuiltinFunction:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::ResultAt:
            return true;
        case Opcode::Cast:
            return value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Member;
        default:
            return false;
        }
    }

    void ignoreAllocationAssertionUses(Vector<unsigned>& uses)
    {
        BitVector hasPlacement(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.parameters)
                hasPlacement.quickSet(id.index());
            for (ValueID id : block.values)
                hasPlacement.quickSet(id.index());
        }
        for (auto& block : m_function.blocks) {
            block.values.removeAllMatching([&](ValueID id) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::AssertNotAllocated)
                    return false;
                for (auto* operand = &m_function.at(value.child(0)); operand->opcode == Opcode::Cast && operand->castReason() != CastReason::As && operand->castReason() != CastReason::Coerce; operand = &m_function.at(value.child(0)))
                    value.children[0] = operand->child(0);
                if (!hasPlacement.quickGet(value.child(0).index()))
                    return true;
                --uses[value.child(0).index()];
                return false;
            });
        }
    }

    ValueID skipUpcasts(ValueID id) const
    {
        while (m_function.at(id).opcode == Opcode::Cast && m_function.at(id).castReason() == CastReason::Upcast)
            id = m_function.at(id).child(0);
        return id;
    }

    struct LocalObjectStore {
        ValueID object;
        ValueID stored;
    };

    std::optional<LocalObjectStore> localObjectStore(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::PutField:
        case Opcode::PutById: {
            ValueID object = skipUpcasts(value.child(0));
            switch (m_function.at(object).opcode) {
            case Opcode::NewObjectWithShape:
                if (value.opcode == Opcode::PutById && !mayStoreDirectlyToObject(object, value))
                    return std::nullopt;
                break;
            case Opcode::NewObject:
                if (value.opcode != Opcode::PutById || equalSpans(m_program.string(value.string()), asBytes("__proto__"_s.span8())))
                    return std::nullopt;
                break;
            default:
                return std::nullopt;
            }
            return LocalObjectStore { object, value.child(1) };
        }
        case Opcode::Operation:
        case Opcode::CallBuiltin: {
            if (languageOperationOf(m_program, value) != LanguageOperation::DefineField || value.arguments.size() != 3)
                return std::nullopt;
            ValueID object = skipUpcasts(value.arguments[0]);
            if (m_function.at(object).opcode != Opcode::NewObject || m_function.at(value.arguments[1]).opcode != Opcode::String)
                return std::nullopt;
            return LocalObjectStore { object, value.arguments[2] };
        }
        default:
            return std::nullopt;
        }
    }

    bool removeWriteOnlyObjects()
    {
        BitVector isLocalObject(m_function.values.size());
        bool hasLocalObjects = false;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                Opcode opcode = m_function.at(id).opcode;
                if (opcode != Opcode::NewObjectWithShape && opcode != Opcode::NewObject)
                    continue;
                isLocalObject.quickSet(id.index());
                hasLocalObjects = true;
            }
        }
        if (!hasLocalObjects)
            return false;

        auto throwingValueOf = [](const Block& block) {
            return block.terminator.kind == TerminatorKind::Invoke && !block.values.isEmpty() ? block.values.last() : ValueID();
        };
        auto isUpcast = [](const Value& value) {
            return value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast;
        };
        BitVector isNeeded(m_function.values.size());
        auto need = [&](ValueID id) {
            isNeeded.quickSet(skipUpcasts(id).index());
        };
        Vector<LocalObjectStore, 8> held;
        for (auto& block : m_function.blocks) {
            ValueID watched = throwingValueOf(block);
            for (ValueID id : block.values) {
                auto& value = std::as_const(m_function).at(id);
                if (id == watched)
                    need(id);
                else if (value.opcode == Opcode::AssertNotAllocated || isUpcast(value))
                    continue;
                else if (auto store = localObjectStore(value)) {
                    ValueID stored = skipUpcasts(store->stored);
                    if (isLocalObject.quickGet(stored.index()))
                        held.append({ store->object, stored });
                    continue;
                }
                value.forEachChild(need);
            }
            block.terminator.forEachChild([&](ValueID& id) {
                need(id);
            });
        }
        for (bool changed = true; changed;) {
            changed = false;
            for (auto& [object, stored] : held) {
                if (isNeeded.quickGet(object.index()) && !isNeeded.quickSet(stored.index()))
                    changed = true;
            }
        }

        bool hasObjectToRemove = false;
        for (size_t object : isLocalObject)
            hasObjectToRemove |= !isNeeded.quickGet(object);
        if (!hasObjectToRemove)
            return false;
        auto isRemoved = [&](ValueID object) {
            return isLocalObject.quickGet(object.index()) && !isNeeded.quickGet(object.index());
        };
        bool any = false;
        for (auto& block : m_function.blocks) {
            ValueID watched = throwingValueOf(block);
            any |= !!block.values.removeAllMatching([&](ValueID id) {
                if (id == watched)
                    return false;
                auto& value = m_function.at(id);
                if (isUpcast(value) || isLocalObject.quickGet(id.index()))
                    return isRemoved(skipUpcasts(id));
                auto store = localObjectStore(value);
                return store && isRemoved(store->object);
            });
        }
        return any;
    }

    bool removeUnusedParameters(Vector<unsigned>& uses)
    {
        BitVector isRemoved;
        for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
            for (ValueID parameter : m_function.blocks[b].parameters) {
                if (!uses[parameter.index()])
                    isRemoved.set(parameter.index());
            }
        }
        if (isRemoved.isEmpty())
            return false;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                Arguments kept;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    if (isRemoved.get(parameters[i].index()))
                        --uses[target.arguments[i].index()];
                    else
                        kept.append(target.arguments[i]);
                }
                if (kept.size() != target.arguments.size())
                    target.arguments = WTF::move(kept);
            }
        }
        for (unsigned b = 1; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            unsigned before = block.parameters.size();
            block.parameters.removeAllMatching([&](ValueID parameter) {
                return isRemoved.get(parameter.index());
            });
            if (block.parameters.size() == before)
                continue;
            for (unsigned i = 0; i < block.parameters.size(); ++i)
                m_function.at(block.parameters[i]).immediates = { b, i };
        }
        for (size_t parameter : isRemoved) {
            auto& value = m_function.values[parameter];
            value.opcode = Opcode::Undefined;
            value.type = m_program.types.undefined;
            value.rep = repOf(m_program.at(value.type));
            value.setPrediction({ });
            value.setPredictedClosure({ });
            value.immediates = { Format::none, Format::none };
        }
        return true;
    }

    bool removeUnusedValues()
    {
        bool any = m_allocatesObjects && removeWriteOnlyObjects();
        m_function.didRemoveAllocation |= any;
        Vector<unsigned> uses;
        uses.fill(0, m_function.values.size());
        auto use = [&](ValueID value) {
            ++uses[value.index()];
        };
        if (m_program.hasAllocationAssertions) [[unlikely]]
            ignoreAllocationAssertionUses(uses);
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values)
                std::as_const(m_function.at(id)).forEachChild(use);
            block.terminator.forEachChild([&](ValueID& value) {
                use(value);
            });
        }
        if (!m_program.hasAllocationAssertions) [[likely]]
            any |= removeUnusedParameters(uses);
        for (auto& block : m_function.blocks) {
            unsigned end = block.values.size();
            if (end && block.terminator.kind == TerminatorKind::Invoke)
                --end;
            bool anyHere = false;
            for (unsigned i = end; i--;) {
                ValueID id = block.values[i];
                if (uses[id.index()] || !isNoOp(m_function.at(id)))
                    continue;
                std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                    --uses[child.index()];
                });
                block.values[i] = ValueID();
                anyHere = true;
            }
            if (!anyHere)
                continue;
            block.values.removeAllMatching([](ValueID id) {
                return !id;
            });
            any = true;
        }
        return any;
    }

    struct NaturalLoop {
        unsigned header { 0 };
        BitVector body;
        Vector<unsigned, 2> latches;
    };

    Vector<NaturalLoop> findNaturalLoops() const
    {
        unsigned count = m_function.blocks.size();
        Vector<unsigned> dominators = immediateDominatorsOf(m_function);
        auto dominates = [&](unsigned a, unsigned b) {
            while (b != a && b)
                b = dominators[b];
            return a == b;
        };
        Vector<Vector<unsigned, 2>> predecessors(count);
        for (unsigned b = 0; b < count; ++b) {
            for (auto& target : m_function.blocks[b].terminator.targets)
                predecessors[target.block.index()].append(b);
        }
        Vector<NaturalLoop> loops;
        for (unsigned header = 1; header < count; ++header) {
            NaturalLoop loop;
            loop.header = header;
            for (unsigned predecessor : predecessors[header]) {
                if ((predecessor == header || dominators[predecessor] != predecessor) && dominates(header, predecessor))
                    loop.latches.append(predecessor);
            }
            if (loop.latches.isEmpty())
                continue;
            loop.body.set(header);
            Vector<unsigned, 8> worklist(loop.latches);
            while (!worklist.isEmpty()) {
                unsigned block = worklist.takeLast();
                if (loop.body.set(block))
                    continue;
                worklist.appendVector(predecessors[block]);
            }
            loops.append(WTF::move(loop));
        }
        return loops;
    }

    bool hasNoEffectInLoop(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::LoopHint:
        case Opcode::StrictEq:
        case Opcode::ToBoolean:
            return true;
        case Opcode::Less:
        case Opcode::LessEq:
        case Opcode::Greater:
        case Opcode::GreaterEq:
        case Opcode::Eq:
        case Opcode::ToNumber:
        case Opcode::ToNumeric:
            return !mayThrow(m_program, m_function, value);
        default:
            return isNoOp(value);
        }
    }

    bool isCountedLoop(const NaturalLoop& loop, const Vector<unsigned>& valueBlock) const
    {
        auto& terminator = m_function.blocks[loop.header].terminator;
        if (terminator.kind != TerminatorKind::Branch || !loop.body.get(terminator.targets[0].block.index()) || loop.body.get(terminator.targets[1].block.index()))
            return false;
        auto stripConversions = [&](ValueID id) {
            for (;;) {
                auto& value = m_function.at(id);
                bool isUpcast = value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast;
                bool isIdentityOnIntegers = (value.opcode == Opcode::ToNumber || value.opcode == Opcode::ToNumeric) && m_program.typeOf(m_function, value.child(0)).isInteger();
                if (!isUpcast && !isIdentityOnIntegers)
                    return id;
                id = value.child(0);
            }
        };
        auto isInvariant = [&](auto& self, ValueID id, unsigned depth) -> bool {
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::Integer || !loop.body.get(valueBlock[id.index()]))
                return true;
            if (!depth || value.opcode == Opcode::Parameter || !hasNoEffectInLoop(value))
                return false;
            bool result = true;
            value.forEachChild([&](ValueID child) {
                result = result && self(self, child, depth - 1);
            });
            return result;
        };
        auto counts = [&](ValueID counterID, ValueID boundID, bool isIncreasing) {
            counterID = stripConversions(counterID);
            auto& counter = m_function.at(counterID);
            auto& bound = m_function.at(boundID);
            if (counter.opcode != Opcode::Parameter || counter.immediates[0] != loop.header || !m_program.at(counter.type).isInteger() || !m_program.at(bound.type).isInteger())
                return false;
            if (!isInvariant(isInvariant, boundID, 4))
                return false;
            for (unsigned latch : loop.latches) {
                for (auto& target : m_function.blocks[latch].terminator.targets) {
                    if (target.block.index() != loop.header)
                        continue;
                    auto& next = m_function.at(stripConversions(target.arguments[counter.immediates[1]]));
                    if ((next.opcode != Opcode::Add && next.opcode != Opcode::Sub) || stripConversions(next.child(0)) != counterID || m_function.at(next.child(1)).opcode != Opcode::Integer)
                        return false;
                    int64_t step = m_function.at(next.child(1)).integer();
                    if (!step || (step > 0) != (isIncreasing == (next.opcode == Opcode::Add)))
                        return false;
                }
            }
            return true;
        };
        auto& condition = m_function.at(terminator.value);
        switch (condition.opcode) {
        case Opcode::Less:
        case Opcode::LessEq:
            return counts(condition.child(0), condition.child(1), true) || counts(condition.child(1), condition.child(0), false);
        case Opcode::Greater:
        case Opcode::GreaterEq:
            return counts(condition.child(0), condition.child(1), false) || counts(condition.child(1), condition.child(0), true);
        default:
            return false;
        }
    }

    bool removeDeadCountedLoops()
    {
        bool mayHaveDeadLoop = std::ranges::any_of(m_function.blocks, [&](auto& block) {
            bool hasLoopHint = std::ranges::any_of(block.values, [&](ValueID id) {
                return m_function.at(id).opcode == Opcode::LoopHint;
            });
            return hasLoopHint && std::ranges::all_of(block.values, [&](ValueID id) {
                return hasNoEffectInLoop(m_function.at(id));
            });
        });
        if (!mayHaveDeadLoop)
            return false;
        Vector<NaturalLoop> loops = findNaturalLoops();
        if (loops.isEmpty())
            return false;
        unsigned count = m_function.blocks.size();
        Vector<unsigned> valueBlock(FillWith { }, m_function.values.size(), 0u);
        for (unsigned b = 0; b < count; ++b) {
            for (ValueID id : m_function.blocks[b].parameters)
                valueBlock[id.index()] = b;
            for (ValueID id : m_function.blocks[b].values)
                valueBlock[id.index()] = b;
        }
        std::ranges::sort(loops, [](auto& a, auto& b) {
            return a.body.bitCount() > b.body.bitCount();
        });
        for (auto& loop : loops) {
            std::optional<unsigned> exit;
            bool isDead = true;
            for (size_t b : loop.body) {
                auto& block = m_function.blocks[b];
                isDead &= block.terminator.kind == TerminatorKind::Jump || block.terminator.kind == TerminatorKind::Branch;
                for (ValueID id : block.values)
                    isDead &= hasNoEffectInLoop(m_function.at(id));
                for (auto& target : block.terminator.targets) {
                    unsigned to = target.block.index();
                    if (loop.body.get(to))
                        continue;
                    isDead &= target.arguments.isEmpty() && (!exit || *exit == to);
                    exit = to;
                }
                if (!isDead)
                    break;
            }
            if (!isDead || !exit)
                continue;
            for (auto& inner : loops)
                isDead &= !loop.body.get(inner.header) || isCountedLoop(inner, valueBlock);
            if (!isDead)
                continue;
            auto isDefinedInLoop = [&](ValueID id) {
                return loop.body.get(valueBlock[id.index()]);
            };
            for (unsigned b = 0; b < count && isDead; ++b) {
                if (loop.body.get(b))
                    continue;
                auto& block = m_function.blocks[b];
                for (ValueID id : block.values) {
                    std::as_const(m_function.at(id)).forEachChild([&](ValueID child) {
                        isDead &= !isDefinedInLoop(child);
                    });
                }
                block.terminator.forEachChild([&](ValueID& child) {
                    isDead &= !isDefinedInLoop(child);
                });
            }
            if (!isDead)
                continue;
            for (unsigned b = 0; b < count; ++b) {
                if (loop.body.get(b))
                    continue;
                for (auto& target : m_function.blocks[b].terminator.targets) {
                    if (target.block.index() != loop.header)
                        continue;
                    target.block = BlockID(*exit);
                    target.arguments = { };
                }
            }
            for (size_t b : loop.body) {
                auto& block = m_function.blocks[b];
                for (ValueID parameter : block.parameters) {
                    auto& value = m_function.at(parameter);
                    value.opcode = Opcode::Undefined;
                    value.type = m_program.types.undefined;
                    value.rep = repOf(m_program.at(value.type));
                    value.setPrediction({ });
                    value.setPredictedClosure({ });
                    value.immediates = { Format::none, Format::none };
                }
                block = Block { };
            }
            return true;
        }
        return false;
    }

    const Program& m_program;
    Function& m_function;
    Vector<ValueID> m_replaced;
    bool m_hoistsAllocations { false };
    bool m_hasFolded { false };
    bool has(Opcode opcode) const { return m_opcodes.get(static_cast<unsigned>(opcode)); }
    WTF::BitSet<256> m_opcodes;
    bool m_allocatesObjects { false };
};

} // anonymous namespace

bool hoistReadsOutOfLoops(const Program& program, Function& function)
{
    bool hasLoop = false;
    bool hasRead = false;
    for (auto& value : function.values) {
        hasLoop |= value.opcode == Opcode::LoopHint;
        hasRead |= value.opcode == Opcode::GetModuleVariable || value.opcode == Opcode::GetField;
    }
    return hasLoop && hasRead && !function.blocks.isEmpty() && ReadHoisting(program, function).run();
}

bool eliminateRedundantReads(const Program& program, Function& function)
{
    return RedundantReadElimination(program, function).run();
}

void removeUnusedValues(const Program& program, Function& function)
{
    while (Cleanup(program, function).runUnusedValueRemoval()) { }
}

bool removeDeadCountedLoops(const Program& program, Function& function)
{
    bool changed = false;
    while (Cleanup(program, function).runDeadLoopRemoval()) {
        changed = true;
        removeUnusedValues(program, function);
    }
    return changed;
}

void removeTrivialParameters(const Program& program, Function& function)
{
    Cleanup(program, function).runTrivialParameterRemoval();
}

bool sinkToBooleanIntoPredecessors(const Program& program, Function& function)
{
    return Cleanup(program, function).runSinkToBooleanIntoPredecessors();
}

void hoistAllocationsIntoPredecessors(const Program& program, Function& function)
{
    Cleanup(program, function).hoistAllocationsIntoPredecessors();
}

void compactBlocks(Function& function)
{
    unsigned count = function.blocks.size();
    if (count < 2)
        return;
    BitVector isKept(count);
    isKept.quickSet(0);
    for (unsigned b = 0; b < count; ++b) {
        auto& block = function.blocks[b];
        if (!block.values.isEmpty() || !block.parameters.isEmpty() || block.terminator.kind != TerminatorKind::Unreachable)
            isKept.quickSet(b);
        for (auto& target : block.terminator.targets)
            isKept.quickSet(target.block.index());
    }
    if (isKept.bitCount() == count)
        return;
    Vector<uint32_t> movedTo(FillWith { }, count, Format::none);
    unsigned next = 0;
    for (unsigned b = 0; b < count; ++b) {
        if (!isKept.quickGet(b))
            continue;
        if (next != b)
            function.blocks[next] = WTF::move(function.blocks[b]);
        movedTo[b] = next++;
    }
    function.blocks.shrink(next);
    for (auto& block : function.blocks) {
        for (auto& target : block.terminator.targets)
            target.block = BlockID(movedTo[target.block.index()]);
    }
    for (auto& value : function.values) {
        if (value.opcode == Opcode::Parameter && value.immediates[0] && value.immediates[0] < count)
            value.immediates[0] = movedTo[value.immediates[0]];
    }
}

void cleanUp(const Program& program, Function& function)
{
    recordAllocationSites(program, function);
    function.cleanUpMovedStores = false;
    Cleanup(program, function).run();
}

void cleanUpIfFoldable(const Program& program, Function& function)
{
    recordAllocationSites(program, function);
    Cleanup(program, function).run(true);
}

std::optional<bool> areEqualConstants(const Program& program, const Value& a, const Value& b, bool isStrict)
{
    auto isNil = [](const Value& value) {
        return value.opcode == Opcode::Undefined || value.opcode == Opcode::Null;
    };
    auto numberOf = [](const Value& value) -> std::optional<double> {
        if (value.opcode == Opcode::Integer)
            return static_cast<double>(value.integer());
        if (value.opcode == Opcode::Double)
            return std::bit_cast<double>(value.bits());
        return std::nullopt;
    };
    auto isConstant = [&](const Value& value) {
        return isNil(value) || value.opcode == Opcode::Boolean || value.opcode == Opcode::String || numberOf(value);
    };
    if (!isConstant(a) || !isConstant(b))
        return std::nullopt;
    if (isNil(a) || isNil(b))
        return isStrict ? a.opcode == b.opcode : isNil(a) && isNil(b);
    if (numberOf(a) && numberOf(b))
        return *numberOf(a) == *numberOf(b);
    if (a.opcode != b.opcode)
        return isStrict ? std::optional(false) : std::nullopt;
    if (a.opcode == Opcode::Boolean)
        return a.boolean() == b.boolean();
    return equalSpans(program.string(a.string()), program.string(b.string()));
}

bool mayThrow(const Program& program, const Function& function, const Value& value)
{
    if (isIteratorResultCheckOfObject(program, function, value) || isSpreadOfNonObjectConstant(program, function, value))
        return false;
    switch (value.opcode) {
    case Opcode::Results:
    case Opcode::ResultAt:
    case Opcode::Unspecified:
    case Opcode::Parameter:
    case Opcode::Undefined:
    case Opcode::Null:
    case Opcode::Boolean:
    case Opcode::Integer:
    case Opcode::Double:
    case Opcode::String:
    case Opcode::BigInt:
    case Opcode::ConstantSymbol:
    case Opcode::FunctionReference:
    case Opcode::NewClosure:
    case Opcode::NewClosureOnce:
    case Opcode::GetCapture:
    case Opcode::CreateRest:
    case Opcode::GetRestLength:
    case Opcode::CreateThis:
    case Opcode::PutModuleVariable:
    case Opcode::NewObjectWithShape:
    case Opcode::NewObject:
    case Opcode::PutField:
    case Opcode::StrictEq:
    case Opcode::LogicalNot:
    case Opcode::ToBoolean:
    case Opcode::TypeOf:
    case Opcode::IsUndefinedOrNull:
    case Opcode::HasShapeOf:
    case Opcode::IsClassObjectUnmodified:
    case Opcode::IsClosureOf:
    case Opcode::IsBuiltinFunction:
    case Opcode::BuiltinFunction:
    case Opcode::TypeOfIs:
    case Opcode::AssertNotAllocated:
    case Opcode::MaterializeEnvironment:
    case Opcode::CatchValue:
    case Opcode::LoopHint:
    case Opcode::GetResumeMode:
        return false;
    case Opcode::IsInstanceOfClass:
        return mayWalkPrototypeChain(program, function, value);
    case Opcode::NewArray:
        return !Type::engineClass(Builtins::singleton().arrayClass()).isSubsetOf(program.at(value.type), program);
    case Opcode::GetArgument:
        return !program.at(program.types.anything).isSubsetOf(program.at(value.type), program);
    case Opcode::GetMyArgumentByVal:
        return program.typeOf(function, value.child(0)).hasObjects() || !program.at(program.types.anything).isSubsetOf(program.at(value.type), program);
    case Opcode::GetModuleVariable: {
        auto& variable = program.variables[value.variable().index()];
        return (variable.mayBeReadUninitialized() && !value.readsInitializedVariable()) || !program.at(variable.type ? variable.type : program.types.anything).isSubsetOf(program.at(value.type), program);
    }
    case Opcode::GetGlobal:
    case Opcode::GetGlobalForTypeOf: {
        BuiltinName name = program.builtinName(value.string());
        return !Builtins::singleton().isGlobal(name) || program.isExtended(name);
    }
    case Opcode::GetField: {
        auto& layout = program.at(value.layout());
        if (isObjectLayout(layout.kind))
            return true;
        auto& field = layout.fields[value.field()];
        if (layout.kind == LayoutKind::Environment && !layout.isCoroutineFrame && !(field.flags & (fieldIsImmutable | fieldIsNeverReadUninitialized)))
            return true;
        return !program.at(field.type ? field.type : program.types.anything).isSubsetOf(program.at(value.type), program);
    }
    case Opcode::Cast: {
        if (value.castReason() == CastReason::Upcast || value.castReason() == CastReason::Member)
            return false;
        if (Options::auditAOTTypedAssertions()) [[unlikely]]
            return true;
        auto& operand = program.typeOf(function, value.child(0));
        if (value.castReason() == CastReason::Coerce)
            return !operand.isSubsetOf(program.at(value.type), program) || coercionMayMutateObjects(program, operand, program.at(value.coercedType()), value.coercesFreshData());
        if (value.castReason() == CastReason::NonNull && operand.mayBeUndefinedOrNull())
            return true;
        if (value.accessedName())
            return operand.mayBeUndefinedOrNull();
        return !program.castIsRedundant(operand, program.at(value.type), value);
    }
    case Opcode::Add:
    case Opcode::Sub:
    case Opcode::Mul:
    case Opcode::Div:
    case Opcode::Mod:
    case Opcode::Pow:
    case Opcode::BitAnd:
    case Opcode::BitOr:
    case Opcode::BitXor:
    case Opcode::LShift:
    case Opcode::RShift:
    case Opcode::URShift:
    case Opcode::Negate:
    case Opcode::ToNumber:
    case Opcode::ToNumeric:
    case Opcode::BitNot:
        if (!(program.at(value.type).primitives & Primitive::Number))
            return true;
        [[fallthrough]];
    case Opcode::Less:
    case Opcode::LessEq:
    case Opcode::Greater:
    case Opcode::GreaterEq:
    case Opcode::Eq:
        return !isNumericOperand(program.typeOf(function, value.child(0))) || (value.child(1) && !isNumericOperand(program.typeOf(function, value.child(1))));
    default:
        return true;
    }
}

static bool closesArrayOrStringIterator(const Program& program, const Function& function, const Value& value)
{
    if (value.opcode != Opcode::IteratorClose)
        return false;
    if (keyedAccessOf(program, function, value) != KeyedAccess::Generic)
        return true;
    auto& iterator = function.at(value.child(0));
    if (iterator.opcode != Opcode::IteratorOpen)
        return false;
    auto& type = program.typeOf(function, iterator.child(0));
    return type.primitives & Primitive::String && type.hasOnlyPrimitives(Primitive::String | Primitive::UndefinedOrNull);
}

unsigned removeRedundantCatchBlocks(const Program& program, Function& function)
{
    unsigned removed = 0;
    BitVector isUsedInAnotherBlock;
    Vector<Vector<unsigned, 2>> unwindSources;
    auto findUsesAndUnwindSources = [&] {
        constexpr unsigned none = UINT_MAX;
        Vector<unsigned> definedIn;
        definedIn.fill(none, function.values.size());
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            for (ValueID id : function.blocks[b].values)
                definedIn[id.index()] = b;
        }
        unwindSources.grow(function.blocks.size());
        for (unsigned b = 0; b < function.blocks.size(); ++b) {
            auto noteUse = [&](ValueID used) {
                if (definedIn[used.index()] != b)
                    isUsedInAnotherBlock.set(used.index());
            };
            for (ValueID id : function.blocks[b].values)
                function.at(id).forEachChild(noteUse);
            auto& terminator = function.blocks[b].terminator;
            terminator.forEachChild(noteUse);
            if (terminator.kind == TerminatorKind::Invoke)
                unwindSources[terminator.targets[1].block.index()].append(b);
            else if (terminator.kind == TerminatorKind::Throw && !terminator.targets.isEmpty())
                unwindSources[terminator.targets[0].block.index()].append(b);
        }
    };
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& handler = function.blocks[b];
        if (handler.terminator.kind != TerminatorKind::Throw || handler.values.isEmpty() || !handler.parameters.isEmpty())
            continue;
        ValueID caught = handler.values[0];
        if (function.at(caught).opcode != Opcode::CatchValue || handler.terminator.value != caught)
            continue;
        if (unwindSources.isEmpty())
            findUsesAndUnwindSources();
        bool onlyRethrows = true;
        for (unsigned i = 1; i < handler.values.size(); ++i)
            onlyRethrows &= closesArrayOrStringIterator(program, function, function.at(handler.values[i]));
        Vector<Target, 2> outer = handler.terminator.targets;
        for (ValueID id : handler.values)
            onlyRethrows &= !isUsedInAnotherBlock.get(id.index());
        for (auto& target : outer) {
            for (ValueID argument : target.arguments)
                onlyRethrows &= !handler.values.contains(argument);
        }
        if (!onlyRethrows)
            continue;
        bool isUnwindTarget = false;
        for (unsigned other : std::exchange(unwindSources[b], { })) {
            if (other == b)
                continue;
            auto& terminator = function.blocks[other].terminator;
            if (terminator.kind == TerminatorKind::Throw)
                terminator.targets = outer;
            else if (outer.isEmpty()) {
                terminator.kind = TerminatorKind::Jump;
                terminator.targets.shrink(1);
            } else
                terminator.targets[1] = outer[0];
            if (!outer.isEmpty())
                unwindSources[outer[0].block.index()].append(other);
            isUnwindTarget = true;
        }
        removed += isUnwindTarget;
    }
    return removed;
}

class ForInThreading {
public:
    ForInThreading(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    bool run()
    {
        bool changed = markForInListReads();
        unsigned count = m_function.blocks.size();
        m_hasHandlers = m_function.blocks.containsIf([](auto& block) {
            return block.terminator.kind == TerminatorKind::Invoke;
        });
        for (unsigned b = 0; b < count; ++b) {
            m_threadsReads = false;
            changed |= threadLoopIteration(b);
            m_threadsReads = !m_hasHandlers;
            if (m_threadsReads)
                changed |= threadLoopIteration(b);
        }
        return changed;
    }

private:
    static constexpr unsigned maxCopiedValues = 64;
    static constexpr unsigned maxCopiedBlocks = 8;
    static constexpr uint32_t none = UINT32_MAX;
    enum class OwnUse : uint8_t { None, HasCheck, Read };
    using Copies = UncheckedKeyHashMap<uint32_t, uint32_t, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>>;

    struct Path {
        Copies copyOf;
        Vector<uint32_t, 16> defined;
        Vector<uint32_t, 8> blocks;
    };

    struct Transfer {
        uint32_t from;
        Path path;
    };

    std::optional<InlineBuiltin> inlinedBy(const Value& value) const
    {
        if (value.opcode != Opcode::CallBuiltin)
            return std::nullopt;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        if (implementation.kind != BuiltinImplementation::Inline)
            return std::nullopt;
        return implementation.inlined;
    }

    ValueID originOf(ValueID id) const
    {
        unsigned numberOfParameters = 0;
        while (true) {
            id = m_program.originOf(m_function, id);
            auto& value = m_function.at(id);
            if (value.opcode == Opcode::Parameter && id.index() < m_parameterOrigins.size() && m_parameterOrigins[id.index()] && ++numberOfParameters <= maxCopiedBlocks)
                id = m_parameterOrigins[id.index()];
            else
                return id;
        }
    }

    void findParameterOrigins()
    {
        m_parameterOrigins.clear();
        Vector<ValueID> found;
        found.fill(ValueID(), m_function.values.size());
        BitVector hasSeveral(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.blocks[target.block.index()].parameters;
                for (unsigned i = 0; i < parameters.size() && i < target.arguments.size(); ++i) {
                    ValueID origin = originOf(target.arguments[i]);
                    auto& known = found[parameters[i].index()];
                    if (known && known != origin)
                        hasSeveral.set(parameters[i].index());
                    known = origin;
                }
            }
        }
        for (unsigned i = 0; i < found.size(); ++i) {
            if (hasSeveral.get(i) || found[i] == ValueID(i))
                found[i] = ValueID();
        }
        m_parameterOrigins = WTF::move(found);
    }

    BitVector findEscapingLists() const
    {
        BitVector result(m_function.values.size());
        auto forInListOf = [&](ValueID id) {
            ValueID origin = originOf(id);
            return inlinedBy(m_function.at(origin)) == InlineBuiltin::ForInKeys ? origin : ValueID();
        };
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::Cast)
                    continue;
                value.forEachChild([&](ValueID child) {
                    ValueID keys = forInListOf(child);
                    if (!keys)
                        return;
                    if (value.opcode == Opcode::GetByVal && originOf(value.child(1)) != keys)
                        return;
                    if (inlinedBy(value) == InlineBuiltin::ArrayLength)
                        return;
                    if (inlinedBy(value) == InlineBuiltin::HasEnumerableProperty && value.arguments.size() == 3 && originOf(value.arguments[0]) != keys && originOf(value.arguments[1]) != keys)
                        return;
                    result.set(keys.index());
                });
            }
            if (block.terminator.value) {
                if (ValueID keys = forInListOf(block.terminator.value))
                    result.set(keys.index());
            }
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments) {
                    if (ValueID keys = forInListOf(argument))
                        result.set(keys.index());
                }
            }
        }
        return result;
    }

    bool markForInListReads()
    {
        bool changed = false;
        Vector<unsigned> predecessorCount;
        predecessorCount.fill(0, m_function.blocks.size());
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets)
                ++predecessorCount[target.block.index()];
        }
        std::optional<BitVector> escapingLists;
        for (auto& head : m_function.blocks) {
            if (head.terminator.kind != TerminatorKind::Branch || m_function.at(head.terminator.value).opcode != Opcode::Less)
                continue;
            auto& condition = m_function.at(head.terminator.value);
            auto& length = m_function.at(originOf(condition.child(1)));
            if (inlinedBy(length) != InlineBuiltin::ArrayLength)
                continue;
            ValueID keys = originOf(length.child(0));
            ValueID index = originOf(condition.child(0));
            auto& indexType = m_program.typeOf(m_function, index);
            unsigned turn = head.terminator.targets[0].block.index();
            if (inlinedBy(m_function.at(keys)) != InlineBuiltin::ForInKeys || !indexType.isInteger() || indexType.integers.minimum < 0 || predecessorCount[turn] != 1)
                continue;
            if (!escapingLists)
                escapingLists = findEscapingLists();
            if (escapingLists->get(keys.index()))
                continue;
            for (ValueID id : m_function.blocks[turn].values) {
                auto& value = m_function.at(id);
                if (value.opcode != Opcode::GetByVal || value.readsForInKeys() || originOf(value.child(0)) != keys || originOf(value.child(1)) != index)
                    continue;
                value.immediates[0] = Value::isForInKey;
                changed = true;
            }
        }
        return changed;
    }

    OwnUse classifyOwnUse(const Value& value) const
    {
        if (inlinedBy(value) == InlineBuiltin::ObjectHasOwn && value.arguments.size() == 2)
            return originOf(value.arguments[0]) == m_object && originOf(value.arguments[1]) == m_key ? OwnUse::HasCheck : OwnUse::None;
        if (value.opcode != Opcode::InByVal && value.opcode != Opcode::GetByVal)
            return OwnUse::None;
        if (originOf(value.child(0)) != m_object || originOf(value.child(1)) != m_key)
            return OwnUse::None;
        if (value.opcode == Opcode::InByVal)
            return OwnUse::HasCheck;
        return m_threadsReads && keyedAccessOf(m_program, m_function, value) == KeyedAccess::Generic ? OwnUse::Read : OwnUse::None;
    }

    bool isQuiet(const Value& value) const
    {
        switch (value.opcode) {
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::Boolean:
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::BuiltinFunction:
        case Opcode::IsBuiltinFunction:
        case Opcode::StrictEq:
        case Opcode::LogicalNot:
        case Opcode::ToBoolean:
        case Opcode::IsUndefinedOrNull:
        case Opcode::TypeOf:
        case Opcode::TypeOfIs:
        case Opcode::Cast:
        case Opcode::GetModuleVariable:
        case Opcode::GetCapture:
            return !mayThrow(m_program, m_function, value);
        default:
            return false;
        }
    }

    bool canRematerialize(ValueID id, const BitVector& isDefinedOnPath) const
    {
        auto& value = m_function.at(id);
        switch (value.opcode) {
        case Opcode::Integer:
        case Opcode::Double:
        case Opcode::String:
        case Opcode::Boolean:
        case Opcode::Undefined:
        case Opcode::Null:
        case Opcode::BuiltinFunction:
            return true;
        case Opcode::Cast:
            return !mayThrow(m_program, m_function, value) && (!isDefinedOnPath.get(value.child(0).index()) || canRematerialize(value.child(0), isDefinedOnPath));
        default:
            return false;
        }
    }

    bool canCopyAfterEffect(const Value& value) const
    {
        return isQuiet(value) || value.opcode == Opcode::GetById || value.opcode == Opcode::Cast;
    }

    bool leadsToOwnUse(unsigned b, unsigned index, unsigned depth = 0) const
    {
        if (depth > maxCopiedBlocks || b == m_iterationHeader)
            return false;
        auto& block = m_function.blocks[b];
        for (; index < block.values.size(); ++index) {
            auto& value = m_function.at(block.values[index]);
            if (classifyOwnUse(value) != OwnUse::None)
                return true;
            if (!isQuiet(value))
                return false;
        }
        if (block.terminator.kind != TerminatorKind::Jump && block.terminator.kind != TerminatorKind::Branch)
            return false;
        for (auto& target : block.terminator.targets) {
            if (leadsToOwnUse(target.block.index(), 0, depth + 1))
                return true;
        }
        return false;
    }

    void findDefinitionsAndUses()
    {
        m_blockOf.fill(none, m_function.values.size());
        m_useBlocks.clear();
        m_useBlocks.grow(m_function.values.size());
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            auto use = [&](ValueID id) {
                auto& blocks = m_useBlocks[id.index()];
                if (blocks.isEmpty() || blocks.last() != b)
                    blocks.append(b);
            };
            for (ValueID id : block.parameters)
                m_blockOf[id.index()] = b;
            for (ValueID id : block.values) {
                m_blockOf[id.index()] = b;
                std::as_const(m_function.at(id)).forEachChild(use);
            }
            if (block.terminator.value)
                use(block.terminator.value);
            for (auto& target : block.terminator.targets) {
                for (ValueID argument : target.arguments)
                    use(argument);
            }
        }
    }

    template<typename Functor>
    void forEachBlockReachableAvoiding(const Vector<uint32_t, 4>& starts, uint32_t barrier, uint32_t deadEnd, const Functor& functor) const
    {
        BitVector seen(m_numberOfBlocks);
        Vector<uint32_t, 16> stack;
        for (uint32_t start : starts)
            stack.append(start);
        while (!stack.isEmpty()) {
            uint32_t b = stack.takeLast();
            if (b == barrier || seen.set(b))
                continue;
            functor(b);
            if (b == deadEnd)
                continue;
            for (auto& target : m_function.blocks[b].terminator.targets)
                stack.append(target.block.index());
        }
    }

    bool isAliveAt(uint32_t block, uint32_t value) const
    {
        bool result = false;
        forEachBlockReachableAvoiding({ block }, m_blockOf[value], none, [&](uint32_t b) {
            result |= m_useBlocks[value].contains(b);
        });
        return result;
    }

    bool isInAnotherLoop(uint32_t block) const
    {
        Vector<uint32_t, 4> successors;
        for (auto& target : m_function.blocks[block].terminator.targets)
            successors.append(target.block.index());
        bool result = false;
        forEachBlockReachableAvoiding(successors, m_iterationHeader, none, [&](uint32_t b) {
            result |= b == block;
        });
        return result;
    }

    bool mayEnter(uint32_t block, const Path& path) const
    {
        for (uint32_t value : path.defined) {
            if (isAliveAt(block, value))
                return false;
        }
        return true;
    }

    std::optional<bool> knownBoolean(ValueID id) const
    {
        auto& value = m_function.at(id);
        switch (value.opcode) {
        case Opcode::Boolean:
            return value.boolean();
        case Opcode::ToBoolean:
            return knownBoolean(value.child(0));
        case Opcode::Cast:
            if (value.castReason() != CastReason::Upcast)
                return std::nullopt;
            return knownBoolean(value.child(0));
        case Opcode::LogicalNot:
            if (auto operand = knownBoolean(value.child(0)))
                return !*operand;
            return std::nullopt;
        default:
            return std::nullopt;
        }
    }

    static ValueID inCopy(const Path& path, ValueID id)
    {
        auto found = path.copyOf.find(id.index());
        return found == path.copyOf.end() ? id : ValueID(found->value);
    }

    void bind(Path& path, const Target& target) const
    {
        auto& parameters = m_function.blocks[target.block.index()].parameters;
        for (unsigned i = 0; i < parameters.size(); ++i) {
            path.copyOf.set(parameters[i].index(), target.arguments[i].index());
            path.defined.append(parameters[i].index());
        }
    }

    uint32_t copyFrom(uint32_t b, Path path, bool isAlive)
    {
        if (++m_copiedBlocks > maxCopiedBlocks)
            return none;
        uint32_t result = m_function.blocks.size();
        m_function.blocks.append(Block { });
        BlockValues values;
        while (true) {
            if (b == m_iterationHeader || path.blocks.contains(b))
                return none;
            path.blocks.append(b);
            unsigned size = m_function.blocks[b].values.size();
            auto goesOn = [&](unsigned index) {
                auto& block = m_function.blocks[b];
                std::optional<uint32_t> next;
                if (index == size && block.terminator.kind == TerminatorKind::Jump)
                    next = block.terminator.targets[0].block.index();
                else if (index == size && block.terminator.kind == TerminatorKind::Branch) {
                    if (auto known = knownBoolean(inCopy(path, block.terminator.value)))
                        next = block.terminator.targets[*known ? 0 : 1].block.index();
                }
                if (next && isInAnotherLoop(*next))
                    return false;
                if (leadsToOwnUse(b, index))
                    return true;
                if (!m_folded || m_copiedValues + 8 > maxCopiedValues)
                    return false;
                if (index < size)
                    return isQuiet(m_function.at(block.values[index]));
                if (block.terminator.kind == TerminatorKind::Jump) {
                    return !m_function.blocks[*next].terminator.targets.containsIf([&](auto& target) {
                        return target.block.index() == m_iterationHeader;
                    });
                }
                return !!next;
            };
            for (unsigned index = 0; index <= size; ++index) {
                if (isAlive && !goesOn(index)) {
                    if (index == size && m_function.blocks[b].terminator.kind == TerminatorKind::Invoke)
                        return none;
                    if (m_transferBlock != none && (m_transferBlock != b || m_transferIndex != index))
                        return none;
                    m_transferBlock = b;
                    m_transferIndex = index;
                    m_function.blocks[result].values = WTF::move(values);
                    m_transfers.append({ result, WTF::move(path) });
                    return result;
                }
                if (index == size)
                    break;
                ValueID id = m_function.blocks[b].values[index];
                if (!isAlive && !canCopyAfterEffect(m_function.at(id)))
                    return none;
                if (++m_copiedValues > maxCopiedValues)
                    return none;
                OwnUse use = isAlive ? classifyOwnUse(m_function.at(id)) : OwnUse::None;
                Value copy(m_function.at(id));
                copy.forEachChild([&](ValueID& child) {
                    child = inCopy(path, child);
                });
                if (use == OwnUse::HasCheck) {
                    Value constant;
                    constant.opcode = Opcode::Boolean;
                    constant.position = copy.position;
                    constant.inlineFrame = copy.inlineFrame;
                    constant.type = m_program.types.boolean;
                    constant.rep = repOf(m_program.at(constant.type));
                    constant.immediates = { 1, Format::none };
                    ValueID constantID(m_function.values.size());
                    m_function.values.append(WTF::move(constant));
                    values.append(constantID);
                    copy.opcode = Opcode::Cast;
                    copy.children = { constantID, ValueID(), ValueID() };
                    copy.arguments.clear();
                    copy.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                    ++m_folded;
                } else if (use == OwnUse::Read) {
                    copy.immediates[0] = Value::keyIsListed;
                    ++m_folded;
                    ++m_reads;
                }
                ValueID created(m_function.values.size());
                m_function.values.append(WTF::move(copy));
                values.append(created);
                path.copyOf.set(id.index(), created.index());
                path.defined.append(id.index());
            }
            Terminator terminator = m_function.blocks[b].terminator;
            terminator.forEachChild([&](ValueID& child) {
                child = inCopy(path, child);
            });
            auto finish = [&] {
                m_function.blocks[result].values = WTF::move(values);
                m_function.blocks[result].terminator = WTF::move(terminator);
                return result;
            };
            switch (terminator.kind) {
            case TerminatorKind::Return:
            case TerminatorKind::Unreachable:
                return finish();
            case TerminatorKind::Jump:
            case TerminatorKind::Branch:
                break;
            default:
                return none;
            }
            if (terminator.kind == TerminatorKind::Branch) {
                if (auto known = isAlive ? knownBoolean(terminator.value) : std::nullopt) {
                    Target taken = terminator.targets[*known ? 0 : 1];
                    terminator.kind = TerminatorKind::Jump;
                    terminator.value = ValueID();
                    terminator.targets = { taken };
                }
            }
            if (isAlive && terminator.kind == TerminatorKind::Jump) {
                bind(path, terminator.targets[0]);
                b = terminator.targets[0].block.index();
                continue;
            }
            for (auto& target : terminator.targets) {
                Path next = path;
                bind(next, target);
                uint32_t to = target.block.index();
                bool leads = isAlive && leadsToOwnUse(to, 0);
                if (!leads && mayEnter(to, path))
                    continue;
                uint32_t copied = copyFrom(to, WTF::move(next), leads);
                if (copied == none)
                    return none;
                target.block = BlockID(copied);
                target.arguments.clear();
            }
            return finish();
        }
    }

    bool threadLoopIteration(unsigned turn)
    {
        if (m_function.blocks[turn].terminator.kind != TerminatorKind::Branch || m_function.blocks[turn].values.isEmpty())
            return false;
        ValueID condition = m_function.blocks[turn].terminator.value;
        ValueID test = m_function.at(condition).opcode == Opcode::ToBoolean ? m_function.at(condition).child(0) : condition;
        if (inlinedBy(m_function.at(test)) != InlineBuiltin::HasEnumerableProperty || m_function.at(test).arguments.size() != 3)
            return false;
        if (!std::exchange(m_originsAreCurrent, true))
            findParameterOrigins();
        bool isThreaded = m_function.at(m_function.at(test).arguments[1]).opcode == Opcode::Undefined;
        if (m_function.at(m_function.at(test).arguments[1]).opcode == Opcode::Boolean || (isThreaded && test != condition))
            return false;
        unsigned numberOfLast = test == condition && !isThreaded ? 1 : 2;
        auto& last = m_function.blocks[turn].values;
        if (last.size() < numberOfLast || last.last() != condition || last[last.size() - numberOfLast] != (isThreaded ? m_function.at(test).arguments[1] : test))
            return false;
        m_iterationHeader = turn;
        m_object = originOf(m_function.at(test).arguments[0]);
        m_key = originOf(m_function.at(test).arguments[1]);
        if (isThreaded) {
            for (ValueID id : last) {
                if (m_function.at(id).opcode == Opcode::GetByVal && originOf(m_function.at(id).child(0)) == originOf(m_function.at(test).arguments[2]))
                    m_key = id;
            }
        }
        if (m_function.at(m_key).opcode != Opcode::GetByVal || originOf(m_function.at(m_key).child(0)) != originOf(m_function.at(test).arguments[2]))
            return false;
        Target there = m_function.blocks[turn].terminator.targets[0];
        if (!leadsToOwnUse(there.block.index(), 0))
            return false;
        m_numberOfBlocks = m_function.blocks.size();
        findDefinitionsAndUses();
        if (m_useBlocks[test.index()].size() != 1 || m_useBlocks[condition.index()].size() != 1)
            return false;

        unsigned numberOfValues = m_function.values.size();
        m_copiedBlocks = 0;
        m_copiedValues = 0;
        m_folded = 0;
        m_reads = 0;
        m_transferBlock = none;
        m_transfers.clear();
        auto giveUp = [&] {
            m_function.values.shrink(numberOfValues);
            m_function.blocks.shrink(m_numberOfBlocks);
            return false;
        };
        Path path;
        bind(path, there);
        uint32_t listedCopy = copyFrom(there.block.index(), WTF::move(path), true);
        if (listedCopy == none || !m_folded || (isThreaded && !m_reads))
            return giveUp();

        Vector<uint32_t, 8> passed;
        Vector<Vector<uint32_t, 4>, 8> passedValueUsers;
        Vector<uint32_t, 8> rematerialized;
        Vector<Vector<uint32_t, 4>, 8> rematerializedUsers;
        if (m_transferBlock != none) {
            auto& block = m_function.blocks[m_transferBlock];
            BitVector isUsedInRest(numberOfValues);
            for (unsigned index = m_transferIndex; index < block.values.size(); ++index) {
                std::as_const(m_function.at(block.values[index])).forEachChild([&](ValueID child) {
                    isUsedInRest.set(child.index());
                });
            }
            if (block.terminator.value)
                isUsedInRest.set(block.terminator.value.index());
            Vector<uint32_t, 4> successors;
            for (auto& target : block.terminator.targets) {
                successors.append(target.block.index());
                for (ValueID argument : target.arguments)
                    isUsedInRest.set(argument.index());
            }
            BitVector isCandidate(numberOfValues);
            for (auto& transfer : m_transfers) {
                for (uint32_t value : transfer.path.defined)
                    isCandidate.set(value);
            }
            for (uint32_t value : isCandidate) {
                Vector<uint32_t, 4> users;
                bool isSplitBlockReachedAgain = false;
                forEachBlockReachableAvoiding(successors, m_blockOf[value], none, [&](uint32_t b) {
                    isSplitBlockReachedAgain |= b == m_transferBlock;
                    if (m_useBlocks[value].contains(b))
                        users.append(b);
                });
                if (users.contains(m_transferBlock))
                    return giveUp();
                if (users.isEmpty() && !isUsedInRest.get(value))
                    continue;
                if (m_blockOf[value] != m_transferBlock) {
                    Vector<uint32_t, 4> afterDefinition;
                    for (auto& target : m_function.blocks[m_blockOf[value]].terminator.targets)
                        afterDefinition.append(target.block.index());
                    bool isReachedByOtherPaths = false;
                    forEachBlockReachableAvoiding(afterDefinition, m_blockOf[value], m_transferBlock, [&](uint32_t b) {
                        isReachedByOtherPaths |= b != m_transferBlock && users.contains(b);
                    });
                    if (isReachedByOtherPaths)
                        return giveUp();
                }
                if (canRematerialize(ValueID(value), isCandidate)) {
                    rematerialized.append(value);
                    rematerializedUsers.append(WTF::move(users));
                    continue;
                }
                if (isSplitBlockReachedAgain)
                    return giveUp();
                for (auto& transfer : m_transfers) {
                    if (!transfer.path.copyOf.contains(value))
                        return giveUp();
                }
                passed.append(value);
                passedValueUsers.append(WTF::move(users));
            }

            uint32_t rest = m_function.blocks.size();
            m_function.blocks.append(Block { });
            auto& head = m_function.blocks[m_transferBlock];
            auto& tail = m_function.blocks[rest];
            tail.values.append(head.values.span().subspan(m_transferIndex));
            head.values.shrink(m_transferIndex);
            tail.terminator = WTF::move(head.terminator);
            head.terminator = Terminator { };
            head.terminator.kind = TerminatorKind::Jump;
            head.terminator.targets.append(Target { BlockID(rest), { } });
            for (unsigned i = 0; i < passed.size(); ++i) {
                ValueID original(passed[i]);
                Value parameter;
                parameter.opcode = Opcode::Parameter;
                parameter.position = m_function.at(original).position;
                parameter.inlineFrame = m_function.at(original).inlineFrame;
                parameter.type = m_function.at(original).type;
                parameter.rep = m_function.at(original).rep;
                parameter.copyPredictionFrom(m_function.at(original));
                parameter.immediates = { rest, i };
                ValueID created(m_function.values.size());
                m_function.values.append(WTF::move(parameter));
                m_function.blocks[rest].parameters.append(created);
                m_function.blocks[m_transferBlock].terminator.targets[0].arguments.append(original);
                auto replace = [&](ValueID& child) {
                    if (child == original)
                        child = created;
                };
                passedValueUsers[i].append(rest);
                for (uint32_t b : passedValueUsers[i]) {
                    for (ValueID id : m_function.blocks[b].values)
                        m_function.at(id).forEachChild(replace);
                    m_function.blocks[b].terminator.forEachChild(replace);
                }
            }
            Vector<ValueID> atRestStart;
            Copies anew;
            auto rematerialize = [&](auto& self, ValueID id) -> ValueID {
                if (auto found = anew.find(id.index()); found != anew.end())
                    return ValueID(found->value);
                Value clone(m_function.at(id));
                if (clone.opcode == Opcode::Cast && isCandidate.get(clone.child(0).index()))
                    clone.children[0] = self(self, clone.child(0));
                ValueID created(m_function.values.size());
                m_function.values.append(WTF::move(clone));
                atRestStart.append(created);
                anew.set(id.index(), created.index());
                return created;
            };
            for (unsigned i = 0; i < rematerialized.size(); ++i) {
                ValueID original(rematerialized[i]);
                ValueID created = rematerialize(rematerialize, original);
                auto replace = [&](ValueID& child) {
                    if (child == original)
                        child = created;
                };
                rematerializedUsers[i].append(rest);
                for (uint32_t b : rematerializedUsers[i]) {
                    for (ValueID id : m_function.blocks[b].values)
                        m_function.at(id).forEachChild(replace);
                    m_function.blocks[b].terminator.forEachChild(replace);
                }
            }
            m_function.blocks[rest].values.insertVector(0, atRestStart);
            for (auto& transfer : m_transfers) {
                auto& terminator = m_function.blocks[transfer.from].terminator;
                terminator.kind = TerminatorKind::Jump;
                terminator.targets.append(Target { BlockID(rest), { } });
                for (uint32_t value : passed)
                    terminator.targets[0].arguments.append(ValueID(transfer.path.copyOf.get(value)));
            }
        }

        uint32_t general = m_function.blocks.size();
        m_function.blocks.append(Block { });
        {
            auto& iterationBlock = m_function.blocks[turn];
            auto& genericBlock = m_function.blocks[general];
            genericBlock.values.append(iterationBlock.values.span().subspan(iterationBlock.values.size() - numberOfLast));
            iterationBlock.values.shrink(iterationBlock.values.size() - numberOfLast);
            genericBlock.terminator = WTF::move(iterationBlock.terminator);
        }
        auto endWithTest = [&](uint32_t block, bool checksFinalObject, Target&& isSo, uint32_t isNot) {
            Value noKey;
            noKey.opcode = checksFinalObject ? Opcode::Boolean : Opcode::Undefined;
            noKey.position = m_function.at(test).position;
            noKey.inlineFrame = m_function.at(test).inlineFrame;
            noKey.type = checksFinalObject ? m_program.types.boolean : m_program.types.undefined;
            noKey.rep = repOf(m_program.at(noKey.type));
            if (checksFinalObject)
                noKey.immediates = { 1, Format::none };
            ValueID noKeyID(m_function.values.size());
            m_function.values.append(WTF::move(noKey));
            Value stampTest(m_function.at(test));
            stampTest.arguments[1] = noKeyID;
            stampTest.type = m_program.types.boolean;
            stampTest.rep = repOf(m_program.at(stampTest.type));
            stampTest.copyPredictionFrom(Value { });
            ValueID stampTestID(m_function.values.size());
            m_function.values.append(WTF::move(stampTest));
            auto& ended = m_function.blocks[block];
            ended.values.append(noKeyID);
            ended.values.append(stampTestID);
            ended.terminator = Terminator { };
            ended.terminator.kind = TerminatorKind::Branch;
            ended.terminator.value = stampTestID;
            ended.terminator.targets.append(WTF::move(isSo));
            ended.terminator.targets.append(Target { BlockID(isNot), { } });
        };
        if (isThreaded) {
            endWithTest(turn, true, Target { BlockID(listedCopy), { } }, general);
            m_originsAreCurrent = false;
            return true;
        }
        uint32_t otherwise = general;
        if (m_reads) {
            otherwise = m_function.blocks.size();
            m_function.blocks.append(Block { });
            endWithTest(otherwise, false, Target(m_function.blocks[general].terminator.targets[0]), general);
        }
        endWithTest(turn, m_reads, Target { BlockID(listedCopy), { } }, otherwise);
        m_function.at(test).arguments.shrink(2);
        m_originsAreCurrent = false;
        return true;
    }

    const Program& m_program;
    Function& m_function;
    unsigned m_iterationHeader { 0 };
    ValueID m_object;
    ValueID m_key;
    unsigned m_numberOfBlocks { 0 };
    Vector<uint32_t> m_blockOf;
    Vector<Vector<uint32_t, 2>> m_useBlocks;
    unsigned m_copiedBlocks { 0 };
    unsigned m_copiedValues { 0 };
    unsigned m_folded { 0 };
    unsigned m_reads { 0 };
    bool m_hasHandlers { false };
    bool m_threadsReads { false };
    Vector<ValueID> m_parameterOrigins;
    bool m_originsAreCurrent { false };
    uint32_t m_transferBlock { none };
    unsigned m_transferIndex { 0 };
    Vector<Transfer, 2> m_transfers;
};

class ObjectEntriesFlattening {
public:
    ObjectEntriesFlattening(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    unsigned run()
    {
        unsigned flattened = 0;
        for (unsigned index = 0, count = m_function.values.size(); index < count; ++index) {
            if (isObjectEntriesCall(m_function.values[index]))
                flattened += flatten(ValueID(index));
        }
        return flattened;
    }

private:
    struct Uses {
        Vector<ValueID, 4> users;
        bool isUsedByTerminator { false };
    };

    struct Location {
        unsigned block { 0 };
        size_t index { notFound };
    };

    static bool isObjectEntriesCall(const Value& value)
    {
        if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != 1 || value.children[2])
            return false;
        auto& implementation = Builtins::singleton().at(value.builtin()).implementation;
        return implementation.kind == BuiltinImplementation::Inline && implementation.inlined == InlineBuiltin::ObjectEntries;
    }

    bool isAnythingOrArray(TypeID type) const
    {
        if (type == m_program.types.anything)
            return true;
        auto* engineClass = m_program.at(type).singleEngineClass();
        return engineClass && engineClass->type == ArrayType;
    }

    Uses usesOf(ValueID id)
    {
        Uses uses;
        for (auto& block : m_function.blocks) {
            for (ValueID user : block.values) {
                bool isUser = false;
                m_function.at(user).forEachChild([&](ValueID& child) {
                    isUser |= child == id;
                });
                if (isUser)
                    uses.users.append(user);
            }
            block.terminator.forEachChild([&](ValueID& child) {
                uses.isUsedByTerminator |= child == id;
            });
        }
        return uses;
    }

    Location placeOf(ValueID id)
    {
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            if (size_t index = m_function.blocks[b].values.find(id); index != notFound)
                return { b, index };
        }
        return { };
    }

    ValueID append(Value&& value)
    {
        ValueID id(m_function.values.size());
        m_function.values.append(WTF::move(value));
        return id;
    }

    bool flatten(ValueID call)
    {
        if (!isAnythingOrArray(m_function.at(call).type))
            return false;
        Uses callUses = usesOf(call);
        if (callUses.isUsedByTerminator || callUses.users.size() != 1)
            return false;
        ValueID iterator = callUses.users[0];
        if (m_function.at(iterator).opcode != Opcode::IteratorOpen)
            return false;
        Uses iteratorUses = usesOf(iterator);
        if (iteratorUses.isUsedByTerminator)
            return false;
        ValueID pair;
        ValueID counter;
        for (ValueID user : iteratorUses.users) {
            auto& step = m_function.at(user);
            if (step.opcode == Opcode::IteratorClose)
                continue;
            if ((step.opcode != Opcode::IteratorHasNext && step.opcode != Opcode::IteratorValue) || step.child(0) != iterator || step.child(1) == iterator)
                return false;
            if (counter && counter != step.child(1))
                return false;
            counter = step.child(1);
            if (step.opcode != Opcode::IteratorValue)
                continue;
            if (pair)
                return false;
            pair = user;
        }
        if (!pair || !isAnythingOrArray(m_function.at(pair).type) || m_function.at(pair).immediates[0] != Format::none || m_function.at(counter).opcode != Opcode::Parameter)
            return false;
        Location pairLocation = placeOf(pair);
        if (pairLocation.index == notFound)
            return false;
        if (auto& block = m_function.blocks[pairLocation.block]; block.terminator.kind == TerminatorKind::Invoke && block.values.last() == pair)
            return false;

        Uses counterUses = usesOf(counter);
        if (counterUses.isUsedByTerminator)
            return false;
        ValueID next;
        for (ValueID user : counterUses.users) {
            auto& use = m_function.at(user);
            if (use.opcode == Opcode::IteratorHasNext || use.opcode == Opcode::IteratorValue) {
                if (use.child(0) != iterator)
                    return false;
                continue;
            }
            if (use.opcode != Opcode::Add || use.child(0) != counter || next)
                return false;
            auto& step = m_function.at(use.child(1));
            if (step.opcode != Opcode::Integer || step.integer() != 1)
                return false;
            next = user;
        }
        if (!next || !usesOf(next).users.isEmpty())
            return false;
        Location nextLocation = placeOf(next);
        if (nextLocation.index == notFound)
            return false;
        BlockID header = m_function.at(counter).parameterBlock();
        uint32_t position = m_function.at(counter).parameterIndex();
        for (auto& block : m_function.blocks) {
            if (block.terminator.value == next)
                return false;
            for (auto& target : block.terminator.targets) {
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    ValueID argument = target.arguments[i];
                    bool isLoopCounter = target.block == header && i == position;
                    if (argument == next) {
                        if (!isLoopCounter)
                            return false;
                        continue;
                    }
                    if (!isLoopCounter)
                        continue;
                    auto& first = m_function.at(argument);
                    if (first.opcode != Opcode::Integer || first.integer())
                        return false;
                }
            }
        }

        TypeID predictedPairType = m_function.at(pair).prediction();
        Value key = m_function.at(pair);
        key.type = m_program.types.anything;
        key.rep = repOf(m_program.at(key.type));
        key.setAssertedType(TypeID());
        key.setPrediction(m_program.types.string);
        key.setPredictedClosure(FunctionID());
        key.setPredictedByKey(StringID());
        Value held = key;
        held.setPrediction(TypeID());
        Value one = m_function.at(m_function.at(next).child(1));
        Value two = one;
        two.setBits(2);
        two.type = m_program.types.int32;
        two.rep = repOf(m_program.at(two.type));
        one.position = key.position;
        one.inlineFrame = key.inlineFrame;
        Value following = m_function.at(next);
        following.position = key.position;
        following.inlineFrame = key.inlineFrame;

        ValueID keyID = append(WTF::move(key));
        ValueID oneID = append(WTF::move(one));
        following.children[1] = oneID;
        ValueID nextID = append(WTF::move(following));
        held.children[1] = nextID;
        ValueID heldID = append(WTF::move(held));
        ValueID twoID = append(WTF::move(two));

        m_function.at(next).children[1] = twoID;
        m_function.blocks[nextLocation.block].values.insert(nextLocation.index, twoID);
        if (nextLocation.block == pairLocation.block && nextLocation.index <= pairLocation.index)
            ++pairLocation.index;
        auto& values = m_function.blocks[pairLocation.block].values;
        values.insert(pairLocation.index, heldID);
        values.insert(pairLocation.index, nextID);
        values.insert(pairLocation.index, oneID);
        values.insert(pairLocation.index, keyID);

        auto& literal = m_function.at(pair);
        literal.opcode = Opcode::NewArray;
        literal.children = { ValueID(), ValueID(), ValueID() };
        literal.immediates = { Format::none, Format::none };
        literal.arguments.clear();
        literal.arguments.append(keyID);
        literal.arguments.append(heldID);
        auto& list = m_function.at(call);
        list.children[2] = list.arguments[0];
        if (list.type == m_program.types.anything && predictedPairType && predictedPairType != m_program.types.anything && isAnythingOrArray(predictedPairType)) {
            list.type = predictedPairType;
            list.rep = repOf(m_program.at(list.type));
            list.setPrediction(TypeID());
        }
        return true;
    }

    const Program& m_program;
    Function& m_function;
};

unsigned flattenIteratedObjectEntries(const Program& program, Function& function)
{
    return ObjectEntriesFlattening(program, function).run();
}

bool threadForInOwnKeys(const Program& program, Function& function)
{
    return ForInThreading(program, function).run();
}

static bool isObjectConstructorCall(const Value& value, ASCIILiteral name, size_t numberOfArguments)
{
    if (value.opcode != Opcode::CallBuiltin || value.arguments.size() != numberOfArguments || value.children[2])
        return false;
    auto& builtin = Builtins::singleton().at(value.builtin());
    return builtin.kind == BuiltinKind::StaticFunction && builtin.holder == "Object"_s && builtin.name == name;
}

unsigned fuseOwnPropertyDescriptorCopies(const Program& program, Function& function)
{
    unsigned fused = 0;
    for (auto& block : function.blocks) {
        for (size_t index = 1; index < block.values.size(); ++index) {
            Value& definition = function.at(block.values[index]);
            if (!isObjectConstructorCall(definition, "defineProperties"_s, 2) || (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size()))
                continue;
            ValueID descriptors = block.values[index - 1];
            if (definition.arguments[1] != descriptors || definition.arguments[0] == descriptors || !isObjectConstructorCall(function.at(descriptors), "getOwnPropertyDescriptors"_s, 1))
                continue;
            unsigned uses = 0;
            for (auto& other : function.blocks) {
                for (ValueID user : other.values) {
                    function.at(user).forEachChild([&](ValueID& child) {
                        uses += child == descriptors;
                    });
                }
                other.terminator.forEachChild([&](ValueID& child) {
                    uses += child == descriptors;
                });
            }
            if (uses != 1)
                continue;
            ValueID source = function.at(descriptors).arguments[0];
            definition.children[2] = source;
            Value& first = function.at(descriptors);
            first.children[2] = source;
            first.type = program.types.anything;
            first.rep = repOf(program.at(first.type));
            ++fused;
        }
    }
    return fused;
}

unsigned splitDefinePropertiesOverLiterals(const Program& program, Function& function)
{
    auto& builtins = Builtins::singleton();
    BuiltinID defineProperty = builtins.findStatic(BuiltinKind::StaticFunction, builtins.objectClass().name, builtins.nameOf(asBytes("defineProperty"_s.span8())));
    if (!defineProperty)
        return 0;
    struct Literal {
        size_t allocation;
        Vector<size_t, 6> fields;
    };
    auto useCountOf = [&](ValueID id) {
        unsigned uses = 0;
        auto count = [&](ValueID& child) {
            uses += child == id;
        };
        for (auto& block : function.blocks) {
            for (ValueID user : block.values)
                function.at(user).forEachChild(count);
            block.terminator.forEachChild(count);
        }
        return uses;
    };
    auto literalUsedOnlyBy = [&](const Block& block, ValueID id, size_t user) -> std::optional<Literal> {
        size_t allocation = block.values.find(id);
        if (allocation == notFound || allocation >= user || function.at(id).opcode != Opcode::NewObject)
            return std::nullopt;
        Literal literal { allocation, fieldsInitializedWithAllocation(program, function, block, allocation, false, false) };
        if (literal.fields.isEmpty() || literal.fields.last() >= user || useCountOf(id) != literal.fields.size() + 1)
            return std::nullopt;
        return literal;
    };
    auto memberName = [&](const Value& member) {
        return program.string(member.opcode == Opcode::PutById ? member.string() : function.at(member.arguments[1]).string());
    };
    unsigned split = 0;
    for (auto& block : function.blocks) {
        for (size_t index = 0; index < block.values.size(); ++index) {
            ValueID call = block.values[index];
            if (!isObjectConstructorCall(function.at(call), "defineProperties"_s, 2) || (block.terminator.kind == TerminatorKind::Invoke && index + 1 == block.values.size()))
                continue;
            ValueID target = function.at(call).arguments[0];
            auto outer = literalUsedOnlyBy(block, function.at(call).arguments[1], index);
            if (!outer || target == block.values[outer->allocation])
                continue;
            Vector<Literal, 4> descriptors;
            BitVector isMoved(index);
            isMoved.quickSet(outer->allocation);
            bool isScalarized = true;
            for (size_t member : outer->fields) {
                auto& definition = function.at(block.values[member]);
                auto name = memberName(definition);
                if (definition.opcode == Opcode::PutById || name.empty() || isASCIIDigit(name[0]) || equalSpans(name, asBytes("__proto__"_s.span8()))) {
                    isScalarized = false;
                    break;
                }
                auto descriptor = literalUsedOnlyBy(block, definition.arguments[2], member);
                if (!descriptor) {
                    isScalarized = false;
                    break;
                }
                for (size_t field : descriptor->fields) {
                    auto fieldName = memberName(function.at(block.values[field]));
                    isScalarized &= equalSpans(fieldName, asBytes("value"_s.span8())) || equalSpans(fieldName, asBytes("writable"_s.span8())) || equalSpans(fieldName, asBytes("enumerable"_s.span8())) || equalSpans(fieldName, asBytes("configurable"_s.span8()));
                    isMoved.quickSet(field);
                }
                isMoved.quickSet(member);
                isMoved.quickSet(descriptor->allocation);
                descriptors.append(WTF::move(*descriptor));
            }
            if (!isScalarized)
                continue;
            BlockValues values;
            for (size_t i = 0; i < index; ++i) {
                if (!isMoved.quickGet(i))
                    values.append(block.values[i]);
            }
            for (size_t i = 0; i < descriptors.size(); ++i) {
                values.append(block.values[descriptors[i].allocation]);
                for (size_t field : descriptors[i].fields)
                    values.append(block.values[field]);
                Value definition = function.at(call);
                definition.immediates[0] = defineProperty.index();
                definition.arguments = { target, function.at(block.values[outer->fields[i]]).arguments[1], block.values[descriptors[i].allocation] };
                if (i + 1 == descriptors.size()) {
                    function.at(call) = WTF::move(definition);
                    values.append(call);
                    continue;
                }
                definition.isInTailPosition = false;
                definition.mustBeProperTailCall = false;
                definition.dropsDroppableCallerFrame = false;
                definition.leavesDroppableFrame = false;
                values.append(ValueID(function.values.size()));
                function.values.append(WTF::move(definition));
            }
            size_t next = values.size();
            for (size_t i = index + 1; i < block.values.size(); ++i)
                values.append(block.values[i]);
            block.values = WTF::move(values);
            index = next - 1;
            ++split;
        }
    }
    return split;
}

unsigned hoistDefinePropertyIntoPredecessors(Function& function)
{
    unsigned hoisted = 0;
    Vector<Vector<unsigned, 2>> predecessors(function.blocks.size());
    BitVector hasNonJumpPredecessor(function.blocks.size());
    for (unsigned b = 0; b < function.blocks.size(); ++b) {
        auto& terminator = function.blocks[b].terminator;
        for (auto& target : terminator.targets) {
            predecessors[target.block.index()].append(b);
            if (terminator.kind != TerminatorKind::Jump || target.block.index() == b)
                hasNonJumpPredecessor.set(target.block.index());
        }
    }
    for (unsigned j = 1; j < function.blocks.size(); ++j) {
        if (function.blocks[j].values.isEmpty() || predecessors[j].size() < 2 || hasNonJumpPredecessor.get(j))
            continue;
        ValueID id = function.blocks[j].values[0];
        Value definition = function.at(id);
        if (!isObjectConstructorCall(definition, "defineProperty"_s, 3) || (function.blocks[j].terminator.kind == TerminatorKind::Invoke && function.blocks[j].values.size() == 1))
            continue;
        ValueID descriptor = definition.arguments[2];
        size_t position = function.blocks[j].parameters.find(descriptor);
        if (position == notFound)
            continue;
        bool hasLiteralArgument = false;
        for (unsigned predecessor : predecessors[j]) {
            ValueID given = function.blocks[predecessor].terminator.targets[0].arguments[position];
            hasLiteralArgument |= function.at(given).opcode == Opcode::NewObject && function.blocks[predecessor].values.contains(given);
        }
        if (!hasLiteralArgument)
            continue;
        unsigned uses = 0;
        auto count = [&](ValueID& child) {
            uses += child == descriptor ? 1 : child == id ? 2 : 0;
        };
        for (auto& block : function.blocks) {
            for (ValueID user : block.values)
                function.at(user).forEachChild(count);
            block.terminator.forEachChild(count);
        }
        if (uses != 1)
            continue;
        Vector<ValueID, 4> parameters(function.blocks[j].parameters.span());
        for (unsigned predecessor : predecessors[j]) {
            Arguments& given = function.blocks[predecessor].terminator.targets[0].arguments;
            Value copy = definition;
            copy.forEachChild([&](ValueID& child) {
                if (size_t location = parameters.find(child); location != notFound)
                    child = given[location];
            });
            ValueID copyID(function.values.size());
            function.values.append(WTF::move(copy));
            function.blocks[predecessor].values.append(copyID);
            given.removeAt(position);
        }
        Block& join = function.blocks[j];
        join.values.removeAt(0);
        join.parameters.removeAt(position);
        for (unsigned i = 0; i < join.parameters.size(); ++i)
            function.at(join.parameters[i]).immediates = { j, i };
        ++hoisted;
    }
    return hoisted;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
