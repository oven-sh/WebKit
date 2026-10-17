/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTypedPasses.h"

#if ENABLE(AOT)

#include <wtf/BitVector.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

class CastSimplification {
public:
    CastSimplification(const Program& program, Function& function)
        : m_program(program)
        , m_function(function)
    {
    }

    unsigned run(CleanUpTiming timing)
    {
        unsigned changed = narrowBlockParameters();
        for (;;) {
            unsigned changedInRound = removeCasts();
            changedInRound += removeRepeatedCasts();
            changedInRound += specializeAllocations();
            changedInRound += specializeAllocationsAcrossBlocks();
            if (!changedInRound)
                break;
            changed += changedInRound;
        }
        if (changed && timing == CleanUpTiming::Immediate)
            cleanUp(m_program, m_function);
        updateAllocations();
        return changed;
    }

private:
    const Type& typeOf(ValueID value) const { return m_program.typeOf(m_function, value); }

    ValueID resolve(ValueID value) const
    {
        while (m_replaced[value.index()] != value)
            value = m_replaced[value.index()];
        return value;
    }

    void resetReplacements()
    {
        m_replaced.resize(m_function.values.size());
        for (unsigned i = 0; i < m_replaced.size(); ++i)
            m_replaced[i] = ValueID(i);
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

    bool finds(LayoutID layout, const Field& field) const
    {
        return m_program.at(layout).fieldNamed(field.name)
            || findMethod(m_program, layout, field.name, MethodKind::Method)
            || findMethod(m_program, layout, field.name, MethodKind::Getter);
    }

    bool canReplaceWithOperand(ValueID cast, const Type& operand, const Type& target) const
    {
        if (operand.hasLiteralString() != target.hasLiteralString())
            return false;
        LayoutID own = operand.singleLayout();
        LayoutID supertype = target.singleLayout();
        if (own == supertype)
            return true;
        if (m_isStoredThrough.get(cast.index()))
            return false;
        if (!supertype || !operand.hasObjects())
            return true;
        if (!own)
            return false;
        for (auto& field : m_program.at(supertype).fields) {
            if (!(field.flags & (fieldIsNeverStored | fieldIsPlaceholder)) && !finds(own, field))
                return false;
        }
        return true;
    }

    bool isSubtype(LayoutID created, LayoutID supertype) const
    {
        return std::ranges::binary_search(m_program.at(supertype).subtypes, created);
    }

    bool checksLocalAllocation(const Value& cast, ValueID operand) const
    {
        if (!isCheckOnlyCast(cast) || m_function.at(operand).opcode != Opcode::NewObjectWithShape)
            return false;
        LayoutID supertype = m_program.at(cast.type).singleLayout();
        return supertype && isSubtype(m_function.at(operand).layout(), supertype) && !m_program.at(m_function.at(operand).layout()).acceptsUncheckedObjects();
    }

    bool predictionIsNarrowerThanType(const Value& cast, ValueID operand) const
    {
        if (!cast.hasDifferentPredictionThan(m_function.at(operand)))
            return false;
        return cast.predictedClosure() || cast.predictedByKey() || !typeOf(operand).isSubsetOf(m_program.at(cast.prediction()), m_program);
    }

    unsigned removeCasts()
    {
        resetReplacements();
        m_isStoredThrough.clearAll();
        m_isStoredThrough.ensureSize(m_function.values.size());
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                if (m_function.at(id).opcode != Opcode::PutById)
                    continue;
                for (ValueID receiver = m_function.at(id).child(0); m_function.at(receiver).opcode == Opcode::Cast; receiver = m_function.at(receiver).child(0))
                    m_isStoredThrough.set(receiver.index());
            }
        }
        unsigned changed = 0;
        for (auto& block : m_function.blocks) {
            bool removedAny = false;
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                auto& cast = m_function.at(id);
                if (cast.opcode != Opcode::Cast)
                    continue;
                ValueID operand = resolve(cast.child(0));
                if (cast.castReason() != CastReason::Upcast) {
                    while (m_function.at(operand).opcode == Opcode::Cast && m_function.at(operand).castReason() == CastReason::Upcast && !predictionIsNarrowerThanType(m_function.at(operand), resolve(m_function.at(operand).child(0))))
                        operand = resolve(m_function.at(operand).child(0));
                    cast.children[0] = operand;
                }
                if (!m_program.castIsRedundant(typeOf(operand), typeOf(id), cast) && !checksLocalAllocation(cast, operand))
                    continue;
                if (cast.castReason() == CastReason::Coerce) {
                    auto& coercedTo = m_program.at(cast.coercedType());
                    if ((!typeOf(operand).isSubsetOf(coercedTo, m_program, false) || mayTransitionTo(typeOf(operand), coercedTo)) && coercionMayMutateObjects(m_program, typeOf(operand), coercedTo, cast.coercesFreshData()))
                        continue;
                }
                if ((cast.accessedName() || cast.castReason() == CastReason::NonNull) && typeOf(operand).mayBeUndefinedOrNull())
                    continue;
                if (!predictionIsNarrowerThanType(cast, operand) && canReplaceWithOperand(id, typeOf(operand), typeOf(id))) {
                    m_replaced[id.index()] = operand;
                    block.values[i] = ValueID();
                    removedAny = true;
                } else if (cast.castReason() != CastReason::Upcast)
                    cast.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                else
                    continue;
                ++changed;
                if (i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            if (removedAny) {
                block.values.removeAllMatching([](ValueID id) {
                    return !id;
                });
            }
        }
        if (changed)
            rewrite();
        return changed;
    }

    static bool isCheckOnlyCast(const Value& value)
    {
        if (value.opcode != Opcode::Cast)
            return false;
        return value.castReason() == CastReason::Store || value.castReason() == CastReason::Claim || value.castReason() == CastReason::Flow || value.castReason() == CastReason::NonNull;
    }

    ValueID withoutCheckCasts(ValueID value) const
    {
        for (value = resolve(value); isCheckOnlyCast(m_function.at(value)) || (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast); value = resolve(m_function.at(value).child(0))) { }
        return value;
    }

    bool canAccessThrough(const Value& access, ValueID known) const
    {
        LayoutID supertype = typeOf(known).singleLayout();
        if (!supertype || typeOf(access.child(0)).isSubsetOf(typeOf(known), m_program))
            return false;
        auto field = m_program.at(supertype).fieldNamed(access.string());
        if (!field)
            return false;
        TypeID type = m_program.at(supertype).fields[*field].type;
        return access.opcode != Opcode::PutById || !type || typeOf(access.child(1)).isSubsetOf(m_program.at(type), m_program);
    }

    unsigned removeRepeatedCasts()
    {
        auto isRepeatable = [&](ValueID id) {
            return isCheckOnlyCast(m_function.at(id)) || (m_function.at(id).opcode == Opcode::Cast && (m_function.at(id).castReason() == CastReason::Coerce || m_function.at(id).castReason() == CastReason::As)) || m_function.at(id).opcode == Opcode::NewClosureOnce;
        };
        unsigned numberOfCasts = 0;
        unsigned numberOfTests = 0;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                numberOfCasts += isRepeatable(id);
                numberOfTests += isCheckOnlyCast(m_function.at(id));
            }
        }
        if (numberOfCasts < 2 && !numberOfTests)
            return 0;
        resetReplacements();
        auto keyOf = [&](ValueID id) {
            auto& cast = m_function.at(id);
            if (cast.opcode == Opcode::NewClosureOnce)
                return static_cast<uint64_t>(resolve(cast.child(0)).index()) << 32 | 3u << 30 | cast.field();
            uint32_t type = cast.castReason() == CastReason::Coerce ? cast.coercedType().index() | 1u << 31 : cast.keepsUnconvertibleObjects() ? cast.type.index() | 1u << 29 : cast.type.index();
            return static_cast<uint64_t>(resolve(cast.child(0)).index()) << 32 | type;
        };
        static constexpr unsigned maxTestsToCompare = 64;
        if (numberOfTests <= maxTestsToCompare) {
            Vector<uint64_t, 32> keys;
            Vector<std::pair<ValueID, ValueID>, 16> tests;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    if (!isRepeatable(id))
                        continue;
                    keys.append(keyOf(id));
                    if (isCheckOnlyCast(m_function.at(id)))
                        tests.append({ withoutCheckCasts(id), id });
                }
            }
            std::ranges::sort(keys);
            bool mayChange = std::ranges::adjacent_find(keys) != keys.end();
            for (unsigned b = 0; b < m_function.blocks.size() && !mayChange && !tests.isEmpty(); ++b) {
                for (ValueID id : m_function.blocks[b].values) {
                    auto& value = m_function.at(id);
                    if (value.opcode != Opcode::GetById && value.opcode != Opcode::PutById && value.opcode != Opcode::InById && value.opcode != Opcode::CallMethod)
                        continue;
                    ValueID tested = withoutCheckCasts(value.child(0));
                    for (auto& [of, test] : tests)
                        mayChange |= of == tested && canAccessThrough(value, test);
                }
            }
            if (!mayChange)
                return 0;
        }

        unsigned numberOfBlocks = m_function.blocks.size();
        static constexpr unsigned unknown = UINT_MAX;
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
        Vector<Vector<unsigned, 2>> dominated(numberOfBlocks);
        for (unsigned i = 1; i < order.size(); ++i)
            dominated[dominator[order[i]]].append(order[i]);

        unsigned changed = 0;
        UncheckedKeyHashMap<uint64_t, ValueID, IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> passed;
        Vector<uint64_t> added;
        UncheckedKeyHashMap<uint32_t, ValueID, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> knownType;
        Vector<std::pair<uint32_t, ValueID>> knownBefore;
        auto add = [&](ValueID id) {
            if (passed.add(keyOf(id), id).isNewEntry)
                added.append(keyOf(id));
            if (!isCheckOnlyCast(m_function.at(id)))
                return;
            uint32_t tested = withoutCheckCasts(id).index();
            ValueID before = knownType.get(tested);
            if (before && typeOf(before).isSubsetOf(typeOf(id), m_program))
                return;
            knownBefore.append({ tested, before });
            knownType.set(tested, id);
        };
        auto enter = [&](unsigned b) {
            auto& block = m_function.blocks[b];
            if (predecessors[b].size() == 1) {
                auto& before = m_function.blocks[predecessors[b][0]];
                auto& terminator = before.terminator;
                if (terminator.kind == TerminatorKind::Invoke && terminator.targets[0].block.index() == b && terminator.targets[1].block.index() != b && !before.values.isEmpty() && isRepeatable(before.values.last()))
                    add(before.values.last());
            }
            for (unsigned i = 0; i < block.values.size(); ++i) {
                ValueID id = block.values[i];
                auto& value = m_function.at(id);
                if (value.opcode == Opcode::GetById || value.opcode == Opcode::PutById || value.opcode == Opcode::InById || value.opcode == Opcode::CallMethod) {
                    if (ValueID known = knownType.get(withoutCheckCasts(value.child(0)).index()); known && canAccessThrough(value, known)) {
                        value.children[0] = known;
                        ++changed;
                    }
                    continue;
                }
                if (!isRepeatable(id))
                    continue;
                bool isWatched = i + 1 == block.values.size() && block.terminator.kind == TerminatorKind::Invoke;
                ValueID earlier = passed.get(keyOf(id));
                if (earlier && m_function.at(earlier).type != value.type)
                    earlier = { };
                if (!earlier) {
                    if (!isWatched)
                        add(id);
                    continue;
                }
                m_replaced[id.index()] = earlier;
                block.values[i] = ValueID();
                ++changed;
                if (isWatched) {
                    block.terminator.kind = TerminatorKind::Jump;
                    block.terminator.targets.shrink(1);
                }
            }
            block.values.removeAllMatching([](ValueID id) {
                return !id;
            });
        };
        struct Visit {
            unsigned block;
            unsigned next;
            unsigned addedBeforeCount;
            unsigned knownBeforeCount;
        };
        Vector<Visit> visits { Visit { 0, 0, 0, 0 } };
        enter(0);
        while (!visits.isEmpty()) {
            auto& visit = visits.last();
            if (visit.next < dominated[visit.block].size()) {
                unsigned inner = dominated[visit.block][visit.next++];
                visits.append(Visit { inner, 0, static_cast<unsigned>(added.size()), static_cast<unsigned>(knownBefore.size()) });
                enter(inner);
                continue;
            }
            while (added.size() > visit.addedBeforeCount)
                passed.remove(added.takeLast());
            while (knownBefore.size() > visit.knownBeforeCount) {
                auto [tested, before] = knownBefore.takeLast();
                if (before)
                    knownType.set(tested, before);
                else
                    knownType.remove(tested);
            }
            visits.removeLast();
        }
        if (changed)
            rewrite();
        return changed;
    }

    ValueID withoutUpcasts(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast && m_function.at(value).castReason() == CastReason::Upcast)
            value = m_function.at(value).child(0);
        return value;
    }

    TypeID commonType(TypeID first, TypeID second) const
    {
        if (!first || first == second)
            return second;
        auto& a = m_program.at(first);
        auto& b = m_program.at(second);
        if (b.isSubsetOf(a, m_program, false))
            return first;
        if (a.isSubsetOf(b, m_program, false))
            return second;
        if (a.isInteger() && b.isInteger()) {
            for (TypeID candidate : { m_program.types.int32, m_program.types.integer }) {
                if (a.isSubsetOf(m_program.at(candidate), m_program) && b.isSubsetOf(m_program.at(candidate), m_program))
                    return candidate;
            }
        }
        return { };
    }

    unsigned narrowBlockParameters()
    {
        unsigned numberOfBlocks = m_function.blocks.size();
        bool hasCandidate = false;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                auto& parameters = m_function.at(target.block).parameters;
                for (unsigned i = 0; i < target.arguments.size() && i < parameters.size() && !hasCandidate; ++i)
                    hasCandidate = m_function.at(withoutUpcasts(target.arguments[i])).type != m_function.at(parameters[i]).type;
            }
        }
        if (!hasCandidate)
            return 0;

        static constexpr unsigned none = UINT_MAX;
        Vector<unsigned> firstParameter(numberOfBlocks);
        Vector<unsigned> numberOf(FillWith { }, m_function.values.size(), none);
        unsigned numberOfParameters = 0;
        for (unsigned b = 0; b < numberOfBlocks; ++b) {
            firstParameter[b] = numberOfParameters;
            for (ValueID parameter : m_function.blocks[b].parameters)
                numberOf[parameter.index()] = numberOfParameters++;
        }
        Vector<Vector<ValueID, 2>> given(numberOfParameters);
        BitVector staysAsDeclared(numberOfParameters);
        for (unsigned k = 0; k < m_function.blocks[0].parameters.size(); ++k)
            staysAsDeclared.set(k);
        BitVector isReachable(numberOfBlocks);
        Vector<unsigned, 16> worklist { 0u };
        isReachable.set(0);
        while (!worklist.isEmpty()) {
            for (auto& target : m_function.blocks[worklist.takeLast()].terminator.targets) {
                unsigned b = target.block.index();
                if (!isReachable.set(b))
                    worklist.append(b);
                unsigned count = m_function.blocks[b].parameters.size();
                for (unsigned i = 0; i < count; ++i) {
                    if (target.arguments.size() != count)
                        staysAsDeclared.set(firstParameter[b] + i);
                    else
                        given[firstParameter[b] + i].append(withoutUpcasts(target.arguments[i]));
                }
            }
        }

        Vector<TypeID> found(numberOfParameters);
        for (bool changed = true; changed;) {
            changed = false;
            for (unsigned b = 1; b < numberOfBlocks; ++b) {
                auto& parameters = m_function.blocks[b].parameters;
                for (unsigned i = 0; i < parameters.size(); ++i) {
                    unsigned k = firstParameter[b] + i;
                    if (staysAsDeclared.get(k))
                        continue;
                    TypeID declared = m_function.at(parameters[i]).type;
                    TypeID common = found[k];
                    bool fails = !declared;
                    for (ValueID argument : given[k]) {
                        unsigned other = numberOf[argument.index()];
                        if (other == k)
                            continue;
                        bool isNarrowed = other != none && !staysAsDeclared.get(other);
                        TypeID type = isNarrowed ? found[other] : m_function.at(argument).type;
                        if (isNarrowed && !type)
                            continue;
                        common = type ? commonType(common, type) : TypeID();
                        if (!common) {
                            fails = true;
                            break;
                        }
                    }
                    if (!fails && common)
                        fails = common == declared || !m_program.at(common).isSubsetOf(m_program.at(declared), m_program, false);
                    if (fails) {
                        staysAsDeclared.set(k);
                        changed = true;
                    } else if (common != found[k]) {
                        found[k] = common;
                        changed = true;
                    }
                }
            }
        }

        unsigned changed = 0;
        for (unsigned b = 1; b < numberOfBlocks; ++b) {
            unsigned at = !m_function.blocks[b].values.isEmpty() && m_function.at(m_function.blocks[b].values[0]).opcode == Opcode::CatchValue ? 1 : 0;
            for (unsigned i = 0; i < m_function.blocks[b].parameters.size(); ++i) {
                unsigned k = firstParameter[b] + i;
                if (staysAsDeclared.get(k) || !found[k])
                    continue;
                ValueID parameter = m_function.blocks[b].parameters[i];
                Value narrowed = m_function.at(parameter);
                narrowed.type = found[k];
                narrowed.rep = repOf(m_program.at(found[k]));
                ValueID created(m_function.values.size());
                m_function.values.append(WTF::move(narrowed));
                m_function.blocks[b].parameters[i] = created;
                auto& declared = m_function.at(parameter);
                declared.opcode = Opcode::Cast;
                declared.children = { created };
                declared.immediates = { Format::none, static_cast<uint32_t>(CastReason::Upcast) };
                m_function.blocks[b].values.insert(at++, parameter);
                ++changed;
            }
        }
        if (!changed)
            return 0;
        for (auto& block : m_function.blocks) {
            for (auto& target : block.terminator.targets) {
                unsigned b = target.block.index();
                if (target.arguments.size() != m_function.blocks[b].parameters.size())
                    continue;
                for (unsigned i = 0; i < target.arguments.size(); ++i) {
                    unsigned k = firstParameter[b] + i;
                    if (!staysAsDeclared.get(k) && found[k])
                        target.arguments[i] = withoutUpcasts(target.arguments[i]);
                }
            }
        }
        return changed;
    }

    ValueID originOf(ValueID value) const
    {
        while (m_function.at(value).opcode == Opcode::Cast)
            value = m_function.at(value).child(0);
        return value;
    }

    const Field& storedFieldOf(const Value& store) const
    {
        if (store.opcode == Opcode::PutField)
            return m_program.at(store.layout()).fields[store.field()];
        auto& layout = m_program.at(m_function.at(store.child(0)).layout());
        return layout.fields[*layout.fieldNamed(store.string())];
    }

    unsigned specializeAllocations()
    {
        unsigned changed = 0;
        for (auto& block : m_function.blocks) {
            for (unsigned k = 0; k < block.values.size(); ++k) {
                auto& cast = m_function.at(block.values[k]);
                if (cast.opcode == Opcode::Cast && (cast.castReason() == CastReason::As || cast.castReason() == CastReason::Coerce))
                    changed += specializeAllocationFor(block, k);
            }
        }
        return changed;
    }

    bool isAllowedLiteral(ValueID value, const Field& field) const
    {
        auto& type = typeOf(value);
        if (!type.isInteger())
            return false;
        ValueID constant = m_program.constantOf(m_function, value);
        Range range = constant && m_function.at(constant).opcode == Opcode::Integer ? Range::one(m_function.at(constant).integer()) : type.integers;
        if (static_cast<uint64_t>(range.maximum) - static_cast<uint64_t>(range.minimum) >= field.literals.size())
            return false;
        for (int64_t number = range.minimum; number <= range.maximum; ++number) {
            bool isListed = field.literals.containsIf([&](auto& literal) {
                return !literal.string && literal.number == static_cast<double>(number);
            });
            if (!isListed)
                return false;
        }
        return true;
    }

    bool specializeAllocationFor(Block& block, unsigned k)
    {
        auto& cast = m_function.at(block.values[k]);
        LayoutID target = m_program.at(cast.castReason() == CastReason::Coerce ? cast.coercedType() : cast.type).singleLayout();
        if (!target)
            return false;
        ValueID object = cast.child(0);
        while (m_function.at(object).opcode == Opcode::Cast) {
            if (m_function.at(object).castReason() != CastReason::Upcast)
                return false;
            object = m_function.at(object).child(0);
        }
        if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
            return false;
        LayoutID created = m_function.at(object).layout();
        auto& from = m_program.at(created);
        auto& to = m_program.at(target);
        bool isOfClass = from.kind == LayoutKind::Instance && from.isEvaluatedOnce && from.constructor && m_program.genericEntrypoints.contains(from.constructor.index());
        for (LayoutID next = created; isOfClass && next; next = m_program.at(next).base)
            isOfClass = m_program.at(next).methods.isEmpty();
        if ((from.kind != LayoutKind::Record && !isOfClass) || to.kind != LayoutKind::Record)
            return false;
        if (!std::ranges::binary_search(from.specializations, target) || !m_program.isUpcast(target, created))
            return false;
        size_t allocationSite = block.values.find(object);
        if (allocationSite == notFound || allocationSite >= k)
            return false;

        for (size_t i = allocationSite + 1; i < k; ++i) {
            auto& value = m_function.at(block.values[i]);
            bool usesObject = false;
            value.forEachChild([&](ValueID child) {
                usesObject |= originOf(child) == object;
            });
            bool storesToTargetField = value.opcode == Opcode::PutById && value.child(0) == object && value.child(1) != object && !from.fieldNamed(value.string()) && to.fieldNamed(value.string()) && !mayInheritAtCreation(m_program, m_function, object, value.string());
            if (usesObject && !(value.opcode == Opcode::Cast && value.castReason() == CastReason::Upcast) && !isInitializingStore(m_program, m_function, block, i) && !storesToTargetField)
                return false;
        }
        Vector<std::pair<ValueID, uint32_t>, 8> stores;
        for (auto& other : m_function.blocks) {
            for (ValueID id : other.values) {
                auto& store = m_function.at(id);
                if ((store.opcode != Opcode::PutField && store.opcode != Opcode::PutById) || store.child(0) != object)
                    continue;
                StringID name = store.opcode == Opcode::PutField ? m_program.at(store.layout()).fields[store.field()].name : store.string();
                auto field = to.fieldNamed(name);
                if (!field && store.opcode == Opcode::PutById && !from.fieldNamed(name))
                    continue;
                if (!field)
                    return false;
                TypeID type = to.fields[*field].type;
                if (type && !typeOf(store.child(1)).isSubsetOf(m_program.at(type), m_program))
                    return false;
                stores.append({ id, *field });
            }
        }
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.layout = target;
        if (isOfClass)
            allocation.constructedBy = from.constructor;
        if (allocation.discriminant != UINT16_MAX) {
            auto mapped = to.fieldNamed(from.fields[allocation.discriminant].name);
            if (!mapped || !isAllowedLiteral(m_function.at(object).child(0), to.fields[*mapped]))
                return false;
            allocation.discriminant = static_cast<uint16_t>(*mapped);
        }
        for (uint16_t& field : allocation.order) {
            auto mapped = to.fieldNamed(from.fields[field].name);
            if (!mapped)
                return false;
            field = *mapped;
        }

        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        auto& allocated = m_function.at(object);
        allocated.immediates = { target.index(), number };
        allocated.type = to.type;
        allocated.rep = repOf(m_program.at(to.type));
        for (auto [store, field] : stores) {
            if (m_function.at(store).opcode == Opcode::PutField)
                m_function.at(store).immediates = { target.index(), field };
        }
        return true;
    }

    bool mayTransitionTo(const Type& from, const Type& to) const
    {
        for (LayoutID target : to.objectLayouts) {
            if (!m_program.at(target).isAllocated)
                continue;
            for (LayoutID supertype : from.objectLayouts) {
                if (to.objectLayouts.contains(supertype))
                    continue;
                for (LayoutID subtype : m_program.at(supertype).subtypes) {
                    if (std::ranges::binary_search(m_program.at(subtype).specializations, target))
                        return true;
                }
            }
        }
        return false;
    }

    bool allReturnsPassThrough(unsigned from, const BitVector& through) const
    {
        if (through.get(from))
            return true;
        BitVector seen = through;
        Vector<unsigned, 16> worklist { from };
        seen.set(from);
        while (!worklist.isEmpty()) {
            auto& terminator = m_function.blocks[worklist.takeLast()].terminator;
            if (terminator.kind == TerminatorKind::Return)
                return false;
            for (auto& target : terminator.targets) {
                if (!seen.set(target.block.index()))
                    worklist.append(target.block.index());
            }
        }
        return true;
    }

    LayoutID targetOf(const Value& cast) const
    {
        return m_program.at(cast.castReason() == CastReason::Coerce ? cast.coercedType() : cast.type).singleLayout();
    }

    bool specializeAllocationAcrossBlocks(ValueID object, unsigned allocationSite)
    {
        LayoutID created = m_function.at(object).layout();
        auto& from = m_program.at(created);
        if (from.kind != LayoutKind::Record)
            return false;
        auto isNoOpCast = [&](const Value& cast) {
            if (cast.castReason() == CastReason::Upcast)
                return true;
            LayoutID target = targetOf(cast);
            if (!target || !isSubtype(created, target))
                return false;
            return isCheckOnlyCast(cast) || !std::ranges::binary_search(from.specializations, target);
        };
        BitVector isSameObject(m_function.values.size());
        isSameObject.set(object.index());
        for (unsigned rounds = 0;; ++rounds) {
            if (rounds == 8)
                return false;
            bool changed = false;
            for (auto& block : m_function.blocks) {
                for (ValueID id : block.values) {
                    auto& value = m_function.at(id);
                    if (value.opcode == Opcode::Cast && isSameObject.get(value.child(0).index()) && isNoOpCast(value))
                        changed |= !isSameObject.set(id.index());
                }
                for (auto& target : block.terminator.targets) {
                    for (unsigned i = 0; i < target.arguments.size(); ++i) {
                        if (isSameObject.get(target.arguments[i].index()))
                            changed |= !isSameObject.set(m_function.at(target.block).parameters[i].index());
                    }
                }
            }
            if (!changed)
                break;
        }

        Vector<std::pair<LayoutID, BitVector>, 4> askedFor;
        auto noteAsked = [&](LayoutID asked, unsigned block) {
            for (auto& [layout, blocks] : askedFor) {
                if (layout == asked) {
                    blocks.set(block);
                    return;
                }
            }
            askedFor.append({ asked, BitVector(m_function.blocks.size()) });
            askedFor.last().second.set(block);
        };
        Vector<ValueID, 8> storesIntoObject;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            auto& block = m_function.blocks[b];
            for (ValueID id : block.values) {
                auto& value = m_function.at(id);
                bool usesValue = false;
                value.forEachChild([&](ValueID child) {
                    usesValue |= isSameObject.get(child.index());
                });
                if (!usesValue)
                    continue;
                if (value.opcode == Opcode::Cast) {
                    if (LayoutID asked = targetOf(value); asked && asked != created && std::ranges::binary_search(from.specializations, asked))
                        noteAsked(asked, b);
                    continue;
                }
                if ((value.opcode != Opcode::PutById && value.opcode != Opcode::PutField) || !isSameObject.get(value.child(0).index()) || isSameObject.get(value.child(1).index()))
                    continue;
                storesIntoObject.append(id);
            }
        }
        LayoutID target;
        for (auto& [asked, blocks] : askedFor) {
            auto& candidate = m_program.at(asked);
            if (!m_program.isUpcast(asked, created) || candidate.kind != LayoutKind::Record || candidate.isAbstract || !allReturnsPassThrough(allocationSite, blocks))
                continue;
            target = asked;
            break;
        }
        if (!target)
            return false;
        auto& to = m_program.at(target);

        unsigned numberOfInitialStores = 0;
        unsigned initializedFieldCount = 0;
        Vector<std::pair<ValueID, uint32_t>, 8> storesToAllocation;
        for (ValueID id : storesIntoObject) {
            auto& store = m_function.at(id);
            if (store.opcode == Opcode::PutField && store.child(0) != object)
                return false;
            StringID name = store.opcode == Opcode::PutField ? m_program.at(store.layout()).fields[store.field()].name : store.string();
            auto field = to.fieldNamed(name);
            if (!field || mayInheritAtCreation(m_program, m_function, object, name) || inheritsThroughLayoutPrototype(m_program, target, name))
                return false;
            TypeID type = to.fields[*field].type;
            if (type && !typeOf(store.child(1)).isSubsetOf(m_program.at(type), m_program))
                return false;
            if (store.child(0) == object)
                storesToAllocation.append({ id, *field });
            if (store.opcode == Opcode::PutById && store.isInitialStore()) {
                ++numberOfInitialStores;
                size_t at = m_function.blocks[allocationSite].values.find(id);
                initializedFieldCount += at != notFound && isInitializingStore(m_program, m_function, m_function.blocks[allocationSite], at);
            }
        }
        if (initializedFieldCount && initializedFieldCount != numberOfInitialStores)
            return false;
        Allocation allocation = m_function.allocations[m_function.at(object).allocation()];
        allocation.layout = target;
        if (allocation.discriminant != UINT16_MAX) {
            auto mapped = to.fieldNamed(from.fields[allocation.discriminant].name);
            if (!mapped || !isAllowedLiteral(m_function.at(object).child(0), to.fields[*mapped]))
                return false;
            allocation.discriminant = static_cast<uint16_t>(*mapped);
        }
        for (uint16_t& field : allocation.order) {
            auto mapped = to.fieldNamed(from.fields[field].name);
            if (!mapped)
                return false;
            field = *mapped;
        }
        uint32_t number = m_function.allocationOf(WTF::move(allocation));
        auto& allocated = m_function.at(object);
        allocated.immediates = { target.index(), number };
        allocated.type = to.type;
        allocated.rep = repOf(m_program.at(to.type));
        for (auto [store, field] : storesToAllocation) {
            if (m_function.at(store).opcode == Opcode::PutField)
                m_function.at(store).immediates = { target.index(), field };
        }
        return true;
    }

    unsigned specializeAllocationsAcrossBlocks()
    {
        Vector<LayoutID, 8> targets;
        for (auto& block : m_function.blocks) {
            for (ValueID id : block.values) {
                auto& cast = m_function.at(id);
                if (cast.opcode != Opcode::Cast || (cast.castReason() != CastReason::As && cast.castReason() != CastReason::Coerce))
                    continue;
                if (LayoutID target = targetOf(cast); target && !targets.contains(target))
                    targets.append(target);
            }
        }
        if (targets.isEmpty())
            return 0;
        unsigned changed = 0;
        for (unsigned b = 0; b < m_function.blocks.size(); ++b) {
            for (size_t i = 0; i < m_function.blocks[b].values.size(); ++i) {
                ValueID id = m_function.blocks[b].values[i];
                if (m_function.at(id).opcode != Opcode::NewObjectWithShape)
                    continue;
                auto& specializations = m_program.at(m_function.at(id).layout()).specializations;
                bool isRequested = targets.containsIf([&](LayoutID target) {
                    return std::ranges::binary_search(specializations, target);
                });
                if (isRequested)
                    changed += specializeAllocationAcrossBlocks(id, b);
            }
        }
        return changed;
    }

    void updateAllocations()
    {
        for (auto& block : m_function.blocks) {
            for (size_t i = 0; i < block.values.size(); ++i) {
                ValueID object = block.values[i];
                if (m_function.at(object).opcode != Opcode::NewObjectWithShape)
                    continue;
                auto& layout = m_program.at(m_function.at(object).layout());
                if (layout.kind != LayoutKind::Record)
                    continue;
                Allocation allocation;
                allocation.layout = m_function.at(object).layout();
                allocation.constructedBy = m_function.allocations[m_function.at(object).allocation()].constructedBy;
                allocation.discriminant = m_function.allocations[m_function.at(object).allocation()].discriminant;
                ImmutableFunctionScope immutableFunctionScope;
                BitVector isInOrder;
                for (size_t next = i + 1; next < block.values.size(); ++next) {
                    if (m_function.at(block.values[next]).opcode == Opcode::NewClosure)
                        continue;
                    if (!isInitializingStore(m_program, m_function, block, next))
                        break;
                    uint16_t field = *layout.fieldNamed(storedFieldOf(m_function.at(block.values[next])).name);
                    if (!isInOrder.set(field))
                        allocation.order.append(field);
                }
                allocation.initialFieldCount = allocation.order.size();
                m_function.at(object).immediates[1] = m_function.allocationOf(WTF::move(allocation));
            }
        }
    }

    const Program& m_program;
    Function& m_function;
    Vector<ValueID> m_replaced;
    BitVector m_isStoredThrough;
};

} // anonymous namespace

unsigned simplifyCasts(const Program& program, Function& function, CleanUpTiming timing)
{
    return CastSimplification(program, function).run(timing);
}

unsigned convertCountersToIntegers(const Program& program, Function& function)
{
    static constexpr int64_t maxStep = 1024;
    unsigned numberOfBlocks = function.blocks.size();
    bool hasCandidate = false;
    for (unsigned b = 1; b < numberOfBlocks && !hasCandidate; ++b) {
        for (ValueID parameter : function.blocks[b].parameters)
            hasCandidate |= function.at(parameter).type == program.types.number;
    }
    if (!hasCandidate)
        return 0;

    Vector<Vector<const Arguments*, 2>> incomingEdges(numberOfBlocks);
    Vector<unsigned> definingBlock(FillWith { }, function.values.size(), UINT_MAX);
    for (unsigned b = 0; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        for (ValueID id : block.values)
            definingBlock[id.index()] = b;
        for (auto& target : block.terminator.targets)
            incomingEdges[target.block.index()].append(&target.arguments);
    }
    auto reaches = [&](unsigned from, unsigned to, unsigned barrier, unsigned otherBarrier) {
        if (from == barrier || from == otherBarrier)
            return false;
        BitVector seen(numberOfBlocks);
        Vector<unsigned, 16> worklist { from };
        seen.set(from);
        while (!worklist.isEmpty()) {
            unsigned block = worklist.takeLast();
            if (block == to)
                return true;
            for (auto& target : function.blocks[block].terminator.targets) {
                unsigned next = target.block.index();
                if (next != barrier && next != otherBarrier && !seen.set(next))
                    worklist.append(next);
            }
        }
        return false;
    };

    unsigned changed = 0;
    for (unsigned b = 1; b < numberOfBlocks; ++b) {
        auto& block = function.blocks[b];
        auto& terminator = block.terminator;
        if (terminator.kind != TerminatorKind::Branch || terminator.targets.size() != 2 || definingBlock[terminator.value.index()] != b)
            continue;
        for (unsigned index = 0; index < block.parameters.size(); ++index) {
            ValueID parameter = block.parameters[index];
            if (function.at(parameter).type != program.types.number)
                continue;
            auto isCounter = [&](ValueID id) {
                return id == parameter || (function.at(id).opcode == Opcode::ToNumeric && function.at(id).child(0) == parameter);
            };
            auto& test = function.at(terminator.value);
            bool counterIsFirst = test.child(0) && isCounter(test.child(0));
            if (!test.child(1) || (!counterIsFirst && !isCounter(test.child(1))))
                continue;
            ValueID bound = test.child(counterIsFirst ? 1 : 0);
            auto& boundType = program.typeOf(function, bound);
            if (!boundType.isInteger() || isCounter(bound))
                continue;
            Opcode relation = test.opcode;
            if (!counterIsFirst) {
                switch (relation) {
                case Opcode::Less:
                    relation = Opcode::Greater;
                    break;
                case Opcode::LessEq:
                    relation = Opcode::GreaterEq;
                    break;
                case Opcode::Greater:
                    relation = Opcode::Less;
                    break;
                case Opcode::GreaterEq:
                    relation = Opcode::LessEq;
                    break;
                default:
                    break;
                }
            }
            std::array<std::optional<int64_t>, 2> minAfter;
            std::array<std::optional<int64_t>, 2> maxAfter;
            switch (relation) {
            case Opcode::Less:
                maxAfter[0] = boundType.integers.maximum - 1;
                minAfter[1] = boundType.integers.minimum;
                break;
            case Opcode::LessEq:
                maxAfter[0] = boundType.integers.maximum;
                minAfter[1] = boundType.integers.minimum + 1;
                break;
            case Opcode::Greater:
                minAfter[0] = boundType.integers.minimum + 1;
                maxAfter[1] = boundType.integers.maximum;
                break;
            case Opcode::GreaterEq:
                minAfter[0] = boundType.integers.minimum;
                maxAfter[1] = boundType.integers.maximum - 1;
                break;
            default:
                continue;
            }

            struct Step {
                ValueID value;
                std::optional<int64_t> minimum;
                std::optional<int64_t> maximum;
            };
            Vector<Step, 2> steps;
            Vector<TypeID, 4> candidates;
            Range entering = Range::empty();
            bool isRegular = true;
            for (auto* arguments : incomingEdges[b]) {
                if (arguments->size() != block.parameters.size()) {
                    isRegular = false;
                    break;
                }
                ValueID argument = arguments->at(index);
                if (argument == parameter)
                    continue;
                auto& value = function.at(argument);
                if (auto& type = program.at(value.type); type.isInteger()) {
                    entering = entering.unionWith(type.integers);
                    if (!candidates.contains(value.type))
                        candidates.append(value.type);
                    continue;
                }
                bool isStep = (value.opcode == Opcode::Add || value.opcode == Opcode::Sub) && isCounter(value.child(0)) && function.at(value.child(1)).opcode == Opcode::Integer && value.type == program.types.number;
                int64_t by = isStep ? function.at(value.child(1)).integer() : 0;
                if (by < 1 || by > maxStep) {
                    isRegular = false;
                    break;
                }
                bool isIncreasing = value.opcode == Opcode::Add;
                unsigned at = definingBlock[argument.index()];
                std::optional<unsigned> side;
                for (unsigned i = 0; i < 2; ++i) {
                    unsigned target = terminator.targets[i].block.index();
                    unsigned other = terminator.targets[1 - i].block.index();
                    if (!(isIncreasing ? maxAfter[i] : minAfter[i]) || target == other || target == b || incomingEdges[target].size() != 1)
                        continue;
                    if (at != UINT_MAX && at != b && !reaches(other, at, b, target))
                        side = i;
                }
                if (!side) {
                    isRegular = false;
                    break;
                }
                if (isIncreasing)
                    steps.append({ argument, std::nullopt, *maxAfter[*side] + by });
                else
                    steps.append({ argument, *minAfter[*side] - by, std::nullopt });
            }
            if (!isRegular || entering.isEmpty() || steps.isEmpty())
                continue;
            if (!candidates.contains(function.at(bound).type))
                candidates.append(function.at(bound).type);
            TypeID found;
            for (TypeID candidate : candidates) {
                Range range = program.at(candidate).integers;
                bool holds = entering.isSubsetOf(range);
                for (auto& step : steps)
                    holds &= (!step.minimum || *step.minimum >= range.minimum) && (!step.maximum || *step.maximum <= range.maximum);
                if (holds) {
                    found = candidate;
                    break;
                }
            }
            if (!found)
                continue;
            auto retype = [&](Value& value) {
                value.type = found;
                value.rep = repOf(program.at(found));
                value.copyPredictionFrom(Value { });
            };
            retype(function.at(parameter));
            for (auto& step : steps)
                retype(function.at(step.value));
            for (auto& value : function.values) {
                if (value.opcode == Opcode::ToNumeric && value.child(0) == parameter)
                    retype(value);
            }
            ++changed;
        }
    }
    return changed;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
