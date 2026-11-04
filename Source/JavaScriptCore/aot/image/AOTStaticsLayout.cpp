/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTNumbering.h"

#if ENABLE(AOT)

#include "AOTArrayAllocationProfile.h"
#include "AOTBuildContext.h"
#include "AOTClassGuards.h"
#include "AOTIRProgram.h"
#include "AOTInstanceLayout.h"
#include "AOTPlan.h"
#include "AOTSort.h"
#include "AOTStringUses.h"
#include "AOTThreads.h"
#include <algorithm>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/MathExtras.h>
#include <wtf/Noncopyable.h>

namespace JSC { namespace AOT {

namespace {

class StaticsLayoutBuilder {
    WTF_MAKE_NONCOPYABLE(StaticsLayoutBuilder);
public:
    StaticsLayoutBuilder(const NumberingInputs& inputs, const GatheredMentions& mentions, const NamesOfTables& tables, BuildContext& context)
        : m_program(inputs.program)
        , m_plan(inputs.plan)
        , m_mentions(mentions)
        , m_tables(tables)
        , m_context(context)
        , m_limit(context.limit(LimitID::MaxStaticsSize))
        , m_numberOfGroupsOfFiles(inputs.plan.staticsGroups().groups.size())
    {
    }

    // Returns false when the statics are too large. The error has been reported by then.
    bool run()
    {
        createTables();
        return placeVariables()
            && placeModuleValues()
            && placeClassGuards()
            && placeModuleBuiltinCells()
            && placeModuleNamespaces()
            && placeData()
            && placeCellsOfLiterals()
            && placeArrayProfiles()
            && placeEnvironmentsHandedBack()
            && placeMethodTable()
            && finish();
    }

    PlacedStatics takeStatics() { return WTF::move(m_statics); }
    Vector<uint32_t> takeOriginsOfMethodSelectors() { return WTF::move(m_originsOfMethodSelectors); }
    uint32_t sizeOfMethodTable() const { return m_sizeOfMethodTable; }

private:
    using PlannedVariable = Typed::PlannedVariable;

    // The variables of a region lie in this order, so that equal first words lie side by side and nothing is lost to alignment.
    // The words that the collector visits lie together, so the image lists them as a few ranges.
    enum class WordKind : uint8_t {
        UndefinedValue,
        UndefinedInteger,
        UndefinedDouble,
        AbsentInteger,
        AbsentDouble,
        UndefinedReference4,
        UndefinedInteger4,
        AbsentInteger4,
        UndefinedByte,
        ZeroValue,
        ZeroFunction,
        ZeroWord8,
        ZeroReference4,
        ZeroWord4,
        ZeroWord2,
        ZeroWord1,
    };

    // The variables of one kind in one region, or in one group of files, in the order of their ids.
    struct Bucket {
        uint64_t size { 0 };
        uint32_t nextOffset { 0 };
        uint32_t lastVariable { 0 };
        unsigned width { 0 };
    };

    static constexpr unsigned numberOfFilledWordKinds = 9;
    static constexpr unsigned numberOfWordKinds = 16;
    static constexpr unsigned wordSize = sizeof(uint64_t);
    static constexpr unsigned bitsPerByte = 8;
    static constexpr uint32_t noBucket = UINT32_MAX;

    static_assert(static_cast<unsigned>(WordKind::ZeroValue) == numberOfFilledWordKinds && static_cast<unsigned>(WordKind::ZeroWord1) + 1 == numberOfWordKinds);
    static_assert(!traitsOf(SlotRepresentation::JSValue).absentBits && !traitsOf(SlotRepresentation::LiteralString).absentBits);
    static_assert(!traitsOf(SlotRepresentation::OptionalCell32).absentBits && !traitsOf(SlotRepresentation::LiteralString32).absentBits && !traitsOf(SlotRepresentation::OptionalBoolean).absentBits);
    static_assert(widthOf(SlotRepresentation::JSValue) == wordSize && widthOf(SlotRepresentation::LiteralString) == wordSize && widthOf(SlotRepresentation::OptionalInt64) == wordSize && widthOf(SlotRepresentation::OptionalDouble) == wordSize);
    static_assert(widthOf(SlotRepresentation::OptionalCell32) == 4 && widthOf(SlotRepresentation::LiteralString32) == 4 && widthOf(SlotRepresentation::OptionalInt32) == 4 && widthOf(SlotRepresentation::OptionalBoolean) == 1);
    static_assert(defaultLimitValue(LimitID::MaxStaticsSize, Architecture::ARM64) <= UINT32_MAX && defaultLimitValue(LimitID::MaxStaticsSize, Architecture::X86_64) <= UINT32_MAX);
    static_assert(sizeof(EncodedJSValue) == wordSize && sizeof(JSCell*) == wordSize);
    static_assert(byteOffsetOfClassGuardBit(3 * bitsPerByte) == 3 && maskOfClassGuardBit(3 * bitsPerByte) == 1 && noClassGuardBit % bitsPerByte);
    static_assert(sizeof(ArrayAllocationProfile) == wordSize && ArrayAllocationProfile::sizeOfLastArrayWord == wordSize);

    static WordKind kindOfZeroWord(const PlannedVariable& planned)
    {
        SlotTraits traits = traitsOf(planned.representation);
        switch (traits.width) {
        case 8:
            if (!traits.holdsCell)
                return WordKind::ZeroWord8;
            return planned.encoding == Typed::VariableEncoding::FunctionAsZero ? WordKind::ZeroFunction : WordKind::ZeroValue;
        case 4:
            return traits.isNarrowReference ? WordKind::ZeroReference4 : WordKind::ZeroWord4;
        case 2:
            return WordKind::ZeroWord2;
        case 1:
            return WordKind::ZeroWord1;
        default:
            RELEASE_ASSERT_NOT_REACHED();
        }
    }

    static WordKind kindOfWord(const PlannedVariable& planned)
    {
        if (!planned.initialWord)
            return kindOfZeroWord(planned);
        bool startsUndefined = planned.initialWord == traitsOf(planned.representation).undefinedBits;
        switch (planned.representation) {
        case SlotRepresentation::JSValue:
        case SlotRepresentation::LiteralString:
            return WordKind::UndefinedValue;
        case SlotRepresentation::OptionalInt64:
            return startsUndefined ? WordKind::UndefinedInteger : WordKind::AbsentInteger;
        case SlotRepresentation::OptionalDouble:
            return startsUndefined ? WordKind::UndefinedDouble : WordKind::AbsentDouble;
        case SlotRepresentation::OptionalCell32:
        case SlotRepresentation::LiteralString32:
            return WordKind::UndefinedReference4;
        case SlotRepresentation::OptionalInt32:
            return startsUndefined ? WordKind::UndefinedInteger4 : WordKind::AbsentInteger4;
        case SlotRepresentation::OptionalBoolean:
            return WordKind::UndefinedByte;
        case SlotRepresentation::Uint8:
        case SlotRepresentation::Int8:
        case SlotRepresentation::Uint16:
        case SlotRepresentation::Int16:
        case SlotRepresentation::Int32:
        case SlotRepresentation::Uint32:
        case SlotRepresentation::Int64:
        case SlotRepresentation::Float:
        case SlotRepresentation::Double:
        case SlotRepresentation::Cell:
        case SlotRepresentation::Boolean:
        case SlotRepresentation::Cell32:
        case SlotRepresentation::DeferredClosure:
            break;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    // Filling a word at instance creation is always correct: no code that names the variable has run yet.
    static StaticsRegion regionOf(const PlannedVariable& planned, std::optional<uint32_t> groupOfFiles)
    {
        // Zero words lie after the words of the files, where the run time fills nothing.
        if (!planned.initialWord)
            return StaticsRegion::FilledAtInstanceCreation;
        switch (planned.region) {
        case StaticsRegion::FilledAtInstanceCreation:
            return StaticsRegion::FilledAtInstanceCreation;
        case StaticsRegion::FilledAtProgramEntry:
            // Until the program is entered these words are neither filled nor writable.
            return planned.isUsedBeforeProgramEntry ? StaticsRegion::FilledAtInstanceCreation : StaticsRegion::FilledAtProgramEntry;
        case StaticsRegion::FilledWithFile:
            ASSERT(groupOfFiles);
            return groupOfFiles ? StaticsRegion::FilledWithFile : StaticsRegion::FilledAtInstanceCreation;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    bool isNamed(VariableID id, const PlannedVariable& planned) const
    {
        return isVariableOfStaticsNamed(planned, id, m_tables, m_mentions);
    }

    std::optional<uint32_t> groupOfFilesOf(VariableID id, const PlannedVariable& planned) const
    {
        if (planned.region != StaticsRegion::FilledWithFile)
            return std::nullopt;
        VariableID written = id.index() < m_program.variables.size() ? id : m_plan.deferredDefinitions().variableBesideAdded(id);
        return m_plan.staticsGroups().groupFilledWhenEntered(m_program.at(written).module);
    }

    // The buckets in order: filled at program entry, filled at instance creation, filled with each group of files, zero.
    unsigned firstBucketOfFiles(uint32_t group) const { return numberOfFilledWordKinds * (2 + group); }
    unsigned firstBucketOfZeroes() const { return firstBucketOfFiles(m_numberOfGroupsOfFiles); }

    unsigned bucketOf(const PlannedVariable& planned, std::optional<uint32_t> groupOfFiles) const
    {
        unsigned kind = static_cast<unsigned>(kindOfWord(planned));
        if (kind >= numberOfFilledWordKinds)
            return firstBucketOfZeroes() + kind - numberOfFilledWordKinds;
        switch (planned.region) {
        case StaticsRegion::FilledAtProgramEntry:
            return kind;
        case StaticsRegion::FilledAtInstanceCreation:
            return numberOfFilledWordKinds + kind;
        case StaticsRegion::FilledWithFile:
            return firstBucketOfFiles(*groupOfFiles) + kind;
        }
        RELEASE_ASSERT_NOT_REACHED();
    }

    uint32_t allocate(uint64_t size, unsigned alignment)
    {
        m_end = WTF::roundUpToMultipleOf(alignment, m_end);
        return static_cast<uint32_t>(std::exchange(m_end, m_end + size));
    }

    uint32_t alignToWord() { return allocate(0, wordSize); }

    uint32_t allocateRoot()
    {
        uint32_t offset = allocate(wordSize, wordSize);
        m_statics.roots.append(offset);
        return offset;
    }

    void allocateRootForEach(const BitVector& ids, Vector<uint32_t>& offsets)
    {
        for (size_t id : ids)
            offsets[id] = allocateRoot();
    }

    bool fitsWithin(uint64_t limit)
    {
        if (m_end <= limit)
            return true;
        m_context.reportLimitExceeded(LimitID::MaxStaticsSize, m_end);
        return false;
    }

    // Every table gets its full size first, so a reader after a failed build never indexes out of bounds.
    void createTables()
    {
        uint32_t numberOfVariables = m_plan.slots().numberOfVariables();
        RELEASE_ASSERT(numberOfVariables >= m_program.variables.size());
        m_statics.variables = Vector<PlacedVariable>(numberOfVariables);

        const Vector<Typed::StaticsGroup>& groups = m_plan.staticsGroups().groups;
        m_statics.groupsOfFilesFilledWhenEntered = Vector<PlacedStaticsOfFiles>(groups.size(), [&](size_t index) {
            return PlacedStaticsOfFiles { groups[index].firstFile, 0, 0, groups[index].importedGroups };
        });

        auto withoutOffsets = [](size_t size) {
            return Vector<uint32_t>(FillWith { }, size, PlacedStatics::noOffset);
        };
        m_statics.moduleValueOffsets = withoutOffsets(m_program.modules.size());
        m_statics.moduleNamespaceOffsets = withoutOffsets(m_program.modules.size());
        m_statics.dataCellOffsets = withoutOffsets(m_program.data.size());
        m_statics.dataStorageOffsets = withoutOffsets(m_program.data.size());
        m_statics.regExpLiteralCellOffsets = withoutOffsets(m_mentions.keys(NumberSpace::RegExpLiteralCell).size());
        m_statics.templateObjectCellOffsets = withoutOffsets(m_mentions.keys(NumberSpace::TemplateObjectCell).size());
        m_statics.arrayProfileOffsets = withoutOffsets(m_mentions.keys(NumberSpace::ArrayProfile).size());
        m_statics.environmentHandedBackOffsets = withoutOffsets(m_program.layouts.size());
        m_statics.classGuardBits = Vector<uint32_t>(FillWith { }, m_program.layouts.size() * numberOfClassSides, noClassGuardBit);
    }

    // Returns the last bucket that holds a variable, or null.
    const Bucket* placeBuckets(std::span<Bucket> buckets)
    {
        const Bucket* last = nullptr;
        for (Bucket& bucket : buckets) {
            if (!bucket.size)
                continue;
            bucket.nextOffset = allocate(bucket.size, bucket.width);
            last = &bucket;
        }
        return last;
    }

    void fillRestOfWordLike(const PlacedVariable& variable)
    {
        unsigned width = widthOf(variable.plan.representation);
        while (m_end % wordSize)
            m_statics.paddingOfFilesFilledWhenEntered.append({ allocate(width, width), variable.plan.representation, variable.initialWord });
    }

    // The counts come behind every guard: firstWordOfClassGuardCountKey.
    uint32_t placeOfFirstClassGuardCount() const
    {
        const NumberedKeys& guards = m_mentions.keys(NumberSpace::ClassGuard);
        uint32_t first = guards.size();
        while (first && isClassGuardCountKey(guards.keyAt(first - 1)))
            --first;
        return first;
    }

    // Every bit of a guard starts clear, which says of a guard that begins with the first evaluation of its class that it does not hold.
    // Counts with equal first values lie side by side, in the order of their keys, so the image lists what they start as in a few runs.
    void placeClassGuardCounts()
    {
        struct CountToPlace {
            uint32_t firstValue { 0 };
            uint32_t index { 0 };
        };
        const NumberedKeys& guards = m_mentions.keys(NumberSpace::ClassGuard);
        uint32_t firstCount = placeOfFirstClassGuardCount();
        Vector<CountToPlace> counts(guards.size() - firstCount, [&](size_t index) {
            CountToPlace count { 0, static_cast<uint32_t>(index) };
            forEachGuardInClassGuardCountKey(guards.keyAt(firstCount + index), [&](std::span<const uint32_t> guard) {
                LayoutID classLayout(guard[0]);
                // placeClassGuards() reports the guard of a class without a record.
                if (m_plan.hasClassRecord(classLayout) && !m_plan.bitsOfClassGuardsAreClearWhileTheyHold(classLayout, sideOfClassGuardKey(guard)))
                    ++count.firstValue;
            });
            return count;
        });
        AOT::stableSort(counts, { }, &CountToPlace::firstValue);

        m_offsetsOfClassGuardCounts.fill(0, counts.size());
        m_statics.classGuardCounts.reserveInitialCapacity(counts.size());
        for (const CountToPlace& count : counts) {
            uint32_t offset = allocate(sizeof(uint32_t), sizeof(uint32_t));
            m_offsetsOfClassGuardCounts[count.index] = offset;
            m_statics.classGuardCounts.append({ offset, count.firstValue });
        }
    }

    // Behind the bits of the guards.
    void listGuardsOfClassGuardCounts()
    {
        const NumberedKeys& guards = m_mentions.keys(NumberSpace::ClassGuard);
        uint32_t firstCount = placeOfFirstClassGuardCount();
        size_t numberOfCounts = guards.size() - firstCount;
        RELEASE_ASSERT(numberOfCounts == m_offsetsOfClassGuardCounts.size());

        Vector<size_t> firstGuardOfCount;
        firstGuardOfCount.reserveInitialCapacity(numberOfCounts);
        size_t numberOfGuards = 0;
        for (size_t index = 0; index < numberOfCounts; ++index) {
            firstGuardOfCount.append(numberOfGuards);
            forEachGuardInClassGuardCountKey(guards.keyAt(firstCount + index), [&](std::span<const uint32_t>) {
                ++numberOfGuards;
            });
        }
        m_statics.guardsOfClassGuardCounts.grow(numberOfGuards);
        forEachRangeOfIndicesInParallel(m_context.parallelLoops(), numberOfCounts, [&](size_t first, size_t end) {
            for (size_t index = first; index < end; ++index) {
                size_t next = firstGuardOfCount[index];
                forEachGuardInClassGuardCountKey(guards.keyAt(firstCount + index), [&](std::span<const uint32_t> guard) {
                    // gatherMentions() mentions each guard of a count.
                    std::optional<uint32_t> place = guards.find(guard);
                    RELEASE_ASSERT(place && *place < firstCount);
                    m_statics.guardsOfClassGuardCounts[next++] = { m_statics.bitsOfClassGuards[*place], m_offsetsOfClassGuardCounts[index] };
                });
            }
        });
        parallelSort(m_context.parallelLoops(), m_statics.guardsOfClassGuardCounts.mutableSpan(), std::less<PlacedGuardOfClassGuardCount> { });
    }

    bool placeVariables()
    {
        Vector<Bucket> buckets(firstBucketOfZeroes() + numberOfWordKinds - numberOfFilledWordKinds);
        Vector<uint32_t> bucketOfVariable(FillWith { }, m_statics.variables.size(), noBucket);
        // What is asked of the plan, the tables and the mentions is asked of each variable by itself. Only the sums go through the variables in their order.
        forEachRangeOfIndicesInParallel(m_context.parallelLoops(), m_statics.variables.size(), [&](size_t first, size_t end) {
            for (size_t index = first; index < end; ++index) {
                VariableID id(index);
                PlacedVariable& placed = m_statics.variables[index];
                placed.plan = m_plan.variable(id);
                placed.function = placed.plan.function;
                placed.hasWord = placed.plan.hasWordInStatics() && isNamed(id, placed.plan);
                if (!placed.hasWord)
                    continue;
                placed.initialWord = placed.plan.initialWord;
                std::optional<uint32_t> groupOfFiles = groupOfFilesOf(id, placed.plan);
                placed.plan.region = regionOf(placed.plan, groupOfFiles);
                bucketOfVariable[index] = bucketOf(placed.plan, groupOfFiles);
            }
        });
        for (uint32_t index = 0; index < m_statics.variables.size(); ++index) {
            if (bucketOfVariable[index] == noBucket)
                continue;
            unsigned width = widthOf(m_statics.variables[index].plan.representation);
            Bucket& bucket = buckets[bucketOfVariable[index]];
            RELEASE_ASSERT(!bucket.size || bucket.width == width);
            bucket.width = width;
            bucket.size += width;
            bucket.lastVariable = index;
        }

        std::span<Bucket> allBuckets = buckets.mutableSpan();
        placeBuckets(allBuckets.first(numberOfFilledWordKinds));
        m_statics.sizeFilledAtProgramEntry = alignToWord();
        placeBuckets(allBuckets.subspan(numberOfFilledWordKinds, numberOfFilledWordKinds));
        placeClassGuardCounts();
        if (m_numberOfGroupsOfFiles) {
            m_statics.firstWordFilledWithFiles = alignToWord() / wordSize;
            // A group without words begins where the next one does.
            for (uint32_t group = 0; group < m_numberOfGroupsOfFiles; ++group) {
                PlacedStaticsOfFiles& files = m_statics.groupsOfFilesFilledWhenEntered[group];
                files.firstWord = alignToWord() / wordSize;
                if (const Bucket* last = placeBuckets(allBuckets.subspan(firstBucketOfFiles(group), numberOfFilledWordKinds)))
                    fillRestOfWordLike(m_statics.variables[last->lastVariable]);
                files.numberOfWords = alignToWord() / wordSize - files.firstWord;
            }
            // The run time expects one byte per group right after the last word.
            m_statics.endWordFilledWithFiles = alignToWord() / wordSize;
            m_statics.offsetOfFilledFiles = allocate(m_numberOfGroupsOfFiles, 1);
        }
        placeBuckets(allBuckets.subspan(firstBucketOfZeroes()));

        for (uint32_t index = 0; index < m_statics.variables.size(); ++index) {
            if (bucketOfVariable[index] == noBucket)
                continue;
            PlacedVariable& placed = m_statics.variables[index];
            Bucket& bucket = buckets[bucketOfVariable[index]];
            placed.offset = std::exchange(bucket.nextOffset, bucket.nextOffset + bucket.width);
            // A root is visited as a word of eight bytes. The image lists the narrow references in a table of their own.
            SlotTraits traits = traitsOf(placed.plan.representation);
            if (!traits.holdsCell || traits.isNarrowReference)
                continue;
            RELEASE_ASSERT(bucket.width == wordSize && !(placed.offset % wordSize));
            m_statics.roots.append(placed.offset);
        }
        return fitsWithin(m_limit);
    }

    bool placeModuleValues()
    {
        for (uint32_t index = 0; index < m_program.modules.size(); ++index) {
            ModuleID module(index);
            if (!m_plan.hasModuleValue(module))
                continue;
            uint32_t offset = allocateRoot();
            m_statics.moduleValueOffsets[index] = offset;
            if (m_plan.hasRequireCacheValue(module)) {
                uint32_t requireCacheValueOffset = allocateRoot();
                RELEASE_ASSERT(requireCacheValueOffset == offset + wordSize);
            }
        }
        return fitsWithin(m_limit);
    }

    // Every byte starts clear, whichever way the guard of the class holds.
    bool placeClassGuards()
    {
        const NumberedKeys& guards = m_mentions.keys(NumberSpace::ClassGuard);
        if (!guards.size())
            return true;
        uint32_t firstCount = placeOfFirstClassGuardCount();
        for (uint32_t index = 0; index < firstCount; ++index) {
            std::span<const uint32_t> key = guards.keyAt(index);
            bool isAskedByStoresAlone = key.size() == 4 && key[1] == sideWordOfClassGuardOfName(ClassSide::Prototype, true);
            RELEASE_ASSERT(key.size() >= 2 && !(key.size() % 2) && (key[1] < numberOfClassSides || isAskedByStoresAlone));
            ClassSide side = isAskedByStoresAlone ? ClassSide::Prototype : static_cast<ClassSide>(key[1]);
            LayoutID classLayout(key[0]);
            // The run time ends a guard through the number in the record of the class. Without one, a guard that holds while clear would hold forever.
            if (!m_plan.hasClassRecord(classLayout)) {
                m_context.diagnostics().add(InternalError {
                    .kind = InternalErrorKind::PlacementCheckFailed,
                    .layout = classLayout,
                    .detail = "code tests a guard of a class that has no record, so nothing would ever end the guard"_s,
                });
            }
            uint32_t bit = allocate(1, 1) * bitsPerByte;
            m_statics.bitsOfClassGuards.append(bit);
            if (key.size() == 2) {
                m_statics.classGuardBits[classLayout.index() * numberOfClassSides + key[1]] = bit;
                continue;
            }
            // The keys come in ascending order, word by word, so each list does too: the one by its keys, the other by class and side.
            Vector<PlacedClassGuardOfName>& list = key.size() == 4 ? m_statics.classGuardsOfNames : m_statics.namesOfClassGuardsOfSeveralNames;
            for (std::span<const uint32_t> names = key.subspan(2); !names.empty(); names = names.subspan(2)) {
                RELEASE_ASSERT(names[0] == static_cast<uint32_t>(NumberSpace::Name) || names[0] == static_cast<uint32_t>(NumberSpace::NameOfText));
                list.append(PlacedClassGuardOfName { classLayout, side, static_cast<NumberSpace>(names[0]), names[1], bit, isAskedByStoresAlone });
            }
        }
        for (uint32_t offset : m_offsetsOfClassGuardCounts)
            m_statics.bitsOfClassGuards.append(offset * bitsPerByte);
        listGuardsOfClassGuardCounts();
        return fitsWithin(std::min<uint64_t>(m_limit, UINT32_MAX / bitsPerByte));
    }

    bool placeModuleBuiltinCells()
    {
        const NumberedKeys& cells = m_mentions.keys(NumberSpace::ModuleBuiltinCell);
        m_statics.moduleBuiltinCells.reserveInitialCapacity(cells.size());
        for (uint32_t index = 0; index < cells.size(); ++index) {
            std::span<const uint32_t> key = cells.keyAt(index);
            RELEASE_ASSERT(key.size() == 2);
            m_statics.moduleBuiltinCells.append({ BuiltinID(key[0]), ModuleID(key[1]), allocateRoot() });
        }
        return fitsWithin(m_limit);
    }

    bool placeModuleNamespaces()
    {
        for (size_t index : m_mentions.byID(NumberSpace::ModuleNamespace).isMentioned) {
            if (m_plan.hasModuleNamespaceWord(ModuleID(index)))
                m_statics.moduleNamespaceOffsets[index] = allocateRoot();
        }
        return fitsWithin(m_limit);
    }

    bool placeData()
    {
        allocateRootForEach(m_mentions.byID(NumberSpace::DataStorage).isMentioned, m_statics.dataStorageOffsets);
        allocateRootForEach(m_mentions.byID(NumberSpace::DataCell).isMentioned, m_statics.dataCellOffsets);
        return fitsWithin(m_limit);
    }

    bool placeCellsOfLiterals()
    {
        for (uint32_t& offset : m_statics.regExpLiteralCellOffsets)
            offset = allocateRoot();
        for (uint32_t& offset : m_statics.templateObjectCellOffsets)
            offset = allocateRoot();
        return fitsWithin(m_limit);
    }

    // A profile of zeroes is due for review at its first allocation. The word of the last array is no root: it neither keeps the array alive nor outlives it.
    bool placeArrayProfiles()
    {
        uint64_t sizeOfProfiles = static_cast<uint64_t>(m_statics.arrayProfileOffsets.size()) * wordSize;
        if (!sizeOfProfiles)
            return true;
        for (uint32_t& offset : m_statics.arrayProfileOffsets)
            offset = allocate(wordSize, wordSize);
        uint32_t firstLastArray = allocate(sizeOfProfiles, wordSize);
        if (!fitsWithin(m_limit))
            return false;
        RELEASE_ASSERT(firstLastArray == m_statics.arrayProfileOffsets.first() + sizeOfProfiles);
        m_statics.distanceFromArrayProfileToLastArray = static_cast<uint32_t>(sizeOfProfiles);
        m_statics.clearedEachCollection.append({ firstLastArray, static_cast<uint32_t>(sizeOfProfiles) });
        return true;
    }

    // The word is no root: nothing else holds the cell that lies there, so a collection frees the cell, and what the cell still names with it.
    bool placeEnvironmentsHandedBack()
    {
        const BitVector& layouts = m_mentions.byID(NumberSpace::EnvironmentHandedBack).isMentioned;
        if (layouts.isEmpty())
            return true;
        uint32_t first = alignToWord();
        for (size_t layout : layouts)
            m_statics.environmentHandedBackOffsets[layout] = allocate(wordSize, wordSize);
        if (!fitsWithin(m_limit))
            return false;
        m_statics.clearedEachCollection.append({ first, static_cast<uint32_t>(m_end - first) });
        return true;
    }

    // A selector that code mentions has a word for each shape of each row of its family. The selectors with the most words are placed first, each as low as the words that are taken allow.
    // The table lies apart from the statics, in pages of its own: InstanceLayout::offsetOfMethodTable().
    // A word between those of the selectors belongs to nobody, and nobody reads it: a call through the table has proven that its receiver has a row.
    bool placeMethodTable()
    {
        const Typed::CallingConventions& conventions = m_plan.conventions();
        m_originsOfMethodSelectors.fill(PlacedStatics::noOffset, conventions.methodSelectors().size());
        const BitVector& mentioned = m_mentions.byID(NumberSpace::MethodSelector).isMentioned;
        if (mentioned.isEmpty())
            return true;

        auto forEachShapeOf = [&](uint32_t selector, NOESCAPE const Invocable<void(uint32_t)> auto& functor) {
            for (const Typed::MethodTableRow& row : conventions.familyOf(conventions.methodSelector(selector)).rows) {
                Typed::ShapeRange shapes = m_plan.shapes().shapesOf(row.layout);
                for (uint32_t shape = shapes.first; shape < shapes.end(); ++shape)
                    functor(shape);
            }
        };

        struct SelectorToPlace {
            uint32_t numberOfWords { 0 };
            uint32_t selector { 0 };
        };
        Vector<SelectorToPlace> selectors;
        for (size_t selector : mentioned) {
            SelectorToPlace toPlace { 0, static_cast<uint32_t>(selector) };
            forEachShapeOf(toPlace.selector, [&](uint32_t) {
                ++toPlace.numberOfWords;
            });
            selectors.append(toPlace);
        }
        AOT::sort(selectors, [](const SelectorToPlace& a, const SelectorToPlace& b) {
            return std::pair(b.numberOfWords, a.selector) < std::pair(a.numberOfWords, b.selector);
        });

        BitVector isTaken;
        uint64_t firstFreeWord = 0;
        uint64_t numberOfWords = 0;
        Vector<uint32_t> shapes;
        for (const SelectorToPlace& toPlace : selectors) {
            shapes.shrink(0);
            forEachShapeOf(toPlace.selector, [&](uint32_t shape) {
                shapes.append(shape);
            });
            // No object is made of any layout with a row, so the calls that mention the selector never run. They still need a number.
            if (shapes.isEmpty()) {
                m_originsOfMethodSelectors[toPlace.selector] = 0;
                continue;
            }
            auto [leastShape, greatestShape] = std::ranges::minmax(shapes);
            // The word of the greatest shape is the lowest, and no word below the first free one is free.
            uint64_t origin = firstFreeWord + greatestShape;
            while (std::ranges::any_of(shapes, [&](uint32_t shape) { return isTaken.get(origin - shape); }))
                ++origin;
            if (origin * sizeof(uint32_t) > m_limit) {
                m_context.reportLimitExceeded(LimitID::MaxStaticsSize, origin * sizeof(uint32_t));
                return false;
            }
            // A BitVector grows to the size asked for and no further, and the origins rise word by word.
            if (uint64_t end = origin - leastShape + 1; end > isTaken.size())
                isTaken.ensureSize(std::max<size_t>(end, 2 * isTaken.size()));
            for (uint32_t shape : shapes)
                isTaken.quickSet(origin - shape);
            while (isTaken.get(firstFreeWord))
                ++firstFreeWord;
            numberOfWords = std::max(numberOfWords, origin - leastShape + 1);
            m_originsOfMethodSelectors[toPlace.selector] = static_cast<uint32_t>(origin);
        }
        // No origin lies below the last word of its selector.
        m_sizeOfMethodTable = static_cast<uint32_t>(numberOfWords * sizeof(uint32_t));
        return true;
    }

    bool finish()
    {
        alignToWord();
        if (!fitsWithin(m_limit))
            return false;
        parallelSort(m_context.parallelLoops(), m_statics.roots.mutableSpan(), std::less<uint32_t> { });
        m_statics.size = static_cast<uint32_t>(m_end);
        return true;
    }

    const Typed::Program& m_program;
    const Typed::Plan& m_plan;
    const GatheredMentions& m_mentions;
    const NamesOfTables& m_tables;
    BuildContext& m_context;
    const uint64_t m_limit;
    const uint32_t m_numberOfGroupsOfFiles;
    PlacedStatics m_statics;
    Vector<uint32_t> m_originsOfMethodSelectors;
    // In the order of their keys.
    Vector<uint32_t> m_offsetsOfClassGuardCounts;
    uint32_t m_sizeOfMethodTable { 0 };
    uint64_t m_end { 0 };
};

} // anonymous namespace

void ImageNumbersBuilder::layOutStatics()
{
    StaticsLayoutBuilder builder(inputs, mentions, tables, context);
    bool fits = builder.run();
    numbers.m_statics = builder.takeStatics();
    numbers.m_originsOfMethodSelectors = builder.takeOriginsOfMethodSelectors();
    // The offsets of these cells are indexed by place in these lists.
    numbers.m_regExpLiteralCells = mentions.takeKeys(NumberSpace::RegExpLiteralCell);
    numbers.m_templateObjectCells = mentions.takeKeys(NumberSpace::TemplateObjectCell);
    numbers.m_arrayProfiles = mentions.takeKeys(NumberSpace::ArrayProfile);
    numbers.m_classGuards = mentions.takeKeys(NumberSpace::ClassGuard);
    numbers.m_siteCaches = mentions.takeKeys(NumberSpace::SiteCache);
    const NumberedKeys& sites = numbers.m_siteCaches;
    numbers.m_numbersOfSites.resize(sites.size() + 1);
    uint64_t numberOfNumbersOfSites = 0;
    for (uint32_t place = 0; place < sites.size(); ++place) {
        std::span<const uint32_t> key = sites.keyAt(place);
        numbers.m_numbersOfSites[place] = static_cast<uint32_t>(numberOfNumbersOfSites);
        numberOfNumbersOfSites += ImageNumbers::isPlaceWithSiteAnswers(key) && !inputs.plan.slots().hasColumn(ImageNumbers::nameOfSite(key)) ? 2 : 1;
    }
    // A number that does not fit is never read: the instance is then too large to be addressed.
    numbers.m_numbersOfSites[sites.size()] = static_cast<uint32_t>(std::min<uint64_t>(numberOfNumbersOfSites, UINT32_MAX));
    if (!fits)
        return;

    // Only a step of the builder can write the sizes, and this is the last step.
    InstanceSizes& sizes = numbers.m_instanceSizes;
    sizes.sizeOfStatics = numbers.m_statics.size;
    if (numbers.m_stringConstants)
        sizes.numberOfStringCells = numbers.m_stringConstants->count();
    sizes.numberOfGlobalBindings = numbers.m_globalBindings.size();
    sizes.numberOfSiteCaches = numbers.m_numbersOfSites.last();
    sizes.sizeOfMethodTable = builder.sizeOfMethodTable();
    context.statistics().count(StatisticID::MethodTableSelectors, mentions.byID(NumberSpace::MethodSelector).isMentioned.bitCount());
    context.statistics().count(StatisticID::MethodTableBytes, sizes.sizeOfMethodTable);
    context.statistics().count(StatisticID::ClassGuardCounts, numbers.m_statics.classGuardCounts.size());
    context.statistics().count(StatisticID::GuardsOfClassGuardCounts, numbers.m_statics.guardsOfClassGuardCounts.size());
    if (!InstanceLayout::isAddressable(sizes))
        context.reportLimitExceeded(LimitID::MaxStaticsSize, std::max(InstanceLayout::bytesReservedBelowInstance(sizes), InstanceLayout::bytesReservedFromInstance(sizes)));
}

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
