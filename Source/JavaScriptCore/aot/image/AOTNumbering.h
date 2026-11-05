/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinTableRealm.h"
#include "AOTCodeRole.h"
#include "AOTID.h"
#include "AOTImageForward.h"
#include "AOTInstanceLayout.h"
#include "AOTOperationData.h"
#include "AOTSlotPlan.h"
#include "AOTVector.h"
#include "AOTWire.h"
#include "JSExportMacros.h"
#include "YarrFlags.h"
#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/ScopedLambda.h>

namespace JSC { namespace AOT {

class BuildContext;
class IdentifierNumbering;
class ParallelLoops;
class LinkedFunctions;
class RegExpRequests;
class StringConstantNumbering;
struct CodeBody;

namespace Typed {
class Facts;
class Plan;
class Program;
class TypeArena;
}

using Typed::CodeUnit;
using Typed::DataID;
using Typed::FieldIndex;
using Typed::LayoutID;
using Typed::PropertyKeyID;
using Typed::TypeID;
using Typed::VariableID;

// The second word of the key of the guard of one name in NumberSpace::ClassGuard.
constexpr uint32_t sideWordOfClassGuardOfName(ClassSide side, bool isAskedByStoresAlone) { return static_cast<uint32_t>(side) + (isAskedByStoresAlone ? numberOfEnumerators<ClassSide> : 0); }
// Of the key of one guard there, of a side or of names.
inline ClassSide sideOfClassGuardKey(std::span<const uint32_t> key) { return static_cast<ClassSide>(key[1] % numberOfEnumerators<ClassSide>); }

// The key of one guard in NumberSpace::ClassGuard.
using ClassGuardKey = Vector<uint32_t, 4>;

// The key there of a word that counts guards (object/AOTClassGuards.h) begins with this word, which is the number of no layout, so the counts come behind every guard.
// Then, for each of its guards, how many words the key of the guard has, and the key. The guards ascend, word by word, and none comes twice: tests that list the same guards mention one word.
inline constexpr uint32_t firstWordOfClassGuardCountKey = UINT32_MAX;
inline bool isClassGuardCountKey(std::span<const uint32_t> key) { return !key.empty() && key[0] == firstWordOfClassGuardCountKey; }

// False: the key lists something else than two guards or more.
template<typename Functor>
bool forEachGuardInClassGuardCountKey(std::span<const uint32_t> key, NOESCAPE const Functor& functor)
{
    if (!isClassGuardCountKey(key))
        return false;
    size_t numberOfGuards = 0;
    for (std::span<const uint32_t> rest = key.subspan(1); !rest.empty(); ++numberOfGuards) {
        size_t numberOfWords = rest[0];
        if (numberOfWords < 2 || numberOfWords >= rest.size() || rest[1] == firstWordOfClassGuardCountKey)
            return false;
        functor(rest.subspan(1, numberOfWords));
        rest = rest.subspan(1 + numberOfWords);
    }
    return numberOfGuards > 1;
}

// Code does not know the number of a thing that only Image can count. It mentions the thing by what it is, and Image patches the number in.
// The words of a key are ids of the program, of Facts' tables or of the plan, or plain content. An id that a thread made while it compiled is never one of them,
// but for a text that the walk has made (TypeArena::internText()), wherever a key names a text that is no name of a property: its bytes go along in the CodeBody, and Image puts an id of the program in its place before it gathers anything.
// Things are numbered in the order of their keys or by a rule over their keys, never in the order in which threads met them.
// What is content already has no space here: a set of shapes that code tests by its bits, a double, a table of a switch are CodeBody::constants and jumpTables, which the constant pool shares by their bytes.
//
//                      key                                             Number                                  InstanceWord
enum class NumberSpace : uint8_t {
    Name,               // PropertyKeyID                                the number of the identifier            -
    NameOfText,         // StringID                                     the number of the identifier            -
    String,             // StringID                                     the number of the string's record       -
    StringCell,         // StringID                                     the number of the string constant       its cell
    KeyCell,            // PropertyKeyID                                the number of the key cell              -
    BuiltinCell,        // BuiltinCellKey::encoded()                    the number of the cell                  its word
    BigInt,             // StringID of the digits                       the number of the constant              -
    GlobalBinding,      // PropertyKeyID, GlobalBindingFlags            the number of the binding               its record
    SiteCache,          // ModuleID and position of each call through which the access was inlined, from the outermost in, then those of the access, then PropertyKeyID, SlotAccess or what tells another use apart      the distance in bytes from the instance register to the index of its SiteAnswers      its Slot
    RequireEdge,        // ModuleID referrer, ModuleID target, StringID the number of the edge                  -
    Variable,           // VariableID                                   the offset in the statics               its word
    CardOfVariable,     // VariableID                                   -                                       the card of its word
    CardOfWord,         // the NumberSpace of a word that holds a cell, then the key of that word               -       the card of that word: MentionList::cardOfWord()
    ModuleValue,        // ModuleID                                     the offset in the statics               its word; the word of require.cache follows
    ModuleNamespace,    // ModuleID                                     the offset in the statics               its word
    ModuleBuiltinCell,  // BuiltinID, ModuleID                          the offset in the statics               its word
    DataCell,           // DataID                                      the offset in the statics               the word of the one cell every evaluation hands out
    DataStorage,        // DataID                                       the offset in the statics               the word of the storage that copies share
    DataElements,       // DataID                                       the offset in bytes from ProgramTables::data() to the first element of the array's node    -
    RegExpLiteralCell,  // ModuleID, position where written, StringID pattern, Yarr::Flags      the offset in the statics       the word of the object that evaluations of the literal share
    TemplateObjectCell, // ModuleID, position where written             the offset in the statics               the word of the object of the tagged template
    ArrayProfile,       // ModuleID and position of each call through which the literal was inlined, from the outermost in, then those of the literal, then the kind of its elements and whether that is decided      the offset in the statics       the ArrayAllocationProfile of the literal
    EnvironmentHandedBack, // LayoutID                                  the offset in the statics               the word of the cell that the next allocation of the layout takes. No root: every collection clears it
    ClassGuard,         // LayoutID, ClassSide; the guard of a name adds Name or NameOfText and the key of the name there, and one guard for several names as much for each      the number of the bit       the byte whose lowest bit is the guard
                        // firstWordOfClassGuardCountKey, then the keys of two guards or more                   the number of its lowest bit            the word of 32 bits that counts those of the guards that do not hold
    ClassRank,          // LayoutID                                     the rank among classes with a record    -
    ClosureFunction,    // FunctionID                                   the closure number of its entry         -
    ClosureCell,        // FunctionID                                   the number of its cell, never 0         -: the closure table lies apart from the instance, at InstanceHeader::closureCells
    Function,           // CallingConventions::indexOf(unit)            the FunctionID of the unit in the image -
    Allocation,         // the words of a PlacedAllocation, then fields the row in the table of allocations     -
    Request,            // RequestKind, then the words of the record    the offset in the table of requests     -
    PropertyRun,        // PropertyKeyID...                             the number of the run                   -
    AtomOfComparedText, // StringID                                     the number of the identifier            -: code compares a pointer with the atom of the text and looks nothing up under it
    ShownString,        // StringID                                     the number of the string's record       -: C++ alone gets the number, and only shows the text: in a message, as the name of what threw
    AtomOfString,       // StringID                                     -                                       -: whoever makes a cell of the text makes it of an atom
    Type,               // TypeID::bits(), of no thread's arena         the offset of the type's record         -
    MethodSelector,     // the place in CallingConventions::methodSelectors()   the number of that word in the table    the word of the method table in which an object with the shape 0 would find its entry
    RegExp,             // StringID pattern, Yarr::Flags                the number of the record                -
};
inline constexpr unsigned numberOfNumberSpaces = 37;
static_assert(static_cast<unsigned>(NumberSpace::RegExp) + 1 == numberOfNumberSpaces);

// The id of a type follows the order in which threads arrived, in Facts and in Plan too. So it is a key that is only compared for equality: the records of types lie in the order of their content,
// ImageNumbers::typesNeededAtRunTime(), and no table or set of bits is indexed by one.

// Whether a name has a column in the front tables changes the code that looks it up, so Plan promises columns in front of Compile: SlotPlan::hasColumn().
// Image gives the promised names the numbers up to RowDisplacementTable::maxColumn, and fails the build where more were promised than there are columns.
// A promised name that no code and no table names any more has no number, and its column goes to a name without a promise.

enum class GlobalBindingFlags : uint8_t {
    SeesScriptBindings = 1 << 0,
    IsStore = 1 << 1,
    IsForTypeOf = 1 << 2,
};

enum class RequestKind : uint32_t {
    // LayoutID
    NewClass,
    // A cooked and a raw StringID for each part. An absent cooked string is Wire::none.
    TemplateObject,
    // EvalScopeID, ModuleID
    DirectEval,
    // DataID, Typed::DataEncoding. MadeOnce also mentions DataCell of the data. CopyOnWriteArray also mentions DataStorage, unless the storage lies in the image.
    Data,
    // DataID, whether it has values
    CollectionData,
    // A FormatPatternHeader and what follows it, in words: the record itself, but for the digest of ICU, which Image fills in.
    FormatPattern,
};

struct Mention {
    NumberSpace space;
    uint8_t numberOfWords;
    // Longer keys: numberOfWords is longKey, and the first word at firstWord is the number of words that follow it.
    static constexpr uint8_t longKey = UINT8_MAX;
    uint16_t reserved { 0 };
    // In CodeBody::mentionWords.
    uint32_t firstWord;
    // Added to the number. Two fields of one word: the number in one, this in the other.
    int32_t addend;
};
static_assert(sizeof(Mention) == 12);

// The key of NumberSpace::Allocation begins with the members of PlacedAllocation in their order, a word each: layout, shape, size, orderWord, constructedBy, numberOfCaptures, isBornUnfinished.
inline constexpr size_t layoutWordOfAllocationKey = 0;
inline constexpr size_t constructorWordOfAllocationKey = 4;

// The mentions of the function that a thread is compiling. Equal mentions get one index.
class MentionList {
    WTF_MAKE_NONCOPYABLE(MentionList);
public:
    MentionList() = default;

    JS_EXPORT_PRIVATE uint32_t add(NumberSpace, std::span<const uint32_t> key, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t add(NumberSpace, uint32_t key, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t addPair(NumberSpace, uint32_t first, uint32_t second, int32_t addend = 0);
    template<typename Tag> uint32_t add(NumberSpace space, Typed::ID<Tag> id, int32_t addend = 0) { return add(space, id.index(), addend); }

    static ClassGuardKey keyOfClassGuard(LayoutID classLayout, ClassSide side) { return { classLayout.index(), static_cast<uint32_t>(side) }; }
    // `spaceOfName` is Name or NameOfText. The name counts as mentioned there too, so it has a number for the record of the class to list the guard under.
    // `isAskedByStoresAlone`: another guard than the one that reads ask, of the side of the prototypes (object/AOTClassGuards.h).
    static ClassGuardKey keyOfClassGuardOfName(LayoutID classLayout, ClassSide side, NumberSpace spaceOfName, uint32_t name, bool isAskedByStoresAlone = false)
    {
        return { classLayout.index(), sideWordOfClassGuardOfName(side, isAskedByStoresAlone), static_cast<uint32_t>(spaceOfName), name };
    }
    // One guard for two names of the side or more, each a key of the program, ascending and none twice.
    static ClassGuardKey keyOfClassGuardOfNames(LayoutID classLayout, ClassSide side, std::span<const PropertyKeyID> names)
    {
        ClassGuardKey key { classLayout.index(), static_cast<uint32_t>(side) };
        for (PropertyKeyID name : names) {
            key.append(static_cast<uint32_t>(NumberSpace::Name));
            key.append(name.index());
        }
        return key;
    }
    uint32_t classGuard(const ClassGuardKey& key) { return add(NumberSpace::ClassGuard, key.span()); }
    // The word that counts two guards or more, of whatever classes and sides. Each of the guards counts as mentioned too, so it has a bit.
    JS_EXPORT_PRIVATE uint32_t classGuardCount(std::span<const ClassGuardKey> guards);
    uint32_t builtinCell(BuiltinCellKey key) { return add(NumberSpace::BuiltinCell, key.encoded()); }
    uint32_t globalBinding(PropertyKeyID key, OptionSet<GlobalBindingFlags> flags) { return addPair(NumberSpace::GlobalBinding, key.index(), flags.toRaw()); }
    // What a Slot says holds for a Structure, a name and a kind of access, wherever it is asked. So the name and the access are part of the key: accesses that are written at one place share a site only where they ask the same question.
    // The places keep the sites apart: a helper that is inlined at two calls meets the objects of each at a site of its own. `access` is a SlotAccess, or for a place with SiteAnswers its SiteUse counted from firstUseOfSiteAnswers.
    static constexpr uint32_t firstUseOfSiteAnswers = 8;
    template<size_t inlineCapacity>
    uint32_t siteCache(Vector<uint32_t, inlineCapacity>&& modulesAndPositions, PropertyKeyID key, uint32_t access)
    {
        modulesAndPositions.append(key.index());
        modulesAndPositions.append(access);
        return add(NumberSpace::SiteCache, modulesAndPositions.span());
    }
    uint32_t regExp(StringID pattern, OptionSet<Yarr::Flags> flags) { return addPair(NumberSpace::RegExp, pattern.index(), flags.toRaw()); }
    // The place where the RegExp is written, so that every inlined copy of it names one cell. Copies of one call of RegExp may make different patterns, and a cell holds the object of one.
    uint32_t regExpLiteralCell(Wire::SourceLocation written, StringID pattern, OptionSet<Yarr::Flags> flags)
    {
        uint32_t key[] { written.module, written.position, pattern.index(), flags.toRaw() };
        return add(NumberSpace::RegExpLiteralCell, std::span<const uint32_t>(key));
    }
    uint32_t templateObjectCell(Wire::SourceLocation written) { return addPair(NumberSpace::TemplateObjectCell, written.module, written.position); }
    // For the barrier behind a store into a word of the statics that holds a cell: `space` is ModuleNamespace, ModuleBuiltinCell, DataCell, RegExpLiteralCell or TemplateObjectCell, `key` what the word itself is mentioned by.
    uint32_t cardOfWord(NumberSpace space, std::span<const uint32_t> key)
    {
        Vector<uint32_t, 4> words;
        words.append(static_cast<uint32_t>(space));
        words.append(key);
        return add(NumberSpace::CardOfWord, words.span());
    }
    JS_EXPORT_PRIVATE uint32_t allocation(const Typed::PlacedAllocation&, std::span<const FieldIndex> initialFields, int32_t addend = 0);
    JS_EXPORT_PRIVATE uint32_t request(RequestKind, std::span<const uint32_t> words);

    bool isEmpty() const { return m_mentions.isEmpty(); }
    // Leaves the list empty for the next function.
    JS_EXPORT_PRIVATE void moveTo(CodeBody&);

private:
    template<size_t extent> uint32_t addKey(NumberSpace, std::span<const uint32_t, extent> key, int32_t addend);
    void growTable();

    Vector<Mention> m_mentions;
    Vector<uint32_t> m_words;
    // Open addressing over indices into m_mentions plus one.
    Vector<uint32_t> m_table;
};

JS_EXPORT_PRIVATE std::span<const uint32_t> keyOf(const Mention&, std::span<const uint32_t> mentionWords);

// Behind MentionList::moveTo(), while the arena of the unit's walk still holds its texts: fills CodeBody::bytesOfMadeTexts, firstsOfMadeTexts and endsOfMadeTexts, and names each made text among the mentions by its place there.
JS_EXPORT_PRIVATE void copyMadeTextsMentionedBy(CodeBody&, const Typed::TypeArena&);
// The first step of Image that looks at mentions, on one thread while it adds to the strings of the program. A made text that the program spells too is that string. The others are added in the order of their bytes, and share bytes as they do in their body.
// Behind it no mention names a made text.
void internTextsMadeByFunctions(Typed::Program&, LinkedFunctions&, BuildContext&);

// A key of NumberSpace::Allocation, read back as MentionList::allocation() wrote it. The fields lie in the key.
struct AllocationKey {
    Typed::PlacedAllocation allocation;
    std::span<const FieldIndex> initialFields;
};
JS_EXPORT_PRIVATE AllocationKey decodeAllocationKey(std::span<const uint32_t> key);

// Whether the keys may be mentioned as NumberSpace::PropertyRun: some, no more than LimitID::MaxPropertyListNames, each an ordinary name, none twice. Whoever mentions a run asks first.
bool canBePropertyRun(const Typed::Program&, std::span<const uint32_t> keys);

// The units that have to have code because finished code mentions the thing: the entry behind a closure number, the members that the record of a class creates,
// the closures that slots of an allocated layout defer, the closure that a variable is born as, the body of a module. They are no roots of the queue: unitsNamedOutsideCode() of plan/AOTPlan.h lists the rest.
// A pure function of the mention, the program, Facts and the plan. The units that relocations of SymbolSpace::Function name are the driver's own to add.
JS_EXPORT_PRIVATE void forEachUnitBehind(const Mention&, std::span<const uint32_t> mentionWords, const Typed::Program&, const Typed::Facts&, const Typed::Plan&, NOESCAPE const ScopedLambda<void(CodeUnit)>&);
// ClosureWord: a closure of the function may exist, and its word names the entry.
enum class MentionKind : uint8_t { Call, ClosureWord };
JS_EXPORT_PRIVATE void forEachUnitBehind(const Mention&, std::span<const uint32_t> mentionWords, const Typed::Program&, const Typed::Facts&, const Typed::Plan&, NOESCAPE const ScopedLambda<void(CodeUnit, MentionKind)>&);

// ---- What Image answers

// The functions of the program keep their ids, and a Body has the id of its function. The other units that have code follow:
// first every unit with code for which isGenericEntry(role) holds, ascending by unit, then the rest, ascending by unit.
// Code that asks for several functions at once compares a closure number with the numbers of two of them, so it is compiled for that order: CallingConventions::haveNeighboringClosureNumbers().
class FunctionNumbers {
public:
    FunctionID idOf(uint32_t unitIndex) const
    {
        if (unitIndex < m_idOfUnit.size())
            return m_idOfUnit[unitIndex];
        auto found = std::ranges::lower_bound(m_idsOfUnitsBehindFunctions, unitIndex, { }, &IDOfUnit::unit);
        return found != m_idsOfUnitsBehindFunctions.end() && found->unit == unitIndex ? found->id : FunctionID();
    }
    // Of an id at or above firstDerived().
    uint32_t unitOf(FunctionID id) const { return m_unitOfDerived[id.index() - m_firstDerived]; }
    uint32_t firstDerived() const { return m_firstDerived; }
    uint32_t numberOfFunctions() const { return m_firstDerived + m_unitOfDerived.size(); }
    FunctionID firstClosureFunction() const { return FunctionID(m_firstDerived); }
    FunctionID endOfClosureFunctions() const { return FunctionID(m_firstDerived + m_numberOfClosureFunctions); }
    // What closure words, requests for closures and the stubs have for a function that closures are made of.
    uint32_t closureNumberOf(FunctionID closureEntry) const { return closureEntry.index() - m_firstDerived; }

private:
    friend struct ImageNumbersBuilder;

    struct IDOfUnit {
        uint32_t unit;
        FunctionID id;
    };

    // [unit index], of the units that a function of the program has by its roles.
    Vector<FunctionID> m_idOfUnit;
    // The indexes behind those count whatever the plan allows for, a hundred times what is compiled. Only the units with code are listed, ascending.
    Vector<IDOfUnit> m_idsOfUnitsBehindFunctions;
    Vector<uint32_t> m_unitOfDerived;
    uint32_t m_firstDerived { 0 };
    uint32_t m_numberOfClosureFunctions { 0 };
};

// A variable that no code and no table mentions has no word, whatever the plan allows it.
struct PlacedVariable {
    bool hasWordInStatics() const { return hasWord; }

    Typed::PlannedVariable plan;
    bool hasWord { false };
    uint32_t offset { 0 };
    uint64_t initialWord { 0 };
    FunctionID function;
};

struct ModuleBuiltinCell {
    BuiltinID builtin;
    ModuleID module;
    uint32_t offset { 0 };
};

// A name that one function spells as a key of the program and another as a text has two guards, and the record of the class lists both under the number of the name.
struct PlacedClassGuardOfName {
    uint32_t sideWord() const { return sideWordOfClassGuardOfName(side, isAskedByStoresAlone); }
    std::array<uint32_t, 4> key() const { return { classLayout.index(), sideWord(), static_cast<uint32_t>(spaceOfName), name }; }
    friend auto operator<=>(const PlacedClassGuardOfName& first, const PlacedClassGuardOfName& second) { return first.key() <=> second.key(); }
    friend bool operator==(const PlacedClassGuardOfName& first, const PlacedClassGuardOfName& second) { return first.key() == second.key(); }

    LayoutID classLayout;
    ClassSide side { ClassSide::Prototype };
    // Name or NameOfText, and the key of the name there.
    NumberSpace spaceOfName { NumberSpace::Name };
    uint32_t name { 0 };
    // From the start of the statics, a multiple of eight.
    uint32_t bit { 0 };
    bool isAskedByStoresAlone { false };
};

// The word that counts the guards which one test lists.
struct PlacedClassGuardCount {
    // From the start of the statics, a multiple of four.
    uint32_t offset { 0 };
    // How many of the guards hold only from the first evaluation of their class on: until then their bits say that they do not hold.
    uint32_t firstValue { 0 };
};

struct PlacedGuardOfClassGuardCount {
    friend auto operator<=>(const PlacedGuardOfClassGuardCount&, const PlacedGuardOfClassGuardCount&) = default;

    uint32_t bit { 0 };
    uint32_t offsetOfCount { 0 };
};

struct PlacedStatics {
    static constexpr uint32_t noOffset = UINT32_MAX;

    // [VariableID]
    Vector<PlacedVariable> variables;
    // [ModuleID], noOffset where the module has none.
    Vector<uint32_t> moduleValueOffsets;
    Vector<uint32_t> moduleNamespaceOffsets;
    // [DataID]
    Vector<uint32_t> dataCellOffsets;
    Vector<uint32_t> dataStorageOffsets;
    // [place in ImageNumbers::regExpLiteralCells() and templateObjectCells()]
    Vector<uint32_t> regExpLiteralCellOffsets;
    Vector<uint32_t> templateObjectCellOffsets;
    // [place in ImageNumbers::arrayProfiles()]. The profiles lie side by side. As far behind each as distanceFromArrayProfileToLastArray says lies the word of the array that its last review made, which every collection clears.
    Vector<uint32_t> arrayProfileOffsets;
    uint32_t distanceFromArrayProfileToLastArray { 0 };
    // Ascending by built-in, then by module.
    Vector<ModuleBuiltinCell> moduleBuiltinCells;
    // [LayoutID * 2 + ClassSide]: the number of the bit from the start of the statics, a multiple of eight, or noClassGuardBit.
    Vector<uint32_t> classGuardBits;
    // Ascending.
    Vector<PlacedClassGuardOfName> classGuardsOfNames;
    // One guard for several names has one bit, and whatever ends the guard of one of the names ends it: the record of the class lists the bit under the number of each.
    // Each name of each such guard with the bit of the guard, in the order of the guards, so ascending by class and side.
    Vector<PlacedClassGuardOfName> namesOfClassGuardsOfSeveralNames;
    // [place in ImageNumbers::classGuards()]. Of a count: the number of the lowest bit of its word.
    Vector<uint32_t> bitsOfClassGuards;
    // Ascending by offset. They lie among the words that are filled with the instance, equal first values side by side.
    Vector<PlacedClassGuardCount> classGuardCounts;
    // Ascending: each guard of each count.
    Vector<PlacedGuardOfClassGuardCount> guardsOfClassGuardCounts;
    // [LayoutID]
    Vector<uint32_t> environmentHandedBackOffsets;
    Vector<uint32_t> roots;
    Vector<StaticsRange> clearedEachCollection;
    uint32_t sizeFilledAtProgramEntry { 0 };
    uint32_t size { 0 };
};

struct PlacedGlobalBinding {
    PropertyKeyID key;
    OptionSet<GlobalBindingFlags> flags;
    bool isStoredByName { false };
    bool isStoredThroughBinding { false };
};

struct RequireEdge {
    ModuleID referrer;
    ModuleID target;
    StringID specifier;

    friend auto operator<=>(const RequireEdge&, const RequireEdge&) = default;
};

// A list of keys of one space, ascending, each once. The number of a key is its place, or what the space's table of numbers has at its place.
struct NumberedKeys {
    std::span<const uint32_t> keyAt(uint32_t index) const { return words.subspan(firstWord[index], firstWord[index + 1] - firstWord[index]); }
    uint32_t size() const { return firstWord.isEmpty() ? 0 : firstWord.size() - 1; }
    JS_EXPORT_PRIVATE std::optional<uint32_t> find(std::span<const uint32_t> key) const;
    JS_EXPORT_PRIVATE void buildHashIndex(const ParallelLoops&);

    Vector<uint32_t> words;
    Vector<uint32_t> firstWord;
    // index + 1, linear probing. Empty: find() searches the sorted list.
    Vector<uint32_t> hashSlots;
};

// Everything that the code of the program mentioned and that the tables name, with its number. Made once, behind Compile. Read by any thread.
class ImageNumbers {
    WTF_MAKE_NONCOPYABLE(ImageNumbers);
public:
    ImageNumbers();
    ~ImageNumbers();

    // The 32 bits that go into the code, the addend included. For SymbolSpace::InstanceWord: the signed distance from the instance register.
    // None: the thing has no such number, which is an error of the compiler.
    JS_EXPORT_PRIVATE std::optional<uint32_t> valueOf(const Mention&, std::span<const uint32_t> mentionWords, bool asInstanceWord) const;
    // Without the addend.
    JS_EXPORT_PRIVATE std::optional<uint32_t> numberOf(NumberSpace, std::span<const uint32_t> key) const;

    const FunctionNumbers& functions() const { return m_functions; }
    const IdentifierNumbering& identifiers() const { return *m_identifiers; }
    const StringConstantNumbering& stringConstants() const { return *m_stringConstants; }
    const RegExpRequests& regExpRequests() const { return *m_regExpRequests; }
    const PlacedStatics& statics() const { return m_statics; }
    InstanceSizes instanceSizes() const { return m_instanceSizes; }

    // By StringID: code or a table names the record of the string. Every string of LiteralStringTable::strings() is among them but for literalStringsWithoutText().
    const BitVector& namedStrings() const { return m_namedStrings; }
    // By StringID: the strings of LiteralStringTable::strings() that no code, no table and no name spells. The image holds a LiteralStringDigest in place of the text of each. An image that runs in several realms has none.
    const BitVector& literalStringsWithoutText() const { return m_literalStringsWithoutText; }
    // Every string whose text the image holds, once, in the order in which the texts lie: the texts of names, then the other short ones, then the long ones, each by the first function that names the text and then by StringID.
    std::span<const StringID> stringsInOrderOfText() const { return m_stringsInOrderOfText.span(); }
    // The named strings that only C++ reads, and seldom: for a message, a stack trace, the name of a function, the path of a module.
    // What tables show and what code mentions in NumberSpace::ShownString. None that code mentions in NumberSpace::String, none of a regular expression, none of NamesOfTables::stringsWithOtherUsesThanShowing. They have no header in the image.
    const BitVector& stringsOnlyShown() const { return m_stringsOnlyShown; }
    // By StringID: the text is used as a key or compared, or names a group of a regular expression. Its cell is made of an atom. Every one of them is among namedStrings().
    const BitVector& atomsOfStrings() const { return m_atomsOfStrings; }

    std::span<const PropertyKeyID> keyCells() const { return m_keyCells.span(); }
    std::span<const BuiltinCellKey> builtinCells() const { return m_builtinCells.span(); }
    // Places in builtinCells().
    std::span<const uint32_t> eagerBuiltinCells() const { return m_eagerBuiltinCells.span(); }
    std::span<const StringID> bigIntConstants() const { return m_bigIntConstants.span(); }
    std::span<const PlacedGlobalBinding> globalBindings() const { return m_globalBindings.span(); }
    std::span<const RequireEdge> requireEdges() const { return m_requireEdges.span(); }

    // [LayoutID]: how many layouts with a smaller index are of the kind Instance and have Plan::hasClassRecord(). Plan::isInUse() is not asked: a class of which no object is made is still defined.
    std::optional<uint32_t> rankOfClass(LayoutID id) const { return id.index() < m_classRanks.size() && m_classRanks[id.index()] != UINT32_MAX ? std::optional(m_classRanks[id.index()]) : std::nullopt; }
    // 0: the function has no cell in the closure table. First the cells that are filled while modules load, then the rest, each by module and then by function.
    uint32_t closureCellNumber(FunctionID id) const { return id.index() < m_closureCellNumbers.size() ? m_closureCellNumbers[id.index()] : 0; }
    // With cell 0, which stands for none: what ProgramTables::numberOfClosureCells holds.
    uint32_t numberOfClosureCells() const { return m_numberOfClosureCells; }

    // The key at a place is what MentionList::allocation(), request() and add(PropertyRun) were given.
    const NumberedKeys& allocations() const { return m_allocations; }
    const NumberedKeys& requests() const { return m_requests; }
    // [place in requests()]
    std::span<const uint32_t> offsetsOfRequests() const { return m_offsetsOfRequests.span(); }
    uint32_t sizeOfRequests() const { return m_sizeOfRequests; }
    const NumberedKeys& propertyRuns() const { return m_propertyRuns; }
    const NumberedKeys& regExpLiteralCells() const { return m_regExpLiteralCells; }
    const NumberedKeys& templateObjectCells() const { return m_templateObjectCells; }
    const NumberedKeys& arrayProfiles() const { return m_arrayProfiles; }
    // The sites of one module lie side by side, in the order in which they are written, and their numbers ascend with their places.
    const NumberedKeys& siteCaches() const { return m_siteCaches; }
    // [place in siteCaches()]: the number of the site. Behind the last: the number of numbers, InstanceSizes::numberOfSiteCaches.
    // ImageSection::NamesOfSites has two bytes for a number, which hold the number of a name that the plan promises a column. A place with SiteAnswers under any other name has two numbers, and so four bytes.
    std::span<const uint32_t> numbersOfSites() const { return m_numbersOfSites.span(); }
    static bool isPlaceWithSiteAnswers(std::span<const uint32_t> keyOfSite) { return keyOfSite.back() >= MentionList::firstUseOfSiteAnswers; }
    static PropertyKeyID nameOfSite(std::span<const uint32_t> keyOfSite) { return PropertyKeyID(keyOfSite[keyOfSite.size() - 2]); }
    // The method table is words of 32 bits, InstanceSizes::sizeOfMethodTable bytes of them. An object with the shape s finds its entry for a selector s words below the origin of the selector: methodTableEntryOf().
    // [place in CallingConventions::methodSelectors()]: the origin, counted in words from the first word of the table. PlacedStatics::noOffset: no code mentions the selector.
    const Vector<uint32_t>& originsOfMethodSelectors() const { return m_originsOfMethodSelectors; }

    // The types that code or a table names, and those that their records name, each once, in the order of TypeArena::compareByContent(): the order of their records.
    std::span<const TypeID> typesNeededAtRunTime() const { return m_typesNeededAtRunTime.span(); }
    // The place in that list.
    std::optional<uint32_t> placeOfType(TypeID type) const
    {
        auto found = std::ranges::lower_bound(m_placesOfTypes, type.bits(), { }, &PlaceOfType::bits);
        return found != m_placesOfTypes.end() && found->bits == type.bits() ? std::optional(found->place) : std::nullopt;
    }
    // [place in typesNeededAtRunTime()]. The table of types is the first part of the tables to be written, and says where each record lies before any code is linked.
    void setOffsetsOfTypes(Vector<uint32_t>&& offsets) { m_offsetsOfTypes = WTF::move(offsets); }
    // [DataID], 0 where the data has no node: FirstTableParts::dataNodes. The table of data is written beside the table of types, before any code is linked.
    void setNodesOfData(Vector<uint32_t>&& nodes) { m_nodesOfData = WTF::move(nodes); }

private:
    friend struct ImageNumbersBuilder;

    struct PlaceOfType {
        uint32_t bits;
        uint32_t place;
    };

    FunctionNumbers m_functions;
    std::unique_ptr<IdentifierNumbering> m_identifiers;
    std::unique_ptr<StringConstantNumbering> m_stringConstants;
    std::unique_ptr<RegExpRequests> m_regExpRequests;
    PlacedStatics m_statics;
    InstanceSizes m_instanceSizes { };
    BitVector m_namedStrings;
    BitVector m_literalStringsWithoutText;
    Vector<StringID> m_stringsInOrderOfText;
    BitVector m_stringsOnlyShown;
    BitVector m_atomsOfStrings;
    Vector<PropertyKeyID> m_keyCells;
    Vector<uint32_t> m_keyCellNumbersPlusOne;
    Vector<BuiltinCellKey> m_builtinCells;
    Vector<uint32_t> m_eagerBuiltinCells;
    Vector<StringID> m_bigIntConstants;
    Vector<PlacedGlobalBinding> m_globalBindings;
    Vector<RequireEdge> m_requireEdges;
    Vector<uint32_t> m_classRanks;
    Vector<uint32_t> m_closureCellNumbers;
    uint32_t m_numberOfClosureCells { 0 };
    NumberedKeys m_allocations;
    NumberedKeys m_requests;
    Vector<uint32_t> m_offsetsOfRequests;
    uint32_t m_sizeOfRequests { 0 };
    NumberedKeys m_propertyRuns;
    NumberedKeys m_regExpLiteralCells;
    NumberedKeys m_templateObjectCells;
    NumberedKeys m_arrayProfiles;
    NumberedKeys m_classGuards;
    NumberedKeys m_siteCaches;
    Vector<uint32_t> m_numbersOfSites { 0 };
    Vector<uint32_t> m_originsOfMethodSelectors;
    Vector<TypeID> m_typesNeededAtRunTime;
    // Ascending by bits, for the lookup alone.
    Vector<PlaceOfType> m_placesOfTypes;
    Vector<uint32_t> m_offsetsOfTypes;
    Vector<uint32_t> m_nodesOfData;
};

// ---- How Image gets there. Each step is one file.

// A function of the program in the order of the sources: by its module, then by where it is written there. A null id is the greatest, so a function without a module comes behind the others, and no function behind all.
struct FunctionByModule {
    // AOTGatherMentions.cpp
    static FunctionByModule forFunction(const Typed::Program&, FunctionID);
    friend auto operator<=>(const FunctionByModule&, const FunctionByModule&) = default;

    ModuleID module;
    uint32_t position { UINT32_MAX };
    FunctionID function;
};

// What was mentioned, gathered from every function in parallel. A space whose key is one id is a set of bits and a count of mentions by id. Any other space is a list of keys.
struct GatheredMentions {
    struct ByID {
        BitVector isMentioned;
        // How many units mention the id, those left out whose code is proven to run at most once: what sets a module up looks a name up once. Zero does not say that no code mentions it. Only Name and NameOfText keep it.
        Vector<uint32_t> numberOfFunctions;
        // The first function with a unit whose code mentions the id. Only Name, NameOfText, AtomOfComparedText, String, StringCell and KeyCell keep it.
        Vector<FunctionByModule> firstFunction;
    };

    ByID& byID(NumberSpace space) { return idSpaces[static_cast<unsigned>(space)]; }
    const ByID& byID(NumberSpace space) const { return idSpaces[static_cast<unsigned>(space)]; }
    const NumberedKeys& keys(NumberSpace space) const { return keySpaces[static_cast<unsigned>(space)]; }
    // For the step that makes the list a table of the numbers as it is, once nothing else reads it here: the longest holds a key for each access of the program.
    NumberedKeys takeKeys(NumberSpace space) { return std::exchange(keySpaces[static_cast<unsigned>(space)], { }); }
    bool isMentioned(NumberSpace space, uint32_t id) const { return byID(space).isMentioned.get(id); }
    // Of a space that keeps it. No function: no code mentions the id.
    FunctionByModule firstFunctionMentioning(NumberSpace space, uint32_t id) const
    {
        const Vector<FunctionByModule>& functions = byID(space).firstFunction;
        return id < functions.size() ? functions[id] : FunctionByModule { };
    }

    std::array<ByID, numberOfNumberSpaces> idSpaces;
    std::array<NumberedKeys, numberOfNumberSpaces> keySpaces;
};

constexpr bool hasOneIDAsKey(NumberSpace space)
{
    switch (space) {
    case NumberSpace::Name:
    case NumberSpace::NameOfText:
    case NumberSpace::String:
    case NumberSpace::StringCell:
    case NumberSpace::KeyCell:
    case NumberSpace::BuiltinCell:
    case NumberSpace::BigInt:
    case NumberSpace::Variable:
    case NumberSpace::CardOfVariable:
    case NumberSpace::ModuleValue:
    case NumberSpace::ModuleNamespace:
    case NumberSpace::DataCell:
    case NumberSpace::DataStorage:
    case NumberSpace::DataElements:
    case NumberSpace::EnvironmentHandedBack:
    case NumberSpace::ClassRank:
    case NumberSpace::ClosureFunction:
    case NumberSpace::ClosureCell:
    case NumberSpace::Function:
    case NumberSpace::AtomOfComparedText:
    case NumberSpace::ShownString:
    case NumberSpace::AtomOfString:
    case NumberSpace::MethodSelector:
        return true;
    case NumberSpace::CardOfWord:
    case NumberSpace::Type:
    case NumberSpace::GlobalBinding:
    case NumberSpace::SiteCache:
    case NumberSpace::RequireEdge:
    case NumberSpace::ModuleBuiltinCell:
    case NumberSpace::ClassGuard:
    case NumberSpace::RegExpLiteralCell:
    case NumberSpace::TemplateObjectCell:
    case NumberSpace::ArrayProfile:
    case NumberSpace::Allocation:
    case NumberSpace::Request:
    case NumberSpace::PropertyRun:
    case NumberSpace::RegExp:
        return false;
    }
    RELEASE_ASSERT_NOT_REACHED_UNDER_CONSTEXPR_CONTEXT();
}

struct NumberingInputs {
    Typed::Program& program;
    const Typed::Facts& facts;
    const Typed::Plan& plan;
    // In the order of their units.
    const LinkedFunctions& functions;
};

// AOTGatherMentions.cpp. A request for data mentions the cell or the storage of the data too: RequestKind::Data.
GatheredMentions gatherMentions(const NumberingInputs&, BuildContext&);

// What the tables of the image name whether or not code does. Each group of tables adds its own: AOTTableParts.h.
struct NamesOfTables {
    explicit NamesOfTables(const Typed::Program&);
    void merge(const NamesOfTables&);

    BitVector keys;
    BitVector textsNamedAsKeys;
    BitVector strings;
    BitVector stringsWithOtherUsesThanShowing;
    BitVector stringConstants;
    // By StringID of the digits: NumberSpace::BigInt. Whoever sets a bit here sets it in strings and stringsWithOtherUsesThanShowing too: the table of the constants names the record of the digits.
    BitVector bigInts;
    // Keys of NumberSpace::Allocation as MentionList::allocation() makes them: the number of words of a key, then its words. In any order, with repeats.
    Vector<uint32_t> allocations;
    // In any order, with repeats.
    Vector<TypeID> types;
    BitVector variables;
    BitVector functionsWithClosureCell;
};

// A variable that the plan puts into the statics gets its word only where somebody names it.
inline bool isVariableOfStaticsNamed(const Typed::PlannedVariable& planned, VariableID id, const NamesOfTables& tables, const GatheredMentions& mentions)
{
    return planned.isDescribedToRunTime
        || tables.variables.get(id.index())
        || mentions.isMentioned(NumberSpace::Variable, id.index())
        || mentions.isMentioned(NumberSpace::CardOfVariable, id.index());
}

struct ImageNumbersBuilder {
    const NumberingInputs& inputs;
    // numberRows() and layOutStatics() take the lists that they hand on: GatheredMentions::takeKeys().
    GatheredMentions& mentions;
    const NamesOfTables& tables;
    ImageNumbers& numbers;
    BuildContext& context;

    // AOTNumberFunctions.cpp
    void numberFunctions();
    void numberClosureCellsAndClassRanks();
    // AOTStringUses.cpp
    void numberIdentifiersAndStrings();
    // AOTNumberConstants.cpp
    void numberConstants();
    void numberRows();
    // typesNeededAtRunTime() and the places: what the mentions of NumberSpace::Type and NamesOfTables::types name, sorted by content through a TypeArena of its own.
    void numberTypes();
    // AOTRegExpRequests.cpp. Adds the atoms and the names of groups to the strings of the program, so it runs in front of numberIdentifiersAndStrings().
    void collectRegExpRequests();
    // AOTStaticsLayout.cpp. The last step: it also fills regExpLiteralCells(), templateObjectCells(), arrayProfiles() and instanceSizes(), so it runs behind numberIdentifiersAndStrings() and numberConstants().
    void layOutStatics();
};

// AOTNumbering.cpp: the steps above. Between the requests for regular expressions and the statics, those that do not lean on each other are lanes: runLanes().
// `laneBesideNumbering` runs once the requests for regular expressions are collected, beside the lanes that number. It reads no other number, and from there on nothing adds a string to the program.
void numberWhatStaticsWaitFor(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&, NOESCAPE const ScopedLambda<void()>& laneBesideNumbering = scopedLambda<void()>([] { }));
// The last step of the numbering. It writes nothing that says which strings the image holds, or in which order.
void layOutStaticsOfMentions(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);
// The one behind the other.
void numberMentions(const NumberingInputs&, GatheredMentions&, const NamesOfTables&, ImageNumbers&, BuildContext&);

} } // namespace JSC::AOT

#endif // ENABLE(AOT)
