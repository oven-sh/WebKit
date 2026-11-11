/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTTableParts.h"

#if ENABLE(AOT)

#include "AOTArrayStorageForms.h"
#include "AOTFormatPatterns.h"
#include "AOTIDPrefix.h"
#include "AOTIRProgram.h"
#include "AOTLayout.h"
#include "AOTLimits.h"
#include "AOTOperationData.h"
#include "AOTPlan.h"
#include "AOTRemovableFeature.h"
#include "AOTSort.h"
#include "AOTVector.h"
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/StdLibExtras.h>
#include <wtf/text/MakeString.h>

namespace JSC { namespace AOT {

template<typename... Records>
static constexpr bool fillWholeWordsOfRequestTable = ((!(sizeof(Records) % sizeof(uint32_t)) && alignof(Records) == alignof(uint32_t)) && ...);

static_assert(fillWholeWordsOfRequestTable<NewClassRequest, TemplateObjectRequest, TemplateObjectRequest::Part, DirectEvalRequest, EvalBindingData, DataRequest, CollectionDataRequest>);

static constexpr bool limitOfRequestTableIs(LimitID limit, uint64_t width)
{
    return defaultLimitValue(limit, Architecture::ARM64) == width && defaultLimitValue(limit, Architecture::X86_64) == width;
}

static_assert(limitOfRequestTableIs(LimitID::MaxEvalBindingDepth, (1u << 24) - 1), "EvalBindingData::depth has 24 bits.");
static_assert(limitOfRequestTableIs(LimitID::MaxEvalBindingOffset, SlotData::maxOffset));

namespace {

struct EvalBindingLocation {
    uint32_t operand { EvalBindingData::noOperand };
    uint32_t offset { 0 };
    SlotRepresentation representation { SlotRepresentation::JSValue };
};

} // anonymous namespace

static std::optional<size_t> sizeOfRecordInRequestTable(const Typed::Program& program, std::span<const uint32_t> key)
{
    if (key.empty())
        return std::nullopt;
    std::span<const uint32_t> words = key.subspan(1);
    size_t numberOfStrings = program.strings.size();
    switch (static_cast<RequestKind>(key[0])) {
    case RequestKind::NewClass:
        if (words.size() != 1 || words[0] >= program.layouts.size())
            return std::nullopt;
        return sizeof(NewClassRequest);
    case RequestKind::TemplateObject:
        if (words.size() % 2)
            return std::nullopt;
        for (size_t index = 0; index < words.size(); index += 2) {
            bool hasCookedString = words[index] != Wire::none;
            if ((hasCookedString && words[index] >= numberOfStrings) || words[index + 1] >= numberOfStrings)
                return std::nullopt;
        }
        return sizeof(TemplateObjectRequest) + sizeof(TemplateObjectRequest::Part) * (words.size() / 2);
    case RequestKind::DirectEval:
        if (words.size() != 2 || words[0] >= program.evalScopes.size() || words[1] >= program.modules.size())
            return std::nullopt;
        return sizeof(DirectEvalRequest) + (sizeof(EvalBindingData) + sizeof(uint32_t)) * program.evalScopes[words[0]].bindings.size();
    case RequestKind::Data:
        if (words.size() != 2 || words[0] >= program.data.size() || words[1] >= numberOfEnumerators<Typed::DataEncoding>)
            return std::nullopt;
        return sizeof(DataRequest);
    case RequestKind::CollectionData:
        if (words.size() != 2 || words[0] >= program.data.size() || words[1] > 1)
            return std::nullopt;
        return sizeof(CollectionDataRequest);
    case RequestKind::FormatPattern:
        if (words.size_bytes() < sizeof(FormatPatternHeader) || reinterpretCastSpanStartTo<const FormatPatternHeader>(asBytes(words)).constructorArgumentsOffset >= words.size_bytes())
            return std::nullopt;
        return words.size_bytes();
    }
    return std::nullopt;
}

static bool evalBindingHasCompleteLocation(const Typed::Program& program, const Typed::EvalBinding& binding)
{
    if (binding.variable)
        return true;
    if (!binding.operand)
        return false;
    if (binding.kind == EvalBindingKind::MappedArgument)
        return binding.field.has_value();
    if (!binding.layout)
        return true;
    return binding.field && *binding.field < program.at(binding.layout).fields.size();
}

static PropertyKeyID privateKeyOfEvalBinding(const Typed::Program& program, const Typed::EvalBinding& binding)
{
    if (binding.variable || !binding.layout || binding.kind == EvalBindingKind::MappedArgument || program.at(binding.layout).isEnvironment())
        return { };
    return Typed::fieldAt(program, Typed::declarationOf(program, { binding.layout, *binding.field })).key;
}

static void nameStringConstantOfRequestTable(NamesOfTables& names, StringID string)
{
    names.strings.set(string.index());
    names.stringsWithOtherUsesThanShowing.set(string.index());
    names.stringConstants.set(string.index());
}

static void nameWhatDirectEvalRequestNames(const TableInputs& inputs, Typed::EvalScopeID scope, NamesOfTables& names)
{
    const Typed::Program& program = inputs.program;
    for (const Typed::EvalBinding& binding : program.at(scope).bindings) {
        if (binding.name)
            names.textsNamedAsKeys.set(binding.name.index());
        if (!evalBindingHasCompleteLocation(program, binding))
            continue;
        if (PropertyKeyID privateKey = privateKeyOfEvalBinding(program, binding))
            names.keys.set(privateKey.index());
        if (!binding.variable)
            continue;
        names.variables.set(binding.variable.index());
        if (FunctionID function = inputs.plan.variable(binding.variable).function)
            names.functionsWithClosureCell.set(function.index());
    }
}

void nameWhatRequestTablesName(const TableInputs& inputs, const GatheredMentions& mentions, NamesOfTables& names)
{
    const NumberedKeys& requests = mentions.keys(NumberSpace::Request);
    for (uint32_t place = 0; place < requests.size(); ++place) {
        std::span<const uint32_t> key = requests.keyAt(place);
        if (!sizeOfRecordInRequestTable(inputs.program, key))
            continue;
        std::span<const uint32_t> words = key.subspan(1);
        switch (static_cast<RequestKind>(key[0])) {
        case RequestKind::TemplateObject:
            for (uint32_t string : words) {
                if (string != Wire::none)
                    nameStringConstantOfRequestTable(names, StringID(string));
            }
            break;
        case RequestKind::DirectEval:
            nameWhatDirectEvalRequestNames(inputs, Typed::EvalScopeID(words[0]), names);
            break;
        case RequestKind::NewClass:
            break;
        case RequestKind::Data:
        case RequestKind::CollectionData:
            break;
        case RequestKind::FormatPattern:
            break;
        }
    }

    const NumberedKeys& runs = mentions.keys(NumberSpace::PropertyRun);
    for (uint32_t run = 0; run < runs.size(); ++run) {
        for (uint32_t key : runs.keyAt(run))
            names.keys.set(key);
    }
}

static void encodeNewClassRequest(const TableEncoder& encoder, LayoutID classLayout, std::span<uint8_t>& bytes)
{
    if (!encoder.numbers.rankOfClass(classLayout))
        encoder.fail("code creates a class whose record the image does not hold"_s, classLayout);
    consumeAndReinterpretCastTo<NewClassRequest>(bytes).classIndex = classLayout.index();
}

static void encodeTemplateObjectRequest(const TableEncoder& encoder, std::span<const uint32_t> cookedAndRawStrings, std::span<uint8_t>& bytes)
{
    consumeAndReinterpretCastTo<TemplateObjectRequest>(bytes).count = static_cast<uint32_t>(cookedAndRawStrings.size() / 2);
    for (size_t index = 0; index < cookedAndRawStrings.size(); index += 2) {
        uint32_t cooked = cookedAndRawStrings[index];
        auto& part = consumeAndReinterpretCastTo<TemplateObjectRequest::Part>(bytes);
        part.cookedPlusOne = cooked == Wire::none ? 0 : encoder.stringConstantNumber(StringID(cooked)) + 1;
        part.raw = encoder.stringConstantNumber(StringID(cookedAndRawStrings[index + 1]));
    }
}

static void setFlagOfDirectEvalRequest(DirectEvalRequest& request, Typed::EvalScopeFlags flag)
{
    switch (flag) {
    case Typed::EvalScopeFlags::IsInFunction:
        request.isInFunction = true;
        return;
    case Typed::EvalScopeFlags::IsInArrowFunction:
        request.isInArrowFunction = true;
        return;
    case Typed::EvalScopeFlags::IsInFieldInitializer:
        request.isInFieldInitializer = true;
        return;
    case Typed::EvalScopeFlags::IsInDerivedConstructor:
        request.isInDerivedConstructor = true;
        return;
    case Typed::EvalScopeFlags::AllowsSuperProperty:
        request.allowsSuperProperty = true;
        return;
    case Typed::EvalScopeFlags::AllowsSuperCall:
        request.allowsSuperCall = true;
        return;
    case Typed::EvalScopeFlags::AllowsNewTarget:
        request.allowsNewTarget = true;
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static uint32_t operandOfDirectEvalRequest(std::optional<uint32_t> environment)
{
    constexpr uint32_t operandsBeforeEnvironments = 2;
    return environment ? operandsBeforeEnvironments + *environment : EvalBindingData::noOperand;
}

static uint32_t checkedEvalBindingAmount(const TableEncoder& encoder, LimitID limit, uint32_t amount, LayoutID layout)
{
    if (amount <= encoder.build.limit(limit))
        return amount;
    encoder.build.reportLimitExceeded(limit, amount, { .layout = layout });
    return 0;
}

static std::optional<EvalBindingLocation> findLocationOfVariableInEvalBinding(const TableEncoder& encoder, VariableID variable)
{
    const Vector<PlacedVariable>& variables = encoder.numbers.statics().variables;
    if (variable.index() < variables.size()) {
        const PlacedVariable& placed = variables[variable.index()];
        switch (placed.plan.storage) {
        case Typed::VariableStorage::Statics:
            if (!placed.hasWordInStatics())
                break;
            return EvalBindingLocation { .offset = placed.offset, .representation = placed.plan.representation };
        case Typed::VariableStorage::ClosureTable:
            if (std::optional<FunctionID> entry = encoder.findEntrypoint(placed.function); entry && encoder.numbers.closureCellNumber(placed.function))
                return EvalBindingLocation { .offset = entry->index(), .representation = SlotRepresentation::DeferredClosure };
            break;
        }
    }
    encoder.fail("a direct eval sees a variable that the run time cannot read where the image keeps it"_s, variable);
    return std::nullopt;
}

static std::optional<EvalBindingLocation> findLocationOfEvalBinding(const TableEncoder& encoder, const Typed::EvalBinding& binding)
{
    if (binding.variable)
        return findLocationOfVariableInEvalBinding(encoder, binding.variable);

    EvalBindingLocation location { .operand = operandOfDirectEvalRequest(binding.operand) };
    if (binding.kind == EvalBindingKind::MappedArgument) {
        location.offset = *binding.field;
        return location;
    }
    if (!binding.layout)
        return location;
    const Typed::PlacedSlot& slot = encoder.plan.slot(binding.layout, *binding.field);
    if (encoder.program.at(binding.layout).isEnvironment() && !slot.hasStorage()) {
        encoder.fail("a direct eval sees a variable of an environment that keeps no slot for it"_s, binding.layout);
        return std::nullopt;
    }
    location.offset = slot.offset;
    location.representation = slot.representation;
    return location;
}

static void encodeEvalBindingOfRequest(const TableEncoder& encoder, const Typed::EvalBinding& binding, ModuleID module, EvalBindingData& data)
{
    data.name = binding.name ? encoder.identifierNumberOfText(binding.name, "what a direct eval sees in"_s, module) : 0;
    data.kind = static_cast<uint32_t>(binding.kind);
    data.depth = checkedEvalBindingAmount(encoder, LimitID::MaxEvalBindingDepth, binding.depth, binding.layout);
    data.operand = EvalBindingData::noOperand;
    bool namesBindings = binding.kind == EvalBindingKind::PrivateMethod || binding.kind == EvalBindingKind::PrivateAccessor;
    data.function = namesBindings ? binding.function.value_or(EvalBindingData::noOperand) : operandOfDirectEvalRequest(binding.function);
    data.setter = namesBindings ? binding.setter.value_or(EvalBindingData::noOperand) : operandOfDirectEvalRequest(binding.setter);
    data.privateKey = 0;
    data.slot = SlotData::create(0, SlotRepresentation::JSValue);
    if (!evalBindingHasCompleteLocation(encoder.program, binding)) {
        encoder.fail("a binding visible to a direct eval has neither a variable nor a complete location among the operands"_s, module);
        return;
    }
    if (PropertyKeyID privateKey = privateKeyOfEvalBinding(encoder.program, binding))
        data.privateKey = encoder.identifierNumber(privateKey, "the key of a private name that a direct eval sees in"_s, module);
    std::optional<EvalBindingLocation> location = findLocationOfEvalBinding(encoder, binding);
    if (!location)
        return;
    data.operand = location->operand;
    data.slot = SlotData::create(checkedEvalBindingAmount(encoder, LimitID::MaxEvalBindingOffset, location->offset, binding.layout), location->representation);
}

static void encodeDirectEvalRequest(const TableEncoder& encoder, Typed::EvalScopeID scopeID, ModuleID module, std::span<uint8_t>& bytes)
{
    const Typed::EvalScope& scope = encoder.program.at(scopeID);
    auto& request = consumeAndReinterpretCastTo<DirectEvalRequest>(bytes);
    request.isStrict = scope.mode == Typed::ECMAMode::Strict;
    for (Typed::EvalScopeFlags flag : scope.flags)
        setFlagOfDirectEvalRequest(request, flag);
    request.module = module.index();
    request.outer = DirectEvalRequest::noOuter;
    if (scope.outer) {
        uint32_t keyOfOuter[] { static_cast<uint32_t>(RequestKind::DirectEval), scope.outer.index(), module.index() };
        if (std::optional<uint32_t> offset = encoder.numbers.numberOf(NumberSpace::Request, keyOfOuter))
            request.outer = *offset;
        else
            encoder.fail("what lies around the scope of a direct eval has no place among the requests"_s, module);
    }
    request.numberOfBindings = static_cast<uint32_t>(scope.bindings.size());

    Vector<uint32_t, 16> names;
    for (const Typed::EvalBinding& binding : scope.bindings) {
        auto& data = consumeAndReinterpretCastTo<EvalBindingData>(bytes);
        encodeEvalBindingOfRequest(encoder, binding, module, data);
        names.append(data.name);
    }
    Vector<uint32_t, 16> bindingsByName(names.size(), [](size_t index) {
        return static_cast<uint32_t>(index);
    });
    AOT::sort(bindingsByName, { }, [&](uint32_t index) {
        return std::pair(names[index], index);
    });
    for (uint32_t index : bindingsByName)
        consumeAndReinterpretCastTo<uint32_t>(bytes) = index;
}

static void failForDataOfRequest(const TableEncoder& encoder, ASCIILiteral rule, DataID data)
{
    encoder.fail(rule, { }, makeString(letterOf(Typed::IDPrefix::Data), data.index()));
}

static bool hasWordForDataOfRequest(const Vector<uint32_t>& offsetsInStatics, DataID data)
{
    return data.index() < offsetsInStatics.size() && offsetsInStatics[data.index()] != PlacedStatics::noOffset;
}

static uint32_t sharedStorageOfDataRequest(const TableEncoder& encoder, DataID data, Typed::DataEncoding encoding)
{
    const PlacedStatics& statics = encoder.numbers.statics();
    switch (encoding) {
    case Typed::DataEncoding::Unselected:
        failForDataOfRequest(encoder, "code hands the run time constant data that it was to build by itself"_s, data);
        return DataRequest::noSharedStorage;
    case Typed::DataEncoding::ReadOnlyTree:
        return DataRequest::noSharedStorage;
    case Typed::DataEncoding::TreeWithOwnStorage:
        return DataRequest::rootHasOwnStorage;
    case Typed::DataEncoding::CopyOnWriteArray: {
        const auto* array = encoder.program.at(data).root.tryAs<Typed::DataNode::Array>();
        if (!array) {
            failForDataOfRequest(encoder, "constant data that is no array is to share the storage of an array"_s, data);
            return DataRequest::noSharedStorage;
        }
        if (Typed::isStorageOfArray(*array))
            return DataRequest::noSharedStorage;
        if (!hasWordForDataOfRequest(statics.dataStorageOffsets, data)) {
            failForDataOfRequest(encoder, "an array of constant data has no word for the storage that its copies share"_s, data);
            return DataRequest::noSharedStorage;
        }
        return statics.dataStorageOffsets[data.index()];
    }
    }
    RELEASE_ASSERT_NOT_REACHED();
}

static void encodeDataRequest(const TableEncoder& encoder, DataID data, Typed::DataEncoding encoding, std::span<uint8_t>& bytes)
{
    auto& request = consumeAndReinterpretCastTo<DataRequest>(bytes);
    request.root = encoder.dataNodeOf(data);
    request.sharedStorageOffset = sharedStorageOfDataRequest(encoder, data, encoding);
}

static void encodeFormatPatternRequest(std::span<const uint32_t> words, std::span<uint8_t>& bytes)
{
    std::span<uint8_t> record = consumeSpan(bytes, words.size_bytes());
    memcpySpan(record, asBytes(words));
    auto& header = reinterpretCastSpanStartTo<FormatPatternHeader>(record);
    std::optional<uint64_t> digest = header.kind == FormatPatternKind::Number
        ? callRemovable<RemovableFeature::IntlNumberFormat, RemovedFeatureStub::Abort, digestOfThisICU>()
        : callRemovable<RemovableFeature::IntlDateTimeFormat, RemovedFeatureStub::Abort, digestOfThisICU>();
    header.digestOfICU[0] = digest ? static_cast<uint32_t>(*digest) : 0;
    header.digestOfICU[1] = digest ? static_cast<uint32_t>(*digest >> 32) : 0;
}

static void encodeTableOfRequests(TableEncoder& encoder)
{
    const NumberedKeys& requests = encoder.numbers.requests();
    std::span<const uint32_t> offsets = encoder.numbers.offsetsOfRequests();
    uint32_t sizeOfTable = encoder.numbers.sizeOfRequests();
    Vector<uint32_t> table(FillWith { }, sizeOfTable / sizeof(uint32_t), 0);
    if (offsets.size() != requests.size() || sizeOfTable % sizeof(uint32_t)) {
        encoder.fail("the requests were not each given a place in a table of whole words"_s);
        encoder.part.setTable(ProgramTableIndex::requests, table);
        return;
    }

    Vector<uint32_t> offsetsOfTablesInImage;
    for (uint32_t place = 0; place < requests.size(); ++place) {
        std::span<const uint32_t> key = requests.keyAt(place);
        std::optional<size_t> size = sizeOfRecordInRequestTable(encoder.program, key);
        if (!size) {
            encoder.fail("the key of a request names no record of the program"_s, { }, makeString("the request numbered "_s, place));
            continue;
        }
        uint32_t offset = offsets[place];
        uint32_t end = place + 1 < requests.size() ? offsets[place + 1] : sizeOfTable;
        if (offset % sizeof(uint32_t) || end > sizeOfTable || offset > end || *size > end - offset) {
            encoder.fail("the place that code holds for a request has no room for its record"_s, { }, makeString("the request numbered "_s, place));
            continue;
        }

        std::span<uint8_t> bytes = asMutableByteSpan(table.mutableSpan()).subspan(offset, *size);
        std::span<const uint32_t> words = key.subspan(1);
        switch (static_cast<RequestKind>(key[0])) {
        case RequestKind::NewClass:
            encodeNewClassRequest(encoder, LayoutID(words[0]), bytes);
            break;
        case RequestKind::TemplateObject:
            encodeTemplateObjectRequest(encoder, words, bytes);
            break;
        case RequestKind::DirectEval:
            encodeDirectEvalRequest(encoder, Typed::EvalScopeID(words[0]), ModuleID(words[1]), bytes);
            break;
        case RequestKind::Data:
            encodeDataRequest(encoder, DataID(words[0]), static_cast<Typed::DataEncoding>(words[1]), bytes);
            break;
        case RequestKind::CollectionData: {
            auto& request = consumeAndReinterpretCastTo<CollectionDataRequest>(bytes);
            request.tableInImage = encoder.firstParts->tablesInImage[place];
            if (request.tableInImage)
                offsetsOfTablesInImage.append(offset + static_cast<uint32_t>(offsetof(CollectionDataRequest, tableInImage)));
            else
                request.elements = encoder.dataNodeOf(DataID(words[0]));
            break;
        }
        case RequestKind::FormatPattern:
            encodeFormatPatternRequest(words, bytes);
            break;
        }
        RELEASE_ASSERT(bytes.empty());
    }
    uint32_t startOfTable = encoder.part.setTable(ProgramTableIndex::requests, table);
    for (uint32_t offsetInTable : offsetsOfTablesInImage)
        encoder.part.link(startOfTable + offsetInTable, TablePartID::Data);
}

static void encodeTableOfPropertyRuns(TableEncoder& encoder)
{
    const NumberedKeys& runs = encoder.numbers.propertyRuns();
    uint64_t numberOfWords = static_cast<uint64_t>(runs.size()) + 1;
    for (uint32_t run = 0; run < runs.size(); ++run)
        numberOfWords += 1 + runs.keyAt(run).size();

    Vector<uint32_t> table;
    if (numberOfWords > UINT32_MAX) {
        encoder.fail("the runs of properties stored together take more words than the start of a run counts"_s);
        encoder.part.setTable(ProgramTableIndex::propertyRuns, table);
        return;
    }

    table.reserveInitialCapacity(static_cast<size_t>(numberOfWords));
    uint32_t start = runs.size() + 1;
    for (uint32_t run = 0; run < runs.size(); ++run) {
        table.append(start);
        start += static_cast<uint32_t>(1 + runs.keyAt(run).size());
    }
    table.append(start);

    for (uint32_t run = 0; run < runs.size(); ++run) {
        std::span<const uint32_t> keys = runs.keyAt(run);
        PropertyRunHeader header { };
        header.count = static_cast<uint32_t>(keys.size());
        if (header.count != keys.size())
            encoder.fail("a run of properties stored together has more names than its header counts"_s, { }, makeString("the run numbered "_s, run));
        table.append(header.encoded());
        for (uint32_t key : keys)
            table.append(encoder.identifierNumber(PropertyKeyID(key), "in a run of properties stored together"_s));
    }
    RELEASE_ASSERT(table.size() == numberOfWords);
    encoder.part.setTable(ProgramTableIndex::propertyRuns, table);
}

void encodeRequestTables(TableEncoder& encoder)
{
    encodeTableOfRequests(encoder);
    encodeTableOfPropertyRuns(encoder);
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
