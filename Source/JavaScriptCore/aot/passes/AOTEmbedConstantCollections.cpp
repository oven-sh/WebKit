/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTEmbedConstantCollections.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTBodyIndex.h"
#include "AOTBuildContext.h"
#include "AOTBuiltinTable.h"
#include "AOTEffectSummaries.h"
#include "AOTFlowResults.h"
#include "AOTFunctionAnalyses.h"
#include "AOTFunctionEditing.h"
#include "AOTIRBuilder.h"
#include "AOTResultRules.h"
#include "AOTThreads.h"
#include "AOTTypePropagation.h"
#include "AOTValueIdentity.h"
#include "ArrayConventions.h"
#include "PureNaN.h"
#include "Structure.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/HashMap.h>
#include <wtf/Hasher.h>

namespace JSC { namespace AOT { namespace Typed {

static constexpr OpcodeSet constantNodeOpcodes { Opcode::Undefined, Opcode::Null, Opcode::Boolean, Opcode::Integer, Opcode::Double, Opcode::String, Opcode::BigInt };

namespace {

struct ConstructionSite {
    ValueID construction;
    ConstantCollectionKind kind;
    // The contents, or the record of the program that holds them already.
    DataNode contents;
    DataID data;
    Vector<ValueID> deadValues;
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
    bool isEmbedded { false };
    DataID data;
};

struct ConstantCollectionSites {
    bool isEmpty() const { return sites.isEmpty() && unwrittenLiterals.isEmpty() && dictionaries.isEmpty(); }

    FunctionID function;
    Vector<ConstructionSite> sites;
    Vector<UnwrittenLiteral> unwrittenLiterals;
    Vector<DictionaryLiteral> dictionaries;
};

using KeyIdentity = std::pair<uint8_t, uint64_t>;
using KeyIdentityTraits = WTF::PairHashTraits<WTF::UnsignedWithZeroKeyHashTraits<uint8_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;
template<typename T> using ListsByHash = UncheckedKeyHashMap<uint64_t, Vector<T, 1>, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

} // anonymous namespace

// The key, and behind it the value if there is one.
template<typename Node>
static std::span<Node> partsOf(Node& entry, ConstantCollectionKind kind)
{
    switch (kind) {
    case ConstantCollectionKind::Set:
        return { &entry, 1 };
    case ConstantCollectionKind::Map: {
        auto& pair = entry.template as<DataNode::Array>().elements;
        return { pair.begin(), pair.size() };
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
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

static bool isUsableAs(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return !contents.elements.isEmpty() && std::ranges::all_of(contents.elements, [&](const DataNode& entry) {
        const auto* pair = entry.tryAs<DataNode::Array>();
        if (kind == ConstantCollectionKind::Map && (!pair || pair->elements.size() != 2))
            return false;
        return std::ranges::none_of(partsOf(entry, kind), [](const DataNode& part) {
            return part.is<DataNode::Hole>();
        });
    });
}

static bool createsObjects(const DataNode::Array& contents, ConstantCollectionKind kind)
{
    return std::ranges::any_of(contents.elements, [&](const DataNode& entry) {
        return std::ranges::any_of(partsOf(entry, kind), [](const DataNode& part) {
            return !identityOf(part);
        });
    });
}

static bool isNegativeZero(const DataNode& node)
{
    const auto* number = node.tryAs<DataNode::Number>();
    return number && !number->value && std::signbit(number->value);
}

static std::optional<Vector<DataNode>> removeRepeatedKeys(std::span<const DataNode> entries, ConstantCollectionKind kind)
{
    UncheckedKeyHashMap<KeyIdentity, uint32_t, DefaultHash<KeyIdentity>, KeyIdentityTraits> positions;
    Vector<uint32_t> sources;
    sources.reserveInitialCapacity(entries.size());
    bool changesEntries = false;
    for (uint32_t index = 0; index < entries.size(); ++index) {
        const DataNode& key = partsOf(entries[index], kind)[0];
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

    return sources.map([&](uint32_t source) {
        DataNode entry = entries[source];
        if (DataNode& key = partsOf(entry, kind)[0]; isNegativeZero(key))
            key = DataNode(DataNode::Number { 0 });
        return entry;
    });
}

static std::optional<DataNode> constantNodeOf(const Program& program, const Function& function, ValueID id)
{
    std::optional<ValueID> constantID = constantThroughCasts(program, function, id);
    if (!constantID)
        return std::nullopt;
    const Value& constant = function.at(*constantID);
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
        ASSERT(!constantNodeOpcodes.contains(constant.opcode));
        return std::nullopt;
    }
}

static std::optional<DataNode> constantNodeOf(FunctionAnalyses& analyses, ValueID id)
{
    return constantNodeOf(analyses.program(), analyses.function(), id);
}

// The arrays of a literal of constants that the optimizer has left in the heap, and that no store of the program reaches, are cells around one storage: it lies in the image unless it holds strings.
static std::optional<DataNode> tryReadUnwrittenLiteral(const AnalysisContext& context, const Function& function, const Value& literal)
{
    if (literal.storage() != AllocationStorage::Heap || literal.capacityOperand() || !literal.holes().empty() || !context.flow)
        return std::nullopt;
    AllocationSiteID site = literal.allocationSite();
    AllocationClassID allocationClass = site ? context.flow->at(site).allocationClass : AllocationClassID();
    if (!allocationClass || !context.flow->at(allocationClass).impossibleMutations.contains(ObjectMutation::StoreAfterCreation))
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

// An array literal without holes that nothing but the user sees.
static std::optional<ObjectID> findPrivateLiteral(FunctionAnalyses& analyses, ValueID value, ValueID user)
{
    const EscapeAnalysis& escape = analyses.escape();
    std::optional<ObjectID> object = escape.singleObject(value);
    if (!object)
        return std::nullopt;
    const Value& literal = analyses.function().at(escape.at(*object).definition);
    if (literal.opcode != Opcode::NewArray || !literal.holes().empty() || literal.capacityOperand())
        return std::nullopt;
    std::span<const ObjectUse> uses = escape.usesOf(*object);
    if (uses.size() != 1 || uses.front().use.kind != Use::Kind::Operand || uses.front().use.user != user)
        return std::nullopt;
    bool canEraseWhenUnused = std::ranges::all_of(escape.aliasesOf(*object), [&](ValueID alias) {
        return analyses.useLists().locationOf(alias).placement == ValuePlacement::InBlock && analyses.isRemovableWhenUnused(alias);
    });
    return canEraseWhenUnused ? object : std::nullopt;
}

// The entries of a set are constants, those of a map are private literals themselves: pairs of constants.
static std::optional<DataNode> tryReadLiteral(FunctionAnalyses& analyses, ValueID value, ValueID user, bool holdsPairs, bool isPair, Vector<ValueID>& deadValues)
{
    std::optional<ObjectID> object = findPrivateLiteral(analyses, value, user);
    if (!object)
        return std::nullopt;
    ValueID id = analyses.escape().at(*object).definition;
    const Value& literal = analyses.function().at(id);
    if (isPair ? literal.elementsOperands().size() != 2 : literal.elementsOperands().empty())
        return std::nullopt;

    deadValues.append(analyses.escape().aliasesOf(*object));
    DataNode::Array array { literal.contents(), { } };
    array.elements.reserveInitialCapacity(literal.elementsOperands().size());
    for (ValueID element : literal.elementsOperands()) {
        std::optional<DataNode> node = holdsPairs ? tryReadLiteral(analyses, element, id, false, true, deadValues) : constantNodeOf(analyses, element);
        if (!node)
            return std::nullopt;
        array.elements.append(WTF::move(*node));
    }
    return DataNode(WTF::move(array));
}

static bool tryReadData(FunctionAnalyses& analyses, ValueID argument, ValueID definition, ConstructionSite& site)
{
    const Function& function = analyses.function();
    DataID data = function.at(definition).data();
    const auto* contents = analyses.program().at(data).root.tryAs<DataNode::Array>();
    if (!contents || !isUsableAs(*contents, site.kind))
        return false;

    const UseLists& useLists = analyses.useLists();
    ValueID user = site.construction;
    for (ValueID id = argument;; id = function.at(id).valueOperand()) {
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

    site.data = data;
    return true;
}

static std::optional<ConstructionSite> tryReadConstruction(FunctionAnalyses& analyses, ValueID construction, ConstantCollectionKind kind)
{
    const Function& function = analyses.function();
    ConstructionSite site { .construction = construction, .kind = kind };
    ValueID argument = function.at(construction).argumentsOperands()[0];
    // What flow proved about the array stands between it and the constructor as a cast.
    ValueID definition = argument;
    while (isCastWithoutTest(function.at(definition).opcode))
        definition = function.at(definition).operand(0);
    if (function.at(definition).opcode != Opcode::Data) {
        std::optional<DataNode> contents = tryReadLiteral(analyses, argument, construction, kind == ConstantCollectionKind::Map, false, site.deadValues);
        if (!contents)
            return std::nullopt;
        site.contents = WTF::move(*contents);
    } else if (!tryReadData(analyses, argument, definition, site))
        return std::nullopt;

    const auto& contents = (site.data ? analyses.program().at(site.data).root : site.contents).as<DataNode::Array>();
    if (std::optional<Vector<DataNode>> entries = removeRepeatedKeys(contents.elements.span(), kind)) {
        site.contents = DataNode(DataNode::Array { contents.contents, WTF::move(*entries) });
        site.data = { };
    }
    site.isUnused = !analyses.useLists().useCount(construction);
    return site;
}

// `{ "+1": "a", smile: "b", .. }` where the type names no keys: an ordinary object that is filled name by name behind its allocation.
static std::optional<DictionaryLiteral> tryReadDictionary(FunctionAnalyses& analyses, ValueID dictionary)
{
    const Program& program = analyses.program();
    const Function& function = analyses.function();
    ValueLocation location = analyses.useLists().locationOf(dictionary);
    if (location.placement != ValuePlacement::InBlock)
        return std::nullopt;
    auto isConstantIndex = [&](ValueID property) {
        std::optional<ValueID> constant = constantThroughCasts(program, function, property);
        return constant && function.at(*constant).opcode == Opcode::Integer && function.at(*constant).integer() >= 0 && function.at(*constant).integer() <= MAX_ARRAY_INDEX;
    };

    DictionaryLiteral literal { .dictionary = dictionary, .runsAtMostOnce = analyses.executionCountOf(location.block) == ExecutionCount::AtMostOnce };
    bool takesConstants = true;
    Vector<ValueID, 2> aliases { dictionary };
    HashMap<PropertyKeyID, uint32_t> positions;
    for (ValueID id : function.at(location.block).values.subspan(location.index + 1)) {
        const Value& value = function.at(id);
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
        if (value.opcode != Opcode::PropDefine || aliases.contains(value.valueOperand()) || program.at(value.key()).kind != PropertyKeyKind::Text || program.isIndexLike(value.key()))
            break;
        auto position = positions.add(value.key(), literal.names.size());
        if (position.isNewEntry)
            literal.names.append(value.key());
        std::optional<DataNode> member = takesConstants ? constantNodeOf(analyses, value.valueOperand()) : std::nullopt;
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

static void collectSites(const AnalysisContext& context, Vector<ConstantCollectionSites>& found)
{
    constexpr OpcodeSet opcodesOfSites { Opcode::NewDictionary, Opcode::NewArray, Opcode::CallBuiltin };
    if (!mayListValueWith(context.program, context.id, opcodesOfSites))
        return;
    const Function& function = context.program.at(context.id);
    if (!function.hasBody() || !context.build.isEnabled(PassID::EmbedConstantCollections, context.id))
        return;

    std::optional<FunctionAnalyses> analysesIfAsked;
    auto analyses = [&]() -> FunctionAnalyses& {
        return analysesIfAsked ? *analysesIfAsked : analysesIfAsked.emplace(context, function);
    };
    ConstantCollectionSites inFunction { .function = context.id };
    forEachListedValue(context.program, context.id, opcodesOfSites, [&](BlockID, uint32_t, ValueID id, const Value& value) {
        if (value.opcode == Opcode::NewDictionary) {
            if (std::optional<DictionaryLiteral> literal = tryReadDictionary(analyses(), id))
                inFunction.dictionaries.append(WTF::move(*literal));
        } else if (value.opcode == Opcode::NewArray) {
            std::optional<DataNode> contents = tryReadUnwrittenLiteral(context, function, value);
            if (!contents)
                return;
            // Storage with cells is made by the first array, for those that follow.
            if (!isStorageOfArray(*contents->tryAs<DataNode::Array>()) && analyses().executionCountOf(analyses().useLists().blockOf(id)) == ExecutionCount::AtMostOnce)
                return;
            if (!hasToStayLiteral(analyses(), id))
                inFunction.unwrittenLiterals.append({ id, WTF::move(*contents), { } });
        } else if (std::optional<ConstantCollectionKind> kind = collectionKindOf(context.program, value)) {
            if (std::optional<ConstructionSite> site = tryReadConstruction(analyses(), id, *kind))
                inFunction.sites.append(WTF::move(*site));
        }
    });
    // The literal that a collection is made of goes away with the construction.
    inFunction.unwrittenLiterals.removeAllMatching([&](const UnwrittenLiteral& unwritten) {
        return std::ranges::any_of(inFunction.sites, [&](const ConstructionSite& site) {
            return site.deadValues.contains(unwritten.literal);
        });
    });
    if (!inFunction.isEmpty())
        found.append(WTF::move(inFunction));
}

static void add(Hasher& hasher, const DataNode& node)
{
    add(hasher, node.kind());
    if (std::optional<KeyIdentity> identity = identityOf(node))
        add(hasher, identity->second);
    else if (const auto* array = node.tryAs<DataNode::Array>()) {
        add(hasher, array->contents);
        for (const DataNode& element : array->elements)
            add(hasher, element);
    } else if (const auto* object = node.tryAs<DataNode::Object>()) {
        add(hasher, object->layout);
        for (const DataMember& member : object->members) {
            add(hasher, member.key);
            add(hasher, member.value);
        }
    }
}

static DataID findOrAddData(ProgramEditor& editor, ListsByHash<DataID>& recordsByHash, DataNode&& root, ModuleID module)
{
    Vector<DataID, 1>& records = recordsByHash.add(computeHash(root), Vector<DataID, 1>()).iterator->value;
    for (DataID record : records) {
        if (editor.program().at(record).root == root)
            return record;
    }
    records.append(editor.addData(Data { WTF::move(root), module }));
    return records.last();
}

static void rewriteFunction(const AnalysisContext& context, Function& function, TypeArena& types, const ConstantCollectionSites& found)
{
    BitVector deadValues(function.values.size());
    auto addDeadValues = [&](const Vector<ValueID>& values) {
        for (ValueID dead : values)
            deadValues.quickSet(dead.index());
    };
    auto becomeReadOfData = [&](ValueID id, Opcode operation, DataID data) -> Value& {
        Value& value = function.edit(id);
        AllocationSiteID allocationSite = value.allocationSite();
        value.become(operation);
        value.setData(data);
        value.setAllocationSite(allocationSite);
        value.type = resultTypeOf(context.program, function, value, types);
        return value;
    };

    if (!found.unwrittenLiterals.isEmpty() || !found.dictionaries.isEmpty()) {
        // The constants that only the literals have used.
        FunctionAnalyses analyses(context, function);
        const UseLists& useLists = analyses.useLists();
        HashMap<ValueID, uint32_t> usesByLiterals;
        for (const UnwrittenLiteral& unwritten : found.unwrittenLiterals) {
            for (ValueID element : function.at(unwritten.literal).elementsOperands())
                ++usesByLiterals.add(element, 0).iterator->value;
        }
        for (const DictionaryLiteral& dictionary : found.dictionaries) {
            for (ValueID definition : dictionary.deadValues)
                ++usesByLiterals.add(function.at(definition).valueOperand(), 0).iterator->value;
        }
        for (auto [id, uses] : usesByLiterals) {
            if (constantNodeOpcodes.contains(function.at(id).opcode) && useLists.useCount(id) == uses && useLists.locationOf(id).placement == ValuePlacement::InBlock)
                deadValues.quickSet(id.index());
        }
    }
    for (const DictionaryLiteral& literal : found.dictionaries) {
        addDeadValues(literal.deadValues);
        Value& dictionary = function.edit(literal.dictionary);
        dictionary.become(Opcode::Data);
        dictionary.setData(literal.data);
    }
    for (const UnwrittenLiteral& unwritten : found.unwrittenLiterals)
        becomeReadOfData(unwritten.literal, Opcode::Data, unwritten.data);
    Vector<ValueID> constructions;
    constructions.reserveInitialCapacity(found.sites.size());
    for (const ConstructionSite& site : found.sites) {
        addDeadValues(site.deadValues);
        if (site.isUnused) {
            deadValues.quickSet(site.construction.index());
            continue;
        }
        becomeReadOfData(site.construction, site.kind == ConstantCollectionKind::Set ? Opcode::NewSetFromData : Opcode::NewMapFromData, site.data).setExpected({ });
        constructions.append(site.construction);
    }

    FunctionAnalyses analyses(context, function);
    IRBuilder builder(context, function, types, &analyses);
    for (unsigned blockIndex = 0; blockIndex < function.blocks.size(); ++blockIndex) {
        const BlockValues& values = function.blocks[blockIndex].values;
        BitVector deadIndexes;
        for (size_t index = 0; index < values.size(); ++index) {
            if (deadValues.quickGet(values[index].index()))
                deadIndexes.set(index);
        }
        if (!deadIndexes.isEmpty())
            builder.eraseValues(BlockID(blockIndex), deadIndexes);
    }
    narrowValueTypes(TypePropagationInputs { context.program, context.id, function, analyses, types, context.build }, constructions.span());
    compactFunction(function);
}

// Objects with the same names in the same order share their Structures: the first pays for them, and a place that reads a name from all of them finds one Structure there.
static void keepDictionariesThatShareNothing(Vector<ConstantCollectionSites>& found)
{
    ListsByHash<DictionaryLiteral*> literalsByNames;
    for (ConstantCollectionSites& entry : found) {
        for (DictionaryLiteral& literal : entry.dictionaries) {
            Hasher hasher;
            for (PropertyKeyID name : literal.names)
                add(hasher, name);
            literalsByNames.add(hasher.hash(), Vector<DictionaryLiteral*, 1>()).iterator->value.append(&literal);
        }
    }
    for (auto& literals : literalsByNames.values()) {
        for (DictionaryLiteral* literal : literals) {
            bool isOnlyOneWithItsNames = literal->runsAtMostOnce && std::ranges::none_of(literals, [&](const DictionaryLiteral* other) {
                return other != literal && other->names == literal->names;
            });
            literal->isEmbedded = !literal->contents.members.isEmpty() && (isOnlyOneWithItsNames || literal->contents.members.size() > static_cast<size_t>(Structure::s_maxTransitionLength));
        }
    }
    for (ConstantCollectionSites& entry : found) {
        entry.dictionaries.removeAllMatching([](const DictionaryLiteral& literal) {
            return !literal.isEmbedded;
        });
    }
    found.removeAllMatching([](const ConstantCollectionSites& entry) {
        return entry.isEmpty();
    });
}

ConstantCollections embedConstantCollections(ProgramEditor& editor, const FlowResults& flow, const EffectSummaries* summariesFromFlow, BuildContext& build)
{
    const Program& program = editor.program();
    std::optional<EffectSummaries> ownSummaries;
    if (!summariesFromFlow)
        summariesFromFlow = &ownSummaries.emplace(createSummariesFromFlow(program, flow, build));
    Vector<ConstantCollectionSites> found = collectInParallel<ConstantCollectionSites>(build.parallelLoops(), program.functions.size(), largeFunctions(program, build.parallelLoops()).span(), [&](unsigned index, Vector<ConstantCollectionSites>& foundInChunk) {
        collectSites({ build, program, &flow, summariesFromFlow, FunctionID(index) }, foundInChunk);
    });
    keepDictionariesThatShareNothing(found);

    ConstantCollections collections;
    ListsByHash<DataID> recordsByHash;
    for (ConstantCollectionSites& entry : found) {
        ModuleID module = program.at(entry.function).module;
        for (ConstructionSite& site : entry.sites) {
            if (site.isUnused)
                continue;
            if (!site.data)
                site.data = findOrAddData(editor, recordsByHash, WTF::move(site.contents), module);
            collections.m_collections.append(ConstantCollection { site.data, site.kind });
        }
        for (UnwrittenLiteral& unwritten : entry.unwrittenLiterals)
            unwritten.data = findOrAddData(editor, recordsByHash, WTF::move(unwritten.contents), module);
        for (DictionaryLiteral& dictionary : entry.dictionaries)
            dictionary.data = findOrAddData(editor, recordsByHash, DataNode(WTF::move(dictionary.contents)), module);
    }
    build.statistics().count(StatisticID::ConstantCollectionsEmbedded, collections.m_collections.size());

    Vector<FunctionID> functions = found.map([](const ConstantCollectionSites& entry) {
        return entry.function;
    });
    editFunctionsInParallel(editor, build, functions.span(), [&](unsigned index, FunctionID id, Function& function, TaskArenas& arenas) {
        rewriteFunction({ build, program, &flow, summariesFromFlow, id }, function, arenas.types, found[index]);
    });

    sortAndRemoveDuplicates(build.parallelLoops(), collections.m_collections);
    if (ownSummaries)
        build.destroyLater(WTF::move(*ownSummaries));
    return collections;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
