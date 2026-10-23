/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEmbedConstantCollections.h"

#if ENABLE(AOT)

#include "AOTAnalysisDump.h"
#include "AOTArrayStorageForms.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTIRPrinter.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "AOTVector.h"
#include "ArrayConventions.h"
#include "PureNaN.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantNodeOpcodes { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

namespace {

struct ConstructionSite {
    ValueID construction;
    ConstantCollectionKind kind;
    DataID existingData;
    DataNode contents;
    Vector<ValueID> deadValues;
    DataID data;
    // A collection of constants that nobody reads is made for nothing: no code runs, and only the lack of memory keeps it from being made.
    bool isUnused { false };
};

struct UnwrittenLiteral {
    ValueID literal;
    DataNode contents;
    DataID data;
};

struct DictionaryLiteral {
    ValueID dictionary;
    DataNode::Object contents;
    Vector<ValueID> deadValues;
    // Every name that the literal defines, in the order of the definitions: those of the contents, then those that are defined with something else.
    Vector<PropertyKeyID> names;
    bool runsAtMostOnce { false };
    DataID data;
};

struct FunctionSites {
    FunctionID function;
    Vector<ConstructionSite> sites;
    Vector<UnwrittenLiteral> unwrittenLiterals;
    Vector<DictionaryLiteral> dictionaries;
};

using KeyIdentity = std::pair<uint8_t, uint64_t>;
using KeyIdentityTraits = WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint8_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
using PositionsByKey = UncheckedKeyHashMap<KeyIdentity, uint32_t, DefaultHash<KeyIdentity>, KeyIdentityTraits>;
using RecordsByHash = UncheckedKeyHashMap<uint64_t, Vector<DataID, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

} // anonymous namespace

static AnalysisContext contextOf(const AnalysisContext& programContext, FunctionID id)
{
    return { programContext.build, programContext.program, programContext.flow, programContext.effects, id };
}

static Opcode operationFor(ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return Opcode::NewSetFromData;
    case ConstantCollectionKind::Map:
        return Opcode::NewMapFromData;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

template<typename Node>
static Node& keyOf(Node& entry, ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return entry;
    case ConstantCollectionKind::Map:
        return entry.template as<DataNode::Array>().elements[0];
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isPrimitive(const DataNode& node)
{
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Boolean:
    case DataNode::Kind::Number:
    case DataNode::Kind::String:
    case DataNode::Kind::BigInt:
        return true;
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static bool isHole(const DataNode& node)
{
    return node.is<DataNode::Hole>();
}

static bool isUsableAs(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    if (contents.elements.isEmpty())
        return false;
    return std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        switch (kind) {
        case ConstantCollectionKind::Set:
            return !isHole(entry);
        case ConstantCollectionKind::Map: {
            const auto* pair = entry.tryAs<DataNode::Array>();
            return pair && pair->elements.size() == 2 && std::ranges::none_of(pair->elements, isHole);
        }
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool createsObjects(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return !std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        switch (kind) {
        case ConstantCollectionKind::Set:
            return isPrimitive(entry);
        case ConstantCollectionKind::Map:
            return std::ranges::all_of(entry.as<DataNode::Array>().elements, isPrimitive);
        }
        RELEASE_ASSERT_NOT_REACHED();
    });
}

static bool isNegativeZero(const DataNode& node)
{
    const auto* number = node.tryAs<DataNode::Number>();
    return number && !number->value && std::signbit(number->value);
}

static std::optional<KeyIdentity> identityOf(const DataNode& key)
{
    auto identity = [&](uint64_t payload) {
        return KeyIdentity { static_cast<uint8_t>(key.kind()), payload };
    };

    switch (key.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
        return identity(0);
    case DataNode::Kind::Boolean:
        return identity(key.as<DataNode::Boolean>().value);
    case DataNode::Kind::Number: {
        double number = key.as<DataNode::Number>().value;
        return identity(std::bit_cast<uint64_t>(number ? purifyNaN(number) : 0.0));
    }
    case DataNode::Kind::String:
        return identity(key.as<DataNode::String>().value.index());
    case DataNode::Kind::BigInt:
        return identity(key.as<DataNode::BigInt>().digits.index());
    case DataNode::Kind::Hole:
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        return std::nullopt;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static std::optional<Vector<DataNode>> removeRepeatedKeys(std::span<const DataNode> entries, ConstantCollectionKind kind)
{
    PositionsByKey positions;
    Vector<uint32_t> sources;
    sources.reserveInitialCapacity(entries.size());
    bool changesEntries = false;
    for (uint32_t index = 0; index < entries.size(); ++index) {
        const DataNode& key = keyOf(entries[index], kind);
        changesEntries |= isNegativeZero(key);
        if (std::optional<KeyIdentity> identity = identityOf(key)) {
            auto position = positions.add(*identity, static_cast<uint32_t>(sources.size()));
            if (!position.isNewEntry) {
                // A repeated key keeps its first position and takes the last value. Equal keys are interchangeable.
                sources[position.iterator->value] = index;
                changesEntries = true;
                continue;
            }
        }
        sources.append(index);
    }
    if (!changesEntries)
        return std::nullopt;

    Vector<DataNode> result;
    result.reserveInitialCapacity(sources.size());
    for (uint32_t source : sources) {
        DataNode entry = entries[source];
        if (isNegativeZero(keyOf(entry, kind)))
            keyOf(entry, kind) = DataNode(DataNode::Number { 0 });
        result.append(WTF::move(entry));
    }
    return result;
}

static std::optional<DataNode> constantNodeOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constantID = constantThroughCasts(program, function, id);
    if (!constantID)
        return std::nullopt;
    const Value& constant = function.at(*constantID);
    if (!constantNodeOpcodes.contains(constant.opcode))
        return std::nullopt;

    switch (constant.opcode) {
    case Opcode::Undefined:
        return DataNode(DataNode::Undefined { });
    case Opcode::Null:
        return DataNode(DataNode::Null { });
    case Opcode::Boolean:
        return DataNode(DataNode::Boolean { constant.boolean() });
    case Opcode::Integer:
        return DataNode(DataNode::Number { static_cast<double>(constant.integer()) });
    case Opcode::Double:
        return DataNode(DataNode::Number { constant.number() });
    case Opcode::String:
        return DataNode(DataNode::String { constant.string() });
    case Opcode::BigInt:
        return DataNode(DataNode::BigInt { constant.digits() });
    default:
        RELEASE_ASSERT_NOT_REACHED();
    }
}

static bool isNeverStoredInto(const FlowResults& flow, const Value& allocation)
{
    AllocationSiteID site = allocation.allocationSite();
    AllocationClassID allocationClass = site ? flow.at(site).allocationClass : AllocationClassID();
    return allocationClass && flow.at(allocationClass).impossibleMutations.contains(ObjectMutation::StoreAfterCreation);
}

// The arrays of a literal of constants that the optimizer has left in the heap, and that no store of the program reaches, are cells around one storage: it lies in the image unless it holds strings.
static std::optional<DataNode> tryReadUnwrittenLiteral(const AnalysisContext& context, const Function& function, const Value& literal)
{
    if (literal.storage() != AllocationStorage::Heap || literal.capacityOperand() || !literal.holes().empty() || !context.flow || !isNeverStoredInto(*context.flow, literal))
        return std::nullopt;

    DataNode::Array array { literal.contents(), { } };
    array.elements.reserveInitialCapacity(literal.elementsOperands().size());
    for (ValueID element : literal.elementsOperands()) {
        std::optional<DataNode> constant = constantNodeOf(context.program, function, element);
        if (!constant || constant->is<DataNode::BigInt>())
            return std::nullopt;
        array.elements.append(WTF::move(*constant));
    }
    if (array.elements.isEmpty() || arrayStorageOfData(context.program, array) != arrayStorageAtAllocation(context.program, context.program.at(literal.type)))
        return std::nullopt;
    return DataNode(WTF::move(array));
}

static bool namesGuard(const auto& user)
{
    bool result = false;
    user.forEachOperand([&](ValueID, uint32_t operandIndex) {
        result |= user.operandKind(operandIndex) == OperandKind::Guard;
    });
    return result;
}

// What completes a literal, the elements behind a spread for one, is no store for flow.
static bool hasToStayLiteral(FunctionAnalyses& analyses, ValueID literal)
{
    const Function& function = analyses.function();
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> array = escape.singleObject(literal);
    if (!array)
        return true;

    auto isUnderGuard = [&](const Use& use) {
        switch (use.kind) {
        case Use::Kind::Operand: {
            // A store of the array into another object may stand under a guard on that object.
            const Value& user = function.at(use.user);
            return user.operandKind(use.operandIndex) == OperandKind::Guard || (namesGuard(user) && guardedOf(user.opcode) == use.operandIndex);
        }
        case Use::Kind::TerminatorOperand:
            return namesGuard(function.at(use.block).terminator);
        case Use::Kind::EdgeArgument:
        case Use::Kind::HandlerArgument:
        case Use::Kind::BlockSubject:
            return false;
        }
        RELEASE_ASSERT_NOT_REACHED();
    };
    if (std::ranges::any_of(escape.usesOf(*array), [&](const ObjectUse& use) { return use.uses.containsAny(operandWritingUses) || isUnderGuard(use.use); }))
        return true;
    return std::ranges::any_of(escape.aliasesOf(*array), [&](ValueID alias) {
        return std::ranges::any_of(analyses.useLists().usesOf(alias), isUnderGuard);
    });
}

namespace {

class LiteralReader {
public:
    LiteralReader(const AnalysisContext& context, const Function& function)
        : m_program(context.program)
        , m_function(function)
        , m_analyses(context, function)
    {
    }

    FunctionAnalyses& analyses() { return m_analyses; }

    std::optional<ConstructionSite> tryRead(ValueID construction, ConstantCollectionKind kind)
    {
        ConstructionSite site { .construction = construction, .kind = kind };
        ValueID argument = m_function.at(construction).argumentsOperands()[0];
        // What flow proved about the array stands between it and the constructor as a cast.
        ValueID definition = argument;
        while (isCastWithoutTest(m_function.at(definition).opcode))
            definition = m_function.at(definition).operand(0);
        bool isData = m_function.at(definition).opcode == Opcode::Data;
        if (!(isData ? tryReadData(argument, definition, site) : tryReadLiteral(argument, site)))
            return std::nullopt;

        const auto& contents = (site.existingData ? m_program.at(site.existingData).root : site.contents).as<DataNode::Array>();
        if (std::optional<Vector<DataNode>> entries = removeRepeatedKeys(contents.elements.span(), kind)) {
            site.contents = DataNode(DataNode::Array { contents.contents, WTF::move(*entries) });
            site.existingData = { };
        }
        return site;
    }

    // `{ "+1": "a", smile: "b", .. }` where the type names no keys: an ordinary object that is filled name by name behind its allocation.
    std::optional<DictionaryLiteral> tryReadDictionary(ValueID dictionary)
    {
        ValueLocation location = m_analyses.useLists().locationOf(dictionary);
        if (location.placement != ValuePlacement::InBlock)
            return std::nullopt;

        DictionaryLiteral literal { .dictionary = dictionary, .runsAtMostOnce = m_analyses.executionCountOf(location.block) == ExecutionCount::AtMostOnce };
        bool takesConstants = true;
        Vector<ValueID, 2> aliases { dictionary };
        UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> positions;
        const BlockValues& values = m_function.at(location.block).values;
        for (size_t index = location.index + 1; index < values.size(); ++index) {
            ValueID id = values[index];
            const Value& value = m_function.at(id);
            bool takesDictionary = false;
            value.forEachOperand([&](ValueID operand, uint32_t) {
                takesDictionary |= aliases.contains(operand);
            });
            if (!takesDictionary)
                continue;
            if (isCastWithoutTest(value.opcode)) {
                aliases.append(id);
                continue;
            }
            if (value.opcode == Opcode::ElemDefine && !aliases.contains(value.valueOperand()) && isConstantIndex(value.propertyOperand()))
                continue;
            if (value.opcode != Opcode::PropDefine || aliases.contains(value.valueOperand()) || !isNameOfOrdinaryProperty(value.key()))
                break;
            auto position = positions.add(value.key().index(), literal.names.size());
            if (position.isNewEntry)
                literal.names.append(value.key());
            std::optional<DataNode> member = takesConstants ? constantNodeOf(value.valueOperand()) : std::nullopt;
            takesConstants = !!member;
            if (!member)
                continue;
            if (position.isNewEntry)
                literal.contents.members.append(DataMember { value.key(), WTF::move(*member) });
            else
                literal.contents.members[position.iterator->value].value = WTF::move(*member);
            literal.deadValues.append(id);
        }
        if (literal.names.isEmpty())
            return std::nullopt;
        return literal;
    }

private:
    bool isConstantIndex(ValueID property) const
    {
        std::optional<ValueID> constant = constantThroughCasts(m_program, m_function, property);
        return constant && m_function.at(*constant).opcode == Opcode::Integer && m_function.at(*constant).integer() >= 0 && m_function.at(*constant).integer() <= MAX_ARRAY_INDEX;
    }

    bool isNameOfOrdinaryProperty(PropertyKeyID key) const
    {
        return m_program.at(key).kind == PropertyKeyKind::Text && !m_program.isIndexLike(key);
    }

    bool tryReadData(ValueID argument, ValueID definition, ConstructionSite& site)
    {
        DataID data = m_function.at(definition).data();
        const auto* contents = m_program.at(data).root.tryAs<DataNode::Array>();
        if (!contents || !isUsableAs(*contents, site.kind))
            return false;

        const UseLists& useLists = m_analyses.useLists();
        ValueID user = site.construction;
        for (ValueID id = argument;; id = m_function.at(id).valueOperand()) {
            std::optional<Use> use = useLists.singleUse(id);
            if (!use || use->kind != Use::Kind::Operand || use->user != user || useLists.locationOf(id).placement != ValuePlacement::InBlock)
                return false;
            site.deadValues.append(id);
            if (id == definition)
                break;
            user = id;
        }

        // Every collection built from one array shares its objects, so the array has to be created exactly once per collection.
        if (createsObjects(*contents, site.kind) && useLists.blockOf(definition) != useLists.blockOf(site.construction))
            return false;

        site.existingData = data;
        return true;
    }

    bool tryReadLiteral(ValueID argument, ConstructionSite& site)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> list = findPrivateLiteral(argument, site.construction);
        if (!list)
            return false;
        ValueID literalID = escape.at(*list).definition;
        const Value& literal = m_function.at(literalID);
        if (literal.elementsOperands().empty())
            return false;

        site.deadValues.append(escape.aliasesOf(*list));
        Vector<DataNode> entries;
        entries.reserveInitialCapacity(literal.elementsOperands().size());
        for (ValueID element : literal.elementsOperands()) {
            std::optional<DataNode> entry = tryReadEntry(element, literalID, site);
            if (!entry)
                return false;
            entries.append(WTF::move(*entry));
        }
        site.contents = DataNode(DataNode::Array { literal.contents(), WTF::move(entries) });
        return true;
    }

    std::optional<DataNode> tryReadEntry(ValueID element, ValueID list, ConstructionSite& site)
    {
        switch (site.kind) {
        case ConstantCollectionKind::Set:
            return constantNodeOf(element);
        case ConstantCollectionKind::Map:
            return tryReadPair(element, list, site.deadValues);
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    std::optional<DataNode> tryReadPair(ValueID element, ValueID list, Vector<ValueID>& deadValues)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> pair = findPrivateLiteral(element, list);
        if (!pair)
            return std::nullopt;
        const Value& literal = m_function.at(escape.at(*pair).definition);
        std::span<const ValueID> members = literal.elementsOperands();
        if (members.size() != 2)
            return std::nullopt;

        std::optional<DataNode> key = constantNodeOf(members[0]);
        std::optional<DataNode> value = constantNodeOf(members[1]);
        if (!key || !value)
            return std::nullopt;
        deadValues.append(escape.aliasesOf(*pair));
        return DataNode(DataNode::Array { literal.contents(), Vector<DataNode>::from(WTF::move(*key), WTF::move(*value)) });
    }

    std::optional<ObjectID> findPrivateLiteral(ValueID value, ValueID user)
    {
        const EscapeAnalysis& escape = m_analyses.escape();
        std::optional<ObjectID> object = escape.singleObject(value);
        if (!object)
            return std::nullopt;
        const Value& literal = m_function.at(escape.at(*object).definition);
        if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
            return std::nullopt;
        std::span<const ObjectUse> uses = escape.usesOf(*object);
        if (uses.size() != 1 || uses.front().use.kind != Use::Kind::Operand || uses.front().use.user != user)
            return std::nullopt;
        if (!std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) { return canEraseWhenUnused(alias); }))
            return std::nullopt;
        return object;
    }

    bool canEraseWhenUnused(ValueID id)
    {
        return m_analyses.useLists().locationOf(id).placement == ValuePlacement::InBlock
            && m_analyses.isRemovableWhenUnused(id);
    }

    std::optional<DataNode> constantNodeOf(ValueID id) const { return Typed::constantNodeOf(m_program, m_function, id); }

    const Program& m_program;
    const Function& m_function;
    FunctionAnalyses m_analyses;
};

} // anonymous namespace

static std::optional<ConstantCollectionKind> collectionKindOf(const Program& program, const Value& call)
{
    if (call.opcode != Opcode::CallBuiltin || call.builtinCallForm() != BuiltinCallForm::Whole || call.receiverOperand() || call.argumentsOperands().size() != 1)
        return std::nullopt;

    const BuiltinTable& builtins = program.builtins();
    const BuiltinOverload& overload = builtins.at(call.overload());
    if (overload.mode != BuiltinCallMode::Construct)
        return std::nullopt;

    // A private literal of constants is iterated, and its entries are added, through locked prototypes.
    BuiltinID constructor = builtins.canonicalID(overload.builtin);
    if (constructor == builtins.at(BuiltinClasses::Set).constructor)
        return ConstantCollectionKind::Set;
    if (constructor == builtins.at(BuiltinClasses::Map).constructor)
        return ConstantCollectionKind::Map;
    return std::nullopt;
}

static void collectSites(const AnalysisContext& context, Vector<FunctionSites>& found)
{
    constexpr OpcodeSet opcodesOfSites { Opcode::NewDictionary, Opcode::NewArray, Opcode::CallBuiltin };
    if (!mayListValueWith(context.program, context.id, opcodesOfSites))
        return;
    const Function& function = context.program.at(context.id);
    if (!function.hasBody() || !context.build.isEnabled(PassID::EmbedConstantCollections, context.id))
        return;

    std::optional<LiteralReader> reader;
    Vector<ConstructionSite> sites;
    Vector<UnwrittenLiteral> unwrittenLiterals;
    Vector<DictionaryLiteral> dictionaries;
    forEachListedValue(context.program, context.id, opcodesOfSites, [&](BlockID, uint32_t, ValueID id, const Value& value) {
        if (value.opcode == Opcode::NewDictionary) {
            if (!reader)
                reader.emplace(context, function);
            if (std::optional<DictionaryLiteral> literal = reader->tryReadDictionary(id))
                dictionaries.append(WTF::move(*literal));
            return;
        }
        if (value.opcode == Opcode::NewArray) {
            std::optional<DataNode> contents = tryReadUnwrittenLiteral(context, function, value);
            if (!contents)
                return;
            if (!reader)
                reader.emplace(context, function);
            // Storage with cells is made by the first array, for those that follow.
            if (!isStorageOfArray(*contents->tryAs<DataNode::Array>()) && reader->analyses().executionCountOf(reader->analyses().useLists().blockOf(id)) == ExecutionCount::AtMostOnce)
                return;
            if (!hasToStayLiteral(reader->analyses(), id))
                unwrittenLiterals.append({ id, WTF::move(*contents), { } });
            return;
        }

        std::optional<ConstantCollectionKind> kind = collectionKindOf(context.program, value);
        if (!kind)
            return;
        if (!reader)
            reader.emplace(context, function);
        if (std::optional<ConstructionSite> site = reader->tryRead(id, *kind)) {
            site->isUnused = !reader->analyses().useLists().useCount(id);
            sites.append(WTF::move(*site));
        }
    });
    // The literal that a collection is made of goes away with the construction.
    unwrittenLiterals.removeAllMatching([&](const UnwrittenLiteral& unwritten) {
        return std::ranges::any_of(sites, [&](const ConstructionSite& site) {
            return site.deadValues.contains(unwritten.literal);
        });
    });
    if (!sites.isEmpty() || !unwrittenLiterals.isEmpty() || !dictionaries.isEmpty())
        found.append(FunctionSites { context.id, WTF::move(sites), WTF::move(unwrittenLiterals), WTF::move(dictionaries) });
}

static void add(Hasher& hasher, const DataNode& node)
{
    add(hasher, node.kind());
    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        return;
    case DataNode::Kind::Boolean:
        add(hasher, node.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number:
        add(hasher, node.as<DataNode::Number>().value);
        return;
    case DataNode::Kind::String:
        add(hasher, node.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        add(hasher, node.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Array: {
        auto& array = node.as<DataNode::Array>();
        add(hasher, array.contents);
        add(hasher, static_cast<uint32_t>(array.elements.size()));
        for (const DataNode& element : array.elements)
            add(hasher, element);
        return;
    }
    case DataNode::Kind::Object: {
        auto& object = node.as<DataNode::Object>();
        add(hasher, object.layout);
        add(hasher, static_cast<uint32_t>(object.members.size()));
        for (const DataMember& member : object.members) {
            add(hasher, member.key);
            add(hasher, member.value);
        }
        return;
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static DataID findOrAddData(ProgramEditor& editor, RecordsByHash& recordsByHash, Data&& data)
{
    Vector<DataID, 1>& records = recordsByHash.add(computeHash(data.root), Vector<DataID, 1>()).iterator->value;
    for (DataID record : records) {
        if (editor.program().at(record).root == data.root)
            return record;
    }
    DataID id = editor.addData(WTF::move(data));
    records.append(id);
    return id;
}

static void eraseDeadValues(IRBuilder& builder, const BitVector& deadValues)
{
    Function& function = builder.function();
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        BlockID block(blockIndex);
        const BlockValues& values = function.at(block).values;
        BitVector deadIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            if (deadValues.quickGet(values[index].index()))
                deadIndexes.set(index);
        }
        if (!deadIndexes.isEmpty())
            builder.eraseValues(block, deadIndexes);
    }
}

// The constants that only the literals have used.
static void collectElementsOf(const AnalysisContext& context, const Function& function, std::span<const UnwrittenLiteral> literals, std::span<const DictionaryLiteral> dictionaries, BitVector& deadValues)
{
    if (literals.empty() && dictionaries.empty())
        return;
    FunctionAnalyses analyses(context, function);
    const UseLists& useLists = analyses.useLists();
    UncheckedKeyHashMap<uint32_t, uint32_t, DefaultHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> usesByLiterals;
    for (const UnwrittenLiteral& unwritten : literals) {
        for (ValueID element : function.at(unwritten.literal).elementsOperands())
            ++usesByLiterals.add(element.index(), 0).iterator->value;
    }
    for (const DictionaryLiteral& dictionary : dictionaries) {
        for (ValueID definition : dictionary.deadValues)
            ++usesByLiterals.add(function.at(definition).valueOperand().index(), 0).iterator->value;
    }
    for (auto [element, uses] : usesByLiterals) {
        ValueID id(element);
        if (constantNodeOpcodes.contains(function.at(id).opcode) && useLists.useCount(id) == uses && useLists.locationOf(id).placement == ValuePlacement::InBlock)
            deadValues.quickSet(element);
    }
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, std::span<const ConstructionSite> sites, std::span<const UnwrittenLiteral> literals, std::span<const DictionaryLiteral> dictionaries)
{
    const Program& program = context.program;
    BitVector deadValues(function.values.size());
    collectElementsOf(context, function, literals, dictionaries, deadValues);
    for (const DictionaryLiteral& literal : dictionaries) {
        for (ValueID dead : literal.deadValues)
            deadValues.quickSet(dead.index());
        Value& dictionary = function.edit(literal.dictionary);
        dictionary.become(Opcode::Data);
        dictionary.setData(literal.data);
    }
    for (const UnwrittenLiteral& unwritten : literals) {
        Value& literal = function.edit(unwritten.literal);
        AllocationSiteID allocationSite = literal.allocationSite();
        literal.become(Opcode::Data);
        literal.setData(unwritten.data);
        literal.setAllocationSite(allocationSite);
    }
    Vector<ValueID> constructions;
    constructions.reserveInitialCapacity(sites.size());
    for (const ConstructionSite& site : sites) {
        for (ValueID dead : site.deadValues)
            deadValues.quickSet(dead.index());
        if (site.isUnused) {
            deadValues.quickSet(site.construction.index());
            continue;
        }
        Value& construction = function.edit(site.construction);
        AllocationSiteID allocationSite = construction.allocationSite();
        construction.become(operationFor(site.kind));
        construction.setData(site.data);
        construction.setAllocationSite(allocationSite);
        construction.type = resultTypeOf(program, function, construction, types);
        construction.setExpected({ });
        constructions.append(site.construction);
    }

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    eraseDeadValues(builder, deadValues);
    narrowValueTypes(TypePropagationInputs { program, context.id, function, analyses, types, context.build }, constructions.span());
    compactFunction(function);
}

// Objects with the same names in the same order share their Structures: the first pays for them, and a place that reads a name from all of them finds one Structure there.
static void keepDictionariesThatShareNothing(Vector<FunctionSites>& found)
{
    UncheckedKeyHashMap<uint64_t, Vector<const DictionaryLiteral*, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> literalsByNames;
    for (const FunctionSites& entry : found) {
        for (const DictionaryLiteral& literal : entry.dictionaries) {
            Hasher hasher;
            for (PropertyKeyID name : literal.names)
                add(hasher, name);
            literalsByNames.add(hasher.hash(), Vector<const DictionaryLiteral*, 1>()).iterator->value.append(&literal);
        }
    }
    UncheckedKeyHashSet<const DictionaryLiteral*> kept;
    for (auto& literals : literalsByNames.values()) {
        for (const DictionaryLiteral* literal : literals) {
            bool isOnlyOneWithItsNames = literal->runsAtMostOnce && std::ranges::none_of(literals, [&](const DictionaryLiteral* other) {
                return other != literal && other->names == literal->names;
            });
            if (!literal->contents.members.isEmpty() && (isOnlyOneWithItsNames || literal->contents.members.size() > static_cast<size_t>(Structure::s_maxTransitionLength)))
                kept.add(literal);
        }
    }
    for (FunctionSites& entry : found) {
        Vector<DictionaryLiteral> dictionaries;
        for (DictionaryLiteral& literal : entry.dictionaries) {
            if (kept.contains(&literal))
                dictionaries.append(WTF::move(literal));
        }
        entry.dictionaries = WTF::move(dictionaries);
    }
    found.removeAllMatching([](const FunctionSites& entry) {
        return entry.sites.isEmpty() && entry.unwrittenLiterals.isEmpty() && entry.dictionaries.isEmpty();
    });
}

ConstantCollections embedConstantCollections(ProgramEditor& editor, const FlowResults& flow, const EffectSummaries* summariesFromFlow, BuildContext& build)
{
    const Program& program = editor.program();
    std::optional<EffectSummaries> ownSummaries;
    if (!summariesFromFlow)
        summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(program, flow, build));
    AnalysisContext programContext { build, program, &flow, summariesFromFlow };
    Vector<FunctionSites> found = collectInParallel<FunctionSites>(build.parallelLoops(), program.functions.size(), largeFunctions(program, build.parallelLoops()).span(), [&](unsigned index, Vector<FunctionSites>& foundInChunk) {
        collectSites(contextOf(programContext, FunctionID(index)), foundInChunk);
    });
    keepDictionariesThatShareNothing(found);

    ConstantCollections collections;
    RecordsByHash recordsByHash;
    uint64_t numberOfSites = 0;
    for (FunctionSites& entry : found) {
        ModuleID module = program.at(entry.function).module;
        for (ConstructionSite& site : entry.sites) {
            if (site.isUnused)
                continue;
            site.data = site.existingData ? site.existingData : findOrAddData(editor, recordsByHash, Data { WTF::move(site.contents), module });
            collections.m_collections.append(ConstantCollection { site.data, site.kind });
        }
        for (UnwrittenLiteral& unwritten : entry.unwrittenLiterals)
            unwritten.data = findOrAddData(editor, recordsByHash, Data { WTF::move(unwritten.contents), module });
        for (DictionaryLiteral& dictionary : entry.dictionaries)
            dictionary.data = findOrAddData(editor, recordsByHash, Data { DataNode(WTF::move(dictionary.contents)), module });
        numberOfSites += std::ranges::count(entry.sites, false, &ConstructionSite::isUnused);
    }

    Vector<FunctionID> functions(found.size(), [&](size_t index) {
        return found[index].function;
    });
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction(contextOf(programContext, id), function, arenas.types, found[index].sites.span(), found[index].unwrittenLiterals.span(), found[index].dictionaries.span());
    });

    sortAndRemoveDuplicates(build.parallelLoops(), collections.m_collections);
    build.statistics().count(StatisticID::ConstantCollectionsEmbedded, numberOfSites);
    if (ownSummaries)
        build.destroyLater(WTF::move(*ownSummaries));
    return collections;
}

static void dumpNode(IRPrinter& printer, const DataNode& node)
{
    auto dumpLeaf = [&](auto... payload) {
        printer.out().print(nameOf(node.kind()), "(");
        (printer.print(payload), ...);
        printer.out().print(")");
    };

    switch (node.kind()) {
    case DataNode::Kind::Undefined:
    case DataNode::Kind::Null:
    case DataNode::Kind::Hole:
        dumpLeaf();
        return;
    case DataNode::Kind::Boolean:
        dumpLeaf(node.as<DataNode::Boolean>().value);
        return;
    case DataNode::Kind::Number:
        dumpLeaf(node.as<DataNode::Number>().value);
        return;
    case DataNode::Kind::String:
        dumpLeaf(node.as<DataNode::String>().value);
        return;
    case DataNode::Kind::BigInt:
        dumpLeaf(node.as<DataNode::BigInt>().digits);
        return;
    case DataNode::Kind::Array:
    case DataNode::Kind::Object:
        printVariant(printer, node);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

void ConstantCollections::dump(PrintStream& out, const Program& program) const
{
    IRPrinter printer(out, program);
    for (size_t index = 0; index < m_collections.size(); ++index) {
        const ConstantCollection& collection = m_collections[index];
        std::span<const DataNode> entries = program.at(collection.data).root.as<DataNode::Array>().elements.span();
        out.print("table ", index, " keys: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, keyOf(entry, collection.kind));
        });
        out.print("\n");
        if (collection.kind == ConstantCollectionKind::Set)
            continue;

        out.print("table ", index, " values: ");
        dumpList(out, entries, [&](PrintStream&, const DataNode& entry) {
            dumpNode(printer, entry.as<DataNode::Array>().elements[1]);
        });
        out.print("\n");
    }
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
